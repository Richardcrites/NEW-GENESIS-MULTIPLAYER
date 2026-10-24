#include "PCH.h"
#include "CellContentSync/CellContentSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/PluginLookup.h"
#include "NASLog.h"

#include <RE/F/FormTypes.h>
#include <RE/N/NiPoint.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::CellContentSync {
namespace {

    // -----------------------------------------------------------------------
    // K.6 V0 - TESObjectCELL.references is at cell+0x80 in Starfield 1.16.236.
    //
    // Verified empirically (autodetect probe results from 23:38 test):
    //   POI exterior (FF01BD38): probe +0x80 → size=931 cap=1024 data=ok ✓
    //                            probe +0x88 → size=885735424 cap=532 data=NULL ✗
    //   POI interior (FF01989C): probe +0x80 → size=5216 cap=8960 data=ok ✓
    //                            probe +0x88 → size=4019838400 cap=527 data=NULL ✗
    //
    // CommonLibSF's TESObjectCELL claims references at +0x80 - comment matches
    // reality. But the compiled CLibSF::ForEachReference wrapper read at
    // cell+0x88 because TESHandleForm is declared sizeof=0x38 in CLibSF, while
    // in the real engine it's 0x30 - shifting all subsequent fields. Our
    // bypass ignores the wrapper and reads directly at +0x80.
    //
    // Layout within BSTArray<NiPointer<TESObjectREFR>>:
    //   +0x00  uint32 size
    //   +0x04  uint32 capacity
    //   +0x08  NiPointer<TESObjectREFR>* pData   (each NiPointer = 8 bytes)
    // -----------------------------------------------------------------------
    constexpr std::ptrdiff_t kCellRefsOffset = 0x80;

    // Settle delay - proc-cell construction continues after loadMenu CLOSED.
    // From the crash log: ENUM fired immediately on TransitSync SEND (same
    // millisecond as loadMenu CLOSED) and crashed reading mid-construct
    // BSTArray. 1500ms gives the engine time to finish the worker thread
    // population pass.
    constexpr std::uint64_t kEnumSettleDelayMs = 1500;

    // Throttle: don't re-enumerate the same destination cell within this
    // window. Door ping-pong (open->close->reopen) can fire transit events
    // back-to-back; we only need one snapshot per arrival.
    constexpr std::uint64_t kReEnumSuppressMs = 2000;

    // Per-ref log cap so a hot cell with hundreds of refs doesn't flood logs.
    // The summary line at the end always shows the full count regardless.
    constexpr std::size_t kLogPerRefCap = 80;

    // Sanity cap on size we'll trust from the array header. Empirical
    // observations:
    //   POI exterior cells:   ~900-1700 refs
    //   Standard POI interior: ~5000-6000 refs
    //   Heavy POI interior:    24868 refs (cap 39936) observed 2026-04-29
    // Cap at 65536 - anything beyond is corruption / mid-resize.
    // Increasing capacity costs nothing in V0/V1 (we cap descriptor batch
    // separately at MAX_CELL_CONTENT_REFS_PER_BATCH=512), this is just the
    // ceiling we trust the BSTArray header to.
    constexpr std::uint32_t kMaxSaneRefCount = 65536;

    static std::atomic<std::uint32_t> g_lastEnumCell{ 0 };
    static std::atomic<std::uint64_t> g_lastEnumTick{ 0 };

    // Pending enum, set by OnHostCellChanged, drained by Tick().
    static std::atomic<std::uint32_t> g_pendingPrevCell{ 0 };
    static std::atomic<std::uint32_t> g_pendingNewCell{ 0 };
    static std::atomic<std::uint64_t> g_pendingDueTick{ 0 };

