#include "PCH.h"
#include "WorldSync/LockSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/NPCSync.h"           // GetActiveCellsSnapshot()
#include "NPCSync/PluginLookup.h"

#include <windows.h>
#include <REL/Relocation.h>

#include <RE/F/FormTypes.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::WorldSync::Lock {
namespace {

    // ---- Local raw-offset helpers ----------------------------------------

    static uint32_t RawFormID(const void* form) noexcept
    {
        if (!form) return 0;
        return *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(form) + 0x28);
    }

    static uint8_t RawFormType(const void* form) noexcept
    {
        if (!form) return 0;
        return *reinterpret_cast<const uint8_t*>(
            static_cast<const uint8_t*>(form) + 0x2E);
    }

    // ExtraDataList::GetByType (REL::ID 37878). LootSync resolves the same
    // primitive for kObjectInstance walking; we resolve it independently
    // to keep the module standalone.
    using GetExtraByTypeFn = void* (__fastcall*)(void* extraList, uint32_t type);
    static GetExtraByTypeFn g_getExtraByType = nullptr;

    // Papyrus ObjectReference.SetOpen native handler. RE'd 2026-04-25 via
    // "SetOpen" string xref @ RVA 0x208E039 - the registration block has
    // lea r9 → 0x2080BC0 (handler), lea r8 → "ObjectReference", lea rdx
    // → "SetOpen". REL::ID(118464) @ RVA 0x2080BC0.
    //
    // Internally calls TESObjectREFR::Activate (REL::ID 63380) twice (one
    // for the open path, one for close). On a closed container with valid
    // 3D state the call ends up snapping the lid open via the same toggle
    // path Activate uses - but WITHOUT scheduling the engine's async
    // auto-close that a normal clone-Activate-replay would trigger. That's
    // why SetOpen(true) gives sustained-open with smooth animation.
    //
    // VM args 1+2 are only touched on error paths (None-self, no-3D), so
    // calling with nullptr/0 from C++ is safe for the success path on a
    // valid REFR with 3D loaded - same pattern as F.1.e PapyrusUnequip.
    using PapyrusSetOpenFn = void (__fastcall*)(
        void* vmCtx, uint32_t vmStackId,
        RE::TESObjectREFR* self, bool open);
    static PapyrusSetOpenFn g_setOpenPapyrus = nullptr;

    static bool SafeCallSetOpen(RE::TESObjectREFR* refr, bool open) noexcept
    {
        if (!g_setOpenPapyrus || !refr) return false;
        bool ok = false;
        __try {
            g_setOpenPapyrus(nullptr, 0, refr, open);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    // ---- ExtraLock probe -------------------------------------------------
    // ExtraLock layout (CommonLibSF, verified live on 1.16.236):
    //   BSExtraData header (vtable + next ptr, 0x18 bytes)
    //   +0x18 REFR_LOCK* lock
    // REFR_LOCK (0x10 bytes):
    //   +0x00 TESKey* key
    //   +0x08 uint32  numTries
    //   +0x0C uint8   flags (bit 0 = kLocked, bit 2 = kLeveled)
    //   +0x0D int8    baseLevel (numeric skill threshold, e.g. 50 = Master)

    struct ProbeResult {
        bool      hasExtraLock;
        bool      hasLockStruct;
        uintptr_t lockPtr;
        uint32_t  numTries;
        uint8_t   flags;
        int8_t    baseLevel;
    };

    static ProbeResult ProbeLock(RE::TESObjectREFR* refr) noexcept
    {
        ProbeResult r{};
        if (!refr || !g_getExtraByType) return r;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return r;
            void* extraLock = g_getExtraByType(extraList, 0x28);
            if (!extraLock) return r;
            r.hasExtraLock = true;
            auto* refrLock = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(extraLock) + 0x18);
            if (!refrLock) return r;
            r.hasLockStruct = true;
            r.lockPtr   = reinterpret_cast<uintptr_t>(refrLock);
            r.numTries  = *reinterpret_cast<uint32_t*>(refrLock + 0x08);
            r.flags     = *reinterpret_cast<uint8_t*>(refrLock + 0x0C);
            r.baseLevel = *reinterpret_cast<int8_t*>(refrLock + 0x0D);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return r;
    }

    // Apply lock-state on a REFR. SEH-wrapped because the REFR pointer
    // crosses queue boundaries and may have been freed by cell unload.
    // Sets the kLocked bit per `locked` param and resets numTries to 0.
    static bool ApplyLockState(RE::TESObjectREFR* refr, uint8_t locked) noexcept
    {
        if (!refr || !g_getExtraByType) return false;
        bool ok = false;
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(refr);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return false;
            void* extraLock = g_getExtraByType(extraList, 0x28);
            if (!extraLock) return false;
            auto* refrLock = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(extraLock) + 0x18);
            if (!refrLock) return false;
            uint8_t cur = *reinterpret_cast<uint8_t*>(refrLock + 0x0C);
            if (locked)
                cur |= 0x01;
            else
                cur &= ~0x01;
            *reinterpret_cast<uint8_t*>(refrLock + 0x0C) = cur;
            *reinterpret_cast<uint32_t*>(refrLock + 0x08) = 0;
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    // ---- Sender state - per-REFR last-seen flags -------------------------

    static std::mutex                                       s_seenMutex;
    static std::unordered_map<RE::TESObjectREFR*, uint8_t>  s_seenFlags;

    // ---- Pending queues (receiver) --------------------------------------

    struct PendingLock {
        uint32_t resolvedBase;
        uint8_t  baseType;
        uint8_t  locked;
        float    px, py, pz;
    };
    static std::mutex                  s_pendingMutex;
    static std::vector<PendingLock>    s_pending;

    // F.3.c container-open queue. Separate from PendingLock so the drain
    // can apply locks first (engine constraint: SetOpen on a still-locked
    // REFR is a no-op).
    struct PendingContainerOpen {
        uint32_t resolvedBase;
        float    px, py, pz;
    };
    static std::mutex                          s_pendingOpensMutex;
    static std::vector<PendingContainerOpen>   s_pendingOpens;

    // ---- Cell-scan helper -----------------------------------------------
    // Generic base+pos matcher for kCONT or kDOOR. Mirrors DoorSync's
    // SafeFindDoorByBaseInCell pattern.

    static RE::TESObjectREFR* SafeFindRefByBaseInCell(void* cell,
                                                       uint32_t resolvedBase,
                                                       uint8_t baseType,
                                                       float px, float py, float pz,
                                                       float radius) noexcept
    {
        __try {
            if (!cell) return nullptr;
            auto* cb = reinterpret_cast<uint8_t*>(cell);
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = radius * radius;
            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(ref) + 0x98);
                if (!base) continue;
                if (RawFormType(base) != baseType) continue;
                if (RawFormID(base) != resolvedBase) continue;
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);
                float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = ref;
                }
            }
            return best;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // ---- Wire encoding ---------------------------------------------------

    static void SendContainerOpenPacket(uint32_t refID, uint32_t baseID,
                                          float px, float py, float pz)
    {
        using namespace Pulsar::Network;

        ContainerOpenData data{};
        data.refFormID    = refID;
        data.baseFormID   = baseID;
        data.baseLocalID  = NPCSync::PluginLookup::FormIDToLocalID(baseID);
        data.posX         = px;
        data.posY         = py;
        data.posZ         = pz;
        NPCSync::PluginLookup::FormIDToPluginName(
            baseID, data.basePlugin, sizeof(data.basePlugin));

        uint8_t buf[1 + sizeof(ContainerOpenData)]{};
        buf[0] = PKT_CONTAINER_OPEN;
        std::memcpy(buf + 1, &data, sizeof(data));

        auto& net = Pulsar::Network::GetSingleton();
        net.Send(buf, sizeof(buf), true);

        PLOG_INFO(
            "[PULSAR] ContainerOpen TX: ref=0x{:08X} base=0x{:08X} plugin='{}' @({:.0f},{:.0f},{:.0f})"sv,
            refID, baseID,
            data.basePlugin[0] ? data.basePlugin : "(none)",
            px, py, pz);
    }

    static void SendLockPacket(uint32_t refID, uint32_t baseID,
                                uint8_t baseType, uint8_t locked,
                                float px, float py, float pz)
    {
        using namespace Pulsar::Network;

        LockStateData data{};
        data.refFormID    = refID;
        data.baseFormID   = baseID;
        data.baseLocalID  = NPCSync::PluginLookup::FormIDToLocalID(baseID);
        data.posX         = px;
        data.posY         = py;
        data.posZ         = pz;
        data.baseFormType = baseType;
        data.locked       = locked;
        NPCSync::PluginLookup::FormIDToPluginName(
            baseID, data.basePlugin, sizeof(data.basePlugin));

        uint8_t buf[1 + sizeof(LockStateData)]{};
        buf[0] = PKT_LOCK_STATE;
        std::memcpy(buf + 1, &data, sizeof(data));

        auto& net = Pulsar::Network::GetSingleton();
        net.Send(buf, sizeof(buf), true);  // reliable

        PLOG_INFO(
            "[PULSAR] LockSync TX: ref=0x{:08X} base=0x{:08X} type=0x{:02X} plugin='{}' locked={} @({:.0f},{:.0f},{:.0f})"sv,
            refID, baseID, baseType,
            data.basePlugin[0] ? data.basePlugin : "(none)",
            locked ? "YES" : "no",
            px, py, pz);
    }

}  // anonymous namespace

