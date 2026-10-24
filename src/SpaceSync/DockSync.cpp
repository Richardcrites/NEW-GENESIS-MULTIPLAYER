// L.6 V3 Phase 1 - Ship-Dock-Sync diagnostic probe.
//
// Read-only sink on Spaceship::DockEvent that reads the event payload bytes
// SpaceSync's generic sink ignores. Logs:
// ship A / ship B REFR FormIDs (offsets 0x00 / 0x08 in the event)
// started / finished flags     (offsets 0x10 / 0x14)
// the docking-component pointer at [ship+0x88]
// the state-machine word at *[ship+0x88]+0x80 (if component allocated)
// the BGSDockingActivity vtable RVA at *[ship+0x88]+0x00 (if component allocated)
//
// All RVAs and byte layout from earlier RE work + earlier RE work RE pass (2026-05-02):
// Spaceship::DockEvent GetEventSource @ RVA 0x215FE90 (verified against SFSE 0.2.19)
// Five Notify-from-engine sites in 0x1421371E0 / 0x1421386C0 cluster
// Site 0x14213E0D6 fires (started=1, finished=1) = fully docked
// BGSDockingActivity ctor 0x1408F0250, vtable 0x144AF57C0 (REL::ID 392606),
//     init [rbx+0x80] = 0x100
// SpaceshipDockingFinishedHandler vtable 0x144CC52C0 (REL::ID 453891)
// Dispatcher 0x14213A130 (REL::ID 120238) - covers Docking, Undocking,
//     Landing, LandingFailsafe via single shared dispatcher

#include "PCH.h"
#include "SpaceSync/DockSync.h"

#include "Cell3DAttach.h"
#include "NASLog.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <RE/B/BSScriptUtil.h>
#include <RE/E/Events.h>
#include <RE/G/GameVM.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace std::string_view_literals;

namespace Pulsar::DockSync {
namespace {

    // RVA matches PULSAR existing kRVA_GetDockSrc in SpaceSync.cpp; use the
    // SFSE-confirmed value not the (broken) CommonLibSF placeholder.
    constexpr std::uintptr_t kRVA_GetDockSrc = 0x215FE90;

    // Phase 3b RVAs (earlier RE work RE 2026-05-02):
    // AddOrGetExtraTeleport: ensures REFR has an ExtraTeleport with a
    //   non-null inner data block (allocates 0x28 bytes if absent). Idempotent.
    //   Signature: void* __fastcall(TESObjectREFR*).
    // GetByType (extra-list): REL::ID 37878, used to read existing
    //   ExtraTeleport for capture on host.
    constexpr std::uint32_t  kExtraType_ShipArrivalData = 0xD9;  // marks ship-hatch doors

    // Phase 3i v11 (2026-05-02): trigger engine's native dock state-machine
    // on receiver. Per host-vs-client trace diff (LogHatchTrace 1Hz): the XT
    // bytes are 1:1 identical between host and client, including destDoorRef=0.
    // Engine teleport works on host because BGSDockingActivity / dock-pair
    // tracking is set up by SpaceshipDockingFinishedHandler. Client never
    // runs that handler natively. We call it manually with the local ship
    // REFR; the handler's inner walks ship.extraDataList for kAlias /
    // kSpaceshipArrivalData and sets up BSGalaxy ComponentDB.
    //
    // RE'd by Agent (2026-05-02): actual handler is at RVA 0x1AEC870 (REL::ID
    // 104080), NOT 0x1AEC890 (which is the UNdocking variant). Process is a
    // 21-byte shim that calls inner FUN_14213D160 (RVA 0x213A130, REL::ID
    // 120239). Signature: `bool __fastcall Process(void* this, TESObjectREFR* ship)`
    // `this` is IGNORED, only ship REFR matters. Single call sufficient
    // (no Started handler needed for ComponentDB).
    constexpr std::uintptr_t kRVA_SpaceshipDockingFinishedHandler = 0x1AEC870;
    using SpaceshipDockingFinishedFn = bool (__fastcall*)(void*, RE::TESObjectREFR*);

    static bool TriggerDockFinishedOnClient(RE::TESObjectREFR* a_ship)
    {
        if (!a_ship) return false;
        auto fn = reinterpret_cast<SpaceshipDockingFinishedFn>(
            REL::Offset{ kRVA_SpaceshipDockingFinishedHandler }.address());
        if (!fn) return false;
        bool ok = false;
        __try {
            // 'this' is ignored; pass nullptr per Agent decompile.
            ok = fn(nullptr, a_ship);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync TriggerDockFinishedOnClient: SEH"sv);
            return false;
        }
        return ok;
    }

    constexpr std::uintptr_t kRVA_AddOrGetExtraTeleport = 0xBA0020;
    constexpr std::uintptr_t kRVA_RejectFilter           = 0xB58720;  // FUN_140B58E40 - REJECT filter (1=skip, 0=eligible)
    constexpr std::uintptr_t kRVA_EnableForDock          = 0xB36C40;  // FUN_140B37390 - engine's "enable hatch for docking" + cell-walk
    constexpr std::uintptr_t kRVA_GetTakeOffSrc          = 0x2160390; // matches SpaceSync constant
    // Phase 3g (2026-05-02): direct engine MoveTo. earlier RE work RE'd this as
    // the simple 2-arg MoveTo entry that the activate dispatcher uses
    // internally via 0x140545C00. Bypasses Papyrus VM (which crashes with
    // CommonLibSF FATAL "Invalid ID: 0"). Handles unloaded dest cells via
    // engine cell-streamer. See reference_engine_moveto.md.
    constexpr std::uintptr_t kRVA_EngineMoveTo           = 0x191EFB0;
    constexpr std::uint32_t  kHatchBaseFormID            = 0x00009CF4;
    constexpr std::uint32_t  kExtraType_Teleport         = 0x29;
    constexpr std::uint8_t   kFormType_DOOR              = 0x25;

    // Phase 3e: track whether we have applied dock-state to local hatch.
    // Set on XT apply (receiver side), cleared on undock receive.
    static std::atomic<bool> g_dockAppliedLocally{ false };

    // Phase 3f: track local cockpit info so when player crosses into the
    // station-interior we can also apply the bypass to the STATION-SIDE
    // hatch (otherwise non-pilot can enter station but not return to ship).
    static std::atomic<std::uint32_t> g_localCockpitCellFormID{ 0 };
    static std::atomic<std::uint32_t> g_localCockpitHatchFormID{ 0 };
    static std::atomic<std::uint32_t> g_lastSeenPlayerCell{ 0 };
    static std::atomic<bool>          g_stationSideApplied{ false };
    // Phase 3f v2: defer apply until cell-streaming settled (loadMenu closed +
    // Cell3DAttach quiet). Phase 3f v1 crashed because EnableForDock(1) ran
    // during the cell-load WRITE@0x90 in a partial loadedData → crash.
    static std::atomic<std::uint64_t> g_stationSideApplyDueMs{ 0 };
    constexpr std::uint64_t kStationSideApplyDeferMs = 2500;  // wait 2.5s after cell change

    // Phase 3f v4: tracked station-side hatch FormID for activate intercept.
    // Engine teleport with destDoorRef pointing to a now-unloaded cockpit
    // hatch crashes (null+0x4C deref). We intercept the activate at LootSync
    // HookedActivate and redirect via Papyrus MoveTo instead.
    static std::atomic<std::uint32_t> g_appliedStationSideHatchFormID{ 0 };
    // Phase 3i v14.1: race-fix re-apply may target a different station hatch
    // (host-broadcast vs. client-local nearest-kDOOR can differ on multi-port
    // stations). Track ALL applied hatches so intercept matches client's
    // physical click regardless of which we re-applied last.
    static std::mutex                            g_appliedStationHatchSetMutex;
    static std::unordered_set<std::uint32_t>     g_appliedStationHatchSet;

    // Phase 3i v17: applied-cockpit-hatch tracking - mirror of station set,
    // for the cockpit side. Used by TryHandleCockpitSideHatchActivate to
    // identify "is this E-press on a cockpit hatch we set up for forward
    // dock?" so we can selectively block when waiting for forward save-bridge.
    static std::mutex                            g_appliedCockpitHatchSetMutex;
    static std::unordered_set<std::uint32_t>     g_appliedCockpitHatchSet;

    // Phase 3i v17: forward-bridge wait state on CLIENT. Set when host
    // broadcasts PKT_DOCK_FORWARD_BRIDGE_EXPECTED (host just exited proc-cell
    // ship into station, save-bridge incoming). Cleared on save-load OR after
    // timeout.
    static std::atomic<bool>          g_clientWaitingForForwardBridge{ false };
    static std::atomic<std::uint64_t> g_forwardBridgeExpectedAtMs{ 0 };
    constexpr std::uint64_t kForwardBridgeTimeoutMs = 30000;

    // Phase 3i v17: separate dedup window for forward-bridge fire on host
    // (independent of return-bridge dedup so forward + return in same dock
    // cycle both fire).
    static std::atomic<std::uint64_t> g_lastForwardBridgeFireMs{ 0 };
    constexpr std::uint64_t kForwardBridgeDedupMs = 30000;

    // Phase 3i v17.2/v17.3: deferred bridge trigger. Cell-change events fire
    // DURING engine load (cell ID swaps before loadMenu CLOSED). Triggering
    // save immediately captures partially-loaded state - host's player
    // position is still old (cockpit) instead of new (station). Even after
    // loadMenu CLOSED, the engine may take ~2s to commit player.location.
    // v17.3: defer fire until (a) cell settled, (b) player has moved a real
    // distance from arming-time position (= engine committed new coords),
    // (c) min 1500ms + max 8000ms safety bounds.
    enum class PendingBridge : std::uint8_t { kNone = 0, kForward = 1, kReturn = 2 };
    static std::atomic<std::uint8_t>  g_pendingBridge{ static_cast<std::uint8_t>(PendingBridge::kNone) };
    static std::atomic<std::uint32_t> g_pendingBridgePrevCell{ 0 };
    static std::atomic<std::uint32_t> g_pendingBridgeCurCell{ 0 };
    static std::atomic<std::uint64_t> g_pendingBridgeArmedMs{ 0 };
    static std::atomic<float>         g_pendingBridgeArmedX{ 0.0f };
    static std::atomic<float>         g_pendingBridgeArmedY{ 0.0f };
    static std::atomic<float>         g_pendingBridgeArmedZ{ 0.0f };
    constexpr std::uint64_t kBridgeSettleMinMs    = 1500;
    constexpr std::uint64_t kBridgeSettleMaxMs    = 8000;
    constexpr float         kBridgePosCommitDist2 = 25.0f;  // 5 units squared

    // Phase 3i v14.2: persistent ship FormID from PKT_DOCK_FINISHED. Player.GetSpaceship()
    // in station cell returns dock-port REFR (CLibSF binding mis-resolved) - useless
    // for MoveTo target. Use host's broadcast which is the actual ship REFR FormID.
    static std::atomic<std::uint32_t> g_lastDockedShipFormID{ 0 };

    // Phase 3i v15: HOST-side return-from-station detection for procedural-cell
    // ships. When host walks back from station cell to a procedural FF cockpit
    // cell, trigger BeginHostQuicksaveTransfer so client load-syncs into the
    // cockpit. Vanilla cells skip - engine native return works for those.
    // Dedup matches L.2 V1 Plan B (30s).
    static std::atomic<std::uint32_t> g_hostLastSeenCell{ 0 };
    static std::atomic<bool>          g_hostJustVisitedDockStation{ false };
    static std::atomic<std::uint64_t> g_lastDockReturnResyncMs{ 0 };
    constexpr std::uint64_t kDockReturnResyncDedupMs = 30000;

    // Phase 3i (2026-05-02): pilot-side flag - set when local DockEvent
    // (1,0) fires (we are the pilot for current dock cycle). Triggers the
    // station-XT capture on cockpit→station crossing.
    static std::atomic<bool> g_localPilotForCurrentDock{ false };

    // Phase 3i: receiver-side store for pilot-broadcast station XT bytes.
    // Used by ApplyStationSideHatchBypass instead of hardcoded values.
    static std::atomic<bool>          g_haveReceivedStationXT{ false };
    static std::mutex                 g_receivedStationXTMutex;
    static Pulsar::Network::DockStationXTData g_receivedStationXT{};

    // Phase 3i: pilot-side single-shot guard for station-XT broadcast.
    static std::atomic<bool> g_stationXTBroadcastSent{ false };

    // Phase 3i v6: client-side hint - host broadcasts the base FormID of the
    // chosen dock hatch. Client uses this to find the matching kDOOR locally
    // (different runtime instance, same base since vanilla ship form).
    static std::atomic<std::uint32_t> g_remoteDockHatchBase{ 0 };

    // Phase 3i v7: saved inner-XT pointers for landing hatches we suppressed
    // on apply. Restored on undock so engine state is preserved. Map key =
    // hatch runtime FormID, value = inner-data pointer that was zeroed.
    static std::mutex g_suppressedLandingMutex;
    static std::unordered_map<std::uint32_t, std::uint64_t> g_suppressedLandingInners;

    // Phase 3i v6: pre-dock snapshot of populated-XT kDOORs in cockpit cell.
    // Captured at DockEvent (1,0). Engine populates LANDING-hatch XT at
    // cell-load (always present). DOCK-hatch XT is populated DURING dock
    // state-machine (transition: not-populated → populated). The discriminator
    // is "kDOOR with populated XT NOT in pre-dock snapshot" = the actual
    // dock hatch. Set is keyed by base FormID (stable across peers, identifies
    // hatch role within ship).
    static std::mutex g_preDockSetMutex;
    static std::unordered_set<std::uint32_t> g_preDockPopulatedBases;
    static std::atomic<bool> g_preDockSnapshotTaken{ false };

    // Phase 3f v5: defer Papyrus MoveTo to game-thread Tick. Calling
    // BSScript::PackVariable + vm->DispatchStaticCall from inside the
    // engine's activate handler context triggers REL::ID(0) FATAL - likely
    // the VM is mid-state. Defer to Tick (clean game-thread context) where
    // TransitSync's identical dispatch path works.
    static std::atomic<bool>          g_pendingReturnArmed{ false };
    static std::atomic<std::uint32_t> g_pendingReturnTargetFormID{ 0 };
    static std::atomic<std::uint64_t> g_pendingReturnDueMs{ 0 };
    // Phase 3i v12.2: when set, drain uses GetRemoteActor() instead of
    // LookupByID (clones aren't in global form-table).
    static std::atomic<bool>          g_pendingReturnUseClone{ false };
    constexpr std::uint64_t kReturnDispatchDeferMs = 50;
    // Phase 3i v13: retry-loop for clone-settle race (procedural cells).
    // Clone needs to MOVE from station cell back to cockpit cell before
    // MoveTo will work; NPCSync propagation can lag a few hundred ms.
    static std::atomic<std::uint32_t> g_pendingReturnRetries{ 0 };
    constexpr std::uint32_t           kReturnMaxRetries = 60;     // ~6s @ 100ms cadence
    constexpr std::uint64_t           kReturnRetryDelayMs = 100;

    // ExtraTeleport inner struct (REFR_TELEPORT, 0x28 bytes) - verified RE,
    // see tools/extra_teleport_re.txt and TransitSync.cpp:108.
    struct alignas(8) RefrTeleport {
        std::uint64_t destDoorRef;       // +0x00 NiPointer<TESObjectREFR>
        std::uint32_t flags;             // +0x08
        float         destPosX;          // +0x0C
        float         destPosY;          // +0x10
        float         destPosZ;          // +0x14
        float         destRotX;          // +0x18
        float         destRotY;          // +0x1C
        float         destRotZ;          // +0x20
        std::uint8_t  destFlag;          // +0x24
        std::uint8_t  pad[3];            // +0x25..+0x27
    };
    static_assert(sizeof(RefrTeleport) == 0x28, "RefrTeleport size");

    // Lazy-resolved raw RVA wrapper for AddOrGetExtraTeleport.
    using AddOrGetExtraTeleportFn = void* (__fastcall*)(RE::TESObjectREFR*);
    static AddOrGetExtraTeleportFn ResolveAddOrGetExtraTeleport()
    {
        static AddOrGetExtraTeleportFn fn = nullptr;
        if (!fn) {
            fn = reinterpret_cast<AddOrGetExtraTeleportFn>(
                REL::Offset{ kRVA_AddOrGetExtraTeleport }.address());
        }
        return fn;
    }

    // Reuse REL::ID 37878 GetByType-on-extralist (confirmed from
    // TransitSync.cpp:99-106). Type byte 0x29 = ExtraTeleport.
    using GetExtraByTypeFn = void* (__fastcall*)(void* extraList, std::uint32_t type);
    static GetExtraByTypeFn ResolveGetExtraByType()
    {
        static GetExtraByTypeFn fn = nullptr;
        if (!fn) {
            fn = reinterpret_cast<GetExtraByTypeFn>(
                REL::Relocation<std::uintptr_t>{ REL::ID(37878) }.address());
        }
        return fn;
    }

    // Helper: returning BSTEventSource<DockEvent>* via lazy-init wrapper.
    using GetDockSrcFn = void* (*)();

    // Helper: SEH-safe FormID read off a possibly-bad TESForm* sitting at
    // the given byte offset within an event payload.
    static std::uint32_t SafeFormIDFromPtrAtOffset(const void* a_event,
                                                    std::size_t a_offset)
    {
        std::uint32_t out = 0;
        __try {
            const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(a_event);
            const RE::TESForm* form = *reinterpret_cast<const RE::TESForm* const*>(
                base + a_offset);
            if (form) out = form->formID;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out = 0;
        }
        return out;
    }

    // Helper: SEH-safe dword read at offset.
    static std::uint32_t SafeReadDword(const void* a_base, std::size_t a_offset)
    {
        std::uint32_t out = 0;
        __try {
            const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(a_base);
            out = *reinterpret_cast<const std::uint32_t*>(p + a_offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out = 0;
        }
        return out;
    }

    static std::uint64_t SafeReadQword(const void* a_base, std::size_t a_offset)
    {
        std::uint64_t out = 0;
        __try {
            const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(a_base);
            out = *reinterpret_cast<const std::uint64_t*>(p + a_offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out = 0;
        }
        return out;
    }

    // Dump the docking-component (referenced from REFR+0x88) state. This is
    // the per-ship state holder allocated by BGSDockingActivity ctor - its
    // [+0x80] word starts at 0x100 and transitions through the dock sequence.
    static void LogShipComponentState(const char* a_label,
                                       const RE::TESObjectREFR* a_ship)
    {
        if (!a_ship) {
            PLOG_INFO("[PULSAR] L.6 V3 DockSync {}: ship=null"sv, a_label);
            return;
        }

        const std::uint64_t componentPtr = SafeReadQword(a_ship, 0x88);
        const std::uintptr_t imgBase =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

        if (componentPtr == 0) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync {}: ship=0x{:08X} component@+0x88=null"sv,
                a_label, a_ship->formID);
            NASLOG("L.6 V3 DockSync %s: ship=0x%08X comp@+0x88=null",
                   a_label, a_ship->formID);
            return;
        }

        // Dump component vtable RVA + state word at +0x80.
        const std::uint64_t vt = SafeReadQword(
            reinterpret_cast<const void*>(componentPtr), 0x00);
        const std::uint32_t stateWord = SafeReadDword(
            reinterpret_cast<const void*>(componentPtr), 0x80);
        // Also dump a couple more potentially-interesting offsets for
        // hypothesis-test coverage.
        const std::uint64_t plus40 = SafeReadQword(
            reinterpret_cast<const void*>(componentPtr), 0x40);
        const std::uint64_t plus70 = SafeReadQword(
            reinterpret_cast<const void*>(componentPtr), 0x70);

        const std::uint64_t vtRva = (vt >= imgBase) ? (vt - imgBase) : vt;

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync {}: ship=0x{:08X} comp=0x{:X} "
            "vt-rva=0x{:X} state[+0x80]=0x{:08X} +0x40=0x{:X} +0x70=0x{:X}"sv,
            a_label, a_ship->formID,
            componentPtr, vtRva, stateWord, plus40, plus70);
        NASLOG(
            "L.6 V3 DockSync %s: ship=0x%08X comp=0x%llX vt-rva=0x%llX "
            "state=0x%08X +0x40=0x%llX +0x70=0x%llX",
            a_label, a_ship->formID,
            (unsigned long long)componentPtr,
            (unsigned long long)vtRva,
            stateWord,
            (unsigned long long)plus40,
            (unsigned long long)plus70);
    }

