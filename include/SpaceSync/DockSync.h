#pragma once

#include <cstdint>
#include <cstddef>

namespace Pulsar::DockSync {

    // L.6 V3 - Ship-Dock-Sync.
    //
    // Phase 1 (this iteration) is DIAGNOSTIC ONLY. We sink Spaceship::DockEvent
    // independently from SpaceSync's existing sink, but READ the event payload
    // bytes that SpaceSync ignores. Per earlier RE findings, the payload
    // layout is:
    //   [0x00] qword - ship A REFR ptr (host's ship at fully-docked)
    //   [0x08] qword - ship B REFR ptr (typically the dock partner / station port)
    //   [0x10] dword - `started` flag
    //   [0x14] dword - `finished` flag
    //
    // Site `0x14213E0D6` fires DockEvent with `(started=1, finished=1)` - that is
    // the cutscene-complete moment we'll broadcast on in Phase 2.
    //
    // Phase 1 also dumps the per-ship docking-component pointer at
    // [ship+0x88] and if non-null, the state-machine word at *[ship+0x88]+0x80.
    // Per earlier RE work: BGSDockingActivity ctor at RVA 0x1408F0250 initializes
    // [+0x80] = 0x100, transitions through other values during dock.
    //
    // Phase 2 (future) - bidirectional sync:
    //   Host: on (1,1) DockEvent → broadcast PKT_DOCK_FINISHED with ship+dock
    //         FormIDs, position, orient.
    //   Client: receive → resolve FormIDs via T34 → write ship pos/orient
    //         (SetPosition vt[0x89] + havok body+0x4D0+0x50) → invoke
    //         engine dispatcher 0x14213A130 (REL::ID 120238) with synthesized
    //         event payload so engine runs its own ComponentDB registration +
    //         cell-transit. No MoveTo shortcut.

    void Init();

    // Phase 2 - receiver-side packet handler. NetworkHost dispatches
    // PKT_DOCK_FINISHED here. Stores pending apply for game-thread Tick.
    void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_length);

    // Phase 3a - game-thread tick. Drains pending dock-apply (MoveTo
    // local ship REFR to station dock-port REFR). User-confirmed
    // 2026-05-02: MoveTo restriction is for the PLAYER only; SHIP
    // teleport to dock is OK and is what we want here.
    void Tick();

    // Phase 3f intercept - called from LootSync's HookedActivate. When the
    // activated REFR is the station-side hatch we configured via Phase 3f
    // bypass, redirects player to cockpit hatch via Papyrus MoveTo (avoids
    // engine teleport crash when the cockpit cell has been unloaded after
    // the player walked into the station). Returns true if intercepted -
    // caller should skip the original Activate dispatcher in that case.
    // MoveTo of player here is OK per user's clarification: the constraint
    // is on automatic auto-follow into the station, NOT on hatch press
    // resulting in normal cell-transit back to the ship.
    bool TryHandleStationSideHatchActivate(RE::TESObjectREFR* a_self);

    // Phase 3i v17 - block client's cockpit-side hatch activate while we're
    // waiting for host's forward-bridge save to arrive. Without this, client
    // teleports through the hatch via XT bytes to dock-coords inside their
    // LOCAL procedural ship instance - wrong ship visible at the dock until
    // the return-bridge save corrects state minutes later. Returns true if
    // intercepted (suppress engine activate); false if normal flow.
    bool TryHandleCockpitSideHatchActivate(RE::TESObjectREFR* a_self);

    // Phase 3i v17 - called by WorldSync when client finishes loading host's
    // save. Clears the forward-bridge wait flag so client's hatch activates
    // resume normal flow.
    void OnLocalSaveLoaded();

    // Phase 3h (2026-05-02) - called from SpaceSync::OnPacketReceived when
    // kind=TakeOff is received from a peer. Cleans up our applied dock
    // state on the local cockpit hatch. Replaces the previous DockSync
    // TakeOff sink which somehow broke FlightSync L.3 V3's vt[8] hook on
    // bhkSpaceship (caused client to stop receiving ship pos sync).
    // Idempotent - no-op if no apply on file.
    void OnRemoteTakeOff();

}  // namespace Pulsar::DockSync
