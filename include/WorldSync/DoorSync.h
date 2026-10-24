#pragma once

#include <cstddef>
#include <cstdint>

namespace Pulsar::WorldSync::Door {

    // No-op for V1 - DoorSync piggybacks on LootSync's Activate hook (the
    // hook is domain-agnostic; we only need its dispatch). Reserved for
    // future state-init / late-join door-sync work.
    void Init();

    // Sender entry - called from LootSync's HookedActivate after the engine's
    // original Activate has returned, when the activation was a player-pressed
    // E on a TESObjectDOOR REFR. We broadcast PKT_DOOR_STATE so the peer can
    // replay Activate locally and animate the door in sync.
    void OnHostActivate(std::uint32_t refID, std::uint32_t baseID,
                        float px, float py, float pz, bool result);

    // Receiver entry - called from NetworkHost::OnReceive when a
    // PKT_DOOR_STATE arrives. Resolves the cross-plugin base FormID and
    // queues a PendingDoor for the next game-thread tick.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Drain - must run on the game thread. Walks pending doors, finds the
    // matching local DOOR REFR by (resolved-base, position), and replays
    // Activate via LootSync::CallOrigActivateAsReplay so the engine
    // animates open/closed natively.
    void DrainPending();

    // Cross-source jitter dedup. Both machines run the same deterministic NPCs,
    // so when an NPC opens a door the CLIENT's own local NPC opens it too, AND
    // the host mirror replays it - Activate is a toggle, so the second one shuts
    // the door again ("opened then immediately closed"). LootSync's Activate
    // hook calls NoteLocalDoorActivation for every NON-replay door activation;
    // DrainPending skips a mirror replay whose door was just locally activated.
    void NoteLocalDoorActivation(std::uint32_t refID);
    bool WasLocallyActivatedRecently(std::uint32_t refID) noexcept;

}  // namespace Pulsar::WorldSync::Door