    // -----------------------------------------------------------------------
    // V2-Probe (client-side) - incoming batches queue + pending probe trigger.
    //
    // Goal: when client transits to a proc cell that matches a recent host
    // batch, scan the client's own cell.references and check for each
    // received descriptor whether a matching local REFR already exists
    // (resolved-base + position-radius). Output is a count of matched vs.
    // missing - the data tells us if proc-gen is deterministic enough that
    // V2 spawn-pipeline isn't needed (matched=N, missing=0) or how many
    // refs would have to be spawned (missing>0).
    // -----------------------------------------------------------------------
    constexpr std::uint64_t kBatchExpiryMs    = 30000;  // discard batches older than 30s
    constexpr float         kProbeMatchRadius = 500.0f; // ~7m, lenient for proc-gen variance
    constexpr std::uint64_t kProbeSettleDelayMs = 1500; // same as host enum settle

    struct PendingBatch {
        std::uint32_t hostCellFormID;
        std::uint8_t  isInterior;
        std::uint8_t  isProc;
        std::uint64_t arrivalTick;
        std::vector<Pulsar::Network::CellRefDescriptor> descriptors;
    };

    static std::mutex                g_batchMutex;
    static std::vector<PendingBatch> g_pendingBatches;

    // Pending client probe - set when client's own cell changes, drained by Tick().
    static std::atomic<std::uint32_t> g_pendingProbeNewCell{ 0 };
    static std::atomic<std::uint64_t> g_pendingProbeDueTick{ 0 };

    // De-dupe client probes against same dest cell.
    static std::atomic<std::uint32_t> g_lastProbeCell{ 0 };
    static std::atomic<std::uint64_t> g_lastProbeTick{ 0 };

    static bool IsInterestingFormType(RE::FormType ft) noexcept
    {
        switch (ft) {
            case RE::FormType::kREFR:
            case RE::FormType::kCONT:
            case RE::FormType::kDOOR:
            case RE::FormType::kACTI:
            case RE::FormType::kFURN:
            case RE::FormType::kACHR:
                return true;
            default:
                return false;
        }
    }

    static const char* FormTypeName(RE::FormType ft) noexcept
    {
        switch (ft) {
            case RE::FormType::kREFR: return "REFR";
            case RE::FormType::kCONT: return "CONT";
            case RE::FormType::kDOOR: return "DOOR";
            case RE::FormType::kACTI: return "ACTI";
            case RE::FormType::kFURN: return "FURN";
            case RE::FormType::kACHR: return "ACHR";
            default: return "OTHER";
        }
    }

    // SEH-wrapped raw read of the references BSTArray header.
    // Layout: size at +0x00, capacity at +0x04, pData at +0x08.
    static int SafeReadCellRefsHeader(const void*    cell,
                                        std::uint32_t* outSize,
                                        std::uint32_t* outCapacity,
                                        void**         outData) noexcept
    {
        __try {
            const auto base = reinterpret_cast<const std::uint8_t*>(cell)
                            + kCellRefsOffset;
            *outSize     = *reinterpret_cast<const std::uint32_t*>(base + 0x00);
            *outCapacity = *reinterpret_cast<const std::uint32_t*>(base + 0x04);
            *outData     = *reinterpret_cast<void* const*>(base + 0x08);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *outSize     = 0;
            *outCapacity = 0;
            *outData     = nullptr;
            return 0;
        }
    }

    // SEH-wrapped read of one NiPointer<TESObjectREFR> from the array.
    // The NiPointer has the inner pointer at offset 0. Returns the inner
    // raw pointer (may be null for empty slots).
    static int SafeReadNiPointer(void* arrayBase, std::uint32_t i,
                                   RE::TESObjectREFR** outRef) noexcept
    {
        __try {
            // Each NiPointer slot is 8 bytes (just an inner pointer).
            auto* slot = reinterpret_cast<RE::TESObjectREFR**>(
                reinterpret_cast<std::uint8_t*>(arrayBase) + (i * 8));
            *outRef = *slot;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *outRef = nullptr;
            return 0;
        }
    }

    struct RefProbe {
        std::uint32_t  refFormID;
        std::uint32_t  baseFormID;
        std::uint8_t   refFormType;
        std::uint8_t   baseFormType;
        float          posX, posY, posZ;
        bool           ok;
    };

