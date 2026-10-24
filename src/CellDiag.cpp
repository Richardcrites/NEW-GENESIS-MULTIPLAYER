// Diagnostic-only: trace area-transition behavior in co-op.
// Logs cell changes, LoadingMenu open/close, and (on client) clone-vs-
// player parentCell mismatches. Read-only - no packets sent.
//
// Goal: see what actually happens when host walks through a door,
// fast-travels, lands on a planet, or streams an outpost cell. Outputs
// give us the empirical baseline before designing PKT_CELL_LOAD or
// related coordination packets.

#include "PCH.h"
#include "CellDiag.h"

#include "Network/NetworkHost.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "WorldSync/WorldSync.h"
#include "Forms/SaveBuffer.h"
#include "Cell3DAttach.h"
#include "TransitSync/TransitSync.h"
#include "Network/NetworkHost.h"

#include <windows.h>

#include <REL/Relocation.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSScriptUtil.h>
#include <RE/G/GameVM.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>

using namespace std::string_view_literals;

namespace Pulsar::CellDiag
{
    namespace
    {
        struct State {
            uint32_t lastCellID         = 0;
            bool     lastInLoadMenu     = false;
            uint32_t lastCloneCellID    = 0;
            uint64_t lastSnapshotTick   = 0;
            uint64_t lastMismatchLogTick = 0;
            // Sprint 1: clone-respawn-on-cell-change. After a player
            // cell-change, schedule a check at +3s. If clone's 3D is
            // still detached at check time, force-respawn via Papyrus
            // pipeline. Engine doesn't auto-reattach 3D after position
            // updates from network - only at cell-load streaming.
            uint64_t pendingRespawnCheck = 0;
            uint64_t lastForceRespawnTick = 0;
            uint64_t postRespawnProbeTick = 0;
            // per-second respawn-recovery TRACE. Armed when a respawn/reparent
            // fires; samples clone refID/loadedData/data3D/children every ~1s until the
            // clone is genuinely visible (data3D + biped children>0) or 16s timeout.
            // Diagnostic only - measures the EXACT empty-mesh window the user perceives
            // (logs show ~5s but user reports 12-14s; this pins the true bottleneck).
            uint64_t respawnTraceStart = 0;   // tick the respawn fired (0 = not tracing)
            uint64_t respawnTraceNext  = 0;   // next per-second sample tick
            // debounced clone re-parent respawn: armed on each proc-cell
            // change, fires once cells settle (player stopped). 0 = not armed.
            uint64_t procReparentDueAt = 0;
            // Fix B: consecutive empty-NiNode detections. The destructive
            // ForceRespawn (2s gone-gap) only fires once this reaches the streak
            // threshold - transient empties (engine sheds biped under load, then
            // re-attaches within a few seconds) self-heal first and never respawn.
            int      emptyNiNodeStreak = 0;
            // Sprint 3: stable-state save probe - fire once per session
            // when player has been in real cell + no loadingMenu for 30s.
            uint64_t lastUnstableTick = 0;       // last time something changed
            bool     customSaveProbeFired = false;
            // Phase 4: deferred FormBlob broadcast. Set when loadMenu CLOSED
            // detected on host; fires after kFormBlobDelayMs to let engine's
            // own auto-quicksave finish first (verified by user: every cell
            // transition triggers an engine quicksave).
            uint64_t pendingFormBroadcast = 0;
            // Last MoveTo-refresh tick - debounce empty-NiNode fix attempts.
            uint64_t lastMoveRefreshTick = 0;
            // Phase K.1: snapshot of cell + worldspace at moment loadMenu
            // OPENED, so when it CLOSES we know the source-side of the
            // transit and can broadcast a coherent PKT_TRANSIT.
            uint32_t cellAtLoadMenuOpen     = 0;
            uint32_t wsAtLoadMenuOpen       = 0;
            bool     pendingTransitOnClose  = false;
        };
        State s_hostState;
        State s_client;
        constexpr uint64_t kRespawnCheckDelayMs = 3000;
        constexpr uint64_t kRespawnCooldownMs   = 10000;
        ///proc-cell surfaces shed the clone's 3D on every cell-stream
        // (walking proc tiles). The 10s cooldown can't keep up → clone stays
        // invisible. Use a much shorter cooldown for proc-equivalent cells so it
        // recovers in ~2-3s (esp. when the player stops at a POI).
        constexpr uint64_t kProcRespawnCooldownMs = 3500;
        // settle window: fire the clone re-parent respawn only after the
        // player has stopped crossing proc-cells for this long (kills walk-flicker).
        // lowered 1500->800: now that a still-visible clone returns kVisible
        // (a cheap no-op) instead of respawning, checking sooner is free and only
        // SPEEDS recovery of a genuinely dead clone (the rare data3D==0 teardown).
        constexpr uint64_t kProcReparentDebounceMs = 800;
        // despawn->spawn settle gap for the proc-cell-WALK recovery respawn.
        // Far shorter than the 2000ms default so the rare 3D-teardown reappears fast.
        // Save-load / stale-state respawns keep the safe 2000ms default (they need
        // the longer settle; that path is the heavy save-load case, not walk-flicker).
        constexpr uint32_t kProcRespawnSpawnDelayMs = 750;
        // Fix B: empty-NiNode must persist across this many consecutive
        // checks (~kRespawnCheckDelayMs apart) before the destructive respawn fires.
        constexpr int      kEmptyNiNodeStreak   = 2;
        // Phase 4: settle-time after loadMenu CLOSED before we broadcast our
        // form blob. Engine's auto-quicksave runs DURING the loading screen
        // and shortly after it closes; we wait long enough for it to fully
        // complete before mutating any save-adjacent state.
        constexpr uint64_t kFormBlobDelayMs     = 3000;
        // Cooldown between MoveTo-refresh attempts on the same clone, so a
        // failed refresh doesn't spam every tick.
        constexpr uint64_t kMoveRefreshCooldownMs = 8000;

