#include "PCH.h"
#include "SpaceSync/SpaceSync.h"
#include "SpaceSync/TakeoffProbe.h"
#include "SpaceSync/DockSync.h"
#include "SpaceSync/FastTravelSync.h"

#include "Cell3DAttach.h"
#include "Config.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "TransitSync/TransitSync.h"
#include "NASLog.h"

#include <MinHook.h>

#include <RE/B/BSFixedString.h>
#include <RE/B/BSScriptUtil.h>
#include <RE/B/BSTEvent.h>
#include <RE/E/Events.h>
#include <RE/G/GameVM.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

using namespace std::string_view_literals;

namespace Pulsar::SpaceSync {
namespace {

    // ---- Event-source RVAs (from SFSE 0.2.19 GameEvents.h) ------------------
    // Each is a function returning BSTEventSource<EventT>* (lazy-init singleton).
    // CommonLibSF has these wrapped but with REL::ID(0) placeholders, so we
    // bypass and use the raw RVAs directly - same pattern GrabSync uses.
    //
    // Timeline (verified RE 2026-04-28):
    //   Takeoff:  TakeoffMenu_Launch -> [animation] -> TakeOffEvent
    //   Landing:  [travel] -> LandingEvent -> RampDownEvent
    //   Travel:   FarTravelEvent -> GravJumpEvent
    //   Dock:     [request] -> DockEvent
    //
    // V1 sinks every event to see firing order. V2 picks earliest reliable
    // signal per lifecycle phase for sync.
    constexpr std::uintptr_t kRVA_GetTakeOffSrc        = 0x2160390;
    constexpr std::uintptr_t kRVA_GetLandingSrc        = 0x21602F0;
    constexpr std::uintptr_t kRVA_GetGravJumpSrc       = 0x1678ED0;
    constexpr std::uintptr_t kRVA_GetDockSrc           = 0x215FE90;
    constexpr std::uintptr_t kRVA_GetTakeoffMenuSrc    = 0x1678D90;  // early TakeOff
    constexpr std::uintptr_t kRVA_GetRampDownSrc       = 0x215FA30;  // post-Landing
    constexpr std::uintptr_t kRVA_GetFarTravelSrc      = 0x2160430;
    // L.2 V1 debug - sink CloseMenu/ExitShip to detect what fires during abort.
    constexpr std::uintptr_t kRVA_GetTakeoffMenuCloseSrc = 0x1678C50;
    constexpr std::uintptr_t kRVA_GetTakeoffMenuExitSrc  = 0x1678CF0;

    // CommonLibSF doesn't have TakeoffMenu_CloseMenu - define our own stub.
    struct TakeoffMenu_CloseMenuStub {};  // pre-GravJump

    using GetEventSrcFn = void* (__fastcall*)();

    // Wire-format kind values - must match SpaceTransitionData::kind comments.
    constexpr std::uint8_t kKind_TakeOff           = 0;
    constexpr std::uint8_t kKind_Landing           = 1;
    constexpr std::uint8_t kKind_GravJump          = 2;
    constexpr std::uint8_t kKind_Dock              = 3;
    constexpr std::uint8_t kKind_TakeoffMenuLaunch = 4;
    constexpr std::uint8_t kKind_RampDown          = 5;
    constexpr std::uint8_t kKind_FarTravel         = 6;
    constexpr std::uint8_t kKind_MenuClose         = 7;  // diag only
    constexpr std::uint8_t kKind_MenuExit          = 8;  // diag only
    constexpr std::uint8_t kKindCount              = 9;

    static const char* KindStr(std::uint8_t k)
    {
        switch (k) {
            case kKind_TakeOff:           return "TakeOff";
            case kKind_Landing:           return "Landing";
            case kKind_GravJump:          return "GravJump";
            case kKind_Dock:              return "Dock";
            case kKind_TakeoffMenuLaunch: return "TakeoffMenu_Launch";
            case kKind_RampDown:          return "RampDown";
            case kKind_FarTravel:         return "FarTravel";
            case kKind_MenuClose:         return "TakeoffMenu_Close";
            case kKind_MenuExit:          return "TakeoffMenu_Exit";
        }
        return "?";
    }

    // De-dupe events that fire multiple times in quick succession (engine
    // sometimes double-fires for the same logical state change).
    constexpr std::uint64_t kDedupeMs = 500;
    static std::atomic<std::uint64_t> g_lastSendTick[kKindCount]{};

    // L.2 V1 Step A - debug hotkey to externally fire TakeoffMenu_Launch event.
    // Test: in cockpit, press F8 → does the engine launch into space?
    // If yes, the BSTEventSource SendEvent path is viable for receiver-trigger.
    // If no, we need direct FUN_14213b6f0(ship) call instead.
    static bool g_lastF8Down = false;

    // ---- V2 Auto-Follow state -----------------------------------------------
    // Set when peer's space event arrives (network thread). Drained by
    // TickFollowApply on the game thread. Same de-dupe-by-time pattern as
    // TransitSync's pending-follow.
    struct PendingSpaceFollow {
        std::atomic<bool>          armed{ false };
        std::atomic<std::uint8_t>  kind{ 0 };
        std::atomic<std::uint64_t> armTick{ 0 };
        std::atomic<std::uint32_t> peerCellFormID{ 0 };
    };
    static PendingSpaceFollow g_pendingFollow;

    // L.6 V1/V2/V2.1/V2.2 (2026-05-02) - all the dock-state / hatch-transit
    // sync attempts have been removed. They were MoveTo-shortcut approaches;
    // user wants a real visual-dock sync (Schiff visuell am Dock + Hatch
    // natürlich aktivierbar). V3 plan: engine-state memory-diff like L.5
    // cruise. See memory/project_l6_dock_sync.md.
    //
    // What's KEPT for V3 prep:
    // TakeoffProbe's DockProbe-Window (5Hz sampling for 30s after Dock fire)
    // LootSync's Activate-during-DockProbe logging (Hatch-FURN detection)
    // GetSpaceshipParentDock fix in TakeoffProbe (ship-as-this - even
    //     though it returns 0 for player ship, leave fix in for V3 testing).

    // Don't re-apply follow within this window for the same kind.
    constexpr std::uint64_t kApplyDedupeMs = 2000;
    static std::atomic<std::uint64_t> g_lastApplyTick[kKindCount]{};

    // Per-clone settle window - track when GetRemoteActor() last returned a
    // new pointer. Repeated host-crashes 2026-04-28 21:39 + 21:55 both fired
    // ~150-200ms after Remote-actor-spawned. Engine internals (MovCtrl,
    // Havok, animation graph) finish wiring AFTER all pointers exist -
    // dispatching MoveTo into a still-wiring clone NULL-derefs deep in
    // Starfield's MoveToFunctor pipeline. A 10s post-spawn skip avoids this.
    constexpr std::uint64_t kCloneSettleMs = 10000;
    static std::atomic<void*>            g_lastCloneSeenPtr{ nullptr };
    static std::atomic<std::uint64_t>    g_lastCloneSeenTick{ 0 };

    static bool IsCloneSettled(void* a_clone)
    {
        void* lastPtr = g_lastCloneSeenPtr.load(std::memory_order_acquire);
        if (lastPtr != a_clone) {
            g_lastCloneSeenPtr.store(a_clone, std::memory_order_release);
            g_lastCloneSeenTick.store(::GetTickCount64(), std::memory_order_release);
            return false;  // first time seeing this clone - not settled
        }
        std::uint64_t firstSeen = g_lastCloneSeenTick.load(std::memory_order_acquire);
        return (::GetTickCount64() - firstSeen) >= kCloneSettleMs;
    }

