#pragma once

#include <cstddef>
#include <cstdint>

namespace Pulsar::WorldSync::Grab {

    // F.6 V2b: bidirectional E-Hold grab sync. Sender side hooks the engine's
    // TESGrabReleaseEvent EventSource A (RVA 0xB99E70, verified via Ghidra
    // 2026-04-25; see memory/reference_grab_re.md). Source B exists too but
    // doesn't fire for the in-world E-Hold path - V2a probe confirmed only
    // A fires for player grabs.
    //
    // While grabbed=1, Tick() (game-thread, ~30 Hz) re-broadcasts the held
    // REFR's current position so the peer mirrors the float-in-front-of-
    // player motion. On grabbed=0 we stop tracking and the engine's physics
    // takes over on both sides.
    void Init();

    // Game-thread tick - call from Main.cpp's permanent task loop. Reads
    // the currently-held REFR's position and broadcasts PKT_GRAB_STATE if
    // ~33 ms have elapsed since the last send.
    void Tick();

    // Receiver entry - called from NetworkHost::OnReceive when a
    // PKT_GRAB_STATE arrives. Resolves cross-plugin base FormID and queues
    // a PendingGrab for the next game-thread tick.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Drain - must run on the game thread. For each pending update finds
    // the local REFR via base+position matcher (or remembered last match
    // for position-update packets), then applies SafeRefSetPosition.
    void DrainPending();

}  // namespace Pulsar::WorldSync::Grab