    static int SafeProbeRef(RE::TESObjectREFR* ref, RefProbe* out) noexcept
    {
        __try {
            out->refFormID    = ref->formID;
            out->refFormType  = static_cast<std::uint8_t>(ref->formType.get());
            const auto& pos = ref->data.location;
            out->posX = pos.x;
            out->posY = pos.y;
            out->posZ = pos.z;

            auto* base = ref->data.objectReference.get();
            if (base) {
                out->baseFormID   = base->formID;
                out->baseFormType = static_cast<std::uint8_t>(base->formType.get());
            } else {
                out->baseFormID   = 0;
                out->baseFormType = 0;
            }
            out->ok = true;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out->ok = false;
            return 0;
        }
    }

    // The actual enumeration. Runs on the game thread from Tick(). Reads
    // cell.references via verified raw offsets, NOT via CommonLibSF wrapper.
    static void EnumerateCellNow(std::uint32_t prevCellFormID,
                                  std::uint32_t newCellFormID)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* cell = player->parentCell;
        if (!cell) {
            PLOG_WARN("[PULSAR] CellContentSync: enum skipped (player.parentCell=null)"sv);
            return;
        }

        const std::uint32_t cellFormID = cell->formID;
        if (cellFormID != newCellFormID) {
            PLOG_WARN("[PULSAR] CellContentSync: parentCell drift (queued=0x{:08X} now=0x{:08X}) - using actual"sv,
                      newCellFormID, cellFormID);
        }

        const bool       isInterior      = cell->IsInterior();
        const bool       isProc          = (cellFormID & 0xFF000000u) == 0xFF000000u;
        const auto*      ws              = cell->cellWorldspace;
        const std::uint32_t worldspaceID = ws ? ws->formID : 0u;
        const std::uint32_t playerFormID = player->formID;

        std::uint32_t size = 0;
        std::uint32_t capacity = 0;
        void*         data = nullptr;
        if (!SafeReadCellRefsHeader(cell, &size, &capacity, &data)) {
            PLOG_WARN("[PULSAR] CellContentSync: cell-refs header read SEH-faulted cell=0x{:08X}"sv,
                      cellFormID);
            return;
        }

        PLOG_INFO(
            "[PULSAR] CellContentSync ENUM cell=0x{:08X} ws=0x{:08X} interior={} proc={} prev=0x{:08X} size={} cap={} data={}"sv,
            cellFormID, worldspaceID, isInterior ? 1 : 0, isProc ? 1 : 0, prevCellFormID,
            size, capacity, data ? "ok" : "NULL");

        if (!data || size == 0) {
            PLOG_INFO("[PULSAR] CellContentSync: cell empty (data={} size={}) - skip"sv,
                      data ? "ok" : "NULL", size);
            return;
        }
        if (size > kMaxSaneRefCount) {
            PLOG_WARN("[PULSAR] CellContentSync: insane size={} (cap={}) - skip"sv,
                      size, capacity);
            return;
        }

        std::size_t total           = 0;
        std::size_t interestingHit  = 0;
        std::size_t skippedFiltered = 0;
        std::size_t skippedHostRef  = 0;
        std::size_t skippedNullBase = 0;
        std::size_t skippedNullRef  = 0;
        std::size_t probeFailed     = 0;
        std::size_t logged          = 0;
        std::size_t batchTruncated  = 0;

        std::uint32_t perTypeCount[256] = { 0 };

        // V1: collect interesting refs into a batch we'll broadcast at the end.
        std::vector<Pulsar::Network::CellRefDescriptor> batch;
        batch.reserve(256);