    // ---- Papyrus dispatch helper (mirrors TransitSync::DispatchPlayerFollowMoveTo)
    // Calls PulsarFurnSync.DoPlayerFollowMoveTo(akDestRef) which Papyrus-side
    // does Game.GetPlayer().MoveTo(akDestRef). MoveTo is non-latent (verified
    // 2026-04-27) - engine queues a MoveToFunctor on the task pump and returns
    // synchronously. The functor handles cell-load + worldspace switch.
    static bool DispatchMoveTo(RE::TESObjectREFR* a_dest)
    {
        if (!a_dest) return false;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        RE::BSScript::Variable destVar;
        RE::BSScript::PackVariable(destVar, a_dest);

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

    // Which kinds trigger an auto-follow MoveTo?
    //   TakeOff, Landing, GravJump, Dock          → yes (real state changes)
    //   TakeoffMenu_Launch                        → yes (early signal - gives
    //                                                client time to follow during
    //                                                host's takeoff cutscene)
    //   RampDown                                  → no (redundant w/ Landing,
    //                                                fires basically same instant)
    //   FarTravel                                 → no (didn't fire in V1 test
    // unreliable)
    static bool ShouldFollow(std::uint8_t k)
    {
        switch (k) {
            case kKind_TakeOff:
            case kKind_Landing:
            case kKind_GravJump:
            case kKind_Dock:
            case kKind_TakeoffMenuLaunch:
                return true;
            default:
                return false;
        }
    }

    static bool IsConnected()
    {
        return Pulsar::Network::GetSingleton().IsConnected();
    }

    // Time-since-init guard - gives the engine ~10s to settle after DLL load
    // before we emit any space events. During this window the engine fires
    // queued/stale events from save-load (verified 2026-04-28 host-crash:
    // client's GravJump from save-load init triggered host follow during
    // freshly-spawned clone window → NULL+0x90 deref in MoveTo dispatch).
    constexpr std::uint64_t kStartupQuietMs = 10000;
    static std::atomic<std::uint64_t> g_initTick{ 0 };

    static bool IsStartupQuiet()
    {
        std::uint64_t init = g_initTick.load(std::memory_order_acquire);
        if (init == 0) return true;  // Init() not run yet
        return (::GetTickCount64() - init) < kStartupQuietMs;
    }

    static void Broadcast(std::uint8_t a_kind)
    {
        if (!IsConnected()) return;

        // Sender gate - don't broadcast during loadMenu / cell-stream churn or
        // within the startup quiet window. Both correlate with engine firing
        // stale/replay events that are not real player actions.
        if (Pulsar::Cell3DAttach::IsQuiet() ||
            Pulsar::Cell3DAttach::WasInLoadMenu() ||
            IsStartupQuiet())
        {
            PLOG_INFO(
                "[PULSAR] SpaceSync SEND-skip kind={} ({}) - gate active "
                "(quiet={} loadMenu={} startup={})"sv,
                a_kind, KindStr(a_kind),
                Pulsar::Cell3DAttach::IsQuiet() ? 1 : 0,
                Pulsar::Cell3DAttach::WasInLoadMenu() ? 1 : 0,
                IsStartupQuiet() ? 1 : 0);
            return;
        }

        // De-dupe per kind.
        std::uint64_t now = ::GetTickCount64();
        std::uint64_t last = g_lastSendTick[a_kind].load(std::memory_order_acquire);
        if ((now - last) < kDedupeMs) return;
        g_lastSendTick[a_kind].store(now, std::memory_order_release);

        Pulsar::Network::SpaceTransitionData td{};
        td.kind = a_kind;
        td.posX = td.posY = td.posZ = 0.0f;
        td.playerCellFormID = 0;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            td.posX = player->data.location.x;
            td.posY = player->data.location.y;
            td.posZ = player->data.location.z;
            if (player->parentCell) {
                td.playerCellFormID = player->parentCell->formID;
            }
        }

        std::uint8_t buf[64]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_SPACE_TRANSITION, buf, sizeof(buf));
        w.Write(td);
        Pulsar::Network::GetSingleton().Send(w.Data(), w.Size(), true);

        PLOG_INFO(
            "[PULSAR] SpaceSync SEND kind={} ({}) cell=0x{:08X} pos=({:.0f},{:.0f},{:.0f})"sv,
            a_kind, KindStr(a_kind), td.playerCellFormID,
            td.posX, td.posY, td.posZ);
        NASLOG("SpaceSync: SEND kind=%u (%s) cell=0x%08X",
               a_kind, KindStr(a_kind), td.playerCellFormID);
    }

    // ---- Sinks (one per event type) ----------------------------------------

    // L.2 V1 echo-prevention: when we trigger TakeoffMenu_Launch on receiver
    // side via FUN_141679470, the engine's local sink fires which would re-
    // enter our SpaceshipSink<TakeoffMenu_Launch>::ProcessEvent and try to
    // broadcast back. Set this thread_local before calling fire-helper, sink
    // checks it and skips broadcast if set.
    static thread_local bool t_inReceiveTrigger = false;

    // L.2 V2 - abort-gate suppression window. When Step 2 fires, we set this
    // tick. For ~5 seconds, FUN_1421249a0 hook returns success without
    // running the abort branch. Engine state machine continues takeoff
    // instead of firing Landing event (which was the abort signal).
    static std::atomic<std::uint64_t> g_abortSuppressUntil{ 0 };
    constexpr std::uint64_t kAbortSuppressMs = 8000;

    // L.2 V2 - JCC byte-patch in FUN_1421249a0 abort branch.
    //
    // RE'd location of the validation JCC (jz rel32 to abort branch):
    //   FUN_1421249a0 @ RVA 0x21219B0
    //   vt[0xAC] call @ func+0xB0 (FF 90 60 05 00 00)
    //   test r14,r14 @ func+0xDE (4D 85 F6)
    //   JZ rel32 +0xD8 @ func+0xE1 (0F 84 D8 00 00 00)  ← THE PATCH TARGET
    //
    // We dynamically NOP the JCC during our trigger window so engine takes
    // success-path (full takeoff state mutations: FUN_140ba1520, FUN_140518590,
    // pathing setup) instead of aborting via Landing-fire.
    //
    // Patch only during our 8s window - restores after, so normal landings
    // (which legitimately use the abort branch) are unaffected.
    constexpr std::uintptr_t kRVA_AbortJcc = 0x2121A91;  // 244: func 0x21219B0 + 0xE1 (test r14,r14;jz, bytes 0F 84 D8 00 00 00 unchanged)
    constexpr std::size_t    kAbortJccLen = 6;

    static std::uint8_t g_origJccBytes[kAbortJccLen] = { 0x0F, 0x84, 0xD8, 0x00, 0x00, 0x00 };
    static std::atomic<bool> g_jccPatched{ false };

