#pragma once

#include <cstdint>

namespace Pulsar::TakeoffProbe {

    // Phase L.1 V1 - diagnostic logger for ship takeoff/landing/etc state.
    //
    // SpaceSync's event sinks call OnEventFire(kind) on every Spaceship event
    // (TakeOff/Landing/GravJump/Dock/etc). Probe captures full player+ship
    // state immediately and schedules delayed re-captures (100ms / 500ms /
    // 2s / 5s) so we can see what state changes WHEN. Output goes to PULSAR
    // log + NASLog (so client logs reach <NAS>).
    //
    // Goal: empirical baseline for L.2 architecture decision -
    // does parentCell change? Does cellWorldspace flip live on the cockpit
    // cell? Is GetSpaceship() valid before/after takeoff? Where is the ship
    // REFR positioned?

    void Init();

    // Called by SpaceSync sink when any spaceship event fires (kind = the
    // SpaceSync KindStr enum: 0=TakeOff, 1=Landing, ..., 4=TakeoffMenu_Launch).
    void OnEventFire(std::uint8_t a_kind);

    // Game-thread tick - drains delayed captures.
    void Tick();

    // L.6 V1.2 - Dock-Probe-Window. Returns true if we're currently in the
    // 30s post-DockEvent capture window. Activate-hook calls
    // LogActivateDuringDockProbe to mark any REFR activation (Hatch detection).
    bool IsDockProbeActive();
    void LogActivateDuringDockProbe(std::uint32_t a_targetFormID,
                                     std::uint32_t a_baseFormID,
                                     std::uint8_t a_baseFormType);

}  // namespace Pulsar::TakeoffProbe