        for (std::uint32_t i = 0; i < size; ++i) {
            ++total;
            RE::TESObjectREFR* ref = nullptr;
            if (!SafeReadNiPointer(data, i, &ref)) {
                ++probeFailed;
                continue;
            }
            if (!ref) {
                ++skippedNullRef;
                continue;
            }
            // 2026-04-29: reject obvious-invalid pointer values.
            // Crash dump Starfield_04-28-22-41.dmp showed a slot containing
            // 0xFFFFFFFFFFFFFFFF (-1, kernel address space) - likely a freed
            // BSTArray slot in a transient state. The `if (!ref)` check only
            // catches NULL, not sentinels. Reading *(-1+0x28) raised an
            // AV in kernel-space that SEH didn't catch (paged code path).
            //
            // Valid Windows user-mode pointers are < 0x7FFFFFFFFFFF (47-bit).
            // Reject anything in kernel or unmapped-low range.
            const std::uintptr_t refAddr = reinterpret_cast<std::uintptr_t>(ref);
            if (refAddr < 0x10000ULL || refAddr >= 0x7FFFFFFFFFFFULL) {
                ++skippedNullRef;
                continue;
            }

            RefProbe p{};
            if (!SafeProbeRef(ref, &p) || !p.ok) {
                ++probeFailed;
                continue;
            }

            if (p.refFormID == playerFormID) {
                ++skippedHostRef;
                continue;
            }
            if (p.baseFormID == 0) {
                ++skippedNullBase;
                continue;
            }

            const auto baseFt = static_cast<RE::FormType>(p.baseFormType);
            ++perTypeCount[p.baseFormType];

            if (!IsInterestingFormType(baseFt)) {
                ++skippedFiltered;
                continue;
            }
            ++interestingHit;

            if (logged < kLogPerRefCap) {
                PLOG_INFO(
                    "[PULSAR]   ref=0x{:08X} base=0x{:08X} baseType={} pos=({:.0f},{:.0f},{:.0f})"sv,
                    p.refFormID, p.baseFormID, FormTypeName(baseFt),
                    p.posX, p.posY, p.posZ);
                ++logged;
            }

            // V1: build batch descriptor. Cap at MAX_CELL_CONTENT_REFS_PER_BATCH.
            if (batch.size() < static_cast<std::size_t>(
                    Pulsar::Network::MAX_CELL_CONTENT_REFS_PER_BATCH)) {
                Pulsar::Network::CellRefDescriptor d{};
                d.refFormID    = p.refFormID;
                d.baseFormID   = p.baseFormID;
                // Plugin-local ID is host-local-flag-aware mask; PluginLookup
                // handles the conversion from full FormID using TESDataHandler.
                d.baseLocalID  = Pulsar::NPCSync::PluginLookup::FormIDToLocalID(p.baseFormID);
                Pulsar::NPCSync::PluginLookup::FormIDToPluginName(
                    p.baseFormID, d.basePlugin, sizeof(d.basePlugin));
                d.baseFormType = p.baseFormType;
                d.posX = p.posX; d.posY = p.posY; d.posZ = p.posZ;
                batch.push_back(d);
            } else {
                ++batchTruncated;
            }
        }

        {
            PLOG_INFO(
                "[PULSAR] CellContentSync ENUM-DONE cell=0x{:08X} total={} interesting={} filtered={} hostRef={} nullRef={} nullBase={} probeFail={}"sv,
                cellFormID, total, interestingHit, skippedFiltered,
                skippedHostRef, skippedNullRef, skippedNullBase, probeFailed);
        }

        NASLOG("CellContentSync ENUM cell=0x%08X total=%zu interesting=%zu",
               cellFormID, total, interestingHit);

        int topIdx[3] = { -1, -1, -1 };
        for (int i = 0; i < 256; ++i) {
            if (perTypeCount[i] == 0) continue;
            for (int t = 0; t < 3; ++t) {
                if (topIdx[t] < 0 || perTypeCount[i] > perTypeCount[topIdx[t]]) {
                    for (int s = 2; s > t; --s) topIdx[s] = topIdx[s - 1];
                    topIdx[t] = i;
                    break;
                }
            }
        }
        for (int t = 0; t < 3; ++t) {
            if (topIdx[t] < 0) break;
            const auto ft = static_cast<RE::FormType>(topIdx[t]);
            PLOG_INFO(
                "[PULSAR]   top{} baseType=0x{:02X} {} refs={}"sv,
                t + 1, topIdx[t], FormTypeName(ft), perTypeCount[topIdx[t]]);
        }

