#include "PCH.h"
#include "Forms/SaveBuffer.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <windows.h>
#include <REL/Relocation.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

using namespace std::string_view_literals;

namespace Pulsar::Forms
{
    namespace {
        // Vtable IDs (verified runtime probe + Ghidra cross-check, 1.16.236):
        //   450162 = BGSSaveFormBuffer (abstract base) - marker only
        //   450091 = BGSSaveLoadUtilities::BGSSaveFormBufferIgnoreIDs (concrete)
        //   398652 = BGSLoadFormBuffer
        //   450193 = SaveBufferStorageWrapper - DIFFERENT class, also abstract,
        //            DO NOT use as a BGSSaveFormBuffer impl
        static REL::Relocation<std::uintptr_t> g_saveVtableAbstract{ REL::ID(450162) };
        static REL::Relocation<std::uintptr_t> g_saveVtableConcrete{ REL::ID(450091) };  // IgnoreIDs
        static REL::Relocation<std::uintptr_t> g_loadVtable        { REL::ID(398652) };

        // Inline static read by FUN_14185FE30 to seed changeFlags. Not the
        // BGSSaveLoadManager singleton (that's at +0x4A8); this is a separate
        // global at +0x4A0 holding the engine's "current changeFlags" state.
        static REL::Relocation<std::uintptr_t> g_saveCfgGlobal{ REL::Offset{ 0x5E6D6E0 } };

        // FUN_14186B2D0 - engine's BGSSaveFormBuffer::Grow(buf, newCapacity).
        using GrowFn = void(__fastcall*)(void*, std::uint32_t);
        static REL::Relocation<GrowFn> g_grow{ REL::Offset{ 0x1868B30 } };

        // FUN_1418349D0 - Bethesda's pre-save context-setup. Called BEFORE
        // FUN_141835740 in FUN_14181D690 line 619. Fills a 16-byte stack ctx
        // (worldspace ptr + cell ptr + flag byte) used later by header writer
        // + post-save bookkeeping. Without this, FUN_141835740 has no context.
        // RE'd: takes (ctx[16]*, form*) - see save_chain_step2.txt.
        using PreSetupFn = void(__fastcall*)(void* /*ctx16*/, RE::TESForm*);
        static REL::Relocation<PreSetupFn> g_preSetup{ REL::Offset{ 0x1832260 } };

        // FUN_141835740 - Bethesda's record-header writer. Called between
        // pre-setup and vt[0x1A]. Takes (buffer, form, flags, ctx16, scratch).
        // Internally dispatches to switch(saveMode) where saveMode comes from
        // FUN_141835670(formID, formType). Each case writes formID + type +
        // size header bytes into buffer.dataBuffer using FUN_14186B2D0 grow.
        using SaveHeaderFn = std::uint64_t(__fastcall*)(
            void* /*buffer*/, RE::TESForm* /*form*/, std::uint64_t /*flags*/,
            void* /*ctx16*/, std::uint8_t* /*scratch*/);
        static REL::Relocation<SaveHeaderFn> g_saveHeader{ REL::Offset{ 0x1832FD0 } };

        // FUN_1402BA6E0 - engine memory free, paired with FUN_14186B2D0's
        // grow allocator. Signature: (ptr, sizeBytes). Used in FUN_14185FE30
        // cleanup to free pluginArray + formIDArray data.
        using EngineFreeFn = void(__fastcall*)(void*, std::size_t);
        static REL::Relocation<EngineFreeFn> g_engineFree{ REL::Offset{ 0x2BA340 } };

        // vt[0x1B] LoadGame - receiver-side deserialize entry point.
        // BGSLoadFormBuffer is a different struct (0xF0 bytes, REL::ID 398652).
        // Built when we receive a PKT_FORM_BLOB.

        // 16-byte ctx struct that FUN_1418349D0 fills + FUN_141835740 reads.
        // Layout from FUN_14181D690 stack: first 8 bytes raw + longlong + char.
        struct alignas(8) PreSaveCtx {
            std::uint8_t b8[8];   // +0x00 - partially-init scratch
            std::int64_t b0;      // +0x08 - worldspace/cell ptr (used post-save)
            char         a8;      // +0x10 - flag byte (read post-save)
            std::uint8_t pad[7];  // → 24 bytes total
        };
        static_assert(sizeof(PreSaveCtx) == 24, "PreSaveCtx must be 24 bytes");

