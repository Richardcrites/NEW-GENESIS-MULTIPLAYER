#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <atomic>

#include <RE/N/NiPoint.h>

namespace RE { class PlayerCharacter; }
namespace RE { class TESObjectCELL; }

namespace Pulsar::WorldSync {

    // L.7 V12 - internal helper for pre-load clear (kept here so C2712 doesn't
    // hit the caller).
    void detail_ClearPlayerPreLoadFlag(RE::PlayerCharacter* a_player);

    enum class State : uint8_t {
        IDLE,                  // Not connected or host mode
        WAITING_FOR_SAVE,      // Connected, waiting for PKT_SAVE_TRANSFER
        SAVE_RECEIVED,         // Save received, writing to disk
        PENDING_TRIGGER,       // Save written, waiting for main menu before triggering load
        PENDING_QUIT_FADE,     // L.7 V11 - QuitToMainMenu fired, waiting for fade-out
        LOADING,               // Load queued, waiting for game to finish loading
        LOADED                 // Host save loaded, sync active
    };

    // Called from network thread when PKT_SAVE_TRANSFER arrives
    // Raw packet format: [uint8 type][uint32 totalSize][raw .sfs data]
    void OnSaveReceived(const uint8_t* a_data, size_t a_length);

    // Called from game thread to process pending save load
    void ProcessPending();

    // Get current state
    State GetState();

    // GetTickCount64 timestamp of when LOADING→LOADED last transitioned.
    // 0 means no load has completed. Used by Main.cpp to gate teleport /
    // SetPosition calls during the post-load chaos window - engine is still
    // cell-streaming + actor-init for several seconds after LOADED fires.
    std::uint64_t GetLastLoadCompletedTick();

    // Set state to WAITING_FOR_SAVE (called after handshake)
    void BeginWaitingForSave();

    // Reset state (called on disconnect)
    void Reset();

    // Host-side: set bit 0x8 (kQuickSave) in BGSSaveLoadManager::queuedTasks.
    // Queue processor (FUN_141852ab0) picks this up on next game tick and writes a Quicksave*.sfs.
    // Thread-safe via InterlockedOr.
    bool TriggerHostQuicksave();

    // L.7 kAutoSave (bit 0x1) variant for landing-cinematic post-flip
    // window. Empirical test 2026-05-06: TriggerHostQuicksave (bit 0x8)
    // silent-fails post-landing - engine clears the bit but never writes a
    // .sfs file. Engine's own disembark-autosave uses bit 0x1, which works
    // in this state, so we set 0x1 + 0x8 = 0x9 to maximize chances.
    bool TriggerHostAutoSave();

    // Host-side: set bit 0x1000 in BGSSaveLoadManager::queuedTasks for a
    // regular auto-named save (non-quicksave). Engine generates the filename
    // via FUN_141851790 and writes via FUN_14183ed20. Used for mid-session
    // save+transfer when quicksave's bit-0x4 silent-fail makes it unreliable.
    bool TriggerHostFullSave();

    // L.7 set bit 0x10 (= kQuickLoad) in BGSSaveLoadManager::queuedTasks.
    // Engine's queue processor will load the most-recent save on next game tick.
    bool TriggerQuickLoad();
    // Set bit 0x8 (= kQuickSave) - same as TriggerHostQuicksave but from any
    // role (host or client). Used in the post-bridge save-load reset cycle.
    bool TriggerQuickSave();

    // Direct dispatcher call (REL::ID 98640) - saves with custom name.
    // mode=1 → save with type. Use only when game is in fully stable state.
    bool TriggerHostCustomSaveDirect(const char* name);

    // Bypass wrapper crash trap, call FUN_1418436E0 directly (REL::ID 98638).
    bool TriggerHostSaveBypass(const char* name);

    // L.7 fire the `TakeoffMenu_ExitShip` event by calling the
    // Papyrus native `Spaceship.ExitShip()` impl directly (RVA 0x1675C60,
    // verified Ghidra 2026-05-06: 95-byte fn, 0-arg, just notifies a
    // BSTGlobalEvent<class_TakeoffMenu_ExitShip> source + sets DAT_145edbe75=1).
    //
    // Used post save-load on client to clear the host-save's "I am piloting"
    // engine state - chair-unseat alone leaves the spaceship pilot binding
    // intact and locks the client in ship-control mode (per RE 12:00).
    // Caller-side: only meaningful when client is currently inside a ship
    // (player->GetSpaceship() != null) and just finished a save-bridge load.
    bool TriggerExitShip();