        // V1: broadcast the batch (host → client). Receiver logs only in V1.
        if (batch.empty()) {
            PLOG_INFO("[PULSAR] CellContentSync: nothing to broadcast (empty batch)"sv);
            return;
        }
        if (batchTruncated > 0) {
            PLOG_WARN("[PULSAR] CellContentSync: batch TRUNCATED - {} refs dropped (cap={})"sv,
                      batchTruncated,
                      static_cast<unsigned>(Pulsar::Network::MAX_CELL_CONTENT_REFS_PER_BATCH));
        }

        using namespace Pulsar::Network;
        const std::size_t batchBytes = batch.size() * sizeof(CellRefDescriptor);
        const std::size_t packetSize = 1 + sizeof(CellContentBatchHeader) + batchBytes;
        std::vector<std::uint8_t> buf(packetSize);
        buf[0] = PKT_CELL_CONTENT_BATCH;

        CellContentBatchHeader hdr{};
        hdr.cellFormID       = cellFormID;
        hdr.worldspaceFormID = worldspaceID;
        hdr.isInterior       = isInterior ? 1 : 0;
        hdr.isProc           = isProc ? 1 : 0;
        hdr.refCount         = static_cast<std::uint32_t>(batch.size());
        std::memcpy(&buf[1], &hdr, sizeof(hdr));
        std::memcpy(&buf[1 + sizeof(hdr)], batch.data(), batchBytes);

        // Reliable: receiver needs the full content list to spawn correctly in V2.
        Pulsar::Network::GetSingleton().Send(buf.data(), buf.size(), true);

