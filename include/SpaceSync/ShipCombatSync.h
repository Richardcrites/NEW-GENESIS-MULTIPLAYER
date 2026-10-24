#pragma once

// Co-op ship-combat sync for the shared host ship.
//
// Model: the host is authoritative. It reads its ship's combat ActorValues
// (hull health, shield, ...) and broadcasts them; the passive client mirrors
// them onto its local ship so both HUDs (and ship survival state) agree.
//
// All ship-combat state in Starfield is ActorValues on the ship REFR, reached
// via the ActorValueOwner sub-object at ship+0x70 (RE, 4 Ghidra passes -
// see memory/reference_ship_combat_re.md). Same read/write primitive as NPC HP.
//
// Slice 1 (this file's first cut): READ-ONLY. Both sides read the hull-health
// AV via the RE'd path and LOG it, to live-verify the RE before any write -
// a wrong AV write would destroy the client ship. No packet, no apply yet.

#include <cstddef>
#include <cstdint>

namespace Pulsar::Network { struct ShipFireData; }

namespace Pulsar::ShipCombatSync {

void Init();

// Game-thread tick - slice 1: throttled read + log of the local ship's hull AV.
void Tick();

// Network thread - queues received ship-AV state for game-thread application.
// Slice 1: no-op (no packet yet).
void OnPacketReceived(const std::uint8_t* data, std::size_t length);

// slice 3 - network thread: queue a remote ship weapon-fire so the game
// thread (Tick) can spawn the visible bolt with the host's real aim.
void OnRemoteFire(const Pulsar::Network::ShipFireData& a_fire);

}  // namespace Pulsar::ShipCombatSync