        using SaveGameFn  = void(__fastcall*)(RE::TESForm*, void*);
        using AddChangeFn = bool(__fastcall*)(RE::TESForm*, std::uint32_t);
        using RemChangeFn = void(__fastcall*)(RE::TESForm*, std::uint32_t);
        using CheckSaveFn = bool(__fastcall*)(RE::TESForm*, void*);
        using DtorFn      = void*(__fastcall*)(void*, std::uint8_t);

        static std::uint32_t ReadEngineChangeFlags() noexcept
        {
            __try {
                return *reinterpret_cast<std::uint32_t*>(
                    g_saveCfgGlobal.address() + 0x380);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0xFFFFFFFFu;  // "save everything" fallback
            }
        }

        // Hex-dump first N bytes, returns "AA BB CC ..." string.
        static std::string HexDump(const std::uint8_t* p, std::size_t n)
        {
            std::string s;
            s.reserve(n * 3);
            char buf[4];
            for (std::size_t i = 0; i < n; ++i) {
                std::snprintf(buf, sizeof(buf), "%02X ", p[i]);
                s.append(buf);
            }
            return s;
        }
    }

    void RunProbe()
    {
        const auto vtAbstractAddr = g_saveVtableAbstract.address();
        const auto vtConcreteAddr = g_saveVtableConcrete.address();
        const auto vtLoadAddr     = g_loadVtable.address();

        PLOG_INFO(
            "[PULSAR] FormsProbe: SaveFormBufferRaw size=0x{:X} (expect 0x80)"sv,
            sizeof(SaveFormBufferRaw));

        PLOG_INFO(
            "[PULSAR] FormsProbe: vtables abstract=0x{:X} concrete(IgnoreIDs)=0x{:X} load=0x{:X}"sv,
            vtAbstractAddr, vtConcreteAddr, vtLoadAddr);

        // Verify struct field layout - write a stack instance and check offsets
        // match the Ghidra-derived layout. Phase 2 corrected: dataCapacity@+0x10,
        // dataSize@+0x14, flags@+0x58 (was wrong as +0x54 in iter1).
        SaveFormBufferRaw sb{};
        std::memset(&sb, 0, sizeof(sb));
        sb.changeFlags  = 0xDEADBEEFu;
        sb.flags        = 0x42;
        sb.dataCapacity = 0xCAFE0000u;
        sb.dataSize     = 0x12345678u;

        const auto* base = reinterpret_cast<const std::uint8_t*>(&sb);
        PLOG_INFO(
            "[PULSAR] FormsProbe: changeFlags@+0x50 = 0x{:08X} (expect 0xDEADBEEF)"sv,
            *reinterpret_cast<const std::uint32_t*>(base + 0x50));
        PLOG_INFO(
            "[PULSAR] FormsProbe: dataCapacity@+0x10 = 0x{:08X} (expect 0xCAFE0000)"sv,
            *reinterpret_cast<const std::uint32_t*>(base + 0x10));
        PLOG_INFO(
            "[PULSAR] FormsProbe: dataSize@+0x14 = 0x{:08X} (expect 0x12345678)"sv,
            *reinterpret_cast<const std::uint32_t*>(base + 0x14));
        PLOG_INFO(
            "[PULSAR] FormsProbe: flags@+0x58 = 0x{:02X} (expect 0x42)"sv,
            *reinterpret_cast<const std::uint8_t*>(base + 0x58));
    }

    namespace {
        // C2712 workaround: __try cannot live in a function that has C++
        // unwinding objects (std::vector). Extract each protected call into
        // a noexcept helper.