    static bool ApplyAbortJccPatch()
    {
        if (g_jccPatched.load(std::memory_order_acquire)) return true;

        void* addr = reinterpret_cast<void*>(REL::Offset{ kRVA_AbortJcc }.address());
        DWORD oldProt = 0;
        if (!VirtualProtect(addr, kAbortJccLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
            PLOG_WARN("[PULSAR] L.2 V2: VirtualProtect (apply) failed"sv);
            return false;
        }
        // Verify expected original bytes before patching.
        std::uint8_t cur[kAbortJccLen]{};
        std::memcpy(cur, addr, kAbortJccLen);
        if (std::memcmp(cur, g_origJccBytes, kAbortJccLen) != 0) {
            PLOG_WARN("[PULSAR] L.2 V2: JCC bytes mismatch - abort patch skipped"sv);
            VirtualProtect(addr, kAbortJccLen, oldProt, &oldProt);
            return false;
        }
        // Write 6 NOPs (= never take abort branch, always fall through to success path).
        std::uint8_t nops[kAbortJccLen] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        std::memcpy(addr, nops, kAbortJccLen);
        DWORD dummy = 0;
        VirtualProtect(addr, kAbortJccLen, oldProt, &dummy);
        FlushInstructionCache(GetCurrentProcess(), addr, kAbortJccLen);
        g_jccPatched.store(true, std::memory_order_release);
        PLOG_INFO("[PULSAR] L.2 V2: JCC patched (NOP'd) at 0x{:X}"sv,
                  reinterpret_cast<std::uintptr_t>(addr));
        NASLOG("L.2 V2: JCC patched at 0x%llX",
               (unsigned long long)reinterpret_cast<std::uintptr_t>(addr));
        return true;
    }

    static void RevertAbortJccPatch()
    {
        if (!g_jccPatched.load(std::memory_order_acquire)) return;

        void* addr = reinterpret_cast<void*>(REL::Offset{ kRVA_AbortJcc }.address());
        DWORD oldProt = 0;
        if (!VirtualProtect(addr, kAbortJccLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
            PLOG_WARN("[PULSAR] L.2 V2: VirtualProtect (revert) failed"sv);
            return;
        }
        std::memcpy(addr, g_origJccBytes, kAbortJccLen);
        DWORD dummy = 0;
        VirtualProtect(addr, kAbortJccLen, oldProt, &dummy);
        FlushInstructionCache(GetCurrentProcess(), addr, kAbortJccLen);
        g_jccPatched.store(false, std::memory_order_release);
        PLOG_INFO("[PULSAR] L.2 V2: JCC restored at 0x{:X}"sv,
                  reinterpret_cast<std::uintptr_t>(addr));
        NASLOG("L.2 V2: JCC restored");
    }

    // L.2 V1 Step A v2 - call engine's TakeoffMenu_Launch fire-helper directly.
    //
    // Background: BSTEventSource::Notify (REL::ID 123824) takes a LAMBDA-
    // DISPATCHER struct as param_2, NOT the event itself. CommonLibSF's
    // declared signature `Notify(void* event)` is wrong for engine usage.
    // Each engine fire-site builds its own lambda struct on stack and calls
    // Notify(source, &lambdaStruct).
    //
    // FUN_141679470 (RVA 0x1677090, 72 bytes) is the engine's pre-built
    // helper for firing TakeoffMenu_Launch. It:
    //   1. Constructs a stack-local TakeoffMenu_Launch event (vftable + data)
    //   2. Gets the source via FUN_14167B290 (= GetTakeoffMenuLaunchEventSrc)
    //   3. Sets up the proper lambda::vftable
    //   4. Calls Notify(source, &lambdaStruct) - properly
    //
    // Caveat: FUN_141679470 has 0 callers in engine - Bethesda doesn't use
    // this path. They fire indirectly via TCmd queues. Calling our path
    // bypasses TCmd serialization but should still trigger sinks correctly.
    using FireTakeoffLaunchFn = void(__fastcall*)(std::uint64_t, std::uint64_t);
    constexpr std::uintptr_t kRVA_FireTakeoffMenuLaunch = 0x1677090;

    static void DebugFireTakeoffMenuLaunch()
    {
        PLOG_INFO("[PULSAR] L.2 V1 Step A v2 - F8 calling engine FUN_141679470 directly"sv);
        NASLOG("L.2 Step A v2: F8 fire via engine helper");

        auto fn = reinterpret_cast<FireTakeoffLaunchFn>(
            REL::Offset{ kRVA_FireTakeoffMenuLaunch }.address());

        // Set echo-flag so our own sink won't re-broadcast.
        t_inReceiveTrigger = true;
        __try {
            fn(0, 0);  // engine helper builds event from stack locals, args unused
            PLOG_INFO("[PULSAR] L.2 Step A v2 - fire helper returned, observe game"sv);
            NASLOG("L.2 Step A v2: returned cleanly");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.2 Step A v2 - fire helper threw"sv);
            NASLOG("L.2 Step A v2: SEH caught");
        }
        t_inReceiveTrigger = false;
    }

    // L.2 V1 - receiver 2-step approach:
    //   Step 1: FUN_141677B50(fakeMenu) - opens engine's TakeoffMenu UI on
    //           client (engine creates its own valid menu instance + registers
    //           its sinks on the TakeoffMenu_Launch event source).
    //   Step 2 (~500ms later): FUN_141679470 (fire-helper) - fires
    //           TakeoffMenu_Launch event → engine's now-registered sink
    //           processes with valid state → cutscene + cell transition.
    using LaunchImmediateFn = void(__fastcall*)(void* pMenuPtr);
    constexpr std::uintptr_t kRVA_LaunchImmediate = 0x16757D0;

    // Step-2 pending state (drained by TickFollowApply on game thread).
    static std::atomic<std::uint64_t> g_step2DueTick{ 0 };
    // L.2 V1 iter - 500ms led to black-screen-then-abort on client. Engine
    // started cutscene then aborted. Hypothesis: Step2 fires while engine is
    // mid-cutscene from Step1, redundant event kills it. Try waiting longer.
    constexpr std::uint64_t kStep2DelayMs = 2500;

    // L.2 V1 force-pilot-cell hack: temporarily set parentCell.cellExtra+0x68
    // from 0x05 (passenger) to 0x01 (pilot) so engine's strict-mode
    // GetPlayerSpaceship returns ship → takeoff validation passes → cutscene
    // runs to completion. Restore byte after takeoff.
    static std::atomic<std::uintptr_t> g_savedCellExtraPtr{ 0 };
    static std::atomic<std::uint8_t>   g_savedSeatTypeByte{ 0 };

    static bool ForcePilotCellByte()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            PLOG_WARN("[PULSAR] L.2 V1 force-pilot: no player or parentCell"sv);
            return false;
        }
        std::uintptr_t cellExtra = 0;
        std::uint8_t origByte = 0xFF;
        std::uint32_t cellID = player->parentCell->formID;

        __try {
            cellExtra = *reinterpret_cast<std::uintptr_t*>(
                reinterpret_cast<std::uint8_t*>(player->parentCell) + 0x58);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.2 V1 force-pilot: cell+0x58 read threw"sv);
            return false;
        }

        if (!cellExtra) {
            PLOG_INFO("[PULSAR] L.2 V1 force-pilot: cellExtra=0 (cell=0x{:08X}) - nothing to flip"sv, cellID);
            NASLOG("L.2 V1: force-pilot cellExtra=0 cell=0x%08X", cellID);
            return false;
        }

        __try {
            origByte = *reinterpret_cast<std::uint8_t*>(cellExtra + 0x68);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.2 V1 force-pilot: cellExtra+0x68 read threw"sv);
            return false;
        }

        PLOG_INFO(
            "[PULSAR] L.2 V1 force-pilot diag: cell=0x{:08X} cellExtra=0x{:X} +0x68=0x{:02X}"sv,
            cellID, cellExtra, origByte);
        NASLOG("L.2 V1: force-pilot diag cell=0x%08X cellExtra=0x%llX +0x68=0x%02X",
               cellID, (unsigned long long)cellExtra, origByte);

        // Try writing 0x01 regardless of original (we now know what was there).
        bool wrote = false;
        __try {
            *reinterpret_cast<std::uint8_t*>(cellExtra + 0x68) = 0x01;
            wrote = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.2 V1 force-pilot: write threw (memory protected?)"sv);
        }

        if (wrote) {
            g_savedCellExtraPtr.store(cellExtra, std::memory_order_release);
            g_savedSeatTypeByte.store(origByte, std::memory_order_release);
            PLOG_INFO("[PULSAR] L.2 V1 - force-pilot: byte 0x{:02X} → 0x01"sv, origByte);
            NASLOG("L.2 V1: force-pilot byte 0x%02X->0x01", origByte);
        }
        return wrote;
    }

    static void RestorePilotCellByte()
    {
        std::uintptr_t cellExtra = g_savedCellExtraPtr.load(std::memory_order_acquire);
        std::uint8_t origByte = g_savedSeatTypeByte.load(std::memory_order_acquire);
        if (cellExtra == 0) return;

        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(cellExtra) + 0x68;
            *p = origByte;
            PLOG_INFO("[PULSAR] L.2 V1 - restored cellExtra+0x68 to 0x{:02X}"sv, origByte);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        g_savedCellExtraPtr.store(0, std::memory_order_release);
    }