    // L.7 Path A.4 - engine's full pilot-binding release via
    // FUN_141aeb6f0 (RVA 0xAEB190). Calls FUN_142119780 to fetch the
    // pilot binding context, then FUN_14211cbf0 to do the full 673-byte
    // release which fires BGSActionData and unbinds the ship-control
    // input map (the layer that keeps the client steering the ship even
    // when not seated). MUST be called BEFORE clearing actor+0x37c bit
    // 4 - the wrapper gates on that bit. Verified via Ghidra
    // _PilotReleaseFn_decomp.txt.
    bool TriggerEnginePilotRelease();

    // L.7 V11 - call the engine's QuitToMainMenu native (RVA 0x14200F4A0) to
    // force the engine into main-menu state. Used on the client before a
    // mid-game save-load to bypass the engine 2nd-mid-game-save-load bug -
    // a load from main-menu state is the "always works" path.
    bool TriggerEngineQuitToMainMenu();

    // ─── L.7 Pre-load snapshot architecture ──────────────────────
    //
    // Problem: after a save-bridge load the client's player is repositioned
    // by the engine to wherever the saved state placed them (= host's last
    // pos). If the host was sitting in the pilot seat, both players end up
    // in the same chair. Fix: snapshot the client's pre-load pos/rotZ/cell;
    // after LOADED, if the post-load parentCell matches the pre-load
    // parentCell (= same persistent ship-interior cell), restore the
    // pre-load pos instead of teleporting onto the host.
    //
    // Extensibility - when dual-ship lands (host and client each own a
    // different ship), DetermineRestoreStrategy will gain a kOwnShipMoveTo
    // branch: pre-load cell is the non-pilot's OWN ship interior, post-
    // load cell is the host's ship → MoveTo own ship cell, then SetPosition
    // to pre-load pos. Today's snapshot already carries cellID + pos/rotZ;
    // only the decider + applier need extending.
    //
    // Public types and functions live in WorldSync because the snapshot's
    // lifetime is tied to the save-bridge state machine (capture at
    // SAVE_RECEIVED, consume at first post-LOADED teleport apply).

    struct PreLoadSnapshot {
        RE::NiPoint3   pos;
        float          rotZ;
        std::uint32_t  cellID;
        bool           valid;
    };

    enum class RestoreMode : std::uint8_t {
        kNone,              // L.7 same-cell match → skip teleport entirely.
                            // Engine load already placed the player on host's
                            // saved pos (= host's chair if host pilots). User
                            // manually unseats by holding E for ~2s; engine's
                            // own press-and-hold unseat handler does the proper
                            // anim + state-machine transition.
        kOffsetBehindHost,  // pre-load was elsewhere → 1.5m behind host's rotZ
        kBridgeFailedManualTP,  // L.7 engine bridge load did NOT switch
                            // cells (state byte 0x84 path, no PerformGameReset).
                            // Player parked in solo cell. Resolve host cell via
                            // formID lookup, pin hostClone to host's cell, then
                            // EngineMoveTo(player, hostClone) + SetPosition(hostPos).
                            // Falls back to kNone if cell lookup fails.
        kOwnShipMoveTo,     // FUTURE (dual-ship): MoveTo own ship + SetPosition
    };

    struct RestorePlan {
        RestoreMode    mode;
        RE::NiPoint3   pos;
        float          rotZ;
        std::uint32_t  targetCellID;  // L.7 host cell formID for manual TP
        // FUTURE for kOwnShipMoveTo:
        // RE::TESObjectCELL* targetCell;
    };

    // Capture local player's pre-load state. Called from OnSaveReceived
    // BEFORE HookGuard::Suspend() so player ptrs are still live. Idempotent;
    // safe to call once per save cycle.
    void CapturePreLoadSnapshot();

    // Decide how to restore the local player after LOADED. Pure function of
    // current world state + cached snapshot. No side effects. The caller
    // (Main.cpp tick) supplies the host's target pos/rotZ as a fallback for
    // the kNone case so the existing teleport-to-host behaviour is
    // preserved when no snapshot match is possible.
    RestorePlan DetermineRestoreStrategy(
        RE::PlayerCharacter* a_player,
        const RE::NiPoint3&  a_hostPos,
        float                a_hostRotZ);

    // Mark snapshot consumed (one-shot). Caller invokes after applying the
    // RestorePlan so subsequent ticks fall through to the host-pos
    // fallback.
    void ConsumePreLoadSnapshot();

}  // namespace Pulsar::WorldSync