        PLOG_INFO("[PULSAR] CellContentSync BATCH SENT cell=0x{:08X} refs={} bytes={}"sv,
                  cellFormID, batch.size(), packetSize);
        NASLOG("CellContentSync BATCH SENT cell=0x%08X refs=%zu bytes=%zu",
               cellFormID, batch.size(), packetSize);
    }

    // -----------------------------------------------------------------------
    // V2-Probe (client-side) - Find a local REFR matching (resolvedBase,
    // position) in the client's parentCell.references. Pure C function +
    // SEH so it stays C2712-safe (no C++ unwind through __try). Reuses the
    // same raw +0x80/+0x88 offsets verified in the host enum.
    //
    // Returns: 1 if a matching ref was found within radius, 0 otherwise.
    // -----------------------------------------------------------------------
    static int SafeFindMatchingRefInCell(void*         cell,
                                          std::uint32_t resolvedBaseFormID,
                                          float         px, float py, float pz,
                                          float         radius) noexcept
    {
        __try {
            if (!cell) return 0;
            const auto cb = reinterpret_cast<std::uint8_t*>(cell);
            const std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > kMaxSaneRefCount) return 0;

            const float r2 = radius * radius;
            for (std::uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;

                // ref+0x98 = base TESForm* (per F.1 LootSync verified offset)
                auto* base = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(ref) + 0x98);
                if (!base) continue;

                // base+0x28 = formID (verified TESForm offset)
                const std::uint32_t baseFormID = *reinterpret_cast<std::uint32_t*>(base + 0x28);
                if (baseFormID != resolvedBaseFormID) continue;

                // ref+0x8C = position float[3] (verified F.1 offset)
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<std::uint8_t*>(ref) + 0x8C);
                const float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= r2) return 1;
            }
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // V2-Probe: drain pending batches that target this cell, run the matcher
    // against client's parentCell.references, log matched vs. missing per type.
    static void ProbeBatchesAgainstClientCell(std::uint32_t clientCellFormID)
    {
        NASLOG("CCS-Probe ENTRY clientCell=0x%08X", clientCellFormID);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { NASLOG("CCS-Probe EXIT no-player"); return; }
        auto* cell = player->parentCell;
        if (!cell) { NASLOG("CCS-Probe EXIT no-cell"); return; }
        NASLOG("CCS-Probe player=%p cell=%p cellID=0x%08X",
               (void*)player, (void*)cell, clientCellFormID);

        // Snapshot the queue under lock.
        NASLOG("CCS-Probe before-lock");
        std::vector<PendingBatch> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_batchMutex);
            NASLOG("CCS-Probe got-lock pendingBatches=%zu",
                   g_pendingBatches.size());
            // Drop expired batches
            const std::uint64_t now = ::GetTickCount64();
            g_pendingBatches.erase(
                std::remove_if(g_pendingBatches.begin(), g_pendingBatches.end(),
                    [now](const PendingBatch& b) {
                        return (now - b.arrivalTick) > kBatchExpiryMs;
                    }),
                g_pendingBatches.end());
            snapshot = g_pendingBatches;
            g_pendingBatches.clear();  // consume - one-shot probe per batch
        }

        if (snapshot.empty()) {
            PLOG_INFO("[PULSAR] CellContentSync probe: no pending batches for cell=0x{:08X}"sv,
                      clientCellFormID);
            return;
        }

        // Run probe on each batch (typically just 1 - most-recent transit).
        NASLOG("CCS-Probe iterating %zu batches", snapshot.size());
        size_t batchIdx = 0;
        for (const auto& pb : snapshot) {
            NASLOG("CCS-Probe batch[%zu] hostCell=0x%08X descriptors=%zu",
                   batchIdx, pb.hostCellFormID, pb.descriptors.size());
            // L.7 V14.3 - defensive: only probe if host's cell matches
            // client's. Otherwise the descriptors reference cell content
            // we don't have, and SafeFindMatchingRefInCell may walk into
            // half-loaded refs from a different worldspace.
            if (pb.hostCellFormID != clientCellFormID) {
                NASLOG("CCS-Probe batch[%zu] SKIP cell-mismatch host=0x%08X client=0x%08X",
                       batchIdx, pb.hostCellFormID, clientCellFormID);
                ++batchIdx;
                continue;
            }
            std::size_t matched         = 0;
            std::size_t missing         = 0;
            std::size_t pluginUnloaded  = 0;
            std::uint32_t matchedByType[256] = { 0 };
            std::uint32_t missingByType[256] = { 0 };

            std::size_t descIdx = 0;
            for (const auto& d : pb.descriptors) {
                if ((descIdx & 0xF) == 0) {
                    NASLOG("CCS-Probe batch[%zu] desc[%zu] base=0x%08X type=0x%02X",
                           batchIdx, descIdx, d.baseFormID, d.baseFormType);
                }
                ++descIdx;
                // T34 plugin re-resolve: convert host's plugin+localID to
                // client-local FormID via TESDataHandler load order.
                std::uint32_t resolvedBase = 0;
                if (d.basePlugin[0] != '\0' && d.baseLocalID != 0) {
                    resolvedBase = Pulsar::NPCSync::PluginLookup::ResolveCrossPlugin(
                        d.basePlugin, d.baseLocalID);
                }
                if (resolvedBase == 0) {
                    ++pluginUnloaded;
                    ++missing;
                    ++missingByType[d.baseFormType];
                    continue;
                }

                if (SafeFindMatchingRefInCell(cell, resolvedBase,
                                                d.posX, d.posY, d.posZ,
                                                kProbeMatchRadius)) {
                    ++matched;
                    ++matchedByType[d.baseFormType];
                } else {
                    ++missing;
                    ++missingByType[d.baseFormType];
                }
            }

            PLOG_INFO(
                "[PULSAR] CellContentSync PROBE host-cell=0x{:08X} client-cell=0x{:08X} batch-refs={} matched={} missing={} pluginUnloaded={}"sv,
                pb.hostCellFormID, clientCellFormID,
                pb.descriptors.size(), matched, missing, pluginUnloaded);
            NASLOG("CellContentSync PROBE host-cell=0x%08X client-cell=0x%08X matched=%zu missing=%zu",
                   pb.hostCellFormID, clientCellFormID, matched, missing);

            // Per-FormType breakdown - only show types that had refs.
            for (int ft = 0; ft < 256; ++ft) {
                const std::uint32_t m = matchedByType[ft];
                const std::uint32_t mi = missingByType[ft];
                if (m == 0 && mi == 0) continue;
                PLOG_INFO(
                    "[PULSAR]     type=0x{:02X} {} matched={} missing={}"sv,
                    ft, FormTypeName(static_cast<RE::FormType>(ft)), m, mi);
            }
            ++batchIdx;
        }
        NASLOG("CCS-Probe EXIT clientCell=0x%08X", clientCellFormID);
    }

}  // anonymous namespace