        // SEH-wrapped raw teammate-flag set. C2712 forbids __try in functions
        // with C++ unwinding objects; this helper is noexcept-safe.
        // Sets bit 26 (kPlayerTeammate) of Actor.boolBits at +0x208.
        static bool SafeSetTeammate(RE::Actor* clone,
                                     std::uint32_t& outPrev,
                                     std::uint32_t& outNew) noexcept
        {
            __try {
                auto* p = reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(clone) + 0x208);
                outPrev = *p;
                outNew  = outPrev | (1u << 26);
                *p = outNew;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Dispatch PulsarFurnSync.DoMoveRefresh(clone, target) - engine's
        // cell-attach pipeline is invoked by MoveTo with abCopyCell=true and
        // re-streams 3D including biped attach. Workaround for empty NiNode
        // bug after PlaceAtMe-respawn during cell-transitions.
        static bool CallPapyrusMoveRefresh(RE::TESObjectREFR* ref,
                                            RE::TESObjectREFR* target)
        {
            if (!ref || !target) return false;
            auto* gameVM = RE::GameVM::GetSingleton();
            if (!gameVM) return false;
            auto* vm = gameVM->GetVM();
            if (!vm) return false;

            RE::BSScript::Variable refVar, tgtVar;
            RE::BSScript::PackVariable(refVar, ref);
            RE::BSScript::PackVariable(tgtVar, target);

            auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
                args.resize(2);
                args[0] = refVar;
                args[1] = tgtVar;
                return true;
            };

            RE::BSFixedString objName("PulsarFurnSync");
            RE::BSFixedString funcName("DoMoveRefresh");
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            return vm->DispatchStaticCall(objName, funcName, argFn, callback, 0);
        }

        // SEH-wrapped raw read of player's cellID + position. No C++ object
        // creation inside __try (C2712).
        struct RawSnap { uint32_t cellID; float x, y, z; bool valid; };
        RawSnap RawSnapPlayer(RE::PlayerCharacter* player) noexcept
        {
            RawSnap r{};
            __try {
                if (!player) return r;
                if (player->parentCell) {
                    r.cellID = player->parentCell->formID;
                }
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(player) + 0x8C);
                r.x = loc[0]; r.y = loc[1]; r.z = loc[2];
                r.valid = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return r;
        }

