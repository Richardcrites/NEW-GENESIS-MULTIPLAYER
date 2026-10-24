#pragma once

#include <cstdint>
#include <cstddef>

namespace Pulsar::SpaceSync {

    // K.7 V1 - Sink onto Spaceship engine events (TakeOff, Landing, GravJump,
    // Dock, TakeoffMenu_Launch, RampDown, FarTravel) via SFSE-documented event
    // sources. On host fire, broadcast PKT_SPACE_TRANSITION.
    //
    // K.7 V2 - Receiver auto-follows: when peer event arrives + F7 follow
    // enabled, MoveTo peer's clone so engine switches client to peer's
    // worldspace+cell (cockpit + space WS, or cockpit + planet WS for landing).
    void Init();

    // Receiver entry - called by NetworkHost dispatch for PKT_SPACE_TRANSITION.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // L.3 V1 - receiver entry for PKT_SHIP_POS (ship pos/rot updates).
    void OnShipPosReceived(const std::uint8_t* data, std::size_t length);

    // Game-thread tick - applies pending auto-follow if armed.
    void TickFollowApply();

}  // namespace Pulsar::SpaceSync