void Init()
{
    PLOG_INFO("[PULSAR] CellContentSync K.6 V0 init (logging-only, deferred 1.5s)"sv);
    NASLOG("CellContentSync K.6 V0 init");
}

void OnHostCellChanged(std::uint32_t prevCellFormID,
                        std::uint32_t newCellFormID)
{
    // 2026-04-29: nothing to do if not connected. No peer means no batch
    // to send (host) and no batch to probe (client). Without this gate,
    // host running solo still spawned shadow clones via CellDiag and ran
    // the enum pipeline pointlessly.
    if (!Pulsar::Network::GetSingleton().IsConnected()) return;

    // V2-Probe path (client-only): queue probe against any pending batches
    // for the new cell. Run regardless of host/client gate.
    if (newCellFormID != 0 && !Pulsar::Network::GetSingleton().IsHost()) {
        const std::uint64_t now = ::GetTickCount64();
        const std::uint32_t lastProbe = g_lastProbeCell.load(std::memory_order_acquire);
        const std::uint64_t lastProbeT = g_lastProbeTick.load(std::memory_order_acquire);
        if (!(lastProbe == newCellFormID && (now - lastProbeT) < kReEnumSuppressMs)) {
            g_lastProbeCell.store(newCellFormID, std::memory_order_release);
            g_lastProbeTick.store(now, std::memory_order_release);
            g_pendingProbeNewCell.store(newCellFormID, std::memory_order_release);
            g_pendingProbeDueTick.store(now + kProbeSettleDelayMs, std::memory_order_release);
            PLOG_INFO("[PULSAR] CellContentSync queued PROBE cell=0x{:08X} in {}ms"sv,
                      newCellFormID, kProbeSettleDelayMs);
        }
    }

    // Host enum path (host-only).
    if (!Pulsar::Network::GetSingleton().IsHost()) return;
    if (newCellFormID == 0) return;

    // De-dupe same dest cell within window.
    const std::uint64_t now = ::GetTickCount64();
    const std::uint32_t lastCell = g_lastEnumCell.load(std::memory_order_acquire);
    const std::uint64_t lastTick = g_lastEnumTick.load(std::memory_order_acquire);
    if (lastCell == newCellFormID && (now - lastTick) < kReEnumSuppressMs) {
        return;
    }
    g_lastEnumCell.store(newCellFormID, std::memory_order_release);
    g_lastEnumTick.store(now, std::memory_order_release);

    // Defer enumeration - let proc-cell finish constructing.
    g_pendingPrevCell.store(prevCellFormID, std::memory_order_release);
    g_pendingNewCell.store(newCellFormID, std::memory_order_release);
    g_pendingDueTick.store(now + kEnumSettleDelayMs, std::memory_order_release);

    PLOG_INFO("[PULSAR] CellContentSync queued ENUM cell=0x{:08X} in {}ms"sv,
              newCellFormID, kEnumSettleDelayMs);
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    // [uint8 type][CellContentBatchHeader][CellRefDescriptor × N]
    if (length < 1 + sizeof(CellContentBatchHeader)) {
        PLOG_WARN("[PULSAR] CellContentSync RECV: too short ({})"sv, length);
        return;
    }

    CellContentBatchHeader hdr{};
    std::memcpy(&hdr, data + 1, sizeof(hdr));

    const std::size_t expected = 1 + sizeof(hdr)
                                + static_cast<std::size_t>(hdr.refCount) * sizeof(CellRefDescriptor);
    if (length != expected) {
        PLOG_WARN("[PULSAR] CellContentSync RECV: size mismatch (got={} expected={} refCount={})"sv,
                  length, expected, hdr.refCount);
        return;
    }

    PLOG_INFO(
        "[PULSAR] CellContentSync RECV cell=0x{:08X} ws=0x{:08X} interior={} proc={} refs={}"sv,
        hdr.cellFormID, hdr.worldspaceFormID, hdr.isInterior, hdr.isProc, hdr.refCount);
    NASLOG("CellContentSync RECV cell=0x%08X refs=%u",
           hdr.cellFormID, hdr.refCount);

    // Log a sample of the descriptors.
    const auto* descs = reinterpret_cast<const CellRefDescriptor*>(
        data + 1 + sizeof(hdr));
    const std::uint32_t logCap = (hdr.refCount < 8) ? hdr.refCount : 8;
    for (std::uint32_t i = 0; i < logCap; ++i) {
        const auto& d = descs[i];
        PLOG_INFO(
            "[PULSAR]   recv[{}] ref=0x{:08X} base=0x{:08X} type=0x{:02X} plugin='{}' pos=({:.0f},{:.0f},{:.0f})"sv,
            i, d.refFormID, d.baseFormID,
            static_cast<unsigned>(d.baseFormType),
            d.basePlugin[0] ? d.basePlugin : "?",
            d.posX, d.posY, d.posZ);
    }
    if (hdr.refCount > logCap) {
        PLOG_INFO("[PULSAR]   ... ({} more)"sv, hdr.refCount - logCap);
    }

    // V2-Probe (client-only): queue the batch for matching against client's
    // local cell. Probe runs after client's own cell-change settles (via Tick).
    if (Pulsar::Network::GetSingleton().IsHost()) return;

    PendingBatch pb;
    pb.hostCellFormID = hdr.cellFormID;
    pb.isInterior     = hdr.isInterior;
    pb.isProc         = hdr.isProc;
    pb.arrivalTick    = ::GetTickCount64();
    pb.descriptors.assign(descs, descs + hdr.refCount);
    {
        std::lock_guard<std::mutex> lock(g_batchMutex);
        g_pendingBatches.push_back(std::move(pb));
        // Cap queue size - drop oldest if too many.
        while (g_pendingBatches.size() > 8) {
            g_pendingBatches.erase(g_pendingBatches.begin());
        }
    }
}