        // SEH-wrapped IsMenuOpen - caller provides the BSFixedString to
        // avoid temp-with-dtor inside __try.
        bool RawCheckMenuOpen(RE::UI* ui, const RE::BSFixedString& name) noexcept
        {
            __try {
                if (!ui) return false;
                return ui->IsMenuOpen(name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // SEH-wrapped raw read of remote-clone's cellID. Returns 0 if no
        // clone or unreadable.
        uint32_t RawSnapCloneCell(RE::Actor* clone) noexcept
        {
            __try {
                if (!clone || !clone->parentCell) return 0;
                return clone->parentCell->formID;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // SEH-wrapped probe of NiNode children. Walks data3D as NiNode
        // (BSTArray<NiAVObject*> at +0x130) and logs first N child names.
        // Used post-respawn to see if biped/skeleton actually attached.
        struct NodeInfo {
            int      childCount;
            uint64_t firstChildPtr;
            uint64_t firstChildName;
            uint64_t flags;
        };
        NodeInfo RawProbeNiNodeChildren(uint8_t* data3D, char* outNames, int outNamesSize) noexcept
        {
            NodeInfo n{};
            if (!data3D || !outNames || outNamesSize < 16) return n;
            outNames[0] = '\0';
            __try {
                // NiNode at offset +0x130: BSTArray<NiAVObject*>
                //   +0x00: data ptr
                //   +0x08: capacity (uint32)
                //   +0x0C: size (uint32)
                auto* arr = data3D + 0x130;
                uint8_t** dataPtr = *reinterpret_cast<uint8_t***>(arr);
                uint32_t size = *reinterpret_cast<uint32_t*>(arr + 0x0C);
                // root-cause (2026-06-21): the old cap `size > 200` was FAR
                // too low - a healthy actor's root NiNode has ~650-700 children
                // (verified: 647 host / 699 client). Any healthy clone tripped
                // the cap and returned the default childCount=0, which CellDiag
                // read as "empty-NiNode" → pre-Fix-C this drove the destructive
                // respawn loop (the actual despawn bug); post-Fix-C it still
                // fired a no-op ManualAttachBiped (faulting engine call) every
                // ~3s. Raise to a garbage-only bound so real counts pass through.
                // A garbage non-zero size is the SAFE direction anyway (treated
                // as healthy → no destructive action); only a false ZERO is
                // dangerous, which this fixes.
                if (!dataPtr || size > 65535) return n;
                n.childCount = (int)size;
                n.flags = *reinterpret_cast<uint64_t*>(data3D + 0x118);
                int written = 0;
                for (uint32_t i = 0; i < size && i < 8; ++i) {
                    uint8_t* child = dataPtr[i];
                    if (!child) {
                        written += std::snprintf(outNames + written,
                            outNamesSize - written, "[NULL] ");
                        continue;
                    }
                    if (i == 0) n.firstChildPtr = (uint64_t)child;
                    // NiObjectNET name at +0x18 - points to const char*
                    auto* namePtr = *reinterpret_cast<char**>(child + 0x18);
                    if (namePtr) {
                        if (i == 0) n.firstChildName = (uint64_t)namePtr;
                        // Cap name to 30 chars to fit
                        char tmp[40] = {};
                        for (int k = 0; k < 30 && namePtr[k]; ++k) tmp[k] = namePtr[k];
                        written += std::snprintf(outNames + written,
                            outNamesSize - written, "%s ", tmp);
                    } else {
                        written += std::snprintf(outNames + written,
                            outNamesSize - written, "[NN-anon] ");
                    }
                    if (written > outNamesSize - 50) break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
            return n;
        }

        // SEH-wrapped peek into clone's 3D-load state. Returns:
        //   loadedDataPtr  - refr+0xB8 (BSGuarded ptr)
        //   data3DPtr      - *(loadedData+0x08) (NiAVObject*) - null if 3D
        //                    detached after cell stream-out
        //   cloneRefID     - clone's REFR FormID
        //   posX/posY/posZ - clone's data.location (raw +0x8C)
        struct CloneVisual {
            uintptr_t loadedDataPtr;
            uintptr_t data3DPtr;
            uint32_t  refID;
            float     x, y, z;
        };
        CloneVisual RawSnapCloneVisual(RE::Actor* clone) noexcept
        {
            CloneVisual v{};
            __try {
                if (!clone) return v;
                v.refID = *reinterpret_cast<uint32_t*>(
                    reinterpret_cast<uint8_t*>(clone) + 0x28);
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(clone) + 0x8C);
                v.x = loc[0]; v.y = loc[1]; v.z = loc[2];
                v.loadedDataPtr = *reinterpret_cast<uintptr_t*>(
                    reinterpret_cast<uint8_t*>(clone) + 0xB8);
                if (v.loadedDataPtr) {
                    v.data3DPtr = *reinterpret_cast<uintptr_t*>(
                        v.loadedDataPtr + 0x08);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return v;
        }
    }

    void Tick(bool isHost)
    {
        State& s = isHost ? s_hostState : s_client;
        const char* tag = isHost ? "HOST" : "CLIENT";

        auto* player = RE::PlayerCharacter::GetSingleton();
        RawSnap p = RawSnapPlayer(player);
        if (!p.valid) return;

        // LoadingMenu poll - BSFixedString constructed once per call but
        // outside the __try.
        RE::BSFixedString loadingMenuName("LoadingMenu");
        auto* ui = RE::UI::GetSingleton();
        bool inLoadMenu = RawCheckMenuOpen(ui, loadingMenuName);

        // Transition events
        if (p.cellID != s.lastCellID) {
            PLOG_INFO(
                "[PULSAR] CELL: {} cell-change 0x{:08X} -> 0x{:08X} pos=({:.0f},{:.0f},{:.0f}) loadMenu={}"sv,
                tag, s.lastCellID, p.cellID, p.x, p.y, p.z,
                inLoadMenu ? "YES" : "no");
            s.lastCellID = p.cellID;
            // Sprint 1: schedule clone-3D check after settle-time
            s.pendingRespawnCheck = ::GetTickCount64() + kRespawnCheckDelayMs;
            ///(2026-06-21) - clone RE-PARENT on proc-cell-change.
            // PROVEN: a clone moved across proc-cells via DoSetPosition keeps its
            // data3D + full biped (children=593) but is ORPHANED from the new
            // cell's render scene-graph → INVISIBLE, while NPCs (engine-loaded)
            // render fine. Only a fresh PlaceAtMe respawn re-parents it into the
            // scene. The data3D==0 respawn path does NOT fire here (data3D stays
            // present), so standing still after a transition = permanently
            // invisible until the next cell-change happens to drop data3D. Fix:
            // force a respawn on every proc-cell-change (proc cooldown prevents
            // storms while walking; when the player stops, the last respawn makes
            // the clone visible). NOTE: still flickers ~2s/respawn while walking -
            // the no-flicker fix is a surgical 3D-reparent (future).
            {
                const bool newIsProc = (p.cellID & 0xFF000000u) == 0xFF000000u;
                if (newIsProc) {
                    // DEBOUNCE (not immediate): arm the re-parent respawn 1.5s
                    // after the LAST proc-cell-change. While the player is WALKING
                    // (rapid proc-cell crosses) this keeps re-arming and never
                    // fires → no respawn flicker. When the player STOPS (cells
                    // settle for 1.5s), it fires ONCE → the orphaned clone is
                    // re-parented and becomes visible. Fixes "sehr häufig"
                    // (respawn on every tile) while keeping the standing-still fix.
                    s.procReparentDueAt = ::GetTickCount64() + kProcReparentDebounceMs;
                }
            }
            // Phase H: arm Cell3DAttach fallback for this transition.
            Pulsar::Cell3DAttach::OnPlayerCellChange();
        }

        // fire the debounced clone re-parent respawn once proc-cells have
        // SETTLED (player stopped crossing tiles), gated by the proc cooldown.
        // never act during a load: the load itself rebuilds the clone, and a
        // respawn fired into a load collides with it (the observed ~16s recovery where
        // the first spawn loaded no 3D). The post-load path (LOAD_COMPLETE) covers it.
        if (s.procReparentDueAt != 0 && inLoadMenu) {
            s.procReparentDueAt = 0;  // drop it; re-armed on the next cell-change
        }
        if (s.procReparentDueAt != 0) {
            const uint64_t nowMs = ::GetTickCount64();
            if (nowMs >= s.procReparentDueAt) {
                s.procReparentDueAt = 0;
                if ((nowMs - s.lastForceRespawnTick) > kProcRespawnCooldownMs) {
                    // only act if the clone is GENUINELY orphaned. A visible
                    // clone (3D still parented in the loaded grid) is LEFT ALONE - it
                    // survives across cells on its own, so no respawn, no flicker. An
                    // orphaned clone is MoveTo-reparented (no despawn). Only a truly
                    // dead clone (no 3D) gets the PlaceAtMe respawn.
                    using R = Pulsar::RemotePlayer::RemotePlayerManager::CloneReparentResult;
                    auto& mgr = Pulsar::RemotePlayer::GetRemotePlayerManager();
                    switch (mgr.ReparentCloneIfOrphaned()) {
                        case R::kVisible:
                            // still visible - nothing to do; do NOT burn the cooldown.
                            break;
                        case R::kReparented:
                            PLOG_INFO(
                                "[PULSAR] CELL: {} clone was orphaned → MoveTo-reparent "
                                "(no respawn) cell=0x{:08X} []"sv, tag, p.cellID);
                            s.lastForceRespawnTick = nowMs;
                            s.postRespawnProbeTick = nowMs + 5000;
                            s.respawnTraceStart = nowMs;  // trace
                            s.respawnTraceNext  = nowMs;
                            break;
                        case R::kDead:
                            PLOG_INFO(
                                "[PULSAR] CELL: {} clone dead (no 3D) → respawn "
                                "cell=0x{:08X} (fast {}ms)"sv, tag, p.cellID,
                                kProcRespawnSpawnDelayMs);
                            // proc-walk teardown: short settle so it reappears fast.
                            mgr.RequestForceRespawn(kProcRespawnSpawnDelayMs);
                            s.lastForceRespawnTick = nowMs;
                            s.postRespawnProbeTick = nowMs + 5000;
                            s.respawnTraceStart = nowMs;  // trace
                            s.respawnTraceNext  = nowMs;
                            break;
                    }
                }
            }
        }
        if (inLoadMenu != s.lastInLoadMenu) {
            PLOG_INFO(
                "[PULSAR] CELL: {} loadMenu {} cell=0x{:08X} pos=({:.0f},{:.0f},{:.0f})"sv,
                tag,
                inLoadMenu ? "OPENED" : "CLOSED",
                p.cellID, p.x, p.y, p.z);
            // Pivot C ABANDONED 2026-04-26 after iter5. HookGuard worked
            // (SUSPENDED log verified at correct timing) but crash still
            // happened - UAF source is NOT in our hooked functions. Engine-
            // internal during cell-stream chaos, can't be guarded externally.
            // Pivoting to Companion/Follower approach: make the clone a
            // companion of the host player. Engine's own follower-on-transition
            // logic handles cell transitions properly (Sarah, Sam Coe etc work).
            // if (isHost && !inLoadMenu && s.lastInLoadMenu) {
            //     s.pendingFormBroadcast = ::GetTickCount64() + kFormBlobDelayMs;
            // }
            (void)kFormBlobDelayMs;

            // Phase K.1 - snapshot source-side at OPEN, fire broadcast at CLOSE.
            if (inLoadMenu && !s.lastInLoadMenu) {
                s.cellAtLoadMenuOpen = p.cellID;
                if (player && player->parentCell && player->parentCell->cellWorldspace) {
                    s.wsAtLoadMenuOpen = player->parentCell->cellWorldspace->formID;
                } else {
                    s.wsAtLoadMenuOpen = 0;
                }
                s.pendingTransitOnClose = true;
            } else if (!inLoadMenu && s.lastInLoadMenu && s.pendingTransitOnClose) {
                s.pendingTransitOnClose = false;
                if (player && player->parentCell) {
                    auto* pc = player->parentCell;
                    uint32_t newCell = pc->formID;
                    uint32_t newWS   = pc->cellWorldspace ? pc->cellWorldspace->formID : 0u;
                    bool destInterior = pc->IsInterior();
                    // Phase K.6 (2026-04-27) - fire transit on EITHER cell
                    // change OR worldspace change. Takeoff/landing keep the
                    // player in the same cockpit cell (e.g. 0x05100136) but
                    // change worldspace (planet ↔ space). Without the WS
                    // check we'd never broadcast takeoff/landing transits.
                    bool cellChanged = (newCell != s.cellAtLoadMenuOpen);
                    bool wsChanged   = (newWS != s.wsAtLoadMenuOpen);
                    if (cellChanged || wsChanged) {
                        Pulsar::TransitSync::OnLocalTransit(
                            s.cellAtLoadMenuOpen, newCell,
                            s.wsAtLoadMenuOpen, newWS,
                            p.x, p.y, p.z,
                            player->data.angle.z,
                            destInterior);
                    }
                }
            }

            s.lastInLoadMenu = inLoadMenu;
        }

        // Clone tracking - symmetric on both sides (each side has a clone
        // representing the other player). Logs cell-changes and 3D state.
        auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        uint32_t cloneCellID = RawSnapCloneCell(clone);

        if (cloneCellID != s.lastCloneCellID) {
            CloneVisual cv = RawSnapCloneVisual(clone);
            PLOG_INFO(
                "[PULSAR] CELL: {} CLONE cell-change 0x{:08X} -> 0x{:08X} (player cell=0x{:08X}) refID=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) loadedData=0x{:x} data3D=0x{:x}"sv,
                tag, s.lastCloneCellID, cloneCellID, p.cellID,
                cv.refID, cv.x, cv.y, cv.z,
                cv.loadedDataPtr, cv.data3DPtr);
            s.lastCloneCellID = cloneCellID;
            // Sprint 1: schedule auto-respawn check on CLONE cell-change too
            // (covers case where peer transitioned but local player did not).
            s.pendingRespawnCheck = ::GetTickCount64() + kRespawnCheckDelayMs;
            // Phase H: also arm Cell3DAttach when the CLONE itself moves cell.
            Pulsar::Cell3DAttach::OnPlayerCellChange();
        }

        // Phase H: per-tick driver for the 3D-attach fallback. Cheap (no-op
        // unless armed). Runs after cellID checks so OnPlayerCellChange has
        // already fired on transition ticks.
        Pulsar::Cell3DAttach::Tick(isHost);
        if (cloneCellID != 0 && p.cellID != 0 && cloneCellID != p.cellID) {
            uint64_t mismatchNow = ::GetTickCount64();
            if (mismatchNow - s.lastMismatchLogTick > 2000) {
                s.lastMismatchLogTick = mismatchNow;
                PLOG_INFO(
                    "[PULSAR] CELL: {} mismatch cloneCell=0x{:08X} != playerCell=0x{:08X}"sv,
                    tag, cloneCellID, p.cellID);
            }
        }

        uint64_t now = ::GetTickCount64();

        // Pivot C: deferred save-resync. loadMenu CLOSED on host set the
        // pendingFormBroadcast tick; once it elapses AND we're truly out
        // of loading state, send the engine's just-completed auto-quicksave
        // to the client. Client auto-loads via existing PKT_SAVE_TRANSFER
        // handler → lands in host's new cell → clone visible naturally.
        if (s.pendingFormBroadcast != 0 && now >= s.pendingFormBroadcast) {
            s.pendingFormBroadcast = 0;
            if (isHost && !inLoadMenu &&
                Pulsar::Network::GetSingleton().IsConnected())
            {
                PLOG_INFO(
                    "[PULSAR] CELL: HOST cell-transition resync - sending newest save to client"sv);
                Pulsar::Network::GetSingleton().SendSaveToClient();
            }
        }

        // Sprint 1: clone-respawn-on-cell-change. Restored to pre-teammate-
        // experiment behavior. Empty NiNode bug after rein-transition is
        // accepted as known limitation; respawn ensures at least the SAME
        // CELL coop works reliably.
        //
        // Phase I (2026-04-27): SKIP auto-respawn when remote player is in
        // a different parentCell (procedural-cell instances differ per side
        // after rein). Spawning a clone in the local cell when the remote
        // player isn't here is meaningless and just feeds the cell-flip
        // pingpong loop (verified from 20:25/20:26 logs). When cells match
        // again (both back outdoors etc.) the next pendingRespawnCheck
        // triggers and the clone returns.
        if (s.pendingRespawnCheck != 0 && now >= s.pendingRespawnCheck) {
            s.pendingRespawnCheck = 0;
            // 2026-04-29: gate clone-respawn entirely on network-connected.
            // When no client is connected, there's no remote player to mirror;
            // forcing a respawn just creates an orphan clone REFR that
            // CellDiag keeps trying to fix forever. Verified from logs after
            // host crashed solo: empty-NiNode → respawn → empty NiNode loop
            // running with no peer present.
            if (!inLoadMenu &&
                Pulsar::Network::GetSingleton().IsConnected()) {
                CloneVisual cv = RawSnapCloneVisual(clone);
                // Phase K.2 V4 (2026-04-27) - also respawn when data3D
                // exists but NiNode has no children (= empty-NiNode bug).
                // Without this, cell-transition leaves a "ghost" clone:
                // valid pointer, no biped → invisible avatar, makes peer
                // think they're alone. Verified asymmetric visibility
                // after manual rein/raus follow.
                bool needsRespawnNullData = clone && cv.loadedDataPtr == 0 &&
                                              cv.data3DPtr == 0;
                bool needsRespawnEmptyNode = false;
                if (clone && cv.data3DPtr != 0) {
                    char names[64] = {};
                    NodeInfo ni = RawProbeNiNodeChildren(
                        reinterpret_cast<uint8_t*>(cv.data3DPtr),
                        names, sizeof(names));
                    if (ni.childCount == 0) {
                        needsRespawnEmptyNode = true;
                        PLOG_INFO(
                            "[PULSAR] CELL: {} empty-NiNode detected refID=0x{:08X} data3D=0x{:x} children=0"sv,
                            tag, cv.refID, cv.data3DPtr);
                    }
                }
                uint32_t remoteCell = Pulsar::RemotePlayer::GetRemotePlayerManager()
                                          .GetRemoteCellID();
                // Phase I - only enforce cell-match in INTERIOR cells.
                // Exterior cells of same worldspace are forgiving.
                auto* localPlayer = RE::PlayerCharacter::GetSingleton();
                auto* localParent = localPlayer ? localPlayer->parentCell : nullptr;
                bool isInterior = localParent && localParent->IsInterior();
                // Proc-cells (FF-prefix) are per-machine instances of the
                // same template. K.6 V2-Probe (2026-04-29) verified content
                // is identical across instances (matched=120/120). So if
                // BOTH local and remote are FF-prefix, treat them as
                // equivalent for respawn purposes - clone gets placed in
                // local cell at peer's coords, engine attaches 3D from the
                // local instance's content.
                const bool localIsProc  = (p.cellID    & 0xFF000000u) == 0xFF000000u;
                const bool remoteIsProc = (remoteCell  & 0xFF000000u) == 0xFF000000u;
                const bool procEquivalent = localIsProc && remoteIsProc;

                bool cellMatch = !isInterior || (remoteCell == 0) ||
                                 (remoteCell == p.cellID) ||
                                 procEquivalent;
                bool cooldownOK = (now - s.lastForceRespawnTick) > kRespawnCooldownMs;

                // Fix C (2026-06-21): the empty-NiNode is PERSISTENT under combat
                // load, not transient - each destructive respawn just makes a fresh
                // EMPTY clone (refID churns E06->E98->2A24->..., biped never attaches)
                // → the clone is invisible the whole time = "despawnt alle paar Sekunden".
                // Root: the biped fails to attach to the proc-cell clone's 3D. The
                // engine's AttachBiped (slot 0x13) attaches a biped to an EXISTING 3D
                // without rebuilding it. So split by what's actually missing:
                //   * data3D present, children==0  -> ManualAttachBiped IN-PLACE (no
                //     despawn, no flicker). The destructive respawn was COUNTER-
                //     productive - it destroyed the clone before the biped could attach.
                //   * data3D == 0 (no 3D at all)   -> respawn (PlaceAtMe builds 3D),
                //     debounced so transients don't flicker.
                if (needsRespawnEmptyNode) {
                    s.emptyNiNodeStreak = 0;            // not a respawn case
                    if (cellMatch) {
                        PLOG_INFO(
                            "[PULSAR] CELL: {} empty-NiNode (3D present) refID=0x{:08X} "
                            "-> ManualAttachBiped in-place (no respawn)"sv,
                            tag, cv.refID);
                        Pulsar::Cell3DAttach::ManualAttachBiped(clone);
                    }
                    // Re-check - the attach may need a couple of retries; once children>0
                    // the next pass falls through to the healthy branch and stops.
                    s.pendingRespawnCheck = now + kRespawnCheckDelayMs;
                } else if (needsRespawnNullData) {
                    s.emptyNiNodeStreak++;
                    ///on procedural surfaces the clone loses its 3D on
                    // EVERY cell-stream; the default 2-streak + 10s cooldown recovery
                    // can't keep up → clone perpetually invisible while walking POIs.
                    // For proc-equivalent cells, respawn on the FIRST no-3D detection
                    // with a short cooldown so it reappears fast (esp. when stopped).
                    const int      streakReq = procEquivalent ? 1 : kEmptyNiNodeStreak;
                    const uint64_t cdMs      = procEquivalent ? kProcRespawnCooldownMs
                                                              : kRespawnCooldownMs;
                    const bool     cdOK      = (now - s.lastForceRespawnTick) > cdMs;
                    if (cellMatch && cdOK && s.emptyNiNodeStreak >= streakReq) {
                        PLOG_INFO(
                            "[PULSAR] CELL: {} auto-respawn clone refID=0x{:08X} "
                            "(no 3D for {} checks, cell=0x{:08X}, proc={})"sv,
                            tag, cv.refID, s.emptyNiNodeStreak, p.cellID,
                            procEquivalent ? 1 : 0);
                        // proc-equivalent (walk teardown) reappears fast; a
                        // non-proc/cross-cell case keeps the safe 2000ms default settle.
                        Pulsar::RemotePlayer::GetRemotePlayerManager().RequestForceRespawn(
                            procEquivalent ? kProcRespawnSpawnDelayMs : 2000u);
                        s.lastForceRespawnTick = now;
                        s.postRespawnProbeTick = now + 5000;
                        s.respawnTraceStart    = now;   // trace
                        s.respawnTraceNext     = now;
                        s.emptyNiNodeStreak    = 0;
                    } else {
                        s.pendingRespawnCheck = now + kRespawnCheckDelayMs;
                        if (!cellMatch) {
                            PLOG_INFO(
                                "[PULSAR] CELL: {} respawn deferred - cross-cell "
                                "(local=0x{:08X} remote=0x{:08X})"sv,
                                tag, p.cellID, remoteCell);
                        } else if (!cdOK) {
                            PLOG_INFO(
                                "[PULSAR] CELL: {} respawn deferred (cooldown {}ms remaining)"sv,
                                tag, cdMs - (now - s.lastForceRespawnTick));
                        } else {
                            PLOG_INFO(
                                "[PULSAR] CELL: {} no-3D streak {}/{} - waiting"sv,
                                tag, s.emptyNiNodeStreak, streakReq);
                        }
                    }
                } else {
                    s.emptyNiNodeStreak = 0;            // healthy (biped attached)
                }
            } else {
                s.pendingRespawnCheck = now + kRespawnCheckDelayMs;
            }
        }

        // per-second respawn-recovery TRACE (diagnostic only). Samples the
        // clone's refID/loadedData/data3D/children every ~1s from the moment a respawn
        // fired until it is genuinely visible (data3D + children>0) or 16s timeout.
        // This measures the TRUE empty-mesh window (5s-spaced snapshots can't resolve
        // it). Tells us whether the bottleneck is the Papyrus spawn (refID stays 0),
        // the 3D build (data3D stays 0), or the biped attach (children stays 0).
        if (s.respawnTraceStart != 0 && now >= s.respawnTraceNext) {
            s.respawnTraceNext = now + 1000;
            const uint64_t elapsed = now - s.respawnTraceStart;
            CloneVisual cvt = RawSnapCloneVisual(clone);
            int childCount = -1;
            if (cvt.data3DPtr != 0) {
                char nm[64] = {};
                NodeInfo ni = RawProbeNiNodeChildren(
                    reinterpret_cast<uint8_t*>(cvt.data3DPtr), nm, sizeof(nm));
                childCount = ni.childCount;
            }
            const bool visible = (cvt.data3DPtr != 0 && childCount > 0);
            PLOG_INFO(
                "[PULSAR] CELL: {} [RESPAWN-TRACE] +{}ms refID=0x{:08X} loadedData=0x{:x} "
                "data3D=0x{:x} children={} {}"sv,
                tag, elapsed, cvt.refID, cvt.loadedDataPtr, cvt.data3DPtr, childCount,
                visible ? "VISIBLE" : "(empty)");
            if (visible || elapsed > 16000) {
                PLOG_INFO("[PULSAR] CELL: {} [RESPAWN-TRACE] END +{}ms ({})"sv,
                          tag, elapsed, visible ? "visible" : "TIMEOUT");
                s.respawnTraceStart = 0;
            }
        }

        // Post-respawn 3D inspector - fires once at +5s after auto-respawn.
        // Logs NiNode children of clone's data3D so we see whether biped /
        // skeleton attached or only root NiNode is present.
        //
        // ALSO sets the engine's kPlayerTeammate flag (BOOL_BITS bit 26 at
        // Actor+0x208) on the freshly-spawned clone. Theory (per user
        // observation): Sarah / Sam Coe / Andreja get carried into new cells
        // automatically by engine's follower-on-transition logic. That logic
        // identifies followers via the Teammate flag. By marking our clone
        // as a teammate, engine should bring it along on cell-transitions
        // with proper biped attachment - bypassing the empty-NiNode bug
        // entirely.
        if (s.postRespawnProbeTick != 0 && now >= s.postRespawnProbeTick) {
            s.postRespawnProbeTick = 0;
            CloneVisual cv = RawSnapCloneVisual(clone);
            bool emptyNiNode = false;
            if (cv.data3DPtr != 0) {
                char names[512] = {};
                NodeInfo ni = RawProbeNiNodeChildren(
                    reinterpret_cast<uint8_t*>(cv.data3DPtr),
                    names, sizeof(names));
                emptyNiNode = (ni.childCount == 0);
                PLOG_INFO(
                    "[PULSAR] CELL: {} post-respawn-probe refID=0x{:08X} data3D=0x{:x} children={} flags=0x{:x} firstName=[{}]"sv,
                    tag, cv.refID, cv.data3DPtr,
                    ni.childCount, ni.flags, names);

                // kPlayerTeammate flag DISABLED 2026-04-27 (Pivot D iter3).
                // Side effect: blocks client's door interaction ("stuck in
                // cell can't exit"). Plus asymmetric: works only when client
                // transitions (engine teleports teammate to player's new
                // position), fails when host transitions (clone stays at
                // old coords in new cell → engine refuses biped-attach).
                // Sarah works because she's a quest-aliased companion with
                // AI package; flag alone is insufficient + harmful.
                (void)cv;
            } else {
                PLOG_INFO(
                    "[PULSAR] CELL: {} post-respawn-probe refID=0x{:08X} data3D=NULL - respawn failed to load 3D"sv,
                    tag, cv.refID);
            }

            // Pivot B iter1 DISABLED 2026-04-26: dispatch of MoveTo-refresh
            // crashed the client. Probe detected children=0 and called
            // CallPapyrusMoveRefresh - game then crashed before next log
            // line. MoveTo on an actor with empty NiNode is unsafe.
            //
            // Need a different approach for biped re-attach. Possible paths:
            // Direct engine 3D-attach call (deeper RE pass)
            // Reset3DState global handler (vt @ 0x144BBEE08)
            // Detach3D + re-Attach3D virtual cycle on REFR
            // Find Bethesda's internal "fix this actor" function
            (void)emptyNiNode;
            (void)kMoveRefreshCooldownMs;
        }

        // Sprint 3 STABLE-STATE PROBE - DISABLED.
        // Iter results (REL::ID 98638 direct, mgr+0x948 default=0):
        //   mode 0 = load (per bit 0x4); 1,2 = silent no-op; 3 = hang/freeze.
        // Direct dispatcher call without engine state setup is unreliable.
        // Next: either form-stream via BGSSaveFormBuffer (RE struct first)
        // or reconnect-on-transition fallback.
        (void)s.customSaveProbeFired;

        // Periodic snapshot every 5s - symmetric on both sides, includes
        // clone 3D state so we can detect data3D loss after transitions.
        if (now - s.lastSnapshotTick > 5000) {
            s.lastSnapshotTick = now;
            CloneVisual cv = RawSnapCloneVisual(clone);
            ///RENDER-DIAG - also probe the clone's NiNode children +
            // flags. A clone that is data3D-present + same-cell + tracking but
            // INVISIBLE (while NPCs like Sarah render) is either: children==0 =
            // empty NiNode (biped detached → needs re-attach), or children>0 +
            // invisible = a render/cull flag (compare flags @ data3D+0x118).
            int      cloneChildren = -1;
            uint64_t cloneFlags    = 0;
            if (cv.data3DPtr) {
                char nm[64] = {};
                NodeInfo ni = RawProbeNiNodeChildren(
                    reinterpret_cast<uint8_t*>(cv.data3DPtr), nm, sizeof(nm));
                cloneChildren = ni.childCount;
                cloneFlags    = ni.flags;
            }
            PLOG_INFO(
                "[PULSAR] CELL: {} snapshot cell=0x{:08X} pos=({:.0f},{:.0f},{:.0f}) loadMenu={} cloneRef=0x{:08X} clonePos=({:.0f},{:.0f},{:.0f}) cloneCell=0x{:08X} loadedData=0x{:x} data3D=0x{:x} children={} flags=0x{:x}"sv,
                tag, p.cellID, p.x, p.y, p.z,
                inLoadMenu ? "YES" : "no",
                cv.refID, cv.x, cv.y, cv.z, s.lastCloneCellID,
                cv.loadedDataPtr, cv.data3DPtr, cloneChildren, cloneFlags);
        }
    }
}
