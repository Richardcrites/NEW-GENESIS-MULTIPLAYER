#pragma once

#include <atomic>
#include <cstdint>

namespace RE { class Actor; }

namespace Pulsar::Cell3DAttach {

    // Phase H - Direct fix for the empty-NiNode-after-cell-transition bug.
    //
    // Two cooperating mechanisms:
    //
    //  1. Sink on BSTEventSource<TESCellReference3DAttachEvent> @ 0x145977A40
    //     (RE'd 2026-04-27). Engine fires this event from FUN_14046B770 on
    //     EVERY REFR whose 3D becomes valid for the first time after a cell
    //     attach. We log/track whether our clone's REFR appeared.
    //
    //  2. Fallback timer: if 2s after WorldSync LOADED the clone's data3D
    //     still has children.count == 0 (empty NiNode bug), invoke the
    //     engine's biped-attach directly:
    //         clone->vt[0x95](clone, nullptr, nullptr);
    //     = FUN_140B24E60 (AttachBiped). Re-attaches inv slots
    //     0x29/0x34/0x38/0x4C/0x4E/0x5A and rebuilds 3D via FUN_1405E2CF0.
    //
    // Init runs on plugin load (kPostLoad). Tick is driven from the
    // existing CellDiag/Main-loop tick.
    void Init();

    // Game-thread tick. Polls for empty-NiNode condition + fires fallback
    // when sink hasn't reported attach within the settle window.
    void Tick(bool isHost);

    // Reset state for a new cell-transition cycle. Called when CellDiag
    // detects a player cell-change.
    void OnPlayerCellChange();

    // Returns true if the sink has fired for the currently-tracked clone
    // since the last OnPlayerCellChange. Diagnostic accessor.
    bool DidSinkFireSinceLastTransition();

    // Manual one-shot biped-attach. Public so RemotePlayerManager can call
    // it after a fresh DoSpawn if it wants to pre-attach. Always runs on
    // the calling thread - call from the game thread only.
    void ManualAttachBiped(RE::Actor* clone);

    // ---------------------------------------------------------------------
    // Phase I - Cell-transition position-sync quiet window.
    //
    // Logs 2026-04-27 showed that during cell transitions, lagged network
    // PKT_POSITION updates (containing host's mid-stream / pre-transition
    // coords) cause the receiver's clone to flip between the old and new
    // cell within a single ms (clone cell-flip pingpong), repeatedly
    // detaching 3D and triggering force-respawn loops.
    //
    // Fix: gate BOTH broadcast and apply paths on a "transition quiet"
    // boolean. While loadMenu is open OR within `kQuietPostCloseMs` after
    // it closes, position broadcasts are skipped (host) and applies are
    // dropped (receiver of remote position).
    //
    // Caller responsibilities:
    // Main.cpp's permanent task observes loadMenu open/close and calls
    //     SetLoadMenuState() each tick.
    // RemotePlayerManager::DoSetPosition + Main.cpp host/client send
    //     paths bail early when IsQuiet() returns true.

    void SetLoadMenuState(bool a_inLoadMenu);
    bool IsQuiet();
    bool WasInLoadMenu();  // last observed state - for callers that don't poll directly

}  // namespace Pulsar::Cell3DAttach