// =====================================================================
// Public API
// =====================================================================

void Init()
{
    REL::Relocation<void*> getExtraTarget{ REL::ID(37878) };
    g_getExtraByType = reinterpret_cast<GetExtraByTypeFn>(getExtraTarget.get());
    PLOG_INFO(
        "[PULSAR] LockSync: GetExtraByType resolved at 0x{:x} (REL::ID 37878)"sv,
        reinterpret_cast<uintptr_t>(g_getExtraByType));

    REL::Relocation<void*> setOpenTarget{ REL::ID(118464) };
    g_setOpenPapyrus = reinterpret_cast<PapyrusSetOpenFn>(setOpenTarget.get());
    PLOG_INFO(
        "[PULSAR] LockSync: PapyrusSetOpen resolved at 0x{:x} (REL::ID 118464)"sv,
        reinterpret_cast<uintptr_t>(g_setOpenPapyrus));
}

void OnHostActivate(RE::TESObjectREFR* refr,
                     uint32_t refID, uint32_t baseID,
                     uint8_t baseType,
                     float px, float py, float pz)
{
    if (!refr) return;
    ProbeResult cur = ProbeLock(refr);
    if (!cur.hasExtraLock) return;

    uint8_t curLocked = (cur.flags & 0x01) ? 1 : 0;

    bool seen = false;
    uint8_t lastLocked = 0;
    {
        std::lock_guard g(s_seenMutex);
        auto it = s_seenFlags.find(refr);
        if (it != s_seenFlags.end()) {
            seen = true;
            lastLocked = (it->second & 0x01) ? 1 : 0;
        }
        s_seenFlags[refr] = cur.flags;
    }

    auto& net = Pulsar::Network::GetSingleton();
    bool connected = net.IsConnected();

    if (!seen) {
        PLOG_INFO(
            "[PULSAR] LockSync seed: ref=0x{:08X} base=0x{:08X} type=0x{:02X} locked={} level={}"sv,
            refID, baseID, baseType,
            curLocked ? "YES" : "no",
            static_cast<int>(cur.baseLevel));
        return;
    }

    if (lastLocked == curLocked) return;

    PLOG_INFO(
        "[PULSAR] LockSync flip: ref=0x{:08X} base=0x{:08X} type=0x{:02X} locked {}->{} (connected={})"sv,
        refID, baseID, baseType,
        lastLocked ? "YES" : "no",
        curLocked  ? "YES" : "no",
        connected ? "yes" : "no");

    if (connected) {
        SendLockPacket(refID, baseID, baseType, curLocked, px, py, pz);
    }
}