    // Tracks last (started, finished) flags we observed so we only broadcast
    // on edge transitions (engine fires the same flag-pair multiple times in
    // some sequences; we only want one packet per state-change).
    static std::atomic<std::uint32_t> g_lastStartedFlag{ 0xFFFFFFFFu };
    static std::atomic<std::uint32_t> g_lastFinishedFlag{ 0xFFFFFFFFu };
    static std::atomic<std::uint64_t> g_lastBroadcastMs{ 0 };
    constexpr std::uint64_t kBroadcastDedupMs = 500;

    // Phase 3b host-side polling state. After we broadcast stage=1 (DockEvent
    // 1,0), arm a polling window that walks the player's parentCell looking
    // for the cockpit kDOOR with base 0x9CF4. When found AND its
    // ExtraTeleport's inner data is non-null (engine populated it post-
    // cutscene), capture the 28-byte struct and broadcast PKT_DOCK_EXTRATELEPORT.
    static std::atomic<std::uint64_t> g_hatchPollUntilMs{ 0 };
    static std::atomic<std::uint64_t> g_hatchPollNextMs{ 0 };
    static std::atomic<bool>          g_hatchTeleportSent{ false };
    constexpr std::uint64_t kHatchPollDurationMs = 60000;  // 60s post-broadcast
    constexpr std::uint64_t kHatchPollIntervalMs = 250;    // 4 Hz

    // Phase 3d diagnostic: log ship+dock pose candidates over the dock window
    // so we can identify which field actually drives the post-dock visual.
    // 1Hz log over the same 60s host poll window.
    static std::atomic<std::uint64_t> g_poseLogNextMs{ 0 };
    constexpr std::uint64_t kPoseLogIntervalMs = 1000;

    // Phase 3b client-side apply state. Set by OnPacketReceived for the
    // ExtraTeleport packet, drained by Tick.
    struct PendingExtraTeleport {
        std::atomic<bool>          armed{ false };
        std::atomic<std::uint64_t> dueTick{ 0 };
        std::mutex                 m;
        Pulsar::Network::DockExtraTeleportData payload{};
    };
    static PendingExtraTeleport g_pendingXT;
    constexpr std::uint64_t kXTApplyDeferMs = 200;

    // Phase 3b v7: client-side persistence - once we apply XT, periodically
    // re-set the bits + log gate state. If engine clears bits between ticks,
    // we re-set them. If gate flips from 1→0 we see when + why.
    static std::atomic<bool>          g_xtPersistArmed{ false };
    static std::atomic<std::uint64_t> g_xtPersistUntilMs{ 0 };
    static std::atomic<std::uint64_t> g_xtPersistNextLogMs{ 0 };
    constexpr std::uint64_t kXTPersistDurationMs = 120000;  // 2min after apply
    constexpr std::uint64_t kXTPersistLogIntervalMs = 1000;  // 1Hz
    constexpr std::uint64_t kXTPersistTickIntervalMs = 250;  // 4Hz re-apply

    // Phase 3g (2026-05-02): direct engine MoveTo. Replaces Papyrus VM
    // dispatch which crashed with CommonLibSF FATAL "Invalid ID: 0" (REL::ID
    // placeholder unresolved). Engine RVA 0x14191EFB0 = bool MoveTo(self, dest)
    // same fn the activate dispatcher uses internally. Handles unloaded
    // destination cells via engine cell-streamer. SAFE on game thread.
    using EngineMoveTo_t = bool (__fastcall*)(RE::TESObjectREFR*, RE::TESObjectREFR*);

    static bool DispatchEngineMoveTo(RE::TESObjectREFR* a_self,
                                      RE::TESObjectREFR* a_dest)
    {
        if (!a_self || !a_dest) return false;
        auto fn = reinterpret_cast<EngineMoveTo_t>(
            REL::Offset{ kRVA_EngineMoveTo }.address());
        if (!fn) return false;
        bool ok = false;
        __try { ok = fn(a_self, a_dest); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        return ok;
    }

    static bool DispatchPlayerMoveToRef(RE::TESObjectREFR* a_destRef)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        return DispatchEngineMoveTo(player, a_destRef);
    }

    // Phase 3i v9.2 (2026-05-02): Papyrus-VM MoveTo dispatch via the same
    // PulsarFurnSync.DoPlayerFollowMoveTo helper TransitSync uses. Unlike
    // the direct engine MoveTo (RVA 0x14191EFB0) which fails when the
    // destination cell isn't loaded ("dst's parent worldspace can't be
    // resolved"), Papyrus Game.GetPlayer().MoveTo(dest) goes through the
    // engine's MoveToFunctor → cell-load → teleport pipeline. SAFE on game
    // thread (Tick), NOT in activate-hook context (REL::ID(0) FATAL).
    static bool DispatchPlayerPapyrusMoveTo(RE::TESObjectREFR* a_destRef)
    {
        if (!a_destRef) return false;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        RE::BSScript::Variable destVar;
        RE::BSScript::PackVariable(destVar, a_destRef);

        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(1);
            args[0] = destVar;
            return true;
        };