void Tick()
{
    // 2026-04-29: skip all work when not connected. Drains pending state
    // so a reconnect doesn't fire a stale probe/enum from before disconnect.
    if (!Pulsar::Network::GetSingleton().IsConnected()) {
        if (g_pendingNewCell.load(std::memory_order_acquire) != 0) {
            g_pendingNewCell.store(0, std::memory_order_release);
            g_pendingPrevCell.store(0, std::memory_order_release);
            g_pendingDueTick.store(0, std::memory_order_release);
        }
        if (g_pendingProbeNewCell.load(std::memory_order_acquire) != 0) {
            g_pendingProbeNewCell.store(0, std::memory_order_release);
            g_pendingProbeDueTick.store(0, std::memory_order_release);
        }
        return;
    }

    const std::uint64_t now = ::GetTickCount64();

    // V2-Probe drain: client-side, run probe once cell-change settle elapsed.
    {
        const std::uint32_t pendingProbe = g_pendingProbeNewCell.load(std::memory_order_acquire);
        if (pendingProbe != 0) {
            const std::uint64_t due = g_pendingProbeDueTick.load(std::memory_order_acquire);
            if (now >= due) {
                g_pendingProbeNewCell.store(0, std::memory_order_release);
                g_pendingProbeDueTick.store(0, std::memory_order_release);
                ProbeBatchesAgainstClientCell(pendingProbe);
            }
        }
    }

    const std::uint32_t pendingCell = g_pendingNewCell.load(std::memory_order_acquire);
    if (pendingCell == 0) return;

    const std::uint64_t due = g_pendingDueTick.load(std::memory_order_acquire);
    if (now < due) return;

    const std::uint32_t prevCell = g_pendingPrevCell.load(std::memory_order_acquire);

    // Clear pending BEFORE running so a re-trigger during enum doesn't get lost.
    g_pendingNewCell.store(0, std::memory_order_release);
    g_pendingPrevCell.store(0, std::memory_order_release);
    g_pendingDueTick.store(0, std::memory_order_release);

    EnumerateCellNow(prevCell, pendingCell);
}

}  // namespace Pulsar::CellContentSync