void OnHostContainerOpen(RE::TESObjectREFR* refr,
                          uint32_t refID, uint32_t baseID,
                          float px, float py, float pz, bool result)
{
    if (!refr || !result) return;

    // Locked containers are F.3.b LockSync's responsibility (post-pick
    // auto-open also goes through SetOpen on the receiver). Skip here to
    // avoid double-broadcast.
    ProbeResult lockState = ProbeLock(refr);
    if (lockState.hasExtraLock && (lockState.flags & 0x01)) return;

    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected()) return;

    SendContainerOpenPacket(refID, baseID, px, py, pz);
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    if (data[0] != PKT_LOCK_STATE) return;
    if (length < 1 + sizeof(LockStateData)) return;

    LockStateData d{};
    std::memcpy(&d, data + 1, sizeof(d));

    uint32_t resolved = 0;
    if (d.basePlugin[0] != '\0') {
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(
            d.basePlugin, d.baseLocalID);
    }
    if (!resolved) resolved = d.baseFormID;

    {
        std::lock_guard g(s_pendingMutex);
        s_pending.push_back({
            resolved, d.baseFormType, d.locked,
            d.posX, d.posY, d.posZ
        });
    }

    PLOG_INFO(
        "[PULSAR] LockSync RX: base=0x{:08X} plugin='{}' localID=0x{:X} resolved=0x{:08X} type=0x{:02X} locked={} @({:.0f},{:.0f},{:.0f})"sv,
        d.baseFormID,
        d.basePlugin[0] ? d.basePlugin : "(none)",
        d.baseLocalID, resolved, d.baseFormType,
        d.locked ? "YES" : "no",
        d.posX, d.posY, d.posZ);
}

void OnContainerOpenReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    if (data[0] != PKT_CONTAINER_OPEN) return;
    if (length < 1 + sizeof(ContainerOpenData)) return;

    ContainerOpenData d{};
    std::memcpy(&d, data + 1, sizeof(d));

    uint32_t resolved = 0;
    if (d.basePlugin[0] != '\0') {
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(
            d.basePlugin, d.baseLocalID);
    }
    if (!resolved) resolved = d.baseFormID;

    {
        std::lock_guard g(s_pendingOpensMutex);
        s_pendingOpens.push_back({ resolved, d.posX, d.posY, d.posZ });
    }

    PLOG_INFO(
        "[PULSAR] ContainerOpen RX: base=0x{:08X} plugin='{}' resolved=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
        d.baseFormID,
        d.basePlugin[0] ? d.basePlugin : "(none)",
        resolved, d.posX, d.posY, d.posZ);
}

