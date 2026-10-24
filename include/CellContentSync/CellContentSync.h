#pragma once

#include <cstdint>
#include <cstddef>

namespace Pulsar::CellContentSync {

    // K.6 V1 - Cell-Content-Sync (TiltedEvolution-Pattern, Path B).
    //
    // Goal: when host enters a procedural or static cell, snapshot every
    // "interesting" REFR (loot, container, door, activator, furniture, dead
    // NPC), serialize it, and broadcast a batch to the peer. Peer spawns
    // matching refs in its local cell. Solves: procedural-POI sync, ship-
    // cockpit takeoff (peer gets host's cockpit refs spawned in their copy),
    // interior follow.
    //
    // V0 (this file initially): host-side enumeration with logging only.
    // No packet, no client-side spawn. Verifies the FormType filter and the
    // ref-iteration primitive (TESObjectCELL::ForEachReference) work on
    // proc + static cells alike. After V0 is observed, V1 wires up the
    // packet + receiver log, V2 adds spawn pipeline, V3 adds cleanup.
    //
    // See memory/project_session_entry_next.md for the multi-week plan.

    void Init();

    // Called by TransitSync::OnLocalTransit on host side after a cell change
    // has been detected and the K.1 PKT_TRANSIT was broadcast.
    // We do NOT enumerate immediately - proc-cell construction continues for
    // a beat or two after loadMenu CLOSED. Instead, queue a deferred enum
    // that fires from Tick() once the cell has settled.
    void OnHostCellChanged(std::uint32_t prevCellFormID,
                            std::uint32_t newCellFormID);

    // Game-thread tick - drains a queued cell enumeration once the settle
    // window has elapsed. Called from Main.cpp's permanent task.
    void Tick();

    // Network-thread entry - receiver side (client). V1 logs only.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

}  // namespace Pulsar::CellContentSync