        static bool SafeCallSaveGame(RE::TESForm* form, void* buf) noexcept
        {
            __try {
                auto** formVt = *reinterpret_cast<void***>(form);
                auto saveFn = reinterpret_cast<SaveGameFn>(formVt[0x1A]);
                saveFn(form, buf);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        static bool SafeCallPreSetup(void* ctx16, RE::TESForm* form) noexcept
        {
            __try {
                g_preSetup(ctx16, form);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Returns the save-mode case-result (1-6 typically). 0 if threw.
        static std::uint64_t SafeCallSaveHeader(void* buf, RE::TESForm* form,
                                                 std::uint64_t flags,
                                                 void* ctx16,
                                                 std::uint8_t* scratch) noexcept
        {
            __try {
                return g_saveHeader(buf, form, flags, ctx16, scratch);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // vt[0x17] AddChange - marks the form as having pending changes for
        // the given flag bits. SaveGame writes only fields whose change-bits
        // are set on BOTH the form (engine-tracked) and the buffer. At game
        // start everything is 0, so we have to mark ourselves before save.
        static bool SafeAddChange(RE::TESForm* form, std::uint32_t flags) noexcept
        {
            __try {
                auto** formVt = *reinterpret_cast<void***>(form);
                auto fn = reinterpret_cast<AddChangeFn>(formVt[0x17]);
                return fn(form, flags);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // vt[0x18] RemoveChange - undo AddChange so we don't pollute the
        // engine's actual save with our forced flags.
        static void SafeRemoveChange(RE::TESForm* form, std::uint32_t flags) noexcept
        {
            __try {
                auto** formVt = *reinterpret_cast<void***>(form);
                auto fn = reinterpret_cast<RemChangeFn>(formVt[0x18]);
                fn(form, flags);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        // vt[0x19] CheckSaveGame - engine asks "do I have anything worth
        // saving into this buffer?". Diagnostic only.
        static int SafeCheckSaveGame(RE::TESForm* form, void* buf) noexcept
        {
            __try {
                auto** formVt = *reinterpret_cast<void***>(form);
                auto fn = reinterpret_cast<CheckSaveFn>(formVt[0x19]);
                return fn(form, buf) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        static bool SafeCopyBuffer(const void* src, std::size_t n,
                                    std::uint8_t* dst) noexcept
        {
            __try {
                std::memcpy(dst, src, n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        static bool SafeGrowBuffer(void* buf, std::uint32_t cap) noexcept
        {
            __try {
                g_grow(buf, cap);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Manual cleanup - vt[0] dtor throws because abstract-base dtor expects
        // formID/plugin arrays to be proper engine vector instances with refcounts;
        // we only have raw zero-init'd pointers from save-helper outputs. Just
        // free what we know about: dataBuffer + formID/plugin array data.
        static bool SafeManualCleanup(SaveFormBufferRaw* buf) noexcept
        {
            __try {
                if (buf->dataBuffer && buf->dataCapacity > 0) {
                    g_engineFree(buf->dataBuffer, buf->dataCapacity);
                    buf->dataBuffer = nullptr;
                    buf->dataCapacity = 0;
                    buf->dataSize = 0;
                }
                if (buf->formIDArrayBegin) {
                    const auto bytes = reinterpret_cast<std::uintptr_t>(buf->formIDArrayCapEnd) -
                                       reinterpret_cast<std::uintptr_t>(buf->formIDArrayBegin);
                    g_engineFree(buf->formIDArrayBegin, bytes);
                    buf->formIDArrayBegin = nullptr;
                    buf->formIDArrayEnd = nullptr;
                    buf->formIDArrayCapEnd = nullptr;
                }
                if (buf->pluginArrayBegin) {
                    const auto bytes = reinterpret_cast<std::uintptr_t>(buf->pluginArrayCapEnd) -
                                       reinterpret_cast<std::uintptr_t>(buf->pluginArrayBegin);
                    g_engineFree(buf->pluginArrayBegin, bytes);
                    buf->pluginArrayBegin = nullptr;
                    buf->pluginArrayEnd = nullptr;
                    buf->pluginArrayCapEnd = nullptr;
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    std::vector<std::uint8_t> SerializeForm(RE::TESForm* form,
                                             std::uint32_t changeFlagsOverride)
    {
        if (!form) return {};

        // Stack-init buffer per FUN_14185FE30 canonical pattern.
        SaveFormBufferRaw buf{};
        std::memset(&buf, 0, sizeof(buf));
        // Use ABSTRACT vtable - that's what FUN_140B2EF30 / FUN_14181D690
        // (Bethesda's two save callsites) use. The concrete IgnoreIDs vtable
        // path goes through a different helper (FUN_14185FB50) that needs
        // more state; abstract+FUN_140B2F760 is the simpler entry point.
        buf.vftable     = reinterpret_cast<void**>(g_saveVtableAbstract.address());
        buf.changeFlags = changeFlagsOverride != 0
            ? changeFlagsOverride
            : ReadEngineChangeFlags();
        buf.flags       = 1;
        // Sentinel uint16 at +0x7B = 0xFF00 (per init pattern). Lives in _trailing.
        auto* basePtr = reinterpret_cast<std::uint8_t*>(&buf);
        *reinterpret_cast<std::uint16_t*>(basePtr + 0x7B) = 0xFF00;

        std::vector<std::uint8_t> blob;
        const std::uint32_t formID = form->formID;

        // Pre-alloc dataBuffer via engine grow primitive.
        constexpr std::uint32_t kInitCap = 64 * 1024;
        const bool growOk = SafeGrowBuffer(&buf, kInitCap);

        // AddChange/RemoveChange REMOVED 2026-04-26: caused host save corruption
        // (naked player, empty inventory, frozen). Engine treats RemoveChange
        // as "no pending state to save" and discards runtime data on next
        // auto-save. Bethesda's own save (FUN_14181D690) does NOT call
        // AddChange - it relies on whatever changeFlags the engine has
        // tracked from gameplay. For a live player REFR mid-game, those bits
        // are already populated by movement / inventory / etc. events.

        // Diag: ask the form if it thinks it has anything to save now.
        const int checkResult = SafeCheckSaveGame(form, &buf);

        // Bethesda save sequence per FUN_14181D690 RE pass:
        //   1. PreSetup(ctx16, form)            - fills 16-byte stack ctx
        //   2. SaveHeader(buf, form, flags, ctx16, &scratch) - writes
        //      formID + type + size header into buf.dataBuffer
        //   3. form->vt[0x1A] SaveGame(buf)    - writes form body
        PreSaveCtx ctx{};
        std::memset(&ctx, 0, sizeof(ctx));
        const bool preOk = SafeCallPreSetup(&ctx, form);

        std::uint8_t hdrScratch = 0;
        const std::uint64_t hdrResult =
            SafeCallSaveHeader(&buf, form, 0xFFFFFFFFu, &ctx, &hdrScratch);

        if (!SafeCallSaveGame(form, &buf)) {
            PLOG_WARN(
                "[PULSAR] SerializeForm: vt[0x1A] threw (form=0x{:08X})"sv,
                formID);
        }

        PLOG_INFO(
            "[PULSAR] SerializeForm: form=0x{:08X} cf=0x{:08X} grow={} chk={} pre={} hdr={} → buf{{ ptr={} cap={} size={} }}"sv,
            formID, buf.changeFlags, growOk ? 1 : 0,
            checkResult, preOk ? 1 : 0, hdrResult,
            buf.dataBuffer ? "non-null" : "NULL",
            buf.dataCapacity, buf.dataSize);

        if (buf.dataBuffer && buf.dataSize > 0 &&
            buf.dataSize <= 16 * 1024 * 1024)  // 16 MB sanity cap
        {
            blob.resize(buf.dataSize);
            if (!SafeCopyBuffer(buf.dataBuffer, buf.dataSize, blob.data())) {
                PLOG_WARN(
                    "[PULSAR] SerializeForm: blob copy threw (size={})"sv,
                    buf.dataSize);
                blob.clear();
            }
        }

        // Cleanup - manual free of dataBuffer + formID/plugin arrays via
        // engine's matching free (FUN_1402BA6E0). vt[0] dtor crashed in iter6
        // because abstract-base dtor expects refcount-tracked vectors that
        // we don't have.
        if (!SafeManualCleanup(&buf)) {
            PLOG_WARN("[PULSAR] SerializeForm: manual cleanup threw"sv);
        }

        return blob;
    }

    // ----- DESERIALIZE PATH (receiver-side) ---------------------------------
    //
    // BGSLoadFormBuffer is 0xF0 bytes (240). Layout from TiltedEvolution
    // Skyrim pattern + memory/reference_cell_transition_re.md:
    //   +0x00 vftable (REL::ID 398652 @ 0x144B21CE8)
    //   +0x08 dataBuffer (we point at our blob)
    //   +0x10 dataSize / position state (engine reads sequentially)
    //   +0x14 changeFlags  (mirrors save side)
    //   +0x18..0xEF - sub-buffers + form-table refs (0-init for our use)
    //
    // Engine's vt[0x1B] LoadGame on target reads bytes from buffer.dataBuffer
    // and applies them to the form. Same vtable invariants as save: needs
    // matching record-header/body framing, which our SerializeForm produces.

    namespace {
        struct alignas(8) LoadFormBufferRaw {
            void**        vftable;       // +0x00 - REL::ID 398652
            void*         dataBuffer;    // +0x08
            std::uint32_t dataSize;      // +0x10
            std::uint32_t changeFlags;   // +0x14
            std::uint8_t  _trailing[0xF0 - 0x18];
        };
        static_assert(sizeof(LoadFormBufferRaw) == 0xF0,
                      "LoadFormBufferRaw must be 240 bytes");

        using LoadGameFn = void(__fastcall*)(RE::TESForm*, void*);

        static bool SafeCallLoadGame(RE::TESForm* form, void* buf) noexcept
        {
            __try {
                auto** formVt = *reinterpret_cast<void***>(form);
                auto fn = reinterpret_cast<LoadGameFn>(formVt[0x1B]);
                fn(form, buf);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    bool DeserializeForm(RE::TESForm* target,
                         const std::uint8_t* blob,
                         std::size_t blobSize,
                         std::uint32_t sourceFormID)
    {
        if (!target || !blob || blobSize == 0) return false;

        LoadFormBufferRaw lb{};
        std::memset(&lb, 0, sizeof(lb));
        lb.vftable     = reinterpret_cast<void**>(g_loadVtable.address());
        lb.dataBuffer  = const_cast<std::uint8_t*>(blob);
        lb.dataSize    = static_cast<std::uint32_t>(blobSize);
        lb.changeFlags = 0xFFFFFFFFu;

        const std::uint32_t targetID = target->formID;
        const bool ok = SafeCallLoadGame(target, &lb);

        PLOG_INFO(
            "[PULSAR] DeserializeForm: src=0x{:08X} -> target=0x{:08X} blob={}B {}"sv,
            sourceFormID, targetID, blobSize, ok ? "ok" : "THREW");
        return ok;
    }

    // Phase 2 probe - serialize the player REFR (Actor, formType=kACHR='K').
    // Pivot from iter1-5 TESNPC target: TESNPC base is identical on both
    // host+client (vanilla form 0x7), so serializing it can't carry per-player
    // appearance. The Actor REFR is the actual save-unit Bethesda uses for
    // player state - RE'd chain (FUN_14181D690 → preSetup → header → vt[0x1A])
    // is built around formType=='K' specifically.
    static void RunSerializeProbe()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_INFO("[PULSAR] FormsProbe2: no player singleton"sv);
            return;
        }

        // Player REFR cast to TESForm - Actor inherits TESObjectREFR inherits
        // TESForm, so the cast is valid. formType will be 'K' (kACHR=0x4B).
        auto* form = static_cast<RE::TESForm*>(player);
        const std::uint32_t formID = form->formID;
        const std::uint8_t  formType =
            *reinterpret_cast<const std::uint8_t*>(
                reinterpret_cast<const std::uint8_t*>(form) + 0x2E);

        PLOG_INFO(
            "[PULSAR] FormsProbe2: serializing player REFR 0x{:08X} type=0x{:02X} (expect 'K'=0x4B)"sv,
            formID, formType);

        auto blob = SerializeForm(form, 0xFFFFFFFFu);
        PLOG_INFO(
            "[PULSAR] FormsProbe2: SerializeForm → {} bytes"sv, blob.size());

        if (!blob.empty()) {
            const auto n = std::min<std::size_t>(64, blob.size());
            PLOG_INFO(
                "[PULSAR] FormsProbe2: blob[0..{}] = {}"sv,
                n, HexDump(blob.data(), n));
        }
    }

    // ----- NETWORK BRIDGE ---------------------------------------------------

    namespace {
        struct PendingBlob {
            std::uint32_t sourceFormID;
            std::uint32_t targetFormID;
            std::uint8_t  formType;
            std::int64_t  enqueueTickMs;  // ::GetTickCount64()
            std::vector<std::uint8_t> blob;
        };
        static std::mutex              s_pendingMu;
        static std::vector<PendingBlob> s_pending;
        // Max time to keep retrying a blob before dropping it. After this,
        // the receiver is presumed to have moved on (next cell-transition
        // will produce a fresher blob anyway).
        static constexpr std::int64_t kBlobMaxAgeMs = 5000;
        // Cooldown between successive applies on the same target - gives the
        // engine time to settle after each LoadGame call.
        static constexpr std::int64_t kApplyCooldownMs = 2000;
        static std::int64_t s_lastApplyTickMs = 0;
    }

    void BroadcastPlayerForm(std::uint32_t targetFormID)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;

        auto* form = static_cast<RE::TESForm*>(player);
        const std::uint32_t srcID = form->formID;
        const std::uint8_t  fType = *reinterpret_cast<const std::uint8_t*>(
            reinterpret_cast<const std::uint8_t*>(form) + 0x2E);

        auto blob = SerializeForm(form, 0xFFFFFFFFu);
        if (blob.empty()) {
            PLOG_WARN("[PULSAR] FormBlob: serialize failed for 0x{:08X}"sv, srcID);
            return;
        }
        if (blob.size() > Pulsar::Network::FORM_BLOB_MAX_SIZE) {
            PLOG_WARN(
                "[PULSAR] FormBlob: oversized {}B > cap {}B, dropped"sv,
                blob.size(), Pulsar::Network::FORM_BLOB_MAX_SIZE);
            return;
        }

        const std::size_t total = 1 + sizeof(Pulsar::Network::FormBlobHeader) + blob.size();
        std::vector<std::uint8_t> packet(total);
        packet[0] = Pulsar::Network::PKT_FORM_BLOB;
        Pulsar::Network::FormBlobHeader hdr{};
        hdr.sourceFormID = srcID;
        hdr.targetFormID = targetFormID;
        hdr.blobSize     = static_cast<std::uint32_t>(blob.size());
        hdr.formType     = fType;
        std::memcpy(packet.data() + 1, &hdr, sizeof(hdr));
        std::memcpy(packet.data() + 1 + sizeof(hdr), blob.data(), blob.size());

        net.Send(packet.data(), packet.size(), /*reliable=*/true);
        PLOG_INFO(
            "[PULSAR] FormBlob: sent src=0x{:08X} tgt=0x{:08X} type=0x{:02X} {}B"sv,
            srcID, targetFormID, fType, blob.size());
    }

    void OnPacketReceived(const std::uint8_t* data, std::size_t length)
    {
        if (length < 1 + sizeof(Pulsar::Network::FormBlobHeader)) return;
        Pulsar::Network::FormBlobHeader hdr{};
        std::memcpy(&hdr, data + 1, sizeof(hdr));
        if (hdr.blobSize == 0 ||
            hdr.blobSize > Pulsar::Network::FORM_BLOB_MAX_SIZE ||
            length < 1 + sizeof(hdr) + hdr.blobSize)
        {
            PLOG_WARN("[PULSAR] FormBlob: malformed packet (size mismatch)"sv);
            return;
        }

        PendingBlob p;
        p.sourceFormID  = hdr.sourceFormID;
        p.targetFormID  = hdr.targetFormID;
        p.formType      = hdr.formType;
        p.enqueueTickMs = static_cast<std::int64_t>(::GetTickCount64());
        p.blob.assign(data + 1 + sizeof(hdr),
                      data + 1 + sizeof(hdr) + hdr.blobSize);

        std::lock_guard g(s_pendingMu);
        // Drop any older blob from the same source - newer wins.
        s_pending.erase(
            std::remove_if(s_pending.begin(), s_pending.end(),
                [&](const PendingBlob& q) {
                    return q.sourceFormID == p.sourceFormID;
                }),
            s_pending.end());
        s_pending.push_back(std::move(p));
        PLOG_INFO(
            "[PULSAR] FormBlob: queued src=0x{:08X} tgt=0x{:08X} type=0x{:02X} {}B"sv,
            hdr.sourceFormID, hdr.targetFormID, hdr.formType, hdr.blobSize);
    }

    void DrainPending()
    {
        const std::int64_t nowMs = static_cast<std::int64_t>(::GetTickCount64());
        if (nowMs - s_lastApplyTickMs < kApplyCooldownMs) return;

        // Pop the SINGLE oldest blob (one apply per tick max). Drop if too old.
        PendingBlob p;
        bool have = false;
        {
            std::lock_guard g(s_pendingMu);
            // Drop expired entries first.
            s_pending.erase(
                std::remove_if(s_pending.begin(), s_pending.end(),
                    [nowMs](const PendingBlob& q) {
                        return nowMs - q.enqueueTickMs > kBlobMaxAgeMs;
                    }),
                s_pending.end());
            if (!s_pending.empty()) {
                p = std::move(s_pending.front());
                s_pending.erase(s_pending.begin());
                have = true;
            }
        }
        if (!have) return;

        // Resolve target.
        RE::TESForm* target = nullptr;
        if (p.targetFormID != 0) {
            target = RE::TESForm::LookupByID(p.targetFormID);
        } else {
            auto& mgr = Pulsar::RemotePlayer::GetRemotePlayerManager();
            target = static_cast<RE::TESForm*>(mgr.GetRemoteActor());
        }

        // Stable-state gate - only apply if target is in a usable state. If
        // not, requeue (still bounded by kBlobMaxAgeMs). Avoids hammering the
        // engine during clone respawn cycles.
        bool stable = false;
        if (auto* refr = static_cast<RE::TESObjectREFR*>(
                dynamic_cast<RE::TESObjectREFR*>(target)))
        {
            stable = (refr->parentCell != nullptr);
        } else if (target) {
            stable = true;  // non-REFR forms have no parentCell concept
        }

        if (!target || !stable) {
            // Requeue - preserve enqueue tick so age-out still works.
            std::lock_guard g(s_pendingMu);
            s_pending.push_back(std::move(p));
            return;
        }

        s_lastApplyTickMs = nowMs;
        DeserializeForm(target, p.blob.data(), p.blob.size(), p.sourceFormID);
    }

    // Public entry - chained from Init.
    void Init()
    {
        RunProbe();
        // RunSerializeProbe is deferred to TickProbe - at plugin-init the
        // player singleton is still null (game hasn't loaded any save yet).
    }

    void TickProbe(bool isHost)
    {
        static std::atomic<bool> s_fired{ false };
        if (s_fired.load(std::memory_order_acquire)) return;

        // Gate 0: host-only. Client doesn't serialize forms - it deserializes.
        if (!isHost) return;

        // Gate 1: host actually in-game with player loaded into a cell.
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;

        // Gate 2: client connected. The probe AddChange's flags + may touch
        // save-mgr-adjacent state - must NOT fire while the connect-time
        // save+transfer pipeline is mid-flight.
        if (!Pulsar::Network::GetSingleton().IsConnected()) return;

        // Gate 3: settle-delay after connect - give the engine ~10s to
        // finish the connect-time save sync before we mutate change flags.
        // Captures monotonic "first time we saw a connection" so the actual
        // probe runs ≥10s later.
        static std::atomic<std::int64_t> s_firstConnTick{ 0 };
        const auto nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        std::int64_t prev = 0;
        s_firstConnTick.compare_exchange_strong(prev, nowMs);
        if (nowMs - s_firstConnTick.load() < 10'000) return;

        bool expected = false;
        if (!s_fired.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel))
            return;

        RunSerializeProbe();
    }
}