void DrainPending()
{
    std::vector<PendingLock> work;
    std::vector<PendingContainerOpen> openWork;
    {
        std::lock_guard g(s_pendingMutex);
        if (!s_pending.empty()) work.swap(s_pending);
    }
    {
        std::lock_guard g(s_pendingOpensMutex);
        if (!s_pendingOpens.empty()) openWork.swap(s_pendingOpens);
    }
    if (work.empty() && openWork.empty()) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        if (!work.empty()) {
            std::lock_guard g(s_pendingMutex);
            s_pending.insert(s_pending.begin(), work.begin(), work.end());
        }
        if (!openWork.empty()) {
            std::lock_guard g(s_pendingOpensMutex);
            s_pendingOpens.insert(s_pendingOpens.begin(), openWork.begin(), openWork.end());
        }
        return;
    }

    for (auto& d : work) {
        RE::TESObjectREFR* refr = nullptr;
        if (player->parentCell) {
            refr = SafeFindRefByBaseInCell(player->parentCell,
                                            d.resolvedBase, d.baseType,
                                            d.px, d.py, d.pz, 1200.0f);
        }
        if (!refr) {
            auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
            for (auto* cell : cells) {
                if (!cell) continue;
                if (cell == player->parentCell) continue;
                refr = SafeFindRefByBaseInCell(cell, d.resolvedBase,
                                                d.baseType,
                                                d.px, d.py, d.pz, 1200.0f);
                if (refr) break;
            }
        }

        if (!refr) {
            PLOG_INFO(
                "[PULSAR] LockSync: NO MATCH base=0x{:08X} type=0x{:02X} @({:.0f},{:.0f},{:.0f})"sv,
                d.resolvedBase, d.baseType, d.px, d.py, d.pz);
            continue;
        }

        bool applied = ApplyLockState(refr, d.locked);
        PLOG_INFO(
            "[PULSAR] LockSync: {} ref=0x{:08X} base=0x{:08X} type=0x{:02X} locked={}"sv,
            applied ? "APPLIED" : "FAILED",
            RawFormID(refr), d.resolvedBase, d.baseType,
            d.locked ? "YES" : "no");

        if (applied) {
            std::lock_guard g(s_seenMutex);
            uint8_t flags = d.locked ? 0x01 : 0x00;
            s_seenFlags[refr] = flags;
        }

        // Sustained-open via Papyrus SetOpen native (REL::ID 118464).
        // Animation plays smoothly because SetOpen internally calls
        // Activate; the engine does NOT schedule its async auto-close
        // when called via this path. kDOOR is skipped because F.3
        // DoorSync's Activate-replay handles door open/close animation.
        if (applied && !d.locked
            && d.baseType == static_cast<uint8_t>(RE::FormType::kCONT)) {
            bool ok = SafeCallSetOpen(refr, true);
            PLOG_INFO(
                "[PULSAR] LockSync: SetOpen(true) {} ref=0x{:08X} (CONT) - sustained-open"sv,
                ok ? "ok" : "FAILED",
                RawFormID(refr));
        }
    }

    // F.3.c: drain queued container-open mirrors. Runs AFTER the lock
    // drain so any same-tick PKT_LOCK_STATE has already unlocked the
    // REFR before we try SetOpen on it.
    for (auto& d : openWork) {
        RE::TESObjectREFR* refr = nullptr;
        if (player->parentCell) {
            refr = SafeFindRefByBaseInCell(
                player->parentCell, d.resolvedBase,
                static_cast<uint8_t>(RE::FormType::kCONT),
                d.px, d.py, d.pz, 1200.0f);
        }
        if (!refr) {
            auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
            for (auto* cell : cells) {
                if (!cell) continue;
                if (cell == player->parentCell) continue;
                refr = SafeFindRefByBaseInCell(
                    cell, d.resolvedBase,
                    static_cast<uint8_t>(RE::FormType::kCONT),
                    d.px, d.py, d.pz, 1200.0f);
                if (refr) break;
            }
        }
        if (!refr) {
            PLOG_INFO(
                "[PULSAR] ContainerOpen: NO MATCH base=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
                d.resolvedBase, d.px, d.py, d.pz);
            continue;
        }
        bool ok = SafeCallSetOpen(refr, true);
        PLOG_INFO(
            "[PULSAR] ContainerOpen: SetOpen(true) {} ref=0x{:08X} base=0x{:08X}"sv,
            ok ? "ok" : "FAILED", RawFormID(refr), d.resolvedBase);
    }
}

}  // namespace Pulsar::WorldSync::Lock