        RE::BSFixedString objName("PulsarFurnSync");
        RE::BSFixedString funcName("DoPlayerFollowMoveTo");
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return vm->DispatchStaticCall(objName, funcName, argFn, callback, 0);
    }

    static bool DispatchShipMoveTo(RE::TESObjectREFR* a_ship,
                                    RE::TESObjectREFR* a_dock)
    {
        return DispatchEngineMoveTo(a_ship, a_dock);
    }

    // Phase 3a - receiver-side pending apply state. Set by OnPacketReceived
    // (network thread), drained by Tick (game thread). Stores the FormIDs
    // we need to look up + an apply-due timestamp so we can defer the actual
    // MoveTo by a short window for engine-state to settle.
    struct PendingDockApply {
        std::atomic<bool>          armed{ false };
        std::atomic<std::uint32_t> shipFormID{ 0 };
        std::atomic<std::uint32_t> dockRefrFormID{ 0 };
        std::atomic<std::uint8_t>  stage{ 0 };
        std::atomic<std::uint64_t> dueTick{ 0 };
    };
    static PendingDockApply g_pendingApply;
    constexpr std::uint64_t kApplyDeferMs = 500;
    static std::atomic<std::uint64_t> g_lastApplyTick{ 0 };
    constexpr std::uint64_t kApplyDedupMs = 5000;

    // Phase 2/3 broadcast - fires from the LOCAL pilot's DockEvent sink when
    // the engine commits to docking. Receiver applies. Bidirectional: whoever
    // is the active pilot fires DockEvent locally; the other side receives.
    // Engine fires DockEvent only on the side running the dock state machine,
    // so no echo-suppression needed.
    static void BroadcastDockFinished(std::uint32_t a_shipFormID,
                                       std::uint32_t a_dockRefrFormID,
                                       std::uint8_t a_stage,
                                       const RE::TESObjectREFR* a_ship)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        const std::uint64_t now = ::GetTickCount64();
        const std::uint64_t prev = g_lastBroadcastMs.load(std::memory_order_acquire);
        if (now - prev < kBroadcastDedupMs) {
            PLOG_INFO("[PULSAR] L.6 V3 DockSync broadcast SKIP - dedup window"sv);
            return;
        }
        g_lastBroadcastMs.store(now, std::memory_order_release);

        Pulsar::Network::DockFinishedData payload{};
        payload.shipFormID     = a_shipFormID;
        payload.dockRefrFormID = a_dockRefrFormID;
        payload.stage          = a_stage;

        __try {
            if (a_ship) {
                payload.posX = a_ship->data.location.x;
                payload.posY = a_ship->data.location.y;
                payload.posZ = a_ship->data.location.z;
                // CommonLibSF angle is Euler not quat; pack the Eulers into
                // the quat slots for now and pass through. Phase 3 will read
                // the havok body row3 if a real quat is needed.
                payload.quatX = a_ship->data.angle.x;
                payload.quatY = a_ship->data.angle.y;
                payload.quatZ = a_ship->data.angle.z;
                payload.quatW = 0.0f;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        std::uint8_t buf[256]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_DOCK_FINISHED, buf, sizeof(buf));
        if (!w.Write(payload)) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync broadcast: writer failed"sv);
            return;
        }

        net.Send(buf, w.Size(), /*reliable*/ true);

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⇒ BROADCAST stage={} ship=0x{:08X} "
            "dock=0x{:08X} pos=({:.0f},{:.0f},{:.0f})"sv,
            a_stage, a_shipFormID, a_dockRefrFormID,
            payload.posX, payload.posY, payload.posZ);
        NASLOG(
            "L.6 V3 DockSync BROADCAST stage=%d ship=0x%08X dock=0x%08X "
            "pos=(%.0f,%.0f,%.0f)",
            a_stage, a_shipFormID, a_dockRefrFormID,
            payload.posX, payload.posY, payload.posZ);
    }

    // Phase 3f: find the kDOOR REFR closest to a given position in a cell.
    // Phase 3i v4 helper: check if a kDOOR has kShipArrivalData (0xD9)
    // extra. Engine sets this extra on ship-hatch doors specifically (not
    // on regular interior doors). Works on BOTH host and client because
    // it's vanilla form data, not dock-state. Reliably disambiguates the
    // actual ship hatch from other interior kDOORs.
    static bool DoorHasShipArrivalData(RE::TESObjectREFR* a_door)
    {
        if (!a_door) return false;
        auto getFn = ResolveGetExtraByType();
        if (!getFn) return false;
        bool found = false;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_door);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return false;
            void* xt = getFn(extraList, kExtraType_ShipArrivalData);
            found = (xt != nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return found;
    }

    // Phase 3i v3 helper: check if a kDOOR's ExtraTeleport inner data is
    // engine-populated (non-zero destPos). Used to disambiguate the actual
    // ship hatch from other doors in the same cell.
    static bool DoorHasPopulatedXT(RE::TESObjectREFR* a_door)
    {
        if (!a_door) return false;
        auto getFn = ResolveGetExtraByType();
        if (!getFn) return false;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_door);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return false;
            void* xt = getFn(extraList, kExtraType_Teleport);
            if (!xt) return false;
            std::uint64_t innerPtr = *reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(xt) + 0x18);
            if (innerPtr < 0x10000ull || innerPtr >= 0x00007FFFFFFFFFFFull) return false;
            auto* inner = reinterpret_cast<RefrTeleport*>(innerPtr);
            const float dpx = inner->destPosX, dpy = inner->destPosY, dpz = inner->destPosZ;
            // Reject all-zero (uninitialized) and float-MAX (uninitialized garbage).
            const float kMax = 1.0e6f;
            if (std::fabs(dpx) > kMax || std::fabs(dpy) > kMax || std::fabs(dpz) > kMax) {
                return false;
            }
            const bool nonZero = (dpx != 0.0f) || (dpy != 0.0f) || (dpz != 0.0f);
            return nonZero;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Phase 3i v15 (2026-05-03) - disambiguating station-hatch picker.
    // Replaces raw nearest-door matching that mis-picked regular station doors
    // physically close to the dock hatch (user-reported: client clicked a
    // neighbor door, got dock-back XT applied to it, teleported into ship
    // instead of through the door normally).
    //
    // Priority (first non-null wins):
    //   (1) Broadcast hatchBaseFormID match (RECEIVER path) - pilot broadcasts
    //       the station-hatch BASE FormID via PKT_DOCK_STATION_XT; on stations
    //       where that base is unique this is a guaranteed match. If multiple
    //       refs share the base (modular stations w/ multiple ship-hatches),
    //       pick the one closest to the player.
    //   (2) DoorHasShipArrivalData - vanilla extra (kShipArrivalData=0xD9) the
    //       engine attaches to actual ship-hatch kDOORs. Works on PILOT path
    //       before broadcast is sent. Closest match preferred if multiple.
    //   (3) Nearest kDOOR within radius - last-resort fallback, logged as WARN
    //       so we can spot mis-picks in user logs.
    //
    // Radius widened from 25u to 100u (10000u²) since with disambiguation the
    // ship hatch may legitimately be a few meters away if the cell-cross
    // detection fired before the player walked all the way through.
    static RE::TESObjectREFR* FindStationHatchInCell(RE::TESObjectCELL* a_cell,
                                                      float a_px, float a_py, float a_pz,
                                                      bool a_haveBroadcast,
                                                      std::uint32_t a_broadcastBase,
                                                      float a_maxDist2)
    {
        if (!a_cell) return nullptr;
        std::uint32_t size = 0;
        void* data = nullptr;
        __try {
            const auto base = reinterpret_cast<const std::uint8_t*>(a_cell) + 0x80;
            size = *reinterpret_cast<const std::uint32_t*>(base + 0x00);
            data = *reinterpret_cast<void* const*>(base + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!data || size == 0 || size > 65536) return nullptr;

        RE::TESObjectREFR* baseMatch = nullptr;
        float baseMatchDist2 = a_maxDist2;
        RE::TESObjectREFR* shipArrMatch = nullptr;
        float shipArrMatchDist2 = a_maxDist2;
        RE::TESObjectREFR* nearestKDoor = nullptr;
        float nearestKDoorDist2 = a_maxDist2;

        for (std::uint32_t i = 0; i < size; ++i) {
            RE::TESObjectREFR* ref = nullptr;
            __try {
                auto* slot = reinterpret_cast<RE::TESObjectREFR**>(
                    reinterpret_cast<std::uint8_t*>(data) + (i * 8));
                ref = *slot;
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!ref) continue;
            const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ref);
            if (addr < 0x10000ULL || addr >= 0x7FFFFFFFFFFFULL) continue;

            std::uint8_t  baseFormType = 0;
            std::uint32_t baseFormID   = 0;
            float rx = 0, ry = 0, rz = 0;
            __try {
                auto* baseObj = ref->data.objectReference.get();
                if (!baseObj) continue;
                baseFormType = static_cast<std::uint8_t>(baseObj->formType.get());
                baseFormID   = baseObj->formID;
                rx = ref->data.location.x;
                ry = ref->data.location.y;
                rz = ref->data.location.z;
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (baseFormType != kFormType_DOOR) continue;

            const float dx = rx - a_px, dy = ry - a_py, dz = rz - a_pz;
            const float dist2 = dx*dx + dy*dy + dz*dz;
            if (dist2 >= a_maxDist2) continue;

            if (a_haveBroadcast && a_broadcastBase != 0 && baseFormID == a_broadcastBase) {
                if (dist2 < baseMatchDist2) {
                    baseMatchDist2 = dist2;
                    baseMatch = ref;
                }
            }
            if (DoorHasShipArrivalData(ref)) {
                if (dist2 < shipArrMatchDist2) {
                    shipArrMatchDist2 = dist2;
                    shipArrMatch = ref;
                }
            }
            if (dist2 < nearestKDoorDist2) {
                nearestKDoorDist2 = dist2;
                nearestKDoor = ref;
            }
        }

        if (baseMatch) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync station-hatch pick: BASE-MATCH "
                "ref=0x{:08X} d2={:.1f} (broadcastBase=0x{:08X})"sv,
                baseMatch->formID, baseMatchDist2, a_broadcastBase);
            return baseMatch;
        }
        if (shipArrMatch) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync station-hatch pick: SHIP-ARRIVAL-DATA "
                "ref=0x{:08X} d2={:.1f}"sv,
                shipArrMatch->formID, shipArrMatchDist2);
            return shipArrMatch;
        }
        if (nearestKDoor) {
            PLOG_WARN(
                "[PULSAR] L.6 V3 DockSync station-hatch pick: NEAREST-FALLBACK "
                "ref=0x{:08X} d2={:.1f} - no broadcast match and no "
                "kShipArrivalData; may pick wrong door in dense layouts"sv,
                nearestKDoor->formID, nearestKDoorDist2);
            return nearestKDoor;
        }
        return nullptr;
    }

    // Phase 3b/3i: walk player.parentCell looking for the cockpit hatch.
    // Phase 3i v3 changed: cockpit cells may have multiple kDOORs (Razorleaf
    // has interior doors + the ship hatch). Engine populates ExtraTeleport
    // ONLY on the actual ship hatch via SetUpDockExtraTeleport. So we filter
    // by "kDOOR with populated XT" - guaranteed to be the hatch on host.
    // For receiver side (where engine doesn't populate), we ALSO accept any
    // kDOOR (the receiver-side finder is gated differently anyway).
    // Phase 3i v14.1 helpers - touch unordered_set without __try in caller.
    static void AddAppliedCockpitHatch(std::uint32_t a_fmt)
    {
        std::lock_guard lk(g_appliedCockpitHatchSetMutex);
        g_appliedCockpitHatchSet.insert(a_fmt);
    }
    static bool IsAppliedCockpitHatch(std::uint32_t a_fmt)
    {
        std::lock_guard lk(g_appliedCockpitHatchSetMutex);
        return g_appliedCockpitHatchSet.count(a_fmt) > 0;
    }
    static void ClearAppliedCockpitHatchSet()
    {
        std::lock_guard lk(g_appliedCockpitHatchSetMutex);
        g_appliedCockpitHatchSet.clear();
    }
    static void AddAppliedStationHatch(std::uint32_t a_fmt)
    {
        std::lock_guard lk(g_appliedStationHatchSetMutex);
        g_appliedStationHatchSet.insert(a_fmt);
    }
    static bool IsAppliedStationHatch(std::uint32_t a_fmt)
    {
        std::lock_guard lk(g_appliedStationHatchSetMutex);
        return g_appliedStationHatchSet.count(a_fmt) > 0;
    }
    static void ClearAppliedStationHatchSet()
    {
        std::lock_guard lk(g_appliedStationHatchSetMutex);
        g_appliedStationHatchSet.clear();
    }

    // Phase 3i v6 helper: SEH-only POD reads (no object unwinding).
    struct CellRefSlot { void* data; std::uint32_t size; };
    static CellRefSlot ReadCellRefArray(RE::TESObjectCELL* a_cell)
    {
        CellRefSlot out{ nullptr, 0 };
        if (!a_cell) return out;
        __try {
            const auto base = reinterpret_cast<const std::uint8_t*>(a_cell) + 0x80;
            out.size = *reinterpret_cast<const std::uint32_t*>(base + 0x00);
            out.data = *reinterpret_cast<void* const*>(base + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out.data = nullptr; out.size = 0;
        }
        return out;
    }

    static RE::TESObjectREFR* ReadRefSlot(void* a_data, std::uint32_t a_idx)
    {
        RE::TESObjectREFR* ref = nullptr;
        __try {
            auto* slot = reinterpret_cast<RE::TESObjectREFR**>(
                reinterpret_cast<std::uint8_t*>(a_data) + (a_idx * 8));
            ref = *slot;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        return ref;
    }

    static bool ReadRefBase(RE::TESObjectREFR* a_ref, std::uint32_t& outBase, std::uint8_t& outFt)
    {
        outBase = 0; outFt = 0;
        if (!a_ref) return false;
        __try {
            auto* b = a_ref->data.objectReference.get();
            if (!b) return false;
            outFt = static_cast<std::uint8_t>(b->formType.get());
            outBase = b->formID;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }

    // Phase 3i v6: snapshot pre-dock populated kDOOR base-FormIDs. Called at
    // DockEvent (1,0). Pre-existing populated XT = LANDING hatches (engine
    // populates these at cell-load). The dock hatch's XT gets populated
    // LATER during dock state-machine - that's the one we want.
    static void SnapshotPreDockPopulatedHatches(RE::TESObjectCELL* a_cell)
    {
        const auto slot = ReadCellRefArray(a_cell);
        if (!slot.data || slot.size == 0 || slot.size > 65536) return;

        std::unordered_set<std::uint32_t> tmp;
        for (std::uint32_t i = 0; i < slot.size; ++i) {
            auto* ref = ReadRefSlot(slot.data, i);
            if (!ref) continue;
            const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ref);
            if (addr < 0x10000ULL || addr >= 0x7FFFFFFFFFFFULL) continue;
            std::uint32_t baseFmt = 0; std::uint8_t bft = 0;
            if (!ReadRefBase(ref, baseFmt, bft)) continue;
            if (bft != kFormType_DOOR) continue;
            if (DoorHasPopulatedXT(ref)) tmp.insert(baseFmt);
        }
        {
            std::lock_guard lk(g_preDockSetMutex);
            g_preDockPopulatedBases = std::move(tmp);
        }
        g_preDockSnapshotTaken.store(true, std::memory_order_release);

        std::lock_guard lk(g_preDockSetMutex);
        std::string list;
        for (auto b : g_preDockPopulatedBases) {
            char buf[24]; std::snprintf(buf, sizeof(buf), "0x%08X ", b);
            list += buf;
        }
        PLOG_INFO("[PULSAR] L.6 V3 DockSync pre-dock snapshot: {} populated kDOOR bases [{}]"sv,
                  g_preDockPopulatedBases.size(), list);
    }

    static bool IsPreDockPopulated(std::uint32_t a_baseFmt)
    {
        if (!g_preDockSnapshotTaken.load(std::memory_order_acquire)) return false;
        std::lock_guard lk(g_preDockSetMutex);
        return g_preDockPopulatedBases.count(a_baseFmt) > 0;
    }

    // Phase 3i v7 helper (POD-only SEH wrap): swap inner-XT pointer to a new
    // value, returning the previous. Returns 0 on no-op or error.
    static std::uint64_t SwapInnerXTPtr(RE::TESObjectREFR* a_door, std::uint64_t a_newValue)
    {
        if (!a_door) return 0;
        auto getFn = ResolveGetExtraByType();
        if (!getFn) return 0;
        std::uint64_t prev = 0;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_door);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return 0;
            void* xt = getFn(extraList, kExtraType_Teleport);
            if (!xt) return 0;
            auto* innerSlot = reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(xt) + 0x18);
            prev = *innerSlot;
            *innerSlot = a_newValue;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return prev;
    }

    static std::uint32_t SafeFormID(RE::TESObjectREFR* a_door)
    {
        if (!a_door) return 0;
        std::uint32_t fid = 0;
        __try { fid = a_door->formID; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return fid;
    }

    // Phase 3i v7: zero a hatch's inner-XT pointer so its activate-gate fails.
    // Saves the pre-zero value into a map keyed by hatch runtime FormID for
    // later restore. Returns true if a non-zero inner was zeroed.
    static bool SuppressLandingHatchInner(RE::TESObjectREFR* a_door)
    {
        const std::uint32_t fid = SafeFormID(a_door);
        if (fid == 0) return false;
        const std::uint64_t prev = SwapInnerXTPtr(a_door, 0);
        if (prev == 0) return false;
        std::lock_guard lk(g_suppressedLandingMutex);
        g_suppressedLandingInners[fid] = prev;
        return true;
    }

    // Phase 3i v7: restore previously-suppressed landing hatches by writing
    // the saved inner-XT pointer back. Called on undock cleanup.
    static void RestoreSuppressedLandingHatches()
    {
        std::unordered_map<std::uint32_t, std::uint64_t> tmp;
        {
            std::lock_guard lk(g_suppressedLandingMutex);
            tmp.swap(g_suppressedLandingInners);
        }
        if (tmp.empty()) return;
        for (auto& [fmt, savedInner] : tmp) {
            auto* form = RE::TESForm::LookupByID(fmt);
            if (!form) continue;
            auto* refr = form->As<RE::TESObjectREFR>();
            if (!refr) continue;
            (void)SwapInnerXTPtr(refr, savedInner);
        }
        PLOG_INFO("[PULSAR] L.6 V3 DockSync: restored {} suppressed landing hatches"sv, tmp.size());
    }

    // Phase 3i v7: walk cockpit cell, suppress all populated-XT kDOORs whose
    // base FormID does NOT match the dock-hatch base. Called on client after
    // applying dock XT - prevents accidental activate of landing hatch.
    static void SuppressNonDockHatchesInCell(
        RE::TESObjectCELL* a_cell, std::uint32_t a_dockBase)
    {
        if (!a_cell) return;
        // Phase 3i v12.2 (2026-05-02): ONLY suppress in procedural FF-prefix
        // cells (e.g. Razorleaf, Guardian VI) which unload after client leaves.
        // For VANILLA persistent cells (e.g. Frontier 0x05100136), our zeroed
        // inner-XT-ptr survives forever and crashes engine cell-init later
        // (NULL+8 deref during cell-load on return). Vanilla cells: skip.
        const std::uint32_t cellFmt = a_cell->formID;
        if ((cellFmt & 0xFF000000u) != 0xFF000000u) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync: skip suppression in vanilla cell 0x{:08X}"sv,
                cellFmt);
            return;
        }

        const auto slot = ReadCellRefArray(a_cell);
        if (!slot.data || slot.size == 0 || slot.size > 65536) return;

        int count = 0;
        for (std::uint32_t i = 0; i < slot.size; ++i) {
            auto* ref = ReadRefSlot(slot.data, i);
            if (!ref) continue;
            const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ref);
            if (addr < 0x10000ULL || addr >= 0x7FFFFFFFFFFFULL) continue;
            std::uint32_t baseFmt = 0; std::uint8_t bft = 0;
            if (!ReadRefBase(ref, baseFmt, bft)) continue;
            if (bft != kFormType_DOOR) continue;
            if (baseFmt == a_dockBase) continue;  // don't touch dock hatch
            if (!DoorHasPopulatedXT(ref)) continue;  // only landing-type hatches
            if (SuppressLandingHatchInner(ref)) ++count;
        }
        if (count > 0) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync: suppressed {} non-dock kDOOR(s) in cell 0x{:08X}"sv,
                count, cellFmt);
        }
    }

    static void ClearPreDockSnapshot()
    {
        std::lock_guard lk(g_preDockSetMutex);
        g_preDockPopulatedBases.clear();
        g_preDockSnapshotTaken.store(false, std::memory_order_release);
    }

    // Phase 3i v5 diag: dump every kDOOR in cell with discriminator data,
    // throttled to once per cell-FormID. User reports which is dock vs
    // landing; we identify the bit pattern that distinguishes them.
    static std::atomic<std::uint32_t> g_lastDumpedCellID{ 0 };
    // Phase 3i v10 tracker: log full state of a hatch's XT inner + bits +
    // destDoorRef-resolution at named checkpoint. Both peers run this; diff
    // between host (working) and client (broken) reveals the gap.
    // POD-only SEH wrapper to avoid object-unwinding error in __try.
    struct HatchTrace {
        std::uint32_t hatchFmt; std::uint32_t hatchBase; std::uint32_t parentCellFmt;
        std::uint64_t outerPtr; std::uint64_t innerPtr;
        std::uint64_t destDoorRefVal;
        std::uint32_t destFmt; std::uint32_t destBase; std::uint32_t destParentCellFmt;
        std::uint32_t flags;
        float destX, destY, destZ;
        float rotX, rotY, rotZ;
        std::uint8_t destFlag;
        std::uint64_t bits18; std::uint32_t bits20; std::uint16_t bits2C;
        bool destResolvable;
    };

    static void ReadHatchTrace(RE::TESObjectREFR* a_hatch, HatchTrace& t)
    {
        t = HatchTrace{};
        if (!a_hatch) return;
        auto getFn = ResolveGetExtraByType();
        __try {
            t.hatchFmt = a_hatch->formID;
            auto* b = a_hatch->data.objectReference.get();
            if (b) t.hatchBase = b->formID;
            if (a_hatch->parentCell) t.parentCellFmt = a_hatch->parentCell->formID;

            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            t.bits18 = *reinterpret_cast<std::uint64_t*>(pb + 0x18);
            t.bits20 = *reinterpret_cast<std::uint32_t*>(pb + 0x20);
            t.bits2C = *reinterpret_cast<std::uint16_t*>(pb + 0x2C);

            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList || !getFn) return;
            void* xt = getFn(extraList, kExtraType_Teleport);
            t.outerPtr = reinterpret_cast<std::uint64_t>(xt);
            if (!xt) return;
            std::uint64_t inner = *reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(xt) + 0x18);
            t.innerPtr = inner;
            if (inner < 0x10000ULL || inner >= 0x7FFFFFFFFFFFULL) return;
            auto* in = reinterpret_cast<RefrTeleport*>(inner);
            t.destDoorRefVal = in->destDoorRef;
            t.flags = in->flags;
            t.destX = in->destPosX; t.destY = in->destPosY; t.destZ = in->destPosZ;
            t.rotX = in->destRotX; t.rotY = in->destRotY; t.rotZ = in->destRotZ;
            t.destFlag = in->destFlag;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        // Try to resolve destDoorRef as a TESObjectREFR pointer (works only
        // if engine populated with raw ptr; on us-applied with NiPointer-write
        // it should also resolve here as raw read).
        if (t.destDoorRefVal >= 0x10000ULL && t.destDoorRefVal < 0x7FFFFFFFFFFFULL) {
            __try {
                auto* dr = reinterpret_cast<RE::TESObjectREFR*>(t.destDoorRefVal);
                t.destFmt = dr->formID;
                auto* db = dr->data.objectReference.get();
                if (db) t.destBase = db->formID;
                if (dr->parentCell) t.destParentCellFmt = dr->parentCell->formID;
                t.destResolvable = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                t.destResolvable = false;
            }
        }
    }

    static void LogHatchTrace(const char* a_label, RE::TESObjectREFR* a_hatch)
    {
        HatchTrace t;
        ReadHatchTrace(a_hatch, t);
        PLOG_INFO(
            "[PULSAR] L.6 V3 TRACE [{}] hatch=0x{:08X} base=0x{:08X} cell=0x{:08X} "
            "outer=0x{:X} inner=0x{:X} destRef=0x{:X} {} "
            "destFmt=0x{:08X} destBase=0x{:08X} destCell=0x{:08X} "
            "flags=0x{:X} destPos=({:.2f},{:.2f},{:.2f}) destRot=({:.2f},{:.2f},{:.2f}) destFlag=0x{:02X} "
            "bits=0x{:X}/0x{:X}/0x{:X}"sv,
            a_label, t.hatchFmt, t.hatchBase, t.parentCellFmt,
            t.outerPtr, t.innerPtr, t.destDoorRefVal,
            t.destResolvable ? "RESOLVABLE" : "DANGLING",
            t.destFmt, t.destBase, t.destParentCellFmt,
            t.flags, t.destX, t.destY, t.destZ, t.rotX, t.rotY, t.rotZ, t.destFlag,
            t.bits18, t.bits20, t.bits2C);
    }

    static void DumpAllDoorsInCell(RE::TESObjectCELL* a_cell)
    {
        if (!a_cell) return;
        const std::uint32_t cellID = a_cell->formID;
        const std::uint32_t prev = g_lastDumpedCellID.exchange(cellID, std::memory_order_acq_rel);
        if (prev == cellID) return;  // already dumped this cell

        std::uint32_t size = 0;
        void* data = nullptr;
        __try {
            const auto base = reinterpret_cast<const std::uint8_t*>(a_cell) + 0x80;
            size = *reinterpret_cast<const std::uint32_t*>(base + 0x00);
            data = *reinterpret_cast<void* const*>(base + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (!data || size == 0 || size > 65536) return;

        PLOG_INFO("[PULSAR] L.6 V3 DOOR-DUMP cell=0x{:08X} doors:"sv, cellID);
        auto getFn = ResolveGetExtraByType();
        for (std::uint32_t i = 0; i < size; ++i) {
            RE::TESObjectREFR* ref = nullptr;
            __try {
                auto* slot = reinterpret_cast<RE::TESObjectREFR**>(
                    reinterpret_cast<std::uint8_t*>(data) + (i * 8));
                ref = *slot;
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!ref) continue;
            const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ref);
            if (addr < 0x10000ULL || addr >= 0x7FFFFFFFFFFFULL) continue;

            std::uint32_t baseFmt = 0; std::uint8_t  bft = 0;
            float rx = 0, ry = 0, rz = 0;
            __try {
                auto* b = ref->data.objectReference.get();
                if (!b) continue;
                bft = static_cast<std::uint8_t>(b->formType.get());
                baseFmt = b->formID;
                rx = ref->data.location.x;
                ry = ref->data.location.y;
                rz = ref->data.location.z;
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (bft != kFormType_DOOR) continue;

            bool hasXT = false, hasInner = false, hasShipArr = false;
            std::uint32_t destFmt = 0;
            float dpx = 0, dpy = 0, dpz = 0;
            std::uint32_t flags = 0;
            __try {
                auto* pb = reinterpret_cast<std::uint8_t*>(ref);
                void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
                if (extraList && getFn) {
                    void* xt = getFn(extraList, kExtraType_Teleport);
                    if (xt) {
                        hasXT = true;
                        std::uint64_t innerVal = *reinterpret_cast<std::uint64_t*>(
                            reinterpret_cast<std::uint8_t*>(xt) + 0x18);
                        if (innerVal >= 0x10000ULL && innerVal < 0x7FFFFFFFFFFFULL) {
                            hasInner = true;
                            auto* in = reinterpret_cast<RefrTeleport*>(innerVal);
                            flags = in->flags;
                            dpx = in->destPosX; dpy = in->destPosY; dpz = in->destPosZ;
                            if (in->destDoorRef >= 0x10000ULL
                                && in->destDoorRef < 0x7FFFFFFFFFFFULL) {
                                auto* d = reinterpret_cast<RE::TESObjectREFR*>(in->destDoorRef);
                                destFmt = d->formID;
                            }
                        }
                    }
                    void* sa = getFn(extraList, kExtraType_ShipArrivalData);
                    hasShipArr = (sa != nullptr);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            PLOG_INFO(
                "[PULSAR] L.6 V3 DOOR-DUMP   ref=0x{:08X} base=0x{:08X} pos=({:.1f},{:.1f},{:.1f}) "
                "XT={} inner={} ShipArr={} destRef=0x{:08X} destPos=({:.1f},{:.1f},{:.1f}) flags=0x{:X}"sv,
                ref->formID, baseFmt, rx, ry, rz,
                hasXT ? 1 : 0, hasInner ? 1 : 0, hasShipArr ? 1 : 0,
                destFmt, dpx, dpy, dpz, flags);
        }
    }

    // Phase 3i v4 diag: log once per (cell, picked-ref, path) tuple.
    static std::atomic<std::uint64_t> g_lastPickKey{ 0 };
    static void LogPickPath(RE::TESObjectREFR* a_ref, const char* a_path)
    {
        if (!a_ref) return;
        std::uint32_t fid = 0;
        __try { fid = a_ref->formID; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        const std::uint64_t key = (std::uint64_t(fid) << 32) ^ std::uint64_t(reinterpret_cast<std::uintptr_t>(a_path));
        const std::uint64_t prev = g_lastPickKey.exchange(key, std::memory_order_acq_rel);
        if (prev == key) return;
        PLOG_INFO("[PULSAR] L.6 V3 DockSync → hatch via {} ref=0x{:08X}"sv, a_path, fid);
    }

    static RE::TESObjectREFR* FindLocalHatchInCell(RE::TESObjectCELL* a_cell)
    {
        if (!a_cell) return nullptr;
        std::uint32_t size = 0, capacity = 0;
        void* data = nullptr;
        __try {
            const auto base = reinterpret_cast<const std::uint8_t*>(a_cell) + 0x80;
            size     = *reinterpret_cast<const std::uint32_t*>(base + 0x00);
            capacity = *reinterpret_cast<const std::uint32_t*>(base + 0x04);
            data     = *reinterpret_cast<void* const*>(base + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!data || size == 0 || size > 65536) return nullptr;

        RE::TESObjectREFR* fallback = nullptr;
        RE::TESObjectREFR* populatedXTFallback = nullptr;
        RE::TESObjectREFR* remoteBaseMatch = nullptr;
        RE::TESObjectREFR* freshXTMatch = nullptr;
        RE::TESObjectREFR* shipArrMatch = nullptr;
        for (std::uint32_t i = 0; i < size; ++i) {
            RE::TESObjectREFR* ref = nullptr;
            __try {
                auto* slot = reinterpret_cast<RE::TESObjectREFR**>(
                    reinterpret_cast<std::uint8_t*>(data) + (i * 8));
                ref = *slot;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!ref) continue;
            const std::uintptr_t refAddr = reinterpret_cast<std::uintptr_t>(ref);
            if (refAddr < 0x10000ULL || refAddr >= 0x7FFFFFFFFFFFULL) continue;

            // BUG FIX (Phase 3b v3 2026-05-02): we want to match the BASE
            // form's formType (= kDOOR=0x25) not the REFR's formType (which is
            // always kREFR=0x4A). Earlier v2 logged "no hatch" for 15s
            // straight because we tested ref->formType == 0x25 which is
            // never true.
            std::uint32_t baseFormID = 0;
            std::uint8_t  baseFormType = 0;
            __try {
                auto* base = ref->data.objectReference.get();
                if (base) {
                    baseFormID   = base->formID;
                    baseFormType = static_cast<std::uint8_t>(base->formType.get());
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            // Phase 3i v8: TWO-PASS to avoid premature returns. Per-iteration
            // returns let earlier-priority paths "win" only if a later-priority
            // ref hasn't been seen yet - but iteration order is arbitrary.
            // Track best-by-priority across iteration, decide at end.
            //   (1) remoteBase match (CLIENT receiver - strict)
            //   (2) freshXT - populated AND snapshot taken AND not in snapshot
            //       (HOST pilot - gated on snapshot-taken so this path doesn't
            //       trigger on client where snapshot is empty)
            //   (3) kShipArrivalData (universal ship-hatch marker)
            //   (4) populated XT (post-dock fallback)
            //   (5) first kDOOR (last resort)
            if (baseFormType == kFormType_DOOR) {
                const std::uint32_t remoteBase =
                    g_remoteDockHatchBase.load(std::memory_order_acquire);
                if (remoteBase != 0 && baseFormID == remoteBase && !remoteBaseMatch) {
                    remoteBaseMatch = ref;
                }
                const bool populated = DoorHasPopulatedXT(ref);
                if (populated &&
                    g_preDockSnapshotTaken.load(std::memory_order_acquire) &&
                    !IsPreDockPopulated(baseFormID) &&
                    !freshXTMatch) {
                    freshXTMatch = ref;
                }
                if (DoorHasShipArrivalData(ref) && !shipArrMatch) {
                    shipArrMatch = ref;
                }
                if (populated && !populatedXTFallback) {
                    populatedXTFallback = ref;
                }
                if (!fallback) fallback = ref;
            }
        }
        if (remoteBaseMatch) { LogPickPath(remoteBaseMatch, "remoteBase"); return remoteBaseMatch; }
        if (freshXTMatch)    { LogPickPath(freshXTMatch,    "freshXT");    return freshXTMatch; }
        if (shipArrMatch)    { LogPickPath(shipArrMatch,    "ShipArrivalData"); return shipArrMatch; }
        if (populatedXTFallback) { LogPickPath(populatedXTFallback, "populatedXT"); return populatedXTFallback; }
        if (fallback) LogPickPath(fallback, "firstKDOOR");
        return fallback;
    }

    // Phase 3b host capture - read the 28-byte ExtraTeleport inner block off
    // a populated hatch REFR. Returns true if struct was successfully read
    // AND the inner pos appears valid (not all-zero, suggesting engine has
    // populated it post-cutscene).
    static bool CaptureHatchExtraTeleport(RE::TESObjectREFR* a_hatch,
                                            RefrTeleport& outRT,
                                            std::uint32_t& outDestFormID)
    {
        outDestFormID = 0;
        if (!a_hatch) return false;
        auto getFn = ResolveGetExtraByType();
        if (!getFn) return false;

        void* extraTele = nullptr;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return false;
            extraTele = getFn(extraList, kExtraType_Teleport);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (!extraTele) return false;

        // ExtraTeleport outer +0x18 = pointer to RefrTeleport inner (0x28 bytes).
        RefrTeleport tmp{};
        std::uint64_t innerPtrVal = 0;
        __try {
            innerPtrVal = *reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(extraTele) + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (innerPtrVal < 0x10000ull || innerPtrVal >= 0x00007FFFFFFFFFFFull) {
            return false;
        }

        __try {
            tmp = *reinterpret_cast<RefrTeleport*>(innerPtrVal);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        // Engine considers an ExtraTeleport "populated" when the destDoorRef
        // handle resolves to a real REFR. We extract the destFormID from the
        // pointer (it's a NiPointer<TESObjectREFR>, raw inner ptr at +0x00).
        if (tmp.destDoorRef) {
            __try {
                auto* destRef = reinterpret_cast<RE::TESObjectREFR*>(tmp.destDoorRef);
                outDestFormID = destRef->formID;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                outDestFormID = 0;
            }
        }

        outRT = tmp;
        return outDestFormID != 0;  // require dest FormID known to consider it valid
    }

    // Phase 3b client-side apply: ensure local hatch has ExtraTeleport with
    // populated inner, then write the host-broadcast bytes. Returns true on
    // success.
    static bool ApplyHatchExtraTeleport(
        RE::TESObjectREFR* a_hatch,
        const Pulsar::Network::DockExtraTeleportData& a_pkt)
    {
        if (!a_hatch) return false;
        auto fn = ResolveAddOrGetExtraTeleport();
        if (!fn) return false;

        // Resolve the dest door REFR via FormID lookup. If station cell isn't
        // loaded on client, this returns null - that's OK per earlier RE work
        // recipe; we set destDoorRef=null and rely on pos/rot for the engine
        // teleport, plus PULSAR's existing TransitSync handles the cell-load.
        RE::TESObjectREFR* destRefPtr = nullptr;
        if (a_pkt.destDoorFormID) {
            __try {
                auto* destForm = RE::TESForm::LookupByID(a_pkt.destDoorFormID);
                if (destForm) destRefPtr = destForm->As<RE::TESObjectREFR>();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                destRefPtr = nullptr;
            }
        }

        // Phase 3b v4 (2026-05-02): bypass AddOrGetExtraTeleport's confusing
        // return type (earlier RE work decompile suggested "returns inner" but
        // actually returns OUTER on early-path and INNER on alloc-path -
        // inconsistent). Strategy: call it (allocates if needed, idempotent),
        // discard return value, then re-fetch outer via GetByType from the
        // extraList. Outer is guaranteed to exist + have non-null inner-ptr
        // after the call.
        __try {
            (void)fn(a_hatch);  // ensure ExtraTeleport exists with inner allocated
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: AddOrGetExtraTeleport SEH"sv);
            return false;
        }

        // Re-fetch outer + read inner-ptr.
        auto getFn = ResolveGetExtraByType();
        if (!getFn) return false;
        std::uint64_t innerPtr = 0;
        std::uint64_t outerPtr = 0;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) {
                PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: extraList null after AddOrGet"sv);
                return false;
            }
            void* xt = getFn(extraList, kExtraType_Teleport);
            outerPtr = reinterpret_cast<std::uint64_t>(xt);
            if (!xt) {
                PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: outer null after AddOrGet"sv);
                return false;
            }
            innerPtr = *reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(xt) + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: outer-fetch SEH"sv);
            return false;
        }
        if (innerPtr < 0x10000ull || innerPtr >= 0x00007FFFFFFFFFFFull) {
            PLOG_WARN(
                "[PULSAR] L.6 V3 DockSync ApplyHatch: inner ptr invalid 0x{:X} "
                "(outer=0x{:X})"sv, innerPtr, outerPtr);
            return false;
        }
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ApplyHatch: outer=0x{:X} inner=0x{:X}"sv,
            outerPtr, innerPtr);

        // Write the captured fields into the inner struct.
        auto* inner = reinterpret_cast<RefrTeleport*>(innerPtr);
        __try {
            inner->destDoorRef = reinterpret_cast<std::uint64_t>(destRefPtr);
            inner->flags       = a_pkt.flags;
            inner->destPosX    = a_pkt.destPosX;
            inner->destPosY    = a_pkt.destPosY;
            inner->destPosZ    = a_pkt.destPosZ;
            inner->destRotX    = a_pkt.destRotX;
            inner->destRotY    = a_pkt.destRotY;
            inner->destRotZ    = a_pkt.destRotZ;
            inner->destFlag    = a_pkt.destFlag;

            // Set REFR.flags16 |= 0x40 - what SetUpDockExtraTeleport does post-pair.
            auto* p16 = reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uint8_t*>(a_hatch) + 0x2C);
            *p16 |= 0x40;

            // Phase 3c (2026-05-02): earlier RE work RE corrected the polarity.
            // FUN_140B58E40 is a REJECT filter (1=skip, 0=eligible). Earlier
            // versions set the "skip" bits - engine politely ignored the hatch.
            //
            // What engine's own SetUpDockExtraTeleport (RVA 0x14213B9C0) does:
            //   AND  word [+0x2C], 0xBFFF  ; CLEAR bit 14 (was set, must clear)
            //   OR   dword [+0x20], 0x200  ; SET formFlags bit 9 ("set up by dock")
            //   OR   word  [+0x2C], 0x40   ; SET formChangeFlags bit 6
            //   call vt[23](refr, 1)       ; mark dirty
            //   call FUN_140B37390(refr, 1); enable-for-dock + cell-walk
            //
            // The previously-set bits +0x18 |= 0x200, +0x20 |= 0x100, +0x2C
            // |= 0x4000 ARE what the gate filters out - they suppress the
            // prompt. We CLEAR them now and SET only what engine sets.
            auto* pUnk18 = reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(a_hatch) + 0x18);
            auto* pFormFlags = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(a_hatch) + 0x20);
            // p16 (above) = pointer to *(uint16_t*)(a_hatch + 0x2C)

            // Clear the engine "skip-prompt" early-out bits.
            *pUnk18     &= ~0x200ull;     // unk18 bit 9 (gate skip)
            *pFormFlags &= ~0x100u;       // formFlags bit 8 (kDOOR-only skip)
            *p16        &= 0xBFFF;        // CLEAR bit 14 of formChangeFlags

            // Set engine's "configured by docking" markers.
            *pFormFlags |= 0x200;         // formFlags bit 9 - activation profile route
            *p16        |= 0x0040;        // formChangeFlags bit 6 - XT configured (already done above; idempotent)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: bit-write SEH"sv);
            return false;
        }

        // Phase 3c step 3 + 4: vt[23] mark-dirty + EnableForDock raw call.
        // Both must happen AFTER the bit writes. Separate __try block because
        // these involve indirect calls that have their own SEH boundary.
        __try {
            auto** vtbl = *reinterpret_cast<void***>(a_hatch);
            using DirtyVf = void (__fastcall*)(RE::TESObjectREFR*, std::uint32_t);
            auto dirtyFn = reinterpret_cast<DirtyVf>(vtbl[23]);
            if (dirtyFn) dirtyFn(a_hatch, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: vt[23] dirty-call SEH"sv);
        }

        __try {
            using EnableForDockFn = void (__fastcall*)(RE::TESObjectREFR*, char);
            auto enableFn = reinterpret_cast<EnableForDockFn>(
                REL::Offset{ kRVA_EnableForDock }.address());
            if (enableFn) enableFn(a_hatch, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: EnableForDock SEH"sv);
        }

        // Phase 3c step 5: verify reject-filter now returns 0 (eligible).
        std::uint32_t readback18 = 0, readback20 = 0;
        std::uint16_t readback2C = 0;
        std::int8_t   filterResult = -1;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            readback18 = *reinterpret_cast<std::uint32_t*>(pb + 0x18);
            readback20 = *reinterpret_cast<std::uint32_t*>(pb + 0x20);
            readback2C = *reinterpret_cast<std::uint16_t*>(pb + 0x2C);

            using FilterFn = char (__fastcall*)(RE::TESObjectREFR*);
            auto filterFn = reinterpret_cast<FilterFn>(
                REL::Offset{ kRVA_RejectFilter }.address());
            if (filterFn) filterResult = static_cast<std::int8_t>(filterFn(a_hatch));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ApplyHatch: read-back SEH"sv);
        }
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ApplyHatch: bits +0x18=0x{:08X} "
            "+0x20=0x{:08X} +0x2C=0x{:04X}; rejectFilter={} "
            "(0=ELIGIBLE want this, 1=skip)"sv,
            readback18, readback20, readback2C, filterResult);
        NASLOG("L.6 V3 DockSync ApplyHatch: bits 0x%X/0x%X/0x%X filter=%d",
               readback18, readback20, readback2C, filterResult);

        return true;
    }

    // Phase 3d: Log candidate ship + dock pose fields so we can find which
    // one drives the visual post-cutscene. Runs once per second on host
    // during the dock-poll window. Verified offsets from FlightSync.cpp:
    //   NiAVObject m_local.rotate    @ +0x40
    //   NiAVObject m_local.translate @ +0x70
    //   NiAVObject m_world.rotate    @ +0x80
    //   NiAVObject m_world.translate @ +0xB0
    static void LogPoseCandidates()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* ship = player->GetSpaceship(true);
        if (!ship) return;

        float  dataLoc[3] = {0,0,0};
        float  dataAng[3] = {0,0,0};
        float  niLocalT[3] = {0,0,0};
        float  niWorldT[3] = {0,0,0};
        std::uint64_t loadedDataPtr = 0, data3DPtr = 0;

        __try {
            dataLoc[0] = ship->data.location.x;
            dataLoc[1] = ship->data.location.y;
            dataLoc[2] = ship->data.location.z;
            dataAng[0] = ship->data.angle.x;
            dataAng[1] = ship->data.angle.y;
            dataAng[2] = ship->data.angle.z;

            auto* shipB = reinterpret_cast<std::uint8_t*>(ship);
            loadedDataPtr = *reinterpret_cast<std::uint64_t*>(shipB + 0xC8);
            if (loadedDataPtr >= 0x10000ull
                && loadedDataPtr < 0x00007FFFFFFFFFFFull) {
                auto* ld = reinterpret_cast<std::uint8_t*>(loadedDataPtr);
                data3DPtr = *reinterpret_cast<std::uint64_t*>(ld + 0x68);
                if (data3DPtr >= 0x10000ull
                    && data3DPtr < 0x00007FFFFFFFFFFFull) {
                    auto* d3d = reinterpret_cast<std::uint8_t*>(data3DPtr);
                    // FIXED offsets (Phase 3d v2): local translate @ +0x70.
                    niLocalT[0] = *reinterpret_cast<float*>(d3d + 0x70);
                    niLocalT[1] = *reinterpret_cast<float*>(d3d + 0x74);
                    niLocalT[2] = *reinterpret_cast<float*>(d3d + 0x78);
                    niWorldT[0] = *reinterpret_cast<float*>(d3d + 0xB0);
                    niWorldT[1] = *reinterpret_cast<float*>(d3d + 0xB4);
                    niWorldT[2] = *reinterpret_cast<float*>(d3d + 0xB8);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync pose-ship: data=({:.1f},{:.1f},{:.1f}) "
            "ang=({:.2f},{:.2f},{:.2f}) niLocal=({:.1f},{:.1f},{:.1f}) "
            "niWorld=({:.1f},{:.1f},{:.1f}) ld=0x{:X} 3d=0x{:X}"sv,
            dataLoc[0], dataLoc[1], dataLoc[2],
            dataAng[0], dataAng[1], dataAng[2],
            niLocalT[0], niLocalT[1], niLocalT[2],
            niWorldT[0], niWorldT[1], niWorldT[2],
            loadedDataPtr, data3DPtr);
    }

    // Standalone host-side pose log tick - runs in parallel to hatch-poll
    // and continues after broadcast fires (we want to see pose for full 60s).
    static void TickPoseLog()
    {
        const std::uint64_t until = g_hatchPollUntilMs.load(std::memory_order_acquire);
        if (until == 0) return;  // only during dock window
        const std::uint64_t now = ::GetTickCount64();
        if (now >= until) return;
        const std::uint64_t nextLog = g_poseLogNextMs.load(std::memory_order_acquire);
        if (now < nextLog) return;
        g_poseLogNextMs.store(now + kPoseLogIntervalMs, std::memory_order_release);

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected() || !net.IsHost()) return;

        LogPoseCandidates();
    }

    // Phase 3b host poll - called from Tick during the 60s post-broadcast
    // window. Walks player cell for the cockpit hatch, captures + broadcasts
    // when ExtraTeleport becomes populated. Single-shot per dock event.
    static std::atomic<std::uint64_t> g_hatchLastDiagMs{ 0 };
    constexpr std::uint64_t kHatchDiagIntervalMs = 1000;  // 1Hz diag log

    static void TickHatchTeleportCapture()
    {
        const std::uint64_t until = g_hatchPollUntilMs.load(std::memory_order_acquire);
        if (until == 0) return;
        const std::uint64_t now = ::GetTickCount64();
        if (now >= until) {
            g_hatchPollUntilMs.store(0, std::memory_order_release);
            PLOG_INFO("[PULSAR] L.6 V3 DockSync hatch-XT poll WINDOW CLOSED (no broadcast fired)"sv);
            return;
        }
        if (g_hatchTeleportSent.load(std::memory_order_acquire)) return;
        const std::uint64_t nextSample = g_hatchPollNextMs.load(std::memory_order_acquire);
        if (now < nextSample) return;
        g_hatchPollNextMs.store(now + kHatchPollIntervalMs, std::memory_order_release);

        auto& net = Pulsar::Network::GetSingleton();
        // Bidirectional: any pilot side polls. Engine only populates the
        // ExtraTeleport when local engine runs dock state machine, so non-
        // pilot sides naturally find nothing and no-op.
        if (!net.IsConnected()) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* cell = player->parentCell;
        if (!cell) return;

        // 1Hz diagnostic - log what we observe each second so we can see
        // hatch acquisition + ExtraTeleport population timing.
        const std::uint64_t lastDiag = g_hatchLastDiagMs.load(std::memory_order_acquire);
        const bool wantDiag = (now - lastDiag) >= kHatchDiagIntervalMs;
        if (wantDiag) {
            g_hatchLastDiagMs.store(now, std::memory_order_release);
        }

        // Phase 3i v5: dump all kDOORs once per cockpit cell so we can
        // identify the discriminator between dock-hatch and landing-hatch.
        DumpAllDoorsInCell(cell);

        RE::TESObjectREFR* hatch = FindLocalHatchInCell(cell);
        if (!hatch) {
            if (wantDiag) {
                PLOG_INFO(
                    "[PULSAR] L.6 V3 DockSync poll: no hatch in cell=0x{:08X}"sv,
                    cell->formID);
            }
            return;
        }

        // Read ExtraTeleport state for diagnostic.
        std::uint64_t outerPtr = 0, innerPtr = 0;
        bool hasInner = false;
        std::uint32_t destFmt = 0;
        float dpx = 0, dpy = 0, dpz = 0;
        __try {
            auto getFn = ResolveGetExtraByType();
            auto* pb = reinterpret_cast<std::uint8_t*>(hatch);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (extraList && getFn) {
                void* xt = getFn(extraList, kExtraType_Teleport);
                outerPtr = reinterpret_cast<std::uint64_t>(xt);
                if (xt) {
                    innerPtr = *reinterpret_cast<std::uint64_t*>(
                        reinterpret_cast<std::uint8_t*>(xt) + 0x18);
                    if (innerPtr >= 0x10000ull && innerPtr < 0x00007FFFFFFFFFFFull) {
                        hasInner = true;
                        auto* in = reinterpret_cast<RefrTeleport*>(innerPtr);
                        dpx = in->destPosX; dpy = in->destPosY; dpz = in->destPosZ;
                        if (in->destDoorRef >= 0x10000ull
                            && in->destDoorRef < 0x00007FFFFFFFFFFFull) {
                            auto* destRef = reinterpret_cast<RE::TESObjectREFR*>(in->destDoorRef);
                            destFmt = destRef->formID;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync poll: state-read SEH"sv);
            return;
        }

        if (wantDiag) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync poll: hatch=0x{:08X} cell=0x{:08X} "
                "xt-outer=0x{:X} inner=0x{:X} destRef=0x{:08X} pos=({:.1f},{:.1f},{:.1f})"sv,
                hatch->formID, cell->formID, outerPtr, innerPtr, destFmt, dpx, dpy, dpz);
        }

        // Acceptance criteria for broadcast (loosened from initial v1):
        // We need at MINIMUM the inner data to be allocated AND non-zero
        // destPos (the actual teleport target). destDoorRef is preferred but
        // engine may set it via NiPointer-handle indirection that doesn't
        // resolve through our raw deref - accept its absence too.
        const bool posValid = (dpx != 0.0f) || (dpy != 0.0f) || (dpz != 0.0f);
        if (!hasInner || !posValid) return;

        RefrTeleport rt{};
        std::uint32_t destFormID = 0;
        if (!CaptureHatchExtraTeleport(hatch, rt, destFormID)) {
            // Fallback: capture whatever we can read directly.
            __try {
                rt = *reinterpret_cast<RefrTeleport*>(innerPtr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return;
            }
            destFormID = destFmt;
        }

        // Captured a populated ExtraTeleport. Build + broadcast packet.
        // Phase 3i v6: broadcast the ACTUAL chosen hatch's base FormID so
        // client can match locally (different runtime FormID, same base).
        std::uint32_t actualBase = kHatchBaseFormID;
        __try {
            auto* baseObj = hatch->data.objectReference.get();
            if (baseObj) actualBase = baseObj->formID;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        Pulsar::Network::DockExtraTeleportData payload{};
        payload.hatchBaseFormID   = actualBase;
        payload.cockpitCellFormID = cell->formID;
        payload.destDoorFormID    = destFormID;
        payload.flags             = rt.flags;
        payload.destPosX          = rt.destPosX;
        payload.destPosY          = rt.destPosY;
        payload.destPosZ          = rt.destPosZ;
        payload.destRotX          = rt.destRotX;
        payload.destRotY          = rt.destRotY;
        payload.destRotZ          = rt.destRotZ;
        payload.destFlag          = rt.destFlag;

        std::uint8_t buf[256]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_DOCK_EXTRATELEPORT, buf, sizeof(buf));
        if (!w.Write(payload)) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync XT broadcast: writer failed"sv);
            return;
        }
        net.Send(buf, w.Size(), /*reliable*/ true);
        g_hatchTeleportSent.store(true, std::memory_order_release);

        LogHatchTrace("HOST.cockpit-at-broadcast", hatch);
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⇒ XT BROADCAST hatch=0x{:08X} cell=0x{:08X} "
            "dest=0x{:08X} pos=({:.1f},{:.1f},{:.1f}) rot=({:.2f},{:.2f},{:.2f}) "
            "flags=0x{:X} destFlag=0x{:02X}"sv,
            hatch->formID, cell->formID, destFormID,
            payload.destPosX, payload.destPosY, payload.destPosZ,
            payload.destRotX, payload.destRotY, payload.destRotZ,
            payload.flags, payload.destFlag);
        NASLOG("L.6 V3 DockSync XT BROADCAST hatch=0x%08X dest=0x%08X",
               hatch->formID, destFormID);
    }

    // Phase 3b v7: client-side persistence tick. Periodically re-applies
    // bits + invokes gate fn to log state. Detects engine-side bit clearing.
    static void TickHatchPersistence()
    {
        if (!g_xtPersistArmed.load(std::memory_order_acquire)) return;
        const std::uint64_t now = ::GetTickCount64();
        const std::uint64_t until = g_xtPersistUntilMs.load(std::memory_order_acquire);
        if (now >= until) {
            g_xtPersistArmed.store(false, std::memory_order_release);
            PLOG_INFO("[PULSAR] L.6 V3 DockSync XT persist WINDOW CLOSED"sv);
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* cell = player->parentCell;
        if (!cell) return;

        RE::TESObjectREFR* hatch = FindLocalHatchInCell(cell);
        if (!hatch) return;

        // Phase 3c persistence: re-clear engine "skip" bits + re-set engine
        // "configured" bits in case the engine flips state during ship Tick.
        std::uint64_t bits18 = 0;
        std::uint32_t bits20 = 0;
        std::uint16_t bits2C = 0;
        std::int8_t   filterRes = -1;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(hatch);
            auto* p18 = reinterpret_cast<std::uint64_t*>(pb + 0x18);
            auto* p20 = reinterpret_cast<std::uint32_t*>(pb + 0x20);
            auto* p2C = reinterpret_cast<std::uint16_t*>(pb + 0x2C);
            // Clear skip bits (do this every tick - engine ship-update may re-set them).
            *p18 &= ~0x200ull;
            *p20 &= ~0x100u;
            *p2C &= 0xBFFF;
            // Set configured bits.
            *p20 |= 0x200;
            *p2C |= 0x40;
            bits18 = *p18;
            bits20 = *p20;
            bits2C = *p2C;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        // 1Hz gate-state log.
        const std::uint64_t lastLog = g_xtPersistNextLogMs.load(std::memory_order_acquire);
        if (now < lastLog) return;
        g_xtPersistNextLogMs.store(now + kXTPersistLogIntervalMs,
                                    std::memory_order_release);

        __try {
            using FilterFn = char (__fastcall*)(RE::TESObjectREFR*);
            auto filterFn = reinterpret_cast<FilterFn>(
                REL::Offset{ kRVA_RejectFilter }.address());
            if (filterFn) filterRes = static_cast<std::int8_t>(filterFn(hatch));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Read player + hatch position so we can correlate "gate=1 but no
        // prompt" with player-distance-to-hatch.
        float ppx = 0, ppy = 0, ppz = 0, hpx = 0, hpy = 0, hpz = 0;
        __try {
            ppx = player->data.location.x;
            ppy = player->data.location.y;
            ppz = player->data.location.z;
            hpx = hatch->data.location.x;
            hpy = hatch->data.location.y;
            hpz = hatch->data.location.z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        const float dx = ppx - hpx, dy = ppy - hpy, dz = ppz - hpz;
        const float dist2 = dx*dx + dy*dy + dz*dz;

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync persist: hatch=0x{:08X} "
            "bits=0x{:X}/0x{:X}/0x{:X} rejectFilter={} (want 0) "
            "player=({:.1f},{:.1f},{:.1f}) "
            "hatch=({:.1f},{:.1f},{:.1f}) dist²={:.1f}"sv,
            hatch->formID, bits18, bits20, bits2C, filterRes,
            ppx, ppy, ppz, hpx, hpy, hpz, dist2);

        // Phase 3i v10: also trace full XT state on this hatch + station-side
        // hatch (if known) at 1Hz. Diff between host & client logs reveals
        // what's missing on client for return-trip.
        auto& net2 = Pulsar::Network::GetSingleton();
        const char* role = (net2.IsConnected() && net2.IsHost()) ? "HOST" : "CLIENT";
        char label[64];
        std::snprintf(label, sizeof(label), "%s.persist-cockpit", role);
        LogHatchTrace(label, hatch);
        const std::uint32_t stationFmt =
            g_appliedStationSideHatchFormID.load(std::memory_order_acquire);
        if (stationFmt) {
            __try {
                if (auto* sf = RE::TESForm::LookupByID(stationFmt)) {
                    if (auto* sr = sf->As<RE::TESObjectREFR>()) {
                        char slabel[64];
                        std::snprintf(slabel, sizeof(slabel), "%s.persist-station", role);
                        LogHatchTrace(slabel, sr);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    // SEH-free helper to copy received station-XT under mutex. Separated
    // because std::lock_guard's destructor conflicts with __try in caller.
    static void SnapshotReceivedStationXT(Pulsar::Network::DockStationXTData& a_out,
                                            bool& a_haveValid)
    {
        a_haveValid = g_haveReceivedStationXT.load(std::memory_order_acquire);
        if (a_haveValid) {
            std::lock_guard lock(g_receivedStationXTMutex);
            a_out = g_receivedStationXT;
        } else {
            // Frontier-specific fallback values (Phase 3f v9 hardcoded).
            a_out = {};
            a_out.flags    = 0xD7DBD;
            a_out.destPosX = -0.77f;
            a_out.destPosY = -5.99f;
            a_out.destPosZ = -1.25f;
            a_out.destFlag = 0x00;
        }
    }

    // Phase 3f: apply Phase 3c-style bypass to a station-side hatch REFR.
    // Same as ApplyHatchExtraTeleport but for the OTHER side of the dock -
    // the kDOOR in station-interior that takes the player back to the ship.
    // We allocate inner XT (gate non-null check passes) and write destDoorRef
    // pointing to our cockpit hatch (so engine teleport returns to ship).
    static bool ApplyStationSideHatchBypass(RE::TESObjectREFR* a_hatch,
                                              std::uint32_t a_cockpitHatchFormID)
    {
        if (!a_hatch) return false;
        auto fn = ResolveAddOrGetExtraTeleport();
        if (!fn) return false;

        __try { (void)fn(a_hatch); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        // Resolve cockpit hatch REFR for destDoorRef.
        RE::TESObjectREFR* cockpitHatch = nullptr;
        if (a_cockpitHatchFormID) {
            __try {
                auto* form = RE::TESForm::LookupByID(a_cockpitHatchFormID);
                if (form) cockpitHatch = form->As<RE::TESObjectREFR>();
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // Re-fetch outer + inner pointer.
        auto getFn = ResolveGetExtraByType();
        if (!getFn) return false;
        std::uint64_t innerPtr = 0;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return false;
            void* xt = getFn(extraList, kExtraType_Teleport);
            if (!xt) return false;
            innerPtr = *reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(xt) + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (innerPtr < 0x10000ull || innerPtr >= 0x00007FFFFFFFFFFFull) {
            return false;
        }

        // Phase 3i: use pilot-broadcast XT bytes if available, fallback else.
        Pulsar::Network::DockStationXTData stationXT;
        bool haveReceivedXT;
        SnapshotReceivedStationXT(stationXT, haveReceivedXT);

        // Phase 3i v9.6 (2026-05-02): destDoorRef cleared. v9.5 IncRef-write
        // still crashed (different fault offset 0x10027 vs v9.3's 0x1C00300027).
        // Engine deref's the REFR's parentCell or similar after reading
        // destDoorRef → cell-unloaded state breaks the chain. Refcount on
        // the REFR itself isn't enough. Need either cell-keepalive OR a
        // different return-trip strategy.
        __try {
            auto* inner = reinterpret_cast<RefrTeleport*>(innerPtr);
            inner->destDoorRef = 0;
            inner->flags       = stationXT.flags;
            inner->destPosX    = stationXT.destPosX;
            inner->destPosY    = stationXT.destPosY;
            inner->destPosZ    = stationXT.destPosZ;
            inner->destRotX    = stationXT.destRotX;
            inner->destRotY    = stationXT.destRotY;
            inner->destRotZ    = stationXT.destRotZ;
            inner->destFlag    = stationXT.destFlag;
            (void)cockpitHatch;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync station-side ApplyHatch: source={} "
            "flags=0x{:X} pos=({:.2f},{:.2f},{:.2f})"sv,
            haveReceivedXT ? "PILOT-BROADCAST" : "FALLBACK-FRONTIER",
            stationXT.flags, stationXT.destPosX, stationXT.destPosY, stationXT.destPosZ);

        // Set Phase 3c bits.
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            auto* p18 = reinterpret_cast<std::uint64_t*>(pb + 0x18);
            auto* p20 = reinterpret_cast<std::uint32_t*>(pb + 0x20);
            auto* p2C = reinterpret_cast<std::uint16_t*>(pb + 0x2C);
            *p18 &= ~0x200ull;
            *p20 &= ~0x100u;
            *p2C &= 0xBFFF;
            *p20 |= 0x200;
            *p2C |= 0x40;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        // mark dirty + EnableForDock(1).
        __try {
            auto** vtbl = *reinterpret_cast<void***>(a_hatch);
            using DirtyVf = void (__fastcall*)(RE::TESObjectREFR*, std::uint32_t);
            auto dirtyFn = reinterpret_cast<DirtyVf>(vtbl[23]);
            if (dirtyFn) dirtyFn(a_hatch, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        __try {
            using EnableForDockFn = void (__fastcall*)(RE::TESObjectREFR*, char);
            auto enableFn = reinterpret_cast<EnableForDockFn>(
                REL::Offset{ kRVA_EnableForDock }.address());
            if (enableFn) enableFn(a_hatch, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Verify gate.
        std::int8_t filterRes = -1;
        __try {
            using FilterFn = char (__fastcall*)(RE::TESObjectREFR*);
            auto filterFn = reinterpret_cast<FilterFn>(
                REL::Offset{ kRVA_RejectFilter }.address());
            if (filterFn) filterRes = static_cast<std::int8_t>(filterFn(a_hatch));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync station-side ApplyHatch: hatch=0x{:08X} "
            "destCockpitHatch=0x{:08X} filter={} (want 0)"sv,
            a_hatch->formID, a_cockpitHatchFormID, filterRes);
        return true;
    }

    // Phase 3i: pilot-side capture of station-side hatch XT bytes - runs
    // when pilot crosses cockpit→station, finds local station-side hatch
    // (engine-populated by SetUpDockExtraTeleport), broadcasts bytes to
    // peers so they can apply ship-specific values instead of hardcoded.
    static void CaptureAndBroadcastStationXT(RE::TESObjectREFR* a_stationHatch)
    {
        if (!a_stationHatch) return;
        if (g_stationXTBroadcastSent.load(std::memory_order_acquire)) return;

        std::uint64_t innerPtr = 0;
        Pulsar::Network::DockStationXTData payload{};
        __try {
            auto getFn = ResolveGetExtraByType();
            if (!getFn) return;
            auto* pb = reinterpret_cast<std::uint8_t*>(a_stationHatch);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (!extraList) return;
            void* xt = getFn(extraList, kExtraType_Teleport);
            if (!xt) return;
            innerPtr = *reinterpret_cast<std::uint64_t*>(
                reinterpret_cast<std::uint8_t*>(xt) + 0x18);
            if (innerPtr < 0x10000ull || innerPtr >= 0x00007FFFFFFFFFFFull) return;
            auto* inner = reinterpret_cast<RefrTeleport*>(innerPtr);
            payload.flags    = inner->flags;
            payload.destPosX = inner->destPosX;
            payload.destPosY = inner->destPosY;
            payload.destPosZ = inner->destPosZ;
            payload.destRotX = inner->destRotX;
            payload.destRotY = inner->destRotY;
            payload.destRotZ = inner->destRotZ;
            payload.destFlag = inner->destFlag;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        // Sanity check - only broadcast if values look engine-populated
        // (not float-MAX garbage). Reject if any pos component is huge.
        const float kMaxValid = 1.0e6f;
        if (std::fabs(payload.destPosX) > kMaxValid ||
            std::fabs(payload.destPosY) > kMaxValid ||
            std::fabs(payload.destPosZ) > kMaxValid) {
            PLOG_INFO("[PULSAR] L.6 V3 DockSync station-XT capture: SKIP - engine-populated values look uninitialized (huge pos)"sv);
            return;
        }

        payload.stationHatchFormID = a_stationHatch->formID;
        __try {
            auto* baseObj = a_stationHatch->data.objectReference.get();
            if (baseObj) payload.hatchBaseFormID = baseObj->formID;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        std::uint8_t buf[256]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_DOCK_STATION_XT, buf, sizeof(buf));
        if (!w.Write(payload)) return;
        net.Send(buf, w.Size(), /*reliable*/ true);
        g_stationXTBroadcastSent.store(true, std::memory_order_release);

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⇒ STATION-XT BROADCAST hatch=0x{:08X} base=0x{:08X} "
            "flags=0x{:X} pos=({:.2f},{:.2f},{:.2f}) rot=({:.2f},{:.2f},{:.2f}) destFlag=0x{:02X}"sv,
            payload.stationHatchFormID, payload.hatchBaseFormID,
            payload.flags,
            payload.destPosX, payload.destPosY, payload.destPosZ,
            payload.destRotX, payload.destRotY, payload.destRotZ,
            payload.destFlag);
        NASLOG("L.6 V3 DockSync STATION-XT BROADCAST hatch=0x%08X", payload.stationHatchFormID);
    }

    // Phase 3f: detect player cell-change cockpit→station and apply bypass
    // to the station-side hatch. Single-shot until next dock event. Apply
    // is deferred 2.5s after detection AND until Cell3DAttach reports the
    // cell is quiet - Phase 3f v1 crashed when running during cell-streaming.
    static void TickStationSideHatchSetup()
    {
        const bool isReceiver = g_dockAppliedLocally.load(std::memory_order_acquire);
        const bool isPilot    = g_localPilotForCurrentDock.load(std::memory_order_acquire);
        if (!isReceiver && !isPilot) return;
        // For pilot we just want to capture once. For receiver we apply once.
        if (isReceiver && g_stationSideApplied.load(std::memory_order_acquire)) {
            // Receiver already applied - nothing to do unless we're also pilot
            // (which shouldn't happen; mutually exclusive).
            if (!isPilot) return;
        }
        if (isPilot && g_stationXTBroadcastSent.load(std::memory_order_acquire)) {
            // Pilot already broadcast - nothing to do unless we're also receiver.
            if (!isReceiver) return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* cell = player->parentCell;
        if (!cell) return;

        const std::uint32_t curCell = cell->formID;
        const std::uint32_t cockpitCell = g_localCockpitCellFormID.load(std::memory_order_acquire);
        const std::uint32_t prevCell = g_lastSeenPlayerCell.exchange(curCell,
                                            std::memory_order_acq_rel);

        // Trigger detection: cockpit→other transition.
        const bool justCrossed =
            (prevCell == cockpitCell) && (curCell != cockpitCell) && (curCell != 0);
        if (justCrossed) {
            const std::uint64_t now = ::GetTickCount64();
            g_stationSideApplyDueMs.store(now + kStationSideApplyDeferMs,
                                            std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync station-side: detected cockpit→station "
                "transition (0x{:08X} → 0x{:08X}), apply armed for {}ms"sv,
                prevCell, curCell, kStationSideApplyDeferMs);
        }

        // Wait for the apply-due timer.
        const std::uint64_t dueMs = g_stationSideApplyDueMs.load(std::memory_order_acquire);
        if (dueMs == 0) return;
        const std::uint64_t now2 = ::GetTickCount64();
        if (now2 < dueMs) return;

        // Wait for cell-streaming to fully settle. Cell3DAttach::IsQuiet()
        // returns true while engine is in churn (loadMenu open, refs being
        // populated, etc.). We only apply when settled.
        if (Pulsar::Cell3DAttach::IsQuiet() || Pulsar::Cell3DAttach::WasInLoadMenu()) {
            return;  // keep armed, retry next tick
        }

        // Reset apply-due timer so we don't loop after success or failure.
        g_stationSideApplyDueMs.store(0, std::memory_order_release);

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync station-side: cell settled in 0x{:08X}, "
            "looking for station-side hatch"sv,
            curCell);

        // Phase 3i v15: disambiguate station hatch vs nearby interior doors.
        // Receiver uses pilot-broadcast hatchBaseFormID; pilot uses
        // kShipArrivalData; nearest-door is last-resort fallback.
        // Radius widened from 25u to 100u (10000u²) - safe with disambiguation,
        // robust against the 2.5s defer letting the player walk farther.
        float ppx = 0, ppy = 0, ppz = 0;
        __try {
            ppx = player->data.location.x;
            ppy = player->data.location.y;
            ppz = player->data.location.z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        Pulsar::Network::DockStationXTData broadcastSnap{};
        bool haveBroadcast = false;
        SnapshotReceivedStationXT(broadcastSnap, haveBroadcast);
        const std::uint32_t broadcastBase =
            haveBroadcast ? broadcastSnap.hatchBaseFormID : 0u;

        RE::TESObjectREFR* stationHatch =
            FindStationHatchInCell(cell, ppx, ppy, ppz,
                                   haveBroadcast, broadcastBase,
                                   /*maxDist2*/ 10000.0f);
        if (!stationHatch) {
            PLOG_WARN(
                "[PULSAR] L.6 V3 DockSync station-side: no nearby kDOOR in cell 0x{:08X} "
                "(player @ {:.1f},{:.1f},{:.1f})"sv,
                curCell, ppx, ppy, ppz);
            return;
        }
        std::uint32_t stationHatchBaseID = 0;
        __try {
            auto* baseObj = stationHatch->data.objectReference.get();
            if (baseObj) stationHatchBaseID = baseObj->formID;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync station-side: nearest kDOOR found ref=0x{:08X} "
            "base=0x{:08X}"sv, stationHatch->formID, stationHatchBaseID);

        // Phase 3i: pilot side captures host's engine-populated XT bytes and
        // broadcasts. Receiver side applies bypass using received bytes
        // (or hardcoded fallback if not yet received).
        if (isPilot) {
            CaptureAndBroadcastStationXT(stationHatch);
        }
        if (isReceiver && !g_stationSideApplied.load(std::memory_order_acquire)) {
            const std::uint32_t cockpitHatchFmt =
                g_localCockpitHatchFormID.load(std::memory_order_acquire);
            const bool ok = ApplyStationSideHatchBypass(stationHatch, cockpitHatchFmt);
            if (ok) {
                g_stationSideApplied.store(true, std::memory_order_release);
                g_appliedStationSideHatchFormID.store(stationHatch->formID,
                                                        std::memory_order_release);
                AddAppliedStationHatch(stationHatch->formID);
                LogHatchTrace(isPilot ? "HOST.station-post-apply"
                                       : "CLIENT.station-post-apply", stationHatch);
                // Also trace the cockpit hatch to see if it's still loaded.
                if (auto* form = RE::TESForm::LookupByID(cockpitHatchFmt)) {
                    if (auto* cr = form->As<RE::TESObjectREFR>())
                        LogHatchTrace(isPilot ? "HOST.cockpit-at-station-time"
                                              : "CLIENT.cockpit-at-station-time", cr);
                }
                PLOG_INFO(
                    "[PULSAR] L.6 V3 DockSync ⚡ STATION-SIDE APPLY hatch=0x{:08X} "
                    "destCockpit=0x{:08X} - return-to-ship enabled"sv,
                    stationHatch->formID, cockpitHatchFmt);
                NASLOG("L.6 V3 DockSync STATION-SIDE APPLY hatch=0x%08X",
                       stationHatch->formID);
            } else {
                PLOG_WARN("[PULSAR] L.6 V3 DockSync station-side: ApplyStationSideHatchBypass returned false"sv);
            }
        }
    }

    // Phase 3b client apply - drains pending packet, walks cell, applies XT.
    static void TickHatchTeleportApply()
    {
        if (!g_pendingXT.armed.load(std::memory_order_acquire)) return;
        const std::uint64_t now = ::GetTickCount64();
        const std::uint64_t due = g_pendingXT.dueTick.load(std::memory_order_acquire);
        if (now < due) return;

        Pulsar::Network::DockExtraTeleportData payload{};
        {
            std::lock_guard lock(g_pendingXT.m);
            payload = g_pendingXT.payload;
        }
        g_pendingXT.armed.store(false, std::memory_order_release);

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* cell = player->parentCell;
        if (!cell) return;

        // Sanity: cell FormID should match what host broadcast (cockpit cell
        // is a procedural FF-prefix instance per ship; should be the same on
        // both sides because the ship is a vanilla form).
        if (cell->formID != payload.cockpitCellFormID) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync XT apply: cell mismatch local=0x{:08X} "
                "remote=0x{:08X} - applying to local cell anyway"sv,
                cell->formID, payload.cockpitCellFormID);
        }

        RE::TESObjectREFR* hatch = FindLocalHatchInCell(cell);
        if (!hatch) {
            PLOG_WARN(
                "[PULSAR] L.6 V3 DockSync XT apply: no hatch found in cell 0x{:08X}"sv,
                cell->formID);
            return;
        }

        const bool ok = ApplyHatchExtraTeleport(hatch, payload);
        if (ok) {
            LogHatchTrace("CLIENT.cockpit-post-apply", hatch);
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync ⚡ XT APPLY hatch=0x{:08X} dest=0x{:08X} "
                "pos=({:.1f},{:.1f},{:.1f}) - ready for activate"sv,
                hatch->formID, payload.destDoorFormID,
                payload.destPosX, payload.destPosY, payload.destPosZ);
            NASLOG("L.6 V3 DockSync XT APPLY hatch=0x%08X dest=0x%08X",
                   hatch->formID, payload.destDoorFormID);
            // Phase 3i v17: track this hatch so TryHandleCockpitSideHatchActivate
            // can identify it for forward-bridge wait-block.
            AddAppliedCockpitHatch(hatch->formID);
            // Phase 3i v7: suppress other (landing) kDOORs in cockpit cell so
            // client can't accidentally walk through a non-dock hatch while
            // host is docked. Inner-XT pointers are saved + restored on undock.
            SuppressNonDockHatchesInCell(cell, payload.hatchBaseFormID);
            // Phase 3e: mark that we have applied dock state - used as gate
            // for sending PKT_DOCK_UNDOCKED on local TakeOff and as gate for
            // accepting cleanup (idempotent).
            g_dockAppliedLocally.store(true, std::memory_order_release);
            // Phase 3f: store cockpit info for station-side hatch setup
            // when player crosses into station-interior cell.
            // Phase 3i v8 fix: use CLIENT's local cell FormID (cell->formID),
            // NOT the host-broadcast payload.cockpitCellFormID. They differ
            // (procedural FF-prefix instances per peer). Cell-crossing
            // detection compares against player's parentCell, which is the
            // local instance. Using host's FormID broke the crossing event.
            g_localCockpitCellFormID.store(cell->formID,
                                            std::memory_order_release);
            g_localCockpitHatchFormID.store(hatch->formID,
                                             std::memory_order_release);
            g_stationSideApplied.store(false, std::memory_order_release);
            // Phase 3b v7: arm persistence - periodically re-set bits +
            // log gate state for next 2min so we can see if/when bits clear.
            const std::uint64_t now2 = ::GetTickCount64();
            g_xtPersistUntilMs.store(now2 + kXTPersistDurationMs,
                                      std::memory_order_release);
            g_xtPersistNextLogMs.store(now2, std::memory_order_release);
            g_xtPersistArmed.store(true, std::memory_order_release);
        } else {
            PLOG_WARN(
                "[PULSAR] L.6 V3 DockSync XT apply: ApplyHatchExtraTeleport returned false"sv);
        }
    }

    // Custom DockEvent sink - registers in addition to SpaceSync's sink (both
    // fire on every event, no conflict). This one reads the payload bytes.
    class DockEventSink : public RE::BSTEventSink<RE::Spaceship::DockEvent> {
    public:
        std::atomic<int> fireCount{ 0 };

        RE::BSEventNotifyControl ProcessEvent(
            const RE::Spaceship::DockEvent& e,
            RE::BSTEventSource<RE::Spaceship::DockEvent>* /*src*/) override
        {
            const int n = fireCount.fetch_add(1) + 1;

            // Per earlier RE work: payload bytes
            //   [0x00] = ship A REFR ptr
            //   [0x08] = ship B REFR ptr
            //   [0x10] = `started` flag (dword)
            //   [0x14] = `finished` flag (dword)
            const void* evt = static_cast<const void*>(&e);

            const std::uint32_t shipAFormID = SafeFormIDFromPtrAtOffset(evt, 0x00);
            const std::uint32_t shipBFormID = SafeFormIDFromPtrAtOffset(evt, 0x08);
            const std::uint32_t startedFlag = SafeReadDword(evt, 0x10);
            const std::uint32_t finishedFlag = SafeReadDword(evt, 0x14);

            // Plus a few more bytes around the event for hypothesis coverage.
            const std::uint32_t plus18 = SafeReadDword(evt, 0x18);
            const std::uint32_t plus1C = SafeReadDword(evt, 0x1C);
            const std::uint64_t plus20 = SafeReadQword(evt, 0x20);

            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync EVT #{}: shipA=0x{:08X} shipB=0x{:08X} "
                "started=0x{:X} finished=0x{:X} +18=0x{:X} +1C=0x{:X} +20=0x{:X}"sv,
                n, shipAFormID, shipBFormID, startedFlag, finishedFlag,
                plus18, plus1C, plus20);
            NASLOG(
                "L.6 V3 DockSync EVT #%d: shipA=0x%08X shipB=0x%08X "
                "started=0x%X finished=0x%X +18=0x%X +1C=0x%X +20=0x%llX",
                n, shipAFormID, shipBFormID, startedFlag, finishedFlag,
                plus18, plus1C, (unsigned long long)plus20);

            // Highlight the fully-docked moment per earlier RE work (site 0x14213E0D6
            // fires (1,1)). Empirical observation 2026-05-02: (1,1) does NOT
            // fire for normal player-initiated docks - only (0,0) and (1,0) do.
            if (startedFlag == 1 && finishedFlag == 1) {
                PLOG_INFO("[PULSAR] L.6 V3 DockSync ⚡ FULLY-DOCKED detected - "
                          "(started=1, finished=1)"sv);
                NASLOG("L.6 V3 DockSync FULLY-DOCKED");
            }

            // Phase 2 broadcast logic: trigger on EDGE transitions of the
            // (started, finished) flag-pair. Engine fires the same flag-pair
            // multiple times during a sequence; only broadcast once per
            // distinct stage. Stage encoding:
            //   0 = (0,0) request / abort
            //   1 = (1,0) cutscene-begin (engine committed to dock)
            //   2 = (1,1) fully-docked (rare; only seen for some dock paths)
            const std::uint32_t prevStarted =
                g_lastStartedFlag.exchange(startedFlag, std::memory_order_acq_rel);
            const std::uint32_t prevFinished =
                g_lastFinishedFlag.exchange(finishedFlag, std::memory_order_acq_rel);
            const bool startedEdge =
                (prevStarted != 0xFFFFFFFFu) && (prevStarted != startedFlag);
            const bool finishedEdge =
                (prevFinished != 0xFFFFFFFFu) && (prevFinished != finishedFlag);

            std::uint8_t stage = 0xFF;
            if (startedFlag == 1 && finishedFlag == 1) stage = 2;
            else if (startedFlag == 1 && finishedFlag == 0) stage = 1;
            else if (startedFlag == 0 && finishedFlag == 0) stage = 0;

            // Broadcast on stage 1 (engine commits to docking). Stage 0 and 2
            // are informational; only stage 1 carries the "client-must-react"
            // signal because that's when the host's engine actually starts
            // the cinematic + cell-attach work.
            if (stage == 1 && (startedEdge || finishedEdge)) {
                if (shipAFormID && shipBFormID) {
                    __try {
                        const std::uint8_t* bytes =
                            reinterpret_cast<const std::uint8_t*>(evt);
                        const RE::TESObjectREFR* shipA =
                            *reinterpret_cast<const RE::TESObjectREFR* const*>(bytes + 0x00);
                        BroadcastDockFinished(shipAFormID, shipBFormID, stage, shipA);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        PLOG_WARN("[PULSAR] L.6 V3 DockSync broadcast: shipA deref threw"sv);
                    }
                }
                // Phase 3b: arm the cockpit-hatch-ExtraTeleport poll so that
                // when the engine populates the hatch's ExtraTeleport (post-
                // cutscene, ~5-15s after this event), we capture + broadcast
                // the 28-byte struct so client can replicate without cross-
                // cell MoveTo. Single-shot per dock event.
                auto& net2 = Pulsar::Network::GetSingleton();
                if (net2.IsConnected()) {  // Phase 3e: bidirectional - any pilot side arms poll
                    const std::uint64_t now2 = ::GetTickCount64();
                    g_hatchTeleportSent.store(false, std::memory_order_release);
                    g_hatchPollUntilMs.store(now2 + kHatchPollDurationMs,
                                              std::memory_order_release);
                    g_hatchPollNextMs.store(now2, std::memory_order_release);
                    PLOG_INFO(
                        "[PULSAR] L.6 V3 DockSync hatch-XT poll ARMED ({}ms window @ {}ms interval)"sv,
                        kHatchPollDurationMs, kHatchPollIntervalMs);
                    // Phase 3i: mark this peer as pilot for current dock -
                    // triggers station-XT capture+broadcast on cockpit→station.
                    g_localPilotForCurrentDock.store(true, std::memory_order_release);
                    g_stationXTBroadcastSent.store(false, std::memory_order_release);
                    // Phase 3i: capture pilot's current cell as cockpit cell
                    // so cell-change detection works on pilot side too.
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (player && player->parentCell) {
                        g_localCockpitCellFormID.store(player->parentCell->formID,
                                                        std::memory_order_release);
                        // Phase 3i v6: snapshot pre-dock populated kDOOR bases.
                        // Discriminator for FindLocalHatchInCell - newly
                        // populated kDOOR (= dock hatch) is what we pick.
                        SnapshotPreDockPopulatedHatches(player->parentCell);
                    }
                }
            }

            // Dump player ship docking-component state at the event moment.
            // The "ship A" payload pointer should be the player ship at fully-
            // docked sites; we still cross-reference against PlayerCharacter to
            // verify our resolution path matches the engine's payload.
            __try {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    auto* ship = player->GetSpaceship(true);
                    if (ship) {
                        LogShipComponentState("PLAYER-SHIP", ship);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            // Also dump the shipA from payload (in case it differs from
            // PlayerCharacter::GetSpaceship - could be the dock-partner side).
            __try {
                const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(evt);
                const RE::TESObjectREFR* shipA =
                    *reinterpret_cast<const RE::TESObjectREFR* const*>(base + 0x00);
                if (shipA) {
                    LogShipComponentState("PAYLOAD-SHIP-A", shipA);
                }
                const RE::TESObjectREFR* shipB =
                    *reinterpret_cast<const RE::TESObjectREFR* const*>(base + 0x08);
                if (shipB) {
                    LogShipComponentState("PAYLOAD-SHIP-B", shipB);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    static DockEventSink g_dockSink;

    // Phase 3e: TakeOffEvent sink - fires when local engine runs the takeoff
    // state machine. Both ground takeoff (planet → space) AND station undock
    // fire this. We broadcast PKT_DOCK_UNDOCKED unconditionally; the receiver
    // checks if it has applied dock-state and cleans up. Idempotent.
    class TakeOffEventSink : public RE::BSTEventSink<RE::Spaceship::TakeOffEvent> {
    public:
        std::atomic<int> fireCount{ 0 };

        RE::BSEventNotifyControl ProcessEvent(
            const RE::Spaceship::TakeOffEvent& /*e*/,
            RE::BSTEventSource<RE::Spaceship::TakeOffEvent>* /*src*/) override
        {
            const int n = fireCount.fetch_add(1) + 1;
            PLOG_INFO("[PULSAR] L.6 V3 DockSync TakeOff sink #{} - broadcasting undock"sv, n);

            auto& net = Pulsar::Network::GetSingleton();
            if (!net.IsConnected()) return RE::BSEventNotifyControl::kContinue;

            std::uint32_t shipFormID = 0;
            __try {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    auto* ship = player->GetSpaceship(true);
                    if (ship) shipFormID = ship->formID;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            Pulsar::Network::DockUndockedData payload{};
            payload.shipFormID      = shipFormID;
            payload.hatchBaseFormID = kHatchBaseFormID;

            std::uint8_t buf[64]{};
            Pulsar::Network::PacketWriter w(
                Pulsar::Network::PKT_DOCK_UNDOCKED, buf, sizeof(buf));
            if (w.Write(payload)) {
                net.Send(buf, w.Size(), /*reliable*/ true);
                PLOG_INFO(
                    "[PULSAR] L.6 V3 DockSync ⇒ UNDOCK BROADCAST ship=0x{:08X}"sv,
                    shipFormID);
                NASLOG("L.6 V3 DockSync UNDOCK BROADCAST ship=0x%08X", shipFormID);
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    static TakeOffEventSink g_takeOffSink;

    // Phase 3e - clear our applied dock state on the local cockpit hatch.
    // Inverse of ApplyHatchExtraTeleport: clear configured bits, call
    // EnableForDock(0) to disable kPersistent + clear inner XT pointer.
    static bool ClearHatchDockState(RE::TESObjectREFR* a_hatch)
    {
        if (!a_hatch) return false;

        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            auto* pUnk18 = reinterpret_cast<std::uint64_t*>(pb + 0x18);
            auto* pFormFlags = reinterpret_cast<std::uint32_t*>(pb + 0x20);
            auto* p2C = reinterpret_cast<std::uint16_t*>(pb + 0x2C);

            // Inverse of Phase 3c: clear the "configured by docking" bits.
            *pFormFlags &= ~0x200u;       // formFlags bit 9 - clear "set up by dock"
            *p2C        &= ~static_cast<std::uint16_t>(0x40);  // formChangeFlags bit 6 - clear XT-configured
            // Also keep the previously-cleared skip bits cleared.
            *pUnk18     &= ~0x200ull;
            *pFormFlags &= ~0x100u;
            *p2C        &= 0xBFFF;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ClearHatch: bit-clear SEH"sv);
            return false;
        }

        // Clear the ExtraTeleport's inner-data ptr so the dispatcher's gate
        // (`*[xt+0x18] != 0`) returns false → activate is not eligible.
        __try {
            auto getFn = ResolveGetExtraByType();
            auto* pb = reinterpret_cast<std::uint8_t*>(a_hatch);
            void* extraList = *reinterpret_cast<void**>(pb + 0xC8);
            if (extraList && getFn) {
                void* xt = getFn(extraList, kExtraType_Teleport);
                if (xt) {
                    *reinterpret_cast<std::uint64_t*>(
                        reinterpret_cast<std::uint8_t*>(xt) + 0x18) = 0;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ClearHatch: xt-clear SEH"sv);
        }

        // Engine "disable for dock" - mirrors EnableForDock(refr, 1) at apply.
        __try {
            using EnableForDockFn = void (__fastcall*)(RE::TESObjectREFR*, char);
            auto enableFn = reinterpret_cast<EnableForDockFn>(
                REL::Offset{ kRVA_EnableForDock }.address());
            if (enableFn) enableFn(a_hatch, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync ClearHatch: EnableForDock(0) SEH"sv);
        }

        return true;
    }

    static void TryRegister()
    {
        // DockEvent sink (Phase 1+).
        auto getFn = reinterpret_cast<GetDockSrcFn>(
            REL::Offset{ kRVA_GetDockSrc }.address());

        void* raw = nullptr;
        __try { raw = getFn(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { raw = nullptr; }

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync: DockEvent GetEventSrc=0x{:X} -> singleton 0x{:X}"sv,
            reinterpret_cast<std::uintptr_t>(getFn),
            reinterpret_cast<std::uintptr_t>(raw));

        if (raw) {
            auto* src = reinterpret_cast<RE::BSTEventSource<RE::Spaceship::DockEvent>*>(raw);
            __try { src->RegisterSink(&g_dockSink); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] L.6 V3 DockSync: DockEvent RegisterSink threw"sv);
            }
            PLOG_INFO("[PULSAR] L.6 V3 DockSync: payload-reading DockEvent sink registered"sv);
        } else {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync: DockEvent source null"sv);
        }

        // TakeOffEvent sink (Phase 3e - undock detection).
        auto getToFn = reinterpret_cast<GetDockSrcFn>(
            REL::Offset{ kRVA_GetTakeOffSrc }.address());

        void* toRaw = nullptr;
        __try { toRaw = getToFn(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { toRaw = nullptr; }

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync: TakeOff GetEventSrc=0x{:X} -> singleton 0x{:X}"sv,
            reinterpret_cast<std::uintptr_t>(getToFn),
            reinterpret_cast<std::uintptr_t>(toRaw));

        // TEMPORARILY DISABLED 2026-05-02 - investigating ship-sync regression.
        // The TakeOff sink may be interfering with FlightSync L.3 V3 vt[8]
        // hook on bhkSpaceship somehow. Re-enable once root cause known.
        if (false && toRaw) {
            auto* toSrc = reinterpret_cast<RE::BSTEventSource<RE::Spaceship::TakeOffEvent>*>(toRaw);
            __try { toSrc->RegisterSink(&g_takeOffSink); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] L.6 V3 DockSync: TakeOff RegisterSink threw"sv);
            }
            PLOG_INFO("[PULSAR] L.6 V3 DockSync: TakeOff sink registered (for undock)"sv);
        } else {
            PLOG_INFO("[PULSAR] L.6 V3 DockSync: TakeOff sink DISABLED (ship-sync investigation)"sv);
        }

        NASLOG("L.6 V3 DockSync: sinks registered");
    }

}  // namespace

void Init()
{
    PLOG_INFO("[PULSAR] L.6 V3 DockSync init (Phase 3b: ExtraTeleport sync)"sv);
    NASLOG("L.6 V3 DockSync: init");
    TryRegister();
}

// Phase 3i v17: forward-bridge gate. Block client's cockpit-hatch activate
// while we're waiting for host's forward save-bridge to arrive - otherwise
// client teleports to LOCAL procedural ship instance (not host's), sees a
// different ship at the dock, etc. Once save loads, in-memory state resets
// and the wait flag is cleared, so subsequent activate goes through normally.
bool TryHandleCockpitSideHatchActivate(RE::TESObjectREFR* a_self)
{
    if (!a_self) return false;
    std::uint32_t selfID = 0;
    __try { selfID = a_self->formID; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!IsAppliedCockpitHatch(selfID)) return false;

    if (!g_clientWaitingForForwardBridge.load(std::memory_order_acquire)) {
        return false;  // not waiting → engine native
    }

    // Timeout failsafe: if save hasn't arrived in 30s something went wrong,
    // unblock so user isn't stuck.
    const std::uint64_t now = ::GetTickCount64();
    const std::uint64_t expectedAt =
        g_forwardBridgeExpectedAtMs.load(std::memory_order_acquire);
    if (now - expectedAt > kForwardBridgeTimeoutMs) {
        g_clientWaitingForForwardBridge.store(false, std::memory_order_release);
        PLOG_WARN(
            "[PULSAR] L.6 V3 DockSync forward-bridge: timeout after {}ms - "
            "unblocking cockpit-hatch activate (save did not arrive)"sv,
            now - expectedAt);
        return false;
    }

    // Throttled log so player sees feedback if they keep pressing E.
    static std::atomic<std::uint64_t> g_lastBlockLogMs{ 0 };
    const std::uint64_t prevLog = g_lastBlockLogMs.load(std::memory_order_acquire);
    if ((now - prevLog) > 2000) {
        g_lastBlockLogMs.store(now, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync forward-bridge: blocking cockpit-hatch activate "
            "(hatch=0x{:08X}) until host save arrives - waited {}ms so far"sv,
            selfID, now - expectedAt);
    }
    return true;  // suppress engine activate
}

bool TryHandleStationSideHatchActivate(RE::TESObjectREFR* a_self)
{
    // Phase 3i v14 (2026-05-03): E-press override on station-hatch.
    // VANILLA cockpit cell (Frontier 0x05100136): NO intercept, engine
    //   teleport works natively (destDoorRef + cell are vanilla forms).
    // PROCEDURAL FF-prefix cell (Razorleaf, Guardian, custom): override E.
    //   Bypass engine activate, MoveTo player to PlayerCharacter::GetSpaceship()
    //   via direct engine MoveTo (RVA 0x14191EFB0, cell-load aware).
    //   Hypothesis: engine handles "MoveTo a docked ship → enter cockpit"
    //   semantically. If player ends up in vacuum, fallback strategy needed.
    if (!a_self) return false;
    std::uint32_t selfID = 0;
    __try { selfID = a_self->formID; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // Phase 3i v14.1: check set (re-apply may have tracked multiple hatches).
    if (!IsAppliedStationHatch(selfID)) return false;

    const std::uint32_t cockpitCell =
        g_localCockpitCellFormID.load(std::memory_order_acquire);
    const bool cockpitIsVanilla =
        (cockpitCell != 0) && ((cockpitCell & 0xFF000000u) != 0xFF000000u);

    if (cockpitIsVanilla) {
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync station-side activate: vanilla cockpit "
            "0x{:08X} - letting engine teleport natively"sv, cockpitCell);
        return false;  // engine native handles vanilla
    }

    // Procedural cell - KNOWN HARD LIMIT. Empirically tested v12-v14.3:
    // MoveTo to clone fails (NPCSync renders clone in player cell, not host's)
    // MoveTo to broadcast ship REFR fails (procedural FormID is host-local)
    // MoveTo to local cockpit hatch FormID fails (cell evicted, engine
    //   cell-streamer can't reload procedural cell without spawn context)
    // Engine native activate spawns fresh cockpit instance (wrong ship)
    //   or crashes mid cell-load
    //
    // No persistent cross-peer anchor exists for custom shipyard cockpits.
    // Resolution requires either (a) save-load bridge per dock cycle,
    // (b) cell-pin via direct engine API to keep cockpit loaded during
    // station visit, or (c) BSGalaxy::DockingChildState ComponentDB
    // pre-registration (complex factory RE). All deferred.
    //
    // Fall through silently - engine activate may do nothing or land player
    // in wrong cell, but at least no spam from our intercept loop.
    static std::atomic<std::uint64_t> g_lastProcLogMs{ 0 };
    const std::uint64_t now = ::GetTickCount64();
    const std::uint64_t prev = g_lastProcLogMs.load(std::memory_order_acquire);
    if ((now - prev) > 10000) {
        g_lastProcLogMs.store(now, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync station-side activate: procedural cockpit "
            "0x{:08X} - no return path available (architectural limit)"sv,
            cockpitCell);
    }
    return false;
}

void OnLocalSaveLoaded()
{
    // Phase 3i v17: clear forward-bridge wait flag - save-load means we now
    // have host's exact state, so further hatch activations should run native.
    if (g_clientWaitingForForwardBridge.exchange(false, std::memory_order_acq_rel)) {
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync forward-bridge: save loaded, clearing "
            "cockpit-hatch wait gate"sv);
    }
}

void OnRemoteTakeOff()
{
    // Phase 3h (2026-05-02): replaces the broken TakeOff sink. SpaceSync's
    // OnPacketReceived calls this when kind=TakeOff (kind=0) arrives via
    // existing PKT_SPACE_TRANSITION pipe. Cleans up local hatch state if
    // we have an apply on file.
    if (!g_dockAppliedLocally.load(std::memory_order_acquire)) return;

    PLOG_INFO("[PULSAR] L.6 V3 DockSync: peer TakeOff received - cleaning local hatch state"sv);
    NASLOG("L.6 V3 DockSync: peer TakeOff -> cleanup");

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player) {
        if (auto* cell = player->parentCell) {
            RE::TESObjectREFR* hatch = FindLocalHatchInCell(cell);
            if (hatch) ClearHatchDockState(hatch);
        }
    }

    g_dockAppliedLocally.store(false, std::memory_order_release);
    g_xtPersistArmed.store(false, std::memory_order_release);
    g_stationSideApplied.store(false, std::memory_order_release);
    g_stationSideApplyDueMs.store(0, std::memory_order_release);
    g_appliedStationSideHatchFormID.store(0, std::memory_order_release);
    ClearAppliedStationHatchSet();
    g_localCockpitCellFormID.store(0, std::memory_order_release);
    g_localCockpitHatchFormID.store(0, std::memory_order_release);
    g_lastDockedShipFormID.store(0, std::memory_order_release);
    // Phase 3i: reset pilot/station-XT state.
    g_localPilotForCurrentDock.store(false, std::memory_order_release);
    g_stationXTBroadcastSent.store(false, std::memory_order_release);
    g_haveReceivedStationXT.store(false, std::memory_order_release);
    // Phase 3i v6: invalidate pre-dock snapshot - stale after undock.
    ClearPreDockSnapshot();
    g_remoteDockHatchBase.store(0, std::memory_order_release);
    // Phase 3i v7: restore landing-hatch inner-XT pointers we zeroed on apply.
    RestoreSuppressedLandingHatches();
}

void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_length)
{
    if (a_length < 1) return;
    const std::uint8_t pktType = a_data[0];

    auto& net = Pulsar::Network::GetSingleton();

    // PKT_DOCK_FORWARD_BRIDGE_EXPECTED (0x8E) - Phase 3i v17/v18. Host signals
    // "save-bridge incoming, suppress your auto-teleport". Originally also
    // blocked cockpit-hatch activate during forward-bridge - v18 removed
    // forward-bridge so cockpit gate is dormant. Suppression of ResetTeleport
    // is still needed for return-bridge.
    if (pktType == Pulsar::Network::PKT_DOCK_FORWARD_BRIDGE_EXPECTED) {
        // Suppress ResetTeleport on next save-load - auto-teleport-to-host
        // pushes client into NULL cell when host's pos is in a procedural FF
        // cell that can't cell-stream standalone.
        net.SuppressNextResetTeleport();
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⇐ BRIDGE-EXPECTED - suppressing auto-teleport, "
            "waiting for host save"sv);
        NASLOG("L.6 V3 DockSync BRIDGE-EXPECTED received");
        return;
    }

    // PKT_DOCK_STATION_XT (0x8D) - Phase 3i pilot broadcasts station-XT bytes.
    if (pktType == Pulsar::Network::PKT_DOCK_STATION_XT) {
        Pulsar::Network::PacketReader reader(a_data, a_length);
        if (!reader.IsValid()) return;
        Pulsar::Network::DockStationXTData payload{};
        if (!reader.Read(payload)) return;
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⇐ STATION-XT RECV hatch=0x{:08X} base=0x{:08X} "
            "flags=0x{:X} pos=({:.2f},{:.2f},{:.2f}) rot=({:.2f},{:.2f},{:.2f}) destFlag=0x{:02X}"sv,
            payload.stationHatchFormID, payload.hatchBaseFormID,
            payload.flags,
            payload.destPosX, payload.destPosY, payload.destPosZ,
            payload.destRotX, payload.destRotY, payload.destRotZ,
            payload.destFlag);
        NASLOG("L.6 V3 DockSync STATION-XT RECV hatch=0x%08X", payload.stationHatchFormID);
        // Store for ApplyStationSideHatchBypass to use.
        {
            std::lock_guard lock(g_receivedStationXTMutex);
            g_receivedStationXT = payload;
        }
        g_haveReceivedStationXT.store(true, std::memory_order_release);

        // Phase 3i v9: race-fix - if station-side bypass was already applied
        // with FALLBACK-FRONTIER (broadcast arrived too late), reset the
        // applied flag so TickStationSideHatchSetup re-applies on next tick
        // using the now-received pilot bytes. The station-side cell-crossing
        // detection is idempotent; re-apply just rewrites the inner XT bytes
        // on the same hatch.
        if (g_stationSideApplied.load(std::memory_order_acquire)) {
            g_stationSideApplied.store(false, std::memory_order_release);
            // Re-arm apply-due timer immediately so re-apply runs next tick.
            g_stationSideApplyDueMs.store(::GetTickCount64(),
                                            std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync station-XT received post-apply - "
                "re-arming station-side bypass to use pilot-broadcast bytes"sv);
        }
        return;
    }

    // PKT_DOCK_UNDOCKED (0x8C) - Phase 3e undock cleanup.
    if (pktType == Pulsar::Network::PKT_DOCK_UNDOCKED) {
        Pulsar::Network::PacketReader reader(a_data, a_length);
        if (!reader.IsValid()) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync UNDOCK RECV: invalid packet"sv);
            return;
        }
        Pulsar::Network::DockUndockedData payload{};
        if (!reader.Read(payload)) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync UNDOCK RECV: short read"sv);
            return;
        }
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⇐ UNDOCK RECV ship=0x{:08X} hatchBase=0x{:08X}"sv,
            payload.shipFormID, payload.hatchBaseFormID);
        NASLOG("L.6 V3 DockSync UNDOCK RECV ship=0x%08X", payload.shipFormID);

        // Idempotent - only act if we have applied dock state.
        if (!g_dockAppliedLocally.load(std::memory_order_acquire)) {
            PLOG_INFO("[PULSAR] L.6 V3 DockSync UNDOCK: nothing to clean (no apply on file)"sv);
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* cell = player->parentCell;
        if (!cell) return;

        RE::TESObjectREFR* hatch = FindLocalHatchInCell(cell);
        if (!hatch) {
            PLOG_INFO("[PULSAR] L.6 V3 DockSync UNDOCK: no hatch in current cell - clearing flag only"sv);
            g_dockAppliedLocally.store(false, std::memory_order_release);
            g_xtPersistArmed.store(false, std::memory_order_release);
            return;
        }

        const bool ok = ClearHatchDockState(hatch);
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⚡ UNDOCK CLEANUP hatch=0x{:08X} result={}"sv,
            hatch->formID, ok ? "ok" : "partial");
        NASLOG("L.6 V3 DockSync UNDOCK CLEANUP hatch=0x%08X", hatch->formID);

        g_dockAppliedLocally.store(false, std::memory_order_release);
        g_xtPersistArmed.store(false, std::memory_order_release);
        g_stationSideApplied.store(false, std::memory_order_release);
        g_stationSideApplyDueMs.store(0, std::memory_order_release);
        g_appliedStationSideHatchFormID.store(0, std::memory_order_release);
        g_localCockpitCellFormID.store(0, std::memory_order_release);
        g_localCockpitHatchFormID.store(0, std::memory_order_release);
        return;
    }

    // PKT_DOCK_EXTRATELEPORT (0x8B) - Phase 3b primary apply path.
    if (pktType == Pulsar::Network::PKT_DOCK_EXTRATELEPORT) {
        Pulsar::Network::PacketReader reader(a_data, a_length);
        if (!reader.IsValid()) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync XT RECV: invalid packet"sv);
            return;
        }
        Pulsar::Network::DockExtraTeleportData payload{};
        if (!reader.Read(payload)) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync XT RECV: short read"sv);
            return;
        }
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⇐ XT RECV hatchBase=0x{:08X} cell=0x{:08X} "
            "dest=0x{:08X} pos=({:.1f},{:.1f},{:.1f}) rot=({:.2f},{:.2f},{:.2f}) "
            "flags=0x{:X} destFlag=0x{:02X} - Phase 3b apply armed"sv,
            payload.hatchBaseFormID, payload.cockpitCellFormID,
            payload.destDoorFormID,
            payload.destPosX, payload.destPosY, payload.destPosZ,
            payload.destRotX, payload.destRotY, payload.destRotZ,
            payload.flags, payload.destFlag);
        NASLOG("L.6 V3 DockSync XT RECV hatch=0x%08X dest=0x%08X",
               payload.hatchBaseFormID, payload.destDoorFormID);

        // Bidirectional: any non-pilot side applies. The pilot side that
        // broadcast doesn't receive its own packet (ENet doesn't reflect to
        // sender), so no role-check needed. If we ARE currently the pilot
        // somehow (engine state mismatch), our local hatch likely already
        // has working ExtraTeleport - the apply is idempotent (existing
        // bits stay set, no harm).
        // Phase 3i: hatchBaseFormID sanity is now diagnostic-only - non-Frontier
        // ships use different base forms. Finder is ship-agnostic.
        if (payload.hatchBaseFormID != kHatchBaseFormID) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync XT RECV: non-Frontier hatch base 0x{:08X} "
                "(Frontier=0x9CF4) - proceeding with ship-agnostic finder"sv,
                payload.hatchBaseFormID);
        }
        if (false) {
            PLOG_WARN("[PULSAR] L.6 V3 DockSync XT RECV: unexpected hatch base 0x{:08X}"sv,
                      payload.hatchBaseFormID);
            return;
        }

        {
            std::lock_guard lock(g_pendingXT.m);
            g_pendingXT.payload = payload;
        }
        // Phase 3i v6: hint to FindLocalHatchInCell so client picks the
        // kDOOR with matching base (host's identified dock hatch).
        g_remoteDockHatchBase.store(payload.hatchBaseFormID, std::memory_order_release);
        g_pendingXT.dueTick.store(::GetTickCount64() + kXTApplyDeferMs,
                                   std::memory_order_release);
        g_pendingXT.armed.store(true, std::memory_order_release);
        return;
    }

    // PKT_DOCK_FINISHED (0x8A) - informational; Phase 3a v3 currently SKIPs
    // apply because dock REFR is a stub on client (station cell not loaded).
    // Kept for diagnostics + future strategies. Phase 3b's XT path bypasses
    // this completely.
    Pulsar::Network::PacketReader reader(a_data, a_length);
    if (!reader.IsValid()) {
        PLOG_WARN("[PULSAR] L.6 V3 DockSync RECV: invalid packet ({} bytes)"sv, a_length);
        return;
    }

    Pulsar::Network::DockFinishedData payload{};
    if (!reader.Read(payload)) {
        PLOG_WARN("[PULSAR] L.6 V3 DockSync RECV: short read"sv);
        return;
    }

    PLOG_INFO(
        "[PULSAR] L.6 V3 DockSync ⇐ RECV stage={} ship=0x{:08X} "
        "dock=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) - Phase 3a/3b dual-armed"sv,
        payload.stage, payload.shipFormID, payload.dockRefrFormID,
        payload.posX, payload.posY, payload.posZ);
    NASLOG(
        "L.6 V3 DockSync RECV stage=%d ship=0x%08X dock=0x%08X pos=(%.0f,%.0f,%.0f)",
        payload.stage, payload.shipFormID, payload.dockRefrFormID,
        payload.posX, payload.posY, payload.posZ);

    // Bidirectional: PKT_DOCK_FINISHED is informational logging mostly
    // (Phase 3a apply currently SKIPs because dock REFR data3D is FAKE on
    // non-pilot side - same on both peers). Keep the receive logging path.
    if (payload.shipFormID == 0 || payload.dockRefrFormID == 0) {
        PLOG_WARN("[PULSAR] L.6 V3 DockSync RECV: missing FormIDs - skip apply"sv);
        return;
    }

    g_pendingApply.shipFormID.store(payload.shipFormID, std::memory_order_release);
    // Phase 3i v14.2: persist ship FormID for E-override return path.
    g_lastDockedShipFormID.store(payload.shipFormID, std::memory_order_release);
    g_pendingApply.dockRefrFormID.store(payload.dockRefrFormID, std::memory_order_release);
    g_pendingApply.stage.store(payload.stage, std::memory_order_release);
    g_pendingApply.dueTick.store(::GetTickCount64() + kApplyDeferMs,
                                  std::memory_order_release);
    g_pendingApply.armed.store(true, std::memory_order_release);
}

// Phase 3i v16.1 (2026-05-03) - HOST-side return-to-procedural-cockpit save-bridge.
// v16.0 used g_localCockpitCellFormID to identify "cockpit cell"; that variable
// gets corrupted by the cockpit-post-apply receiver path on host (it stores
// player.parentCell unconditionally, so when host is in station and receives
// XT for client's dock, g_localCockpitCellFormID becomes the station cell).
// v16.1 ditches the cockpit-ID dependency and uses cell-FormID FF-prefix as
// the discriminator: procedural ship interiors and procedural POIs both have
// FF prefix, but the user's flow (re-dock to ship from station) is the only
// flow that produces "FF → vanilla → FF" cell transitions on host.
//
// Engine re-instances procedural cells on re-entry, so the cell ID may DIFFER
// between the original cockpit FF cell and the post-station re-entry FF cell.
// We accept this - strict same-ID match would miss the legitimate return.
//
// State machine (per-tick):
//   STATE 0: idle (most ticks, no transition)
//   STATE 1: prev was FF cell, cur is vanilla cell → "left FF for vanilla", arm
//   STATE 2: prev was vanilla cell, cur is FF cell, AND armed → FIRE
//
// Dedup via 30s window (kDockReturnResyncDedupMs) prevents rapid re-fire and
// also acts as a guard against POI false-positives (random POI re-visits would
// debounce on second trigger).
static void TickHostReturnSaveResync()
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->parentCell) return;

    const std::uint32_t curCell = player->parentCell->formID;
    const std::uint32_t prevCell =
        g_hostLastSeenCell.exchange(curCell, std::memory_order_acq_rel);
    if (curCell == prevCell) return;

    const bool prevIsFF = ((prevCell & 0xFF000000u) == 0xFF000000u);
    const bool curIsFF  = ((curCell  & 0xFF000000u) == 0xFF000000u);

    // Capture player's current position (will be the "before transition" pos
    // since cell-change fires mid-load before pos commits).
    float armX = 0, armY = 0, armZ = 0;
    __try {
        armX = player->data.location.x;
        armY = player->data.location.y;
        armZ = player->data.location.z;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // STATE 1: host left FF cell for vanilla cell → arm return-flag only
    if (prevIsFF && !curIsFF) {
        g_hostJustVisitedDockStation.store(true, std::memory_order_release);

        // Phase 3i v18 (2026-05-03): forward-bridge REMOVED. v17.x tried firing
        // a save here so client would materialize in correct station/ship state.
        // Side effect: client's FF cells got loaded from host's save, then on
        // return-bridge the SECOND save-load couldn't cleanly reload procedural
        // cells → client landed in NULL cell (game menu). User-validated rule:
        // "back to v16.1 - only return-bridge, forward goes through naturally
        // (visible-ship-mismatch is acceptable trade-off)".
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync HOST left FF cell 0x{:08X} → vanilla 0x{:08X} "
            "(armPos=({:.1f},{:.1f},{:.1f})) - armed return-bridge (forward intentionally "
            "skipped per v18 design)"sv,
            prevCell, curCell, armX, armY, armZ);
        return;
    }

    // STATE 2: host returned to ANY FF cell after visiting vanilla → arm return (deferred)
    if (!prevIsFF && curIsFF) {
        const bool visited = g_hostJustVisitedDockStation.exchange(
            false, std::memory_order_acq_rel);
        if (!visited) return;

        g_pendingBridge.store(static_cast<std::uint8_t>(PendingBridge::kReturn),
                              std::memory_order_release);
        g_pendingBridgePrevCell.store(prevCell, std::memory_order_release);
        g_pendingBridgeCurCell.store(curCell, std::memory_order_release);
        g_pendingBridgeArmedMs.store(::GetTickCount64(), std::memory_order_release);
        g_pendingBridgeArmedX.store(armX, std::memory_order_release);
        g_pendingBridgeArmedY.store(armY, std::memory_order_release);
        g_pendingBridgeArmedZ.store(armZ, std::memory_order_release);

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync HOST returned to FF cockpit 0x{:08X} from vanilla "
            "0x{:08X} (armPos=({:.1f},{:.1f},{:.1f})) - ARMED return-bridge (deferred)"sv,
            curCell, prevCell, armX, armY, armZ);
    }
}

// Phase 3i v17.2: deferred bridge fire. Polls for "cell settled" condition
// and triggers BeginHostQuicksaveTransfer once host's engine is stable.
// Without this, save snapshot captures partial in-load state with stale player
// position (host shows new cell ID while still being at old physical pos).
static void TickPendingBridgeFire()
{
    const std::uint8_t pending =
        g_pendingBridge.load(std::memory_order_acquire);
    if (pending == static_cast<std::uint8_t>(PendingBridge::kNone)) return;

    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected() || !net.IsHost()) {
        // Lost host status mid-defer; abort.
        g_pendingBridge.store(static_cast<std::uint8_t>(PendingBridge::kNone),
                              std::memory_order_release);
        return;
    }

    // Wait for cell-streaming to fully settle.
    if (Pulsar::Cell3DAttach::IsQuiet() || Pulsar::Cell3DAttach::WasInLoadMenu()) {
        return;  // still loading, retry next tick
    }

    // Min grace period after arming.
    const std::uint64_t now = ::GetTickCount64();
    const std::uint64_t armedAt = g_pendingBridgeArmedMs.load(std::memory_order_acquire);
    const std::uint64_t elapsed = now - armedAt;
    if (elapsed < kBridgeSettleMinMs) {
        return;
    }

    // Sanity: player + cell present
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->parentCell) {
        g_pendingBridge.store(static_cast<std::uint8_t>(PendingBridge::kNone),
                              std::memory_order_release);
        return;
    }
    const std::uint32_t curCell = player->parentCell->formID;
    const std::uint32_t armedCur = g_pendingBridgeCurCell.load(std::memory_order_acquire);
    if (curCell != armedCur) {
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync bridge-defer: player cell changed from armed "
            "0x{:08X} to 0x{:08X} - aborting"sv, armedCur, curCell);
        g_pendingBridge.store(static_cast<std::uint8_t>(PendingBridge::kNone),
                              std::memory_order_release);
        return;
    }

    // Phase 3i v17.3: position-commit check. Engine commits player.location
    // ASYNC after cell load - even after loadMenu CLOSED + IsQuiet, position
    // can still be the OLD cell's coords for ~1-2 seconds. Wait until player
    // has moved a real distance from arm-time pos, OR max-grace expired.
    float curX = 0, curY = 0, curZ = 0;
    __try {
        curX = player->data.location.x;
        curY = player->data.location.y;
        curZ = player->data.location.z;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const float armX = g_pendingBridgeArmedX.load(std::memory_order_acquire);
    const float armY = g_pendingBridgeArmedY.load(std::memory_order_acquire);
    const float armZ = g_pendingBridgeArmedZ.load(std::memory_order_acquire);
    const float dx = curX - armX, dy = curY - armY, dz = curZ - armZ;
    const float dist2 = dx*dx + dy*dy + dz*dz;
    const bool posCommitted = (dist2 > kBridgePosCommitDist2);
    if (!posCommitted && elapsed < kBridgeSettleMaxMs) {
        // Throttled debug log
        static std::atomic<std::uint64_t> g_lastWaitLog{ 0 };
        const std::uint64_t prev = g_lastWaitLog.load(std::memory_order_acquire);
        if ((now - prev) > 500) {
            g_lastWaitLog.store(now, std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync bridge-defer: waiting for pos commit "
                "(elapsed {}ms, curPos=({:.1f},{:.1f},{:.1f}), armPos=({:.1f},{:.1f},{:.1f}), "
                "dist²={:.1f})"sv,
                elapsed, curX, curY, curZ, armX, armY, armZ, dist2);
        }
        return;
    }
    if (!posCommitted) {
        PLOG_WARN(
            "[PULSAR] L.6 V3 DockSync bridge-defer: max-grace {}ms reached without "
            "pos commit (dist²={:.1f}) - firing anyway"sv, kBridgeSettleMaxMs, dist2);
    } else {
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync bridge-defer: pos committed "
            "(({:.1f},{:.1f},{:.1f}) → ({:.1f},{:.1f},{:.1f}), dist²={:.1f}, elapsed {}ms)"sv,
            armX, armY, armZ, curX, curY, curZ, dist2, elapsed);
    }

    // Clear pending state before firing so retries can't double-fire.
    const std::uint32_t prevCell = g_pendingBridgePrevCell.load(std::memory_order_acquire);
    g_pendingBridge.store(static_cast<std::uint8_t>(PendingBridge::kNone),
                          std::memory_order_release);

    if (pending == static_cast<std::uint8_t>(PendingBridge::kForward)) {
        // Forward dedup
        const std::uint64_t lastFwd =
            g_lastForwardBridgeFireMs.load(std::memory_order_acquire);
        if (now - lastFwd < kForwardBridgeDedupMs) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync bridge-defer: forward fire skipped (dedup)"sv);
            return;
        }
        g_lastForwardBridgeFireMs.store(now, std::memory_order_release);

        // Broadcast EXPECTED first - client will gate cockpit-hatch activate.
        std::uint8_t buf[16]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_DOCK_FORWARD_BRIDGE_EXPECTED, buf, sizeof(buf));
        std::uint64_t ts = now;
        w.Write(ts);
        net.Send(buf, w.Size(), /*reliable*/ true);

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync FIRING forward-bridge save (settled in cell "
            "0x{:08X}, was 0x{:08X}, armed {}ms ago)"sv,
            curCell, prevCell, now - armedAt);
        NASLOG("L.6 V3 DockSync HOST-FORWARD-SAVE-BRIDGE-FIRE cur=0x%08X", curCell);
        net.BeginHostQuicksaveTransfer();
        return;
    }

    if (pending == static_cast<std::uint8_t>(PendingBridge::kReturn)) {
        // Return dedup
        const std::uint64_t lastRet =
            g_lastDockReturnResyncMs.load(std::memory_order_acquire);
        if (now - lastRet < kDockReturnResyncDedupMs) {
            PLOG_INFO(
                "[PULSAR] L.6 V3 DockSync bridge-defer: return fire skipped (dedup)"sv);
            return;
        }
        g_lastDockReturnResyncMs.store(now, std::memory_order_release);

        // Phase 3i v17.4: also broadcast EXPECTED for return so client
        // suppresses ResetTeleport. Without this, the auto-teleport-to-host
        // fires after save load and pushes client into NULL cell (= game menu)
        // because host's FF cockpit cell can't be cell-streamed standalone.
        // Reuse the FORWARD_BRIDGE_EXPECTED packet - semantically the client-
        // side handling is identical (suppress + gate cockpit hatch which is
        // a no-op on return because client isn't in cockpit).
        std::uint8_t buf[16]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_DOCK_FORWARD_BRIDGE_EXPECTED, buf, sizeof(buf));
        std::uint64_t ts = now;
        w.Write(ts);
        net.Send(buf, w.Size(), /*reliable*/ true);

        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync FIRING return-bridge save (settled in cell "
            "0x{:08X}, was 0x{:08X}, armed {}ms ago) + sent EXPECTED to client"sv,
            curCell, prevCell, now - armedAt);
        NASLOG("L.6 V3 DockSync HOST-RETURN-SAVE-BRIDGE-FIRE cur=0x%08X", curCell);
        net.BeginHostQuicksaveTransfer();
        return;
    }
}

void Tick()
{
    // Phase 3i v5: unconditional door-dump (throttled to once per cell-FormID
    // change) so we get data on BOTH peers - host and client - regardless of
    // dock-state. Lets us identify dock-vs-landing-hatch discriminator.
    if (auto* p = RE::PlayerCharacter::GetSingleton()) {
        if (auto* c = p->parentCell) DumpAllDoorsInCell(c);
    }

    // Phase 3i v16: HOST-side return-to-procedural-cockpit save-bridge.
    // Inverted from v15 (which fired on forward dock and crashed client). v16
    // fires when host walks BACK from station into procedural cockpit; client
    // is still in vanilla station with no FF state, so save-load applies cleanly.
    // v17.2: cell-change detection + arm pending bridge.
    TickHostReturnSaveResync();
    // Phase 3i v17.2: deferred fire - wait for cell to settle before snapshot.
    TickPendingBridgeFire();
    // Phase 3b host-side: poll for hatch ExtraTeleport population.
    TickHatchTeleportCapture();
    // Phase 3f client-side: when player crosses cockpit→station, apply
    // bypass to the station-side hatch so they can walk back to the ship.
    TickStationSideHatchSetup();
    // Phase 3f v5 client-side: drain pending return-to-ship dispatch.
    // Dispatching from game-thread Tick avoids the REL::ID(0) FATAL we hit
    // when calling from inside the activate-hook context.
    if (g_pendingReturnArmed.load(std::memory_order_acquire)) {
        const std::uint64_t now3 = ::GetTickCount64();
        const std::uint64_t due3 = g_pendingReturnDueMs.load(std::memory_order_acquire);
        if (now3 >= due3) {
            const std::uint32_t targetID =
                g_pendingReturnTargetFormID.load(std::memory_order_acquire);
            const bool useClone =
                g_pendingReturnUseClone.exchange(false, std::memory_order_acq_rel);
            g_pendingReturnArmed.store(false, std::memory_order_release);

            RE::TESObjectREFR* target = nullptr;
            if (useClone) {
                // Clones are NOT in TESForm global table - use the manager.
                auto* a = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
                target = static_cast<RE::TESObjectREFR*>(a);
            } else {
                __try {
                    auto* form = RE::TESForm::LookupByID(targetID);
                    if (form) target = form->As<RE::TESObjectREFR>();
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

            if (target) {
                // Phase 3i v13 (2026-05-03): clone-settle check before MoveTo.
                // If clone is in same cell as player (still in station, NPCSync
                // propagation lag), MoveTo is no-op-fail. Re-arm with delay
                // up to N retries until clone settles in a different cell.
                bool cloneSettled = true;
                if (useClone) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (player && player->parentCell && target->parentCell) {
                        cloneSettled = (target->parentCell != player->parentCell);
                    }
                }
                if (!cloneSettled) {
                    const std::uint32_t retries =
                        g_pendingReturnRetries.fetch_add(1, std::memory_order_acq_rel);
                    if (retries < kReturnMaxRetries) {
                        // Re-arm for next retry tick.
                        g_pendingReturnTargetFormID.store(targetID, std::memory_order_release);
                        g_pendingReturnUseClone.store(true, std::memory_order_release);
                        g_pendingReturnDueMs.store(now3 + kReturnRetryDelayMs,
                                                    std::memory_order_release);
                        g_pendingReturnArmed.store(true, std::memory_order_release);
                        if ((retries % 5) == 0) {
                            PLOG_INFO(
                                "[PULSAR] L.6 V3 DockSync return-to-ship: clone not settled "
                                "(retry {}/{} - clone still in player cell)"sv,
                                retries + 1, kReturnMaxRetries);
                        }
                        return;
                    }
                    PLOG_WARN(
                        "[PULSAR] L.6 V3 DockSync return-to-ship: timeout waiting for clone settle - attempting MoveTo anyway"sv);
                }

                const bool ok = DispatchPlayerMoveToRef(target);
                PLOG_INFO(
                    "[PULSAR] L.6 V3 DockSync return-to-ship dispatch (engine MoveTo): target=0x{:08X} result={}"sv,
                    targetID, ok ? "ok" : "failed");
                NASLOG("L.6 V3 DockSync RETURN-TO-SHIP dispatch=%s target=0x%08X",
                       ok ? "ok" : "failed", targetID);
            } else {
                PLOG_WARN(
                    "[PULSAR] L.6 V3 DockSync return-to-ship: target lookup failed for 0x{:08X}"sv,
                    targetID);
            }
        }
    }
    // Phase 3d host-side pose-log REMOVED (2026-05-02): ship's NiNode
    // m_world.translate is (0,0,0) on host even AFTER cutscene completes.
    // Ship-visual-at-dock is achieved via cell-context hierarchy, not via
    // direct REFR position. Replicating it requires cell-tree manipulation
    // deferred. See L.5 "planet rendering on cruise" for analogous arch
    // limit. The TickPoseLog + LogPoseCandidates code stays in source for
    // future reference but is not wired into Tick().
    // TickPoseLog();
    // Phase 3b client-side: apply pending captured ExtraTeleport.
    TickHatchTeleportApply();
    // Phase 3b v7 client-side: re-set bits + diagnostic log post-apply.
    TickHatchPersistence();

    if (!g_pendingApply.armed.load(std::memory_order_acquire)) return;

    const std::uint64_t now = ::GetTickCount64();
    const std::uint64_t due = g_pendingApply.dueTick.load(std::memory_order_acquire);
    if (now < due) return;

    // De-dupe - engine may fire the same dock event twice within seconds, we
    // only want to MoveTo once per dock action.
    const std::uint64_t lastApply = g_lastApplyTick.load(std::memory_order_acquire);
    if ((now - lastApply) < kApplyDedupMs) {
        g_pendingApply.armed.store(false, std::memory_order_release);
        PLOG_INFO("[PULSAR] L.6 V3 DockSync apply SKIP - dedup window"sv);
        return;
    }

    const std::uint32_t shipFormID = g_pendingApply.shipFormID.load(std::memory_order_acquire);
    const std::uint32_t dockFormID = g_pendingApply.dockRefrFormID.load(std::memory_order_acquire);
    const std::uint8_t  stage      = g_pendingApply.stage.load(std::memory_order_acquire);
    g_pendingApply.armed.store(false, std::memory_order_release);

    // Look up both REFRs via TESForm::LookupByID. Both are vanilla forms
    // (Frontier 0x0003F7E8, station-dock-port 0x001DE8BE) so cross-machine
    // FormID resolution is direct without T34 plugin lookup.
    RE::TESForm* shipForm = nullptr;
    RE::TESForm* dockForm = nullptr;
    __try {
        shipForm = RE::TESForm::LookupByID(shipFormID);
        dockForm = RE::TESForm::LookupByID(dockFormID);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PLOG_WARN("[PULSAR] L.6 V3 DockSync apply: LookupByID threw"sv);
        return;
    }

    if (!shipForm || !dockForm) {
        PLOG_WARN(
            "[PULSAR] L.6 V3 DockSync apply: FormID lookup failed "
            "(ship=0x{:08X} -> {} dock=0x{:08X} -> {})"sv,
            shipFormID, shipForm ? "ok" : "null",
            dockFormID, dockForm ? "ok" : "null");
        return;
    }

    auto* ship = shipForm->As<RE::TESObjectREFR>();
    auto* dock = dockForm->As<RE::TESObjectREFR>();
    if (!ship || !dock) {
        PLOG_WARN(
            "[PULSAR] L.6 V3 DockSync apply: As<TESObjectREFR> failed "
            "(ship={} dock={})"sv, ship ? "ok" : "null", dock ? "ok" : "null");
        return;
    }

    // Phase 3i v11 (2026-05-02): trigger engine's native dock state-machine
    // on client. RE'd: handler at RVA 0x1AEC870 takes (ignored-this, ship REFR),
    // calls inner FUN_14213D160 that sets up BSGalaxy::DockingChildState +
    // BGSDockingActivity for the ship + walks ship.extraDataList. Without
    // this, client's engine has no dock-pair state and station-hatch activate
    // either lands in a fresh new-instance cell (wrong ship) or crashes mid-
    // cell-load (read at 0xFFFFFFFFFFFFFFFF). Single call per dock event;
    // dedupe handled above.
    if (stage == 1) {
        const bool ok = TriggerDockFinishedOnClient(ship);
        PLOG_INFO(
            "[PULSAR] L.6 V3 DockSync ⚡ DockFinishedHandler dispatched "
            "ship=0x{:08X} → result={}"sv, shipFormID, ok ? "ok" : "failed");
        NASLOG("L.6 V3 DockSync DOCK-FINISHED-HANDLER ship=0x%08X result=%s",
               shipFormID, ok ? "ok" : "failed");
        g_lastApplyTick.store(now, std::memory_order_release);
    }

    // L.6 V3 Phase 3a v1 (2026-05-02): immediate MoveTo dispatch crashed
    // client at the apply moment with NULL+0x90 access violation. Root
    // cause = the destination station-dock-port REFR (e.g. 0x001DE8BE) lives
    // in the station ship-bay cell (0x1DEA48) which is NOT loaded on
    // client (client is in cockpit cell 0x05100136). Engine's MoveToFunctor
    // dereferences the dest REFR's 3D state which is null → crash. Same
    // pattern as feedback_animation_lessons.md kCloneSettleMs=10000 lesson.
    //
    // Phase 3a v2 - readiness gate: only MoveTo if dest REFR has loadedData
    // (REFR+0xC8 in CLib +8 layout) AND data3D (loadedData+0x68) both
    // non-null. If not loaded, log + skip (no apply). User can re-test
    // without crash; engine will sync dest cell when player walks closer
    // or via cell-stream task - at that point a second MoveTo attempt could
    // succeed. For now: skip + diagnostic log so we know how often the
    // gate fires.
    const std::uintptr_t shipBytes = reinterpret_cast<std::uintptr_t>(ship);
    const std::uintptr_t dockBytes = reinterpret_cast<std::uintptr_t>(dock);
    std::uint64_t shipLoadedData = 0, shipData3D = 0;
    std::uint64_t dockLoadedData = 0, dockData3D = 0;
    __try {
        // CommonLibSF +8 shift: REFR.loadedData @ +0xC8, loadedData.data3D @ +0x68
        shipLoadedData = *reinterpret_cast<std::uint64_t*>(shipBytes + 0xC8);
        if (shipLoadedData) {
            shipData3D = *reinterpret_cast<std::uint64_t*>(shipLoadedData + 0x68);
        }
        dockLoadedData = *reinterpret_cast<std::uint64_t*>(dockBytes + 0xC8);
        if (dockLoadedData) {
            dockData3D = *reinterpret_cast<std::uint64_t*>(dockLoadedData + 0x68);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PLOG_WARN("[PULSAR] L.6 V3 DockSync apply: state-read SEH"sv);
        return;
    }

    PLOG_INFO(
        "[PULSAR] L.6 V3 DockSync ⚡ APPLY stage={} ship=0x{:08X} (ld=0x{:X} 3d=0x{:X}) "
        "→ dock=0x{:08X} (ld=0x{:X} 3d=0x{:X})"sv,
        stage, shipFormID, shipLoadedData, shipData3D,
        dockFormID, dockLoadedData, dockData3D);
    NASLOG(
        "L.6 V3 DockSync APPLY ship=0x%08X(ld=%llx 3d=%llx) dock=0x%08X(ld=%llx 3d=%llx)",
        shipFormID,
        (unsigned long long)shipLoadedData, (unsigned long long)shipData3D,
        dockFormID,
        (unsigned long long)dockLoadedData, (unsigned long long)dockData3D);

    // Phase 3a v3 (2026-05-02): v2's `!= 0` check was a false-positive - the
    // dock REFR's loadedData+0x68 returned `0x3F80000000000001` which is the
    // IEEE-754 bit pattern for float `1.0`, not a heap pointer. The dock
    // REFR's loadedData has a different internal layout when its 3D isn't
    // fully loaded (likely a stub object with garbage at +0x68).
    //
    // Real heap pointers are in [0x10000, 0x7FFFFFFFFFFF] on x64 user-mode.
    // Reject anything outside that range as invalid.
    auto isValidPtr = [](std::uint64_t v) {
        return v >= 0x0000000000010000ull && v <  0x00007FFFFFFFFFFFull;
    };

    if (!isValidPtr(shipData3D) || !isValidPtr(dockData3D)) {
        PLOG_WARN(
            "[PULSAR] L.6 V3 DockSync apply: SKIP - REFR data3D invalid "
            "(ship3d=0x{:X} {}; dock3d=0x{:X} {}). Will retry on next packet."sv,
            shipData3D, isValidPtr(shipData3D) ? "ok" : "FAKE",
            dockData3D, isValidPtr(dockData3D) ? "ok" : "FAKE");
        NASLOG("L.6 V3 DockSync APPLY SKIP - data3D invalid (ship=%llx dock=%llx)",
               (unsigned long long)shipData3D, (unsigned long long)dockData3D);
        // Don't update g_lastApplyTick - the next packet will retry.
        return;
    }

    // Both REFRs have data3D - safe to dispatch the Papyrus MoveTo. Engine
    // calls akShip.MoveTo(akDock, 0,0,0, abMatchRotation=true, abCopyCell=true)
    // via DoMoveRefresh shim. SHIP MoveTo is allowed by user (2026-05-02);
    // PLAYER MoveTo is forbidden.
    const bool ok = DispatchShipMoveTo(ship, dock);
    if (ok) {
        PLOG_INFO("[PULSAR] L.6 V3 DockSync apply: dispatch returned true - engine queued MoveTo"sv);
        NASLOG("L.6 V3 DockSync APPLY: MoveTo dispatched");
        g_lastApplyTick.store(now, std::memory_order_release);
    } else {
        PLOG_WARN("[PULSAR] L.6 V3 DockSync apply: DispatchStaticCall returned false"sv);
    }
}

}  // namespace Pulsar::DockSync
