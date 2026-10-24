#pragma once

#include <cstddef>
#include <cstdint>

// SpawnSync  - host-authoritative NPC spawn replication.
//
// Some quest NPCs are spawned/enabled by Block-Event quests (e.g. the dropship
// pirates in the opening) via alias-fill that is host-local + non-deterministic,
// so the client never enables them, and NPCSync can't help (it only UPDATES
// existing NPCs and caps tracking at 20). The host detects an NPC becoming
// active in the shared cell (ActorCellChangeEvent ENTER) and tells the client to
// LookupByID the same PERSISTENT ref and Enable + place it (REL::ID 63386 Enable
// worker, idempotent + safe off-VM). Once enabled, the engine AI / NPCSync take
// over. v1: persistent refs only (FF-dynamic NPCs need a PlaceAtMe remap later).
namespace Pulsar::SpawnSync {

    // Registers the ActorCellChange sink + resolves the Enable worker. Call once
    // in kPostLoad after the other sync modules.
    void Init();

    // Network thread: a PKT_NPC_SPAWN arrived. Queues for game-thread drain.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Game thread (per tick): enables + places any queued host-spawned NPCs.
    void DrainPending();

}  // namespace Pulsar::SpawnSync