    static void TriggerReceiverTakeoff_Step1()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_WARN("[PULSAR] L.2 V1 - no player, skip"sv);
            return;
        }

        RE::TESObjectREFR* ship = nullptr;
        __try { ship = player->GetSpaceship(true); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!ship) {
            PLOG_WARN("[PULSAR] L.2 V1 - not in ship, skip"sv);
            NASLOG("L.2 V1: skip - not in ship");
            return;
        }

        std::uint32_t shipID = 0;
        __try { shipID = ship->formID; } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Force pilot cell-type BEFORE Step 1 so engine validation sees pilot.
        if (!ForcePilotCellByte()) {
            PLOG_WARN("[PULSAR] L.2 V1 - force-pilot failed (cellExtra null or wrong type)"sv);
            // Continue anyway - maybe engine still accepts.
        }

        PLOG_INFO("[PULSAR] L.2 V1 Step 1 - opening engine TakeoffMenu UI (ship=0x{:08X})"sv, shipID);
        NASLOG("L.2 V1 Step 1: open engine UI ship=0x%08X", shipID);

        auto fn = reinterpret_cast<LaunchImmediateFn>(
            REL::Offset{ kRVA_LaunchImmediate }.address());

        alignas(8) static thread_local std::uint8_t fakeMenu[0x300] = {};
        std::memset(fakeMenu, 0, sizeof(fakeMenu));
        void* fakeMenuPtr = fakeMenu;

        t_inReceiveTrigger = true;
        __try {
            fn(&fakeMenuPtr);
            PLOG_INFO("[PULSAR] L.2 V1 Step 1 - UI opened, scheduling Step 2 in {}ms"sv, kStep2DelayMs);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.2 V1 Step 1 - threw"sv);
        }
        t_inReceiveTrigger = false;

        g_step2DueTick.store(::GetTickCount64() + kStep2DelayMs, std::memory_order_release);
    }

    static void TriggerReceiverTakeoff_Step2()
    {
        PLOG_INFO("[PULSAR] L.2 V1 Step 2 - firing TakeoffMenu_Launch event"sv);
        NASLOG("L.2 V1 Step 2: fire Launch event");

        // L.2 V2 attempts archived (didn't deliver cutscene-sync):
        //   1. Hook FUN_1421249a0 return-1: skipped success-path mutations.
        //   2. JCC byte-patch: prevented Landing-fire abort but engine
        //      state-machine still didn't progress (missing "takeoff in
        //      progress" engine globals).
        // Plan B (SaveBuffer-resync) ships as L.2 final.
        // ApplyAbortJccPatch();
        // g_abortSuppressUntil.store(::GetTickCount64() + kAbortSuppressMs, ...);

        auto fn = reinterpret_cast<FireTakeoffLaunchFn>(
            REL::Offset{ kRVA_FireTakeoffMenuLaunch }.address());

        t_inReceiveTrigger = true;
        __try {
            fn(0, 0);
            PLOG_INFO("[PULSAR] L.2 V1 Step 2 - fire returned"sv);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN("[PULSAR] L.2 V1 Step 2 - fire threw"sv);
        }
        t_inReceiveTrigger = false;
    }

    static void TriggerReceiverTakeoff()
    {
        TriggerReceiverTakeoff_Step1();
    }

    template <typename EventT, std::uint8_t Kind>
    class SpaceshipSink : public RE::BSTEventSink<EventT> {
    public:
        std::atomic<int> fireCount{ 0 };

        RE::BSEventNotifyControl ProcessEvent(
            const EventT& /*e*/,
            RE::BSTEventSource<EventT>* /*src*/) override
        {
            int n = fireCount.fetch_add(1) + 1;
            PLOG_INFO(
                "[PULSAR] SpaceSync sink fired: kind={} ({}) #{}"sv,
                Kind, KindStr(Kind), n);
            NASLOG("SpaceSync: %s fire #%d", KindStr(Kind), n);
            Pulsar::TakeoffProbe::OnEventFire(Kind);
            if constexpr (Kind == kKind_GravJump) {
                Pulsar::SpaceSync::FastTravel::OnGravJumpFired();
            }
            if constexpr (Kind == kKind_FarTravel) {
                Pulsar::SpaceSync::FastTravel::OnFarTravelFired();
            }
            if constexpr (Kind == kKind_RampDown) {
                Pulsar::SpaceSync::FastTravel::OnRampDownFired();
            }
            // L.7 V14.1 - arm deferred save-bridge on TakeOff/Landing.
            // V14 immediate-fire crashed host (DEP violation, addr 0)
            // because the quicksave serializer dereferenced transient
            // mid-cutscene REFRs with null vtables. Now we arm a 5s
            // settle timer; the actual fire runs from FastTravel::Tick
            // once the engine has stabilized.
            //
            // Re-arm semantic: each sink fire pushes the timestamp
            // forward, so multifire bursts (4-5 events in same ms during
            // the cutscene) collapse to ONE fire 5s after the last event.
            //
            // V15 - ArmTransitionFire returns bool: true=real transition
            // (inSpace flipped), false=spurious (door/POI/init). On
            // spurious we ALSO suppress the PKT_SPACE_TRANSITION
            // broadcast so client doesn't get a flood of useless events.
            //
            // GravJump path is handled by PFT poll. Dock excluded
            // (handled by L.6 DockSync). Surface POI fast-travel doesn't
            // fire ship sinks → naturally skipped.
            bool spuriousTransition = false;
            if constexpr (Kind == kKind_TakeOff || Kind == kKind_Landing) {
                if (Pulsar::Network::GetSingleton().IsHost()) {
                    spuriousTransition =
                        !Pulsar::SpaceSync::FastTravel::ArmTransitionFire(
                            KindStr(Kind));
                }
            }
            // L.2 V1 echo prevention + diag-only events skip broadcast.
            // V15: also skip broadcast on spurious TakeOff/Landing.
            const bool isDiag = (Kind == kKind_MenuClose) || (Kind == kKind_MenuExit);
            if (!t_inReceiveTrigger && !isDiag && !spuriousTransition) {
                Broadcast(Kind);
            } else {
                if (spuriousTransition) {
                    PLOG_INFO(
                        "[PULSAR] [L.7 V15] SpaceSync sink kind={} "
                        "skip-broadcast (spurious - inSpace unchanged)"sv,
                        Kind);
                } else {
                    PLOG_INFO("[PULSAR] SpaceSync sink kind={} skip-broadcast"sv, Kind);
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // L.3 V1 - Spaceship position/rotation sync. Whoever's piloting (host
    // OR client) broadcasts ship REFR position+rotation while flying. Other
    // side writes to their local same-FormID ship → engine moves cockpit
    // cell with ship → passenger sees correct space scene from cockpit.
    //
    // Throttle: 30 Hz (33ms). Match VehicleSync cadence.
    static std::atomic<std::uint64_t> g_lastShipPosSendTick{ 0 };
    constexpr std::uint64_t kShipPosSendIntervalMs = 33;

    static bool IsLikelyPilot()
    {
        // Heuristic: if I'm in space (player.IsInSpace) AND I have a ship
        // (GetSpaceship returns non-null) → broadcast as if pilot. Engine's
        // pilot validation prevents non-pilots from CONTROLLING but doesn't
        // affect our broadcast. Receiver's local ship REFR position will
        // overwrite engine's last-known - minor risk of pilot-vs-pilot
        // ping-pong if BOTH sides try to be pilot. Mitigated by host-only
        // gate for now (only host broadcasts, until L.3 V2 adds direction
        // arbitration).
        if (!Pulsar::Network::GetSingleton().IsHost()) return false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        bool inSpace = false;
        __try { inSpace = player->IsInSpace(true); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!inSpace) return false;
        RE::TESObjectREFR* ship = nullptr;
        __try { ship = player->GetSpaceship(true); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return ship != nullptr;
    }

    // Diagnostic counters - flushed every 5s to log.
    static std::atomic<std::uint64_t> g_shipSendCount{ 0 };
    static std::atomic<std::uint64_t> g_lastDiagTick{ 0 };

    // L.3 V1 ship-pos sync DISABLED 2026-04-29.
    //
    // Diagnostic 2026-04-29 revealed: player ship REFR (FormID 0x0003F7E8)
    // stays at (0,0,0) of space cell during real-time flight. data.location,
    // data.angle, mesh.world.translate ALL zero throughout piloting.
    //
    // Bethesda's space-flight architecture: ship REFR is a STATIC ANCHOR at
    // space-cell origin. "Flight" is rendered via:
    // Camera/view offset within cockpit
    // Skybox/star-field rotation
    // NPC-ship positions relative to anchor
    // World-relative rendering
    //
    // Syncing ship.data.location is meaningless - it never changes during
    // flight. To truly sync flight in coop, need to RE the camera/world-
    // offset state machine. Tracked as L.3 V2.
    //
    // For now: keep SEND infrastructure (broadcasts (0,0,0) - harmless echo)
    // but disable APPLY on receiver to prevent SetPosition rapid-fire.
    static constexpr bool kShipPosSyncEnabled = false;

    static void TickShipPosSend()
    {
        if (!kShipPosSyncEnabled) return;

        std::uint64_t now = ::GetTickCount64();

        // Diagnostic: log state every 5s regardless of result.
        std::uint64_t lastDiag = g_lastDiagTick.load(std::memory_order_acquire);
        if ((now - lastDiag) > 5000) {
            g_lastDiagTick.store(now, std::memory_order_release);
            bool connected = IsConnected();
            bool isHost = Pulsar::Network::GetSingleton().IsHost();
            auto* player = RE::PlayerCharacter::GetSingleton();
            bool inSpace = false;
            bool hasShip = false;
            if (player) {
                __try { inSpace = player->IsInSpace(true); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                __try { hasShip = (player->GetSpaceship(true) != nullptr); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            bool quiet = Pulsar::Cell3DAttach::IsQuiet();
            bool inLoad = Pulsar::Cell3DAttach::WasInLoadMenu();
            bool startup = IsStartupQuiet();
            // Reset the diag counter BEFORE the log call: in the hardened build PLOG_INFO
            // compiles to ((void)0) and never evaluates its args, so a side-effect inside
            // the arg list would silently differ between Dev and Release. Capture first.
            const std::uint64_t sentSinceLastDiag = g_shipSendCount.exchange(0);
            PLOG_INFO(
                "[PULSAR] L.3 ship-pos diag: connected={} isHost={} inSpace={} hasShip={} "
                "quiet={} inLoad={} startup={} sentSinceLastDiag={}"sv,
                connected ? 1 : 0, isHost ? 1 : 0, inSpace ? 1 : 0, hasShip ? 1 : 0,
                quiet ? 1 : 0, inLoad ? 1 : 0, startup ? 1 : 0,
                sentSinceLastDiag);
        }

        if (!IsConnected()) return;
        if (!IsLikelyPilot()) return;

        if (Pulsar::Cell3DAttach::IsQuiet() ||
            Pulsar::Cell3DAttach::WasInLoadMenu() ||
            IsStartupQuiet()) {
            return;
        }

        std::uint64_t last = g_lastShipPosSendTick.load(std::memory_order_acquire);
        if ((now - last) < kShipPosSendIntervalMs) return;
        g_lastShipPosSendTick.store(now, std::memory_order_release);

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        RE::TESObjectREFR* ship = nullptr;
        __try { ship = player->GetSpaceship(true); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!ship) return;

        Pulsar::Network::ShipPosData sd{};
        // Diagnostic: also read position via mesh node (loadedData->data3D)
        // and physics body to compare. data.location vs visual mesh might
        // differ during flight if physics drives mesh independently.
        float meshX = 0, meshY = 0, meshZ = 0;
        bool meshValid = false;
        __try {
            sd.shipFormID = ship->formID;
            if (ship->parentCell) {
                sd.spaceCellFormID = ship->parentCell->formID;
            }
            sd.posX = ship->data.location.x;
            sd.posY = ship->data.location.y;
            sd.posZ = ship->data.location.z;
            sd.rotX = ship->data.angle.x;
            sd.rotY = ship->data.angle.y;
            sd.rotZ = ship->data.angle.z;

            // Read mesh node world translation: REFR.loadedData->data3D->world.translate (+0xB0 in NiAVObject)
            std::uintptr_t loadedData = *reinterpret_cast<std::uintptr_t*>(
                reinterpret_cast<std::uint8_t*>(ship) + 0xB8);
            if (loadedData) {
                std::uintptr_t data3D = *reinterpret_cast<std::uintptr_t*>(loadedData + 0x08);
                if (data3D) {
                    // NiAVObject world.translate at +0xB0 (3 floats)
                    meshX = *reinterpret_cast<float*>(data3D + 0xB0);
                    meshY = *reinterpret_cast<float*>(data3D + 0xB4);
                    meshZ = *reinterpret_cast<float*>(data3D + 0xB8);
                    meshValid = true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        // Diagnostic: log every 60 sends (~2s at 30Hz) so we see if values change.
        static std::atomic<std::uint64_t> g_sendDiagCount{ 0 };
        std::uint64_t cnt = g_sendDiagCount.fetch_add(1);
        if ((cnt % 60) == 0) {
            PLOG_INFO(
                "[PULSAR] L.3 ship-pos send: data.loc=({:.1f},{:.1f},{:.1f}) angle=({:.2f},{:.2f},{:.2f}) "
                "mesh.world=({:.1f},{:.1f},{:.1f}) meshValid={}"sv,
                sd.posX, sd.posY, sd.posZ,
                sd.rotX, sd.rotY, sd.rotZ,
                meshX, meshY, meshZ, meshValid ? 1 : 0);
        }

        std::uint8_t buf[64]{};
        Pulsar::Network::PacketWriter w(
            Pulsar::Network::PKT_SHIP_POS, buf, sizeof(buf));
        w.Write(sd);
        Pulsar::Network::GetSingleton().Send(w.Data(), w.Size(), false);
        g_shipSendCount.fetch_add(1, std::memory_order_release);
    }

    // L.2 V1 Plan B - DEPRECATED 2026-05-04 V14. The poll-based ship-cell
    // flip detector never fired reliably (ship->parentCell stays in cockpit
    // interior across the takeoff cutscene). Replaced by event-driven
    // FastTravel::FireSaveBridge() called from the SpaceshipSink<TakeOff/
    // Landing/Dock>::ProcessEvent in the same file. Kept here in #if 0 so
    // we don't lose the cell-flip diagnostics if we want to re-enable as
    // a secondary signal later.
#if 0  // V14: superseded by event-driven save-bridge
    static std::atomic<std::uint32_t> g_lastHostShipCellID{ 0 };
    static std::atomic<std::uint64_t> g_lastResyncTick{ 0 };
    constexpr std::uint64_t kResyncDedupeMs = 30000;  // don't spam resync

    static void HostTryTakeoffResync()
    {
        // Only host triggers; client receives via PKT_SAVE_TRANSFER (existing).
        if (!Pulsar::Network::GetSingleton().IsHost()) return;
        if (!IsConnected()) return;

        // Read host's current ship cell.
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        RE::TESObjectREFR* ship = nullptr;
        __try { ship = player->GetSpaceship(true); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!ship || !ship->parentCell) return;

        std::uint32_t curShipCell = 0;
        bool curIsProc = false;
        __try {
            curShipCell = ship->parentCell->formID;
            curIsProc = (curShipCell & 0xFF000000u) == 0xFF000000u;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        std::uint32_t lastCell = g_lastHostShipCellID.load(std::memory_order_acquire);
        if (curShipCell == lastCell || curShipCell == 0) return;

        // Update tracker FIRST so we don't loop on transient states.
        g_lastHostShipCellID.store(curShipCell, std::memory_order_release);

        // We only care about transitions that USE space (FF-proc → static).
        // Initial state (lastCell=0) → static-cell isn't a transition; skip.
        if (lastCell == 0) return;

        bool lastWasProc = (lastCell & 0xFF000000u) == 0xFF000000u;
        // Transition rule: was-proc-planet, now-static-space (or any non-proc).
        if (!(lastWasProc && !curIsProc)) {
            // Other transitions: static-space → static-space (FarTravel) or
            // static-space → proc-planet (landing) - also broadcast.
            // We resync on ANY change that crosses the proc boundary in either direction.
            bool crossesBoundary = (lastWasProc != curIsProc);
            if (!crossesBoundary) return;
        }

        // De-dupe: don't fire resync more than every 30s (cooldown).
        std::uint64_t now = ::GetTickCount64();
        std::uint64_t lastResync = g_lastResyncTick.load(std::memory_order_acquire);
        if ((now - lastResync) < kResyncDedupeMs) {
            PLOG_INFO("[PULSAR] L.2 V1 Plan B: resync cooldown - last={:.1f}s ago"sv,
                      (now - lastResync) / 1000.0);
            return;
        }
        g_lastResyncTick.store(now, std::memory_order_release);

        PLOG_INFO("[PULSAR] L.2 V1 Plan B: HOST shipCell flip 0x{:08X} → 0x{:08X} → quicksave-resync"sv,
                  lastCell, curShipCell);
        NASLOG("L.2 V1 Plan B: shipCell 0x%08X -> 0x%08X, triggering quicksave-resync",
               lastCell, curShipCell);

        Pulsar::Network::GetSingleton().BeginHostQuicksaveTransfer();
    }
#endif  // 0 - V14 disabled HostTryTakeoffResync

    // (2026-06-24) - AUTO co-location into space on host takeoff.
    // The manual F8 host-bridge co-locates the client into the host's space
    // position fine (verified spacecoop-v6), but nothing fired it automatically:
    // the TakeOff event sink broadcasts too early (inSpace not yet flipped → the
    // L.7 V15 "spurious - inSpace unchanged" skip drops it) and the receiver-apply
    // is passive; the old poll detector (HostTryTakeoffResync, #if 0 above) keyed
    // on ship->parentCell which stays in the cockpit interior across takeoff.
    // The reliable signal is the HOST's own IsInSpace edge (false→true), polled on
    // the game thread. On that rising edge, after a settle delay (so the quicksave
    // reflects the in-space position and the takeoff churn is over), auto-fire the
    // proven host save-bridge = an automatic F8. Re-arms when the host leaves space.
    static std::atomic<bool>          g_hostWasInSpace{ false };
    static std::atomic<std::uint64_t> g_hostAutoBridgeDueTick{ 0 };
    constexpr std::uint64_t kHostAutoBridgeSettleMs = 5000;

    // was the host PILOTING (seated in the ship) when it entered space?
    // This is the takeoff-vs-dock-hatch discriminator. At TAKEOFF the host is
    // seated in the pilot seat when IsInSpace flips false→true (verified log
    // 22:09:38 "entered space" → next pilotDiag seated=1). At a DOCKING HATCH
    // transit the host walks ship→station ON FOOT, so the station-entry rising
    // edge has seated=0. Captured at the rising edge; re-checked at fire time.
    static std::atomic<bool>          g_hostEnteredSpaceSeated{ false };

    // Read whether the local player is seated in a ship (pilot seat). Mirrors
    // FlightSync::IsLocalDeterministicPilot's seated probe: PlayerCharacter
    // +0x228 = process, +0x08 = MiddleHighProcessData, +0x428 = furniture
    // handle (0 = not seated). SEH-guarded; returns false on any read error so
    // an unreadable state biases toward "on foot" only when paired with the
    // edge capture (the fire-time gate ORs the two so a transient misread at
    // takeoff cannot wrongly skip the follow-bridge).
    static bool HostSeatedInShip()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        bool seated = false;
        __try {
            auto* a    = reinterpret_cast<std::uint8_t*>(player);
            auto* proc = *reinterpret_cast<std::uint8_t**>(a + 0x228);
            if (proc) {
                auto* mh = *reinterpret_cast<std::uint8_t**>(proc + 0x08);
                if (mh) {
                    seated = (*reinterpret_cast<std::uint32_t*>(mh + 0x428) != 0);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { seated = false; }
        return seated;
    }

    static void TickHostAutoSpaceBridge()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected() || !net.IsHost()) return;
        if (!Pulsar::GetConfig().coopSpaceFollow) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        bool inSpace = false;
        __try { inSpace = player->IsInSpace(true); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        const bool was = g_hostWasInSpace.exchange(inSpace, std::memory_order_acq_rel);
        if (inSpace && !was) {
            // rising edge - host just entered space; arm the bridge after settle.
            // capture whether the host was piloting (seated) at the edge.
            // Seated = takeoff (client must follow → bridge). On foot = walked
            // through a dock hatch into the station (client already there → skip).
            const bool seatedAtEdge = HostSeatedInShip();
            g_hostEnteredSpaceSeated.store(seatedAtEdge, std::memory_order_release);
            g_hostAutoBridgeDueTick.store(::GetTickCount64() + kHostAutoBridgeSettleMs,
                                          std::memory_order_release);
            PLOG_INFO("[PULSAR] SpaceSync: HOST entered space → auto-bridge armed (+{}ms) seatedAtEdge={} [/]"sv,
                      kHostAutoBridgeSettleMs, seatedAtEdge ? 1 : 0);
            NASLOG("SpaceSync: host entered space, auto-bridge armed (seatedAtEdge=%d)",
                   seatedAtEdge ? 1 : 0);
            return;
        }
        if (!inSpace && was) {
            // left space - cancel any pending bridge, re-arm for next takeoff.
            g_hostAutoBridgeDueTick.store(0, std::memory_order_release);
            return;
        }

        const std::uint64_t due = g_hostAutoBridgeDueTick.load(std::memory_order_acquire);
        if (due == 0 || ::GetTickCount64() < due) return;

        // Don't bridge mid load-stream / cell churn - re-arm shortly and retry.
        if (Pulsar::Cell3DAttach::IsQuiet() || Pulsar::Cell3DAttach::WasInLoadMenu()) {
            g_hostAutoBridgeDueTick.store(::GetTickCount64() + 2000, std::memory_order_release);
            return;
        }

        // takeoff-vs-dock discriminator. A cell-equality SKIP guard was
        // tried here first and WRONGLY skipped takeoff-follow: at takeoff both
        // players sit in the same cockpit cell (0x06100136), but those are
        // SEPARATE instances (host in space, client on the ground) so the client
        // still needs the bridge (verified 2026-06-25 22:09 "auto-bridge SKIP ...
        // cell 0x06100136" → client did not follow). The correct signal is HOW the
        // host entered space:
        // seated/piloting  → TAKEOFF, client must follow → fire the bridge
        // on foot          → walked through a DOCK HATCH into the station, the
        //                         client walked through their own hatch too and is
        //                         already co-located → firing = redundant double-load
        // Bias toward firing (never re-break takeoff): only skip when the host is
        // confidently on foot BOTH at the rising edge AND now at fire time.
        const bool seatedAtEdge = g_hostEnteredSpaceSeated.load(std::memory_order_acquire);
        const bool seatedNow    = HostSeatedInShip();
        g_hostAutoBridgeDueTick.store(0, std::memory_order_release);
        if (!seatedAtEdge && !seatedNow) {
            PLOG_INFO("[PULSAR] SpaceSync: HOST auto-bridge SKIP - entered space ON FOOT "
                      "(dock-hatch transit, not takeoff) → no double-load []"sv);
            NASLOG("SpaceSync: host auto-bridge SKIP (on-foot hatch transit, not takeoff)");
            return;
        }
        PLOG_INFO("[PULSAR] SpaceSync: HOST auto-bridge firing (takeoff: seatedAtEdge={} seatedNow={}) [/]"sv,
                  seatedAtEdge ? 1 : 0, seatedNow ? 1 : 0);
        NASLOG("SpaceSync: host auto-bridge firing (takeoff)");
        net.BeginHostQuicksaveTransfer();   // = automatic F8; bridge-nostale handles combat
    }

    static SpaceshipSink<RE::Spaceship::TakeOffEvent,   kKind_TakeOff>           s_takeOffSink;
    static SpaceshipSink<RE::Spaceship::LandingEvent,   kKind_Landing>           s_landingSink;
    static SpaceshipSink<RE::Spaceship::GravJumpEvent,  kKind_GravJump>          s_gravJumpSink;
    static SpaceshipSink<RE::Spaceship::DockEvent,      kKind_Dock>              s_dockSink;
    static SpaceshipSink<RE::TakeoffMenu_Launch,        kKind_TakeoffMenuLaunch> s_takeoffMenuSink;
    static SpaceshipSink<RE::Spaceship::RampDownEvent,  kKind_RampDown>          s_rampDownSink;
    static SpaceshipSink<RE::Spaceship::FarTravelEvent, kKind_FarTravel>         s_farTravelSink;
    static SpaceshipSink<TakeoffMenu_CloseMenuStub,     kKind_MenuClose>         s_menuCloseSink;
    static SpaceshipSink<RE::TakeoffMenu_ExitShip,      kKind_MenuExit>          s_menuExitSink;

    template <typename EventT>
    static void TryRegister(const char* a_label,
                             std::uintptr_t a_rva,
                             RE::BSTEventSink<EventT>* a_sink)
    {
        auto getFn = reinterpret_cast<GetEventSrcFn>(
            REL::Offset{ a_rva }.address());

        void* raw = nullptr;
        __try { raw = getFn(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { raw = nullptr; }

        PLOG_INFO(
            "[PULSAR] SpaceSync: {} GetEventSrc=0x{:X} -> singleton 0x{:X}"sv,
            a_label,
            reinterpret_cast<std::uintptr_t>(getFn),
            reinterpret_cast<std::uintptr_t>(raw));

        if (raw) {
            auto* src = reinterpret_cast<RE::BSTEventSource<EventT>*>(raw);
            __try {
                src->RegisterSink(a_sink);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] SpaceSync: {} RegisterSink threw"sv, a_label);
                return;
            }
            PLOG_INFO("[PULSAR] SpaceSync: {} sink registered"sv, a_label);
        } else {
            PLOG_WARN("[PULSAR] SpaceSync: {} source null - sink not registered"sv,
                      a_label);
        }
    }

}  // namespace

// =====================================================================
// Public API
// =====================================================================

void Init()
{
    PLOG_INFO("[PULSAR] SpaceSync K.7 init"sv);
    g_initTick.store(::GetTickCount64(), std::memory_order_release);

    TryRegister<RE::Spaceship::TakeOffEvent>(
        "TakeOff",            kRVA_GetTakeOffSrc,     &s_takeOffSink);
    TryRegister<RE::Spaceship::LandingEvent>(
        "Landing",            kRVA_GetLandingSrc,     &s_landingSink);
    TryRegister<RE::Spaceship::GravJumpEvent>(
        "GravJump",           kRVA_GetGravJumpSrc,    &s_gravJumpSink);
    TryRegister<RE::Spaceship::DockEvent>(
        "Dock",               kRVA_GetDockSrc,        &s_dockSink);
    TryRegister<RE::TakeoffMenu_Launch>(
        "TakeoffMenu_Launch", kRVA_GetTakeoffMenuSrc, &s_takeoffMenuSink);
    TryRegister<RE::Spaceship::RampDownEvent>(
        "RampDown",           kRVA_GetRampDownSrc,    &s_rampDownSink);
    TryRegister<RE::Spaceship::FarTravelEvent>(
        "FarTravel",          kRVA_GetFarTravelSrc,   &s_farTravelSink);
    TryRegister<TakeoffMenu_CloseMenuStub>(
        "TakeoffMenu_Close",  kRVA_GetTakeoffMenuCloseSrc, &s_menuCloseSink);
    TryRegister<RE::TakeoffMenu_ExitShip>(
        "TakeoffMenu_Exit",   kRVA_GetTakeoffMenuExitSrc,  &s_menuExitSink);

    // L.2 V2 - abort-gate hook DISABLED 2026-04-29. Returning success from
    // FUN_1421249a0 prevents Landing-fire abort but ALSO skips the success-
    // path state mutations (FUN_140ba1520 + FUN_140518590 + pathing setup).
    // Result: engine cutscene visually starts then freezes for ~14s, until
    // Plan B (SaveBuffer-resync) loads + brings client to space anyway.
    // To deliver true cutscene-sync we'd need to additionally CALL the
    // success-path mutations - requires deeper RE. Plan B is the shipped
    // mechanism. See reference_takeoff_abort_gate.md for future work.
    //
    // InstallAbortGateHook();
}

// L.3 V1 - receive ship position update on network thread. Cannot write
// directly: engine REFR mutations from non-game-thread cause race-condition
// crashes with engine save-load (verified 2026-04-29 client crash @ 14:17:31
// during save-load: NULL+0x2E formType deref, r11=0x3F7E8 ship FormID).
//
// Instead: store latest pending in atomic struct. Game-thread drain
// (TickShipPosApply) writes when load-menu closed + ship valid.
struct PendingShipPos {
    std::atomic<bool>          armed{ false };
    std::atomic<std::uint32_t> shipFormID{ 0 };
    std::atomic<std::uint32_t> spaceCellFormID{ 0 };
    std::atomic<float>         posX{ 0 }, posY{ 0 }, posZ{ 0 };
    std::atomic<float>         rotX{ 0 }, rotY{ 0 }, rotZ{ 0 };
};
static PendingShipPos g_pendingShipPos;

// Diagnostic counters for receiver side.
static std::atomic<std::uint64_t> g_shipRecvCount{ 0 };
static std::atomic<std::uint64_t> g_shipApplyCount{ 0 };
static std::atomic<std::uint64_t> g_shipApplySkipNotInSpace{ 0 };
static std::atomic<std::uint64_t> g_shipApplySkipCellMismatch{ 0 };
static std::atomic<std::uint64_t> g_shipApplySkipObjRefNull{ 0 };
static std::atomic<std::uint64_t> g_lastRecvDiagTick{ 0 };

void OnShipPosReceived(const std::uint8_t* data, std::size_t length)
{
    if (!data || length < Pulsar::Network::HEADER_SIZE + sizeof(Pulsar::Network::ShipPosData)) {
        return;
    }
    Pulsar::Network::ShipPosData sd{};
    std::memcpy(&sd, data + Pulsar::Network::HEADER_SIZE, sizeof(sd));

    g_shipRecvCount.fetch_add(1, std::memory_order_release);

    // Just store - game-thread tick drains. Network-thread writes are
    // atomic stores, no engine-state access.
    g_pendingShipPos.shipFormID.store(sd.shipFormID, std::memory_order_release);
    g_pendingShipPos.spaceCellFormID.store(sd.spaceCellFormID, std::memory_order_release);
    g_pendingShipPos.posX.store(sd.posX, std::memory_order_release);
    g_pendingShipPos.posY.store(sd.posY, std::memory_order_release);
    g_pendingShipPos.posZ.store(sd.posZ, std::memory_order_release);
    g_pendingShipPos.rotX.store(sd.rotX, std::memory_order_release);
    g_pendingShipPos.rotY.store(sd.rotY, std::memory_order_release);
    g_pendingShipPos.rotZ.store(sd.rotZ, std::memory_order_release);
    g_pendingShipPos.armed.store(true, std::memory_order_release);
}

// Game-thread drain - apply pending ship pos with full safety gates.
//
// L.3 V1 crash analysis 2026-04-29 (88992e3e dump): engine called
// FUN_140B38840(ship) which reads ship+0x98 (objectReference NiPointer),
// then formType byte of pointed-to base. NULL-deref crash → ship's
// objectReference pointer was either uninitialized post-load OR our writes
// to wrong offsets corrupted it. Fix:
//   (a) write angle to +0x80 (NOT +0x98 which is objectReference)
//   (b) verify local ship's parentCell matches host's spaceCellFormID
//   (c) verify local player IsInSpace (don't apply space coords to planet ship)
//   (d) post-load settle window 5s (let engine finalize ship state)
static std::atomic<std::uint64_t> g_lastLoadCloseTick{ 0 };
constexpr std::uint64_t kPostLoadSettleMs = 5000;

static void TickShipPosApply()
{
    // L.3 V1 disabled - see TickShipPosSend comment above.
    // (kShipPosSyncEnabled is in the anon namespace above; accessible by
    // unqualified name in this translation unit.)
    if (g_pendingShipPos.armed.load(std::memory_order_acquire)) {
        g_pendingShipPos.armed.store(false, std::memory_order_release);
    }
    return;

    // Diagnostic flush every 5s, regardless of armed state.
    std::uint64_t now = ::GetTickCount64();
    std::uint64_t lastDiag = g_lastRecvDiagTick.load(std::memory_order_acquire);
    if ((now - lastDiag) > 5000) {
        g_lastRecvDiagTick.store(now, std::memory_order_release);
        std::uint64_t recv = g_shipRecvCount.exchange(0);
        std::uint64_t applied = g_shipApplyCount.exchange(0);
        std::uint64_t skipNotSpace = g_shipApplySkipNotInSpace.exchange(0);
        std::uint64_t skipCell = g_shipApplySkipCellMismatch.exchange(0);
        std::uint64_t skipObjRef = g_shipApplySkipObjRefNull.exchange(0);
        if (recv > 0 || applied > 0 || skipNotSpace > 0 || skipCell > 0 || skipObjRef > 0) {
            PLOG_INFO(
                "[PULSAR] L.3 ship-pos recv-diag: recv={} applied={} skip(notSpace={} cellMismatch={} objRefNull={})"sv,
                recv, applied, skipNotSpace, skipCell, skipObjRef);
        }
    }

    if (!g_pendingShipPos.armed.load(std::memory_order_acquire)) return;

    // Safety gates - prevent race during save-load.
    if (Pulsar::Cell3DAttach::IsQuiet() || Pulsar::Cell3DAttach::WasInLoadMenu()) {
        // Reset settle-tick when load menu detected.
        g_lastLoadCloseTick.store(::GetTickCount64(), std::memory_order_release);
        return;
    }
    if (IsStartupQuiet()) return;

    // Post-load settle: don't write to ship REFR for 5s after load-menu closes.
    // Engine finalizes ship objectReference + sub-state during this window.
    std::uint64_t closedAt = g_lastLoadCloseTick.load(std::memory_order_acquire);
    if (closedAt != 0 && (::GetTickCount64() - closedAt) < kPostLoadSettleMs) return;

    g_pendingShipPos.armed.store(false, std::memory_order_release);

    if (Pulsar::Network::GetSingleton().IsHost()) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    // Local must be in space too (avoid teleporting planet-ship to space coords).
    bool localInSpace = false;
    __try { localInSpace = player->IsInSpace(true); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!localInSpace) {
        g_shipApplySkipNotInSpace.fetch_add(1, std::memory_order_release);
        return;
    }

    RE::TESObjectREFR* ship = nullptr;
    __try { ship = player->GetSpaceship(true); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!ship) return;

    std::uint32_t expectedID = g_pendingShipPos.shipFormID.load(std::memory_order_acquire);
    std::uint32_t localShipID = 0;
    __try { localShipID = ship->formID; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (localShipID != expectedID) return;

    // Cell match - both ships in same space cell. If client is in different
    // cell (e.g., still loading or different space sector), skip.
    std::uint32_t expectedCell = g_pendingShipPos.spaceCellFormID.load(std::memory_order_acquire);
    std::uint32_t localShipCell = 0;
    __try {
        if (ship->parentCell) localShipCell = ship->parentCell->formID;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (expectedCell != 0 && localShipCell != expectedCell) {
        g_shipApplySkipCellMismatch.fetch_add(1, std::memory_order_release);
        return;
    }

    bool objRefValid = false;
    __try {
        std::uintptr_t objRef = *reinterpret_cast<std::uintptr_t*>(
            reinterpret_cast<std::uint8_t*>(ship) + 0x98);
        objRefValid = (objRef != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!objRefValid) {
        g_shipApplySkipObjRefNull.fetch_add(1, std::memory_order_release);
        return;
    }
    g_shipApplyCount.fetch_add(1, std::memory_order_release);

    // Write to data.location (+0x8C) + data.angle (+0x98).
    float px = g_pendingShipPos.posX.load(std::memory_order_acquire);
    float py = g_pendingShipPos.posY.load(std::memory_order_acquire);
    float pz = g_pendingShipPos.posZ.load(std::memory_order_acquire);
    float rx = g_pendingShipPos.rotX.load(std::memory_order_acquire);
    float ry = g_pendingShipPos.rotY.load(std::memory_order_acquire);
    float rz = g_pendingShipPos.rotZ.load(std::memory_order_acquire);
    // L.3 V1 iter - raw writes to data.location/angle DON'T trigger visual
    // update because client's local engine runs ship physics simulation that
    // overwrites our values each frame. Use SetPosition vt[0x89] which
    // propagates to physics body + mesh node + cell-attach.
    //
    // OBJ_REFR layout (verified): angle@+0x80..0x88, location@+0x8C..0x94.
    // Raw write angle (engine doesn't have a SetAngle equivalent that
    // propagates physics rotation simply - physics handles rot from body).
    __try {
        auto* p = reinterpret_cast<std::uint8_t*>(ship);
        // angle (rotation) at +0x80..+0x88
        *reinterpret_cast<float*>(p + 0x80) = rx;
        *reinterpret_cast<float*>(p + 0x84) = ry;
        *reinterpret_cast<float*>(p + 0x88) = rz;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // SetPosition vt[0x89]: void(this, NiPoint3*, bool). Propagates to
    // physics body + mesh node. Same call NPCSync uses for clone positioning.
    using SetPositionFn = void(__fastcall*)(RE::TESObjectREFR* this_, RE::NiPoint3A* pos, bool b);
    __try {
        auto** vt = *reinterpret_cast<void***>(ship);
        auto setPos = reinterpret_cast<SetPositionFn>(vt[0x89]);
        RE::NiPoint3A target(px, py, pz);
        setPos(ship, &target, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    if (!data || length < Pulsar::Network::HEADER_SIZE +
                            sizeof(Pulsar::Network::SpaceTransitionData)) {
        PLOG_WARN("[PULSAR] SpaceSync RECV: short packet ({} bytes)"sv, length);
        return;
    }
    Pulsar::Network::SpaceTransitionData td{};
    std::memcpy(&td, data + Pulsar::Network::HEADER_SIZE, sizeof(td));

    PLOG_INFO(
        "[PULSAR] SpaceSync RECV kind={} ({}) peer-cell=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) - peer ship event"sv,
        td.kind, KindStr(td.kind), td.playerCellFormID,
        td.posX, td.posY, td.posZ);
    NASLOG("SpaceSync: RECV kind=%u (%s) peer-cell=0x%08X",
           td.kind, KindStr(td.kind), td.playerCellFormID);

    // V2 - arm pending auto-follow if this kind is followable. Game-thread
    // TickFollowApply drains it.
    if (ShouldFollow(td.kind)) {
        g_pendingFollow.kind.store(td.kind, std::memory_order_release);
        g_pendingFollow.armTick.store(::GetTickCount64(), std::memory_order_release);
        g_pendingFollow.peerCellFormID.store(td.playerCellFormID, std::memory_order_release);
        g_pendingFollow.armed.store(true, std::memory_order_release);
        PLOG_INFO(
            "[PULSAR] SpaceSync auto-follow ARMED kind={} ({}) peer-cell=0x{:08X}"sv,
            td.kind, KindStr(td.kind), td.playerCellFormID);
    }

    // L.6 V3 Phase 3h (2026-05-02): peer TakeOff signals undock - clean
    // local hatch state. Replaces DockSync's own TakeOff sink which broke
    // FlightSync L.3 V3 ship sync (vt[8] hook on bhkSpaceship died).
    if (td.kind == kKind_TakeOff) {
        NASLOG("SpaceSync RECV TakeOff -> DockSync::OnRemoteTakeOff");
        Pulsar::DockSync::OnRemoteTakeOff();
        NASLOG("SpaceSync RECV TakeOff -> DockSync::OnRemoteTakeOff DONE");
    }
    NASLOG("SpaceSync OnPacketReceived EXIT kind=%u", td.kind);
}

void TickFollowApply()
{
    Pulsar::SpaceSync::FastTravel::Tick();

    // L.2 V1 Step A v2 - F8 hotkey: call engine's TakeoffMenu_Launch fire-
    // helper FUN_141679470 directly. Sets up proper lambda dispatcher
    // internally → Notify works correctly.
    {
        bool down = (::GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (down && !g_lastF8Down) {
            DebugFireTakeoffMenuLaunch();
        }
        g_lastF8Down = down;
    }

    // L.2 V1 Step 2 drain - if step1 fired the UI prompt, step2 fires the
    // Launch event after kStep2DelayMs so the engine's now-registered sink
    // can process it with valid state → cutscene + cell transition.
    {
        std::uint64_t due = g_step2DueTick.load(std::memory_order_acquire);
        if (due != 0 && ::GetTickCount64() >= due) {
            g_step2DueTick.store(0, std::memory_order_release);
            TriggerReceiverTakeoff_Step2();
        }
    }

    // L.2 V1 Plan B (HostTryTakeoffResync) DELETED 2026-05-04 V14.
    // Replaced by event-driven FireSaveBridge() call inside the
    // SpaceshipSink<TakeOff/Landing/Dock>::ProcessEvent above. The poll-
    // based flip detector never fired reliably (ship->parentCell stays
    // in cockpit interior across takeoff). The event sink fires
    // deterministically from the engine's own takeoff state machine.

    // host auto-fires the save-bridge when it enters space (auto-F8) so
    // the client co-locates into the host's space position without a manual F8.
    TickHostAutoSpaceBridge();

    // L.3 V1 - host broadcasts ship pos/rot at 30Hz while in space.
    TickShipPosSend();

    // L.3 V1 - game-thread drain of pending ship-pos (avoid network-thread
    // race-corruption observed on save-load).
    TickShipPosApply();

    // L.2 V2 JCC revert disabled (Apply not called). Code archived above.
    // L.6 V1/V2/V2.1/V2.2 (2026-05-02) - all dock-sync attempts removed; V3
    // plan = engine-state memory-diff. See memory/project_l6_dock_sync.md.

    if (!g_pendingFollow.armed.load(std::memory_order_acquire)) return;

    // L.2 V1 - for TakeoffMenu_Launch kind, fire engine's takeoff-helper
    // locally so OUR engine plays the cutscene. Both engines independently
    // process takeoff for the shared player ship REFR (0x0003F7E8 static),
    // arriving in same deterministic space cell.
    //
    // Boarding-gate: only trigger if local player is in cockpit cell. If
    // peer launched but we're not in ship, skip - we can't follow without
    // boarding (deferred to L.2 V2 if ever needed).
    std::uint8_t pendingKind = g_pendingFollow.kind.load(std::memory_order_acquire);

    // De-dupe per-kind window (engine fires multiple times per real action).
    std::uint64_t now = ::GetTickCount64();
    std::uint64_t last = g_lastApplyTick[pendingKind].load(std::memory_order_acquire);
    if ((now - last) < kApplyDedupeMs) {
        g_pendingFollow.armed.store(false, std::memory_order_release);
        return;
    }

    // Don't fire during loadMenu / cell-stream churn.
    if (Pulsar::Cell3DAttach::IsQuiet() || Pulsar::Cell3DAttach::WasInLoadMenu()) {
        return;  // keep armed, retry next tick
    }

    // Disarm now so we don't re-fire even if action hiccups.
    g_pendingFollow.armed.store(false, std::memory_order_release);

    if (pendingKind != kKind_TakeoffMenuLaunch) {
        // Only TakeoffMenuLaunch wired for L.2 V1. TakeOff/Landing/etc
        // events are passive notifications - engine state already changed
        // on host, our engine will catch up via the natural takeoff flow
        // we triggered from TakeoffMenu_Launch.
        PLOG_INFO("[PULSAR] SpaceSync apply: kind={} ({}) - passive (no action)"sv,
                  pendingKind, KindStr(pendingKind));
        return;
    }

    // Boarding-gate: must be in cockpit interior to trigger takeoff.
    auto* localPlayer = RE::PlayerCharacter::GetSingleton();
    if (!localPlayer || !localPlayer->parentCell) {
        PLOG_WARN("[PULSAR] SpaceSync apply: TakeoffMenu_Launch - no player/cell, skip"sv);
        return;
    }
    bool inCockpit = false;
    __try {
        // 0x05100136 is the static cockpit cell FormID. cellFlags bit 0 = interior.
        std::uint32_t cellID = localPlayer->parentCell->formID;
        std::uint32_t cellFlags = *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<std::uint8_t*>(localPlayer->parentCell) + 0x48);
        // Either explicit cockpit ID or any interior cell with shipCellReference
        // (we'll only act on confirmed cockpit for safety).
        inCockpit = (cellID == 0x05100136) && ((cellFlags & 0x01) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (!inCockpit) {
        PLOG_INFO("[PULSAR] SpaceSync apply: TakeoffMenu_Launch - local player not in cockpit, skip"sv);
        NASLOG("SpaceSync: takeoff-recv skip - not boarded");
        return;
    }

    g_lastApplyTick[pendingKind].store(now, std::memory_order_release);
    PLOG_INFO("[PULSAR] SpaceSync apply: TakeoffMenu_Launch - firing local cutscene"sv);
    NASLOG("SpaceSync: takeoff-recv firing local engine takeoff");
    TriggerReceiverTakeoff();
}

}  // namespace Pulsar::SpaceSync
