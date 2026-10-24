#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace RE { class TESObjectCELL; }

namespace Pulsar::NPCSync {

    // Called once at plugin load (post-SFSE kPostLoad).
    // Installs the Actor::Update vtable hook.
    bool Init();

    // Host: called on game-thread at ~60 Hz (SFSE task). Flushes pending state to the network.
    // Client: no-op (use ApplyPendingRemoteState on client).
    void OnGameThreadTick();

    // Client: called on network thread when a PKT_NPC_STATE arrives.
    // Parses the batch and queues per-NPC updates to be applied on game thread.
    void OnPacketReceived(const uint8_t* a_data, std::size_t a_length);

    // Client: called on network thread when a PKT_CIVILIAN_SNAPSHOT arrives.
    // Parses the slim civilian-position batch and queues entries for
    // ApplyPendingRemoteState to apply on game thread.
    void OnCivilianSnapshotReceived(const uint8_t* a_data, std::size_t a_length);

    // Host-side: client forwarded combat damage for a host-matched NPC.
    // Called on the ENet thread - only queues; the actual SetActorValue
    // happens in OnGameThreadTick (engine calls must be on the game thread).
    void OnDamageReceived(const uint8_t* a_data, std::size_t a_length);

    // Corpse-loot sync (2026-06-11): host → client authoritative corpse
    // inventory (PKT_CORPSE_LOOT). ENet thread - resolves cross-plugin and
    // queues; applied on the game thread once the matched corpse is dead.
    void OnCorpseLootReceived(const uint8_t* a_data, std::size_t a_length);

    // Client: called on game-thread to apply queued remote state to local NPCs.
    // Safe to call every frame from game-thread.
    void ApplyPendingRemoteState();

    // Called on disconnect or cell change. Clears all tracking.
    void Reset();

    // Fast read-only check: is this FormID a client-side locally-spawned clone
    // of a host-tracked NPC? Used by RemotePlayerManager's EvaluatePackage /
    // PerformAction hooks to block engine AI on those actors (they should
    // only follow network-driven positions, not run companion-follow or
    // combat AI locally). Thread-safe.
    bool IsRemoteOwned(uint32_t formID);

    // P1: mark/unmark a tracked NPC as DirectControl-puppet, which routes it
    // through the same EvaluatePackage + PerformAction AI-block as the clone (via
    // IsRemoteOwned). Set true when DirectControl drives the NPC, false on
    // untrack/death so the engine AI resumes. Separate set from g_remoteOwned.
    void MarkNpcAiBlocked(uint32_t formID, bool on);

    // P1: is this specifically a DirectControl-puppet NPC (g_npcDcBlocked)?
    // Distinct from IsRemoteOwned (which also covers clones). Used to extend the
    // AI-block into proc cells for these NPCs only, leaving clones' proc pass-
    // through untouched.
    bool IsNpcAiBlocked(uint32_t formID);

    // V36 (2026-06-30): scripted-scene suspension. Story NPCs in a quest scene
    // run the SAME deterministic script on both machines (fixed path, scene
    // animations, dialogue), so the host->client DirectControl sync just fights
    // and STALLS their local scene. When a tracked NPC speaks a scene line, the
    // client marks it in-scene for a short window; TickLerp then releases its
    // DirectControl and skips the position/graph apply so the LOCAL scene drives
    // it (correct path + animations + dialogue). Refreshed by every line and
    // expires after the window, so normal sync resumes when the scene ends.
    void MarkNpcInScene(uint32_t formID);
    bool IsNpcInScene(uint32_t formID);

    // P3 combat-live: let a remote NPC's engine combat AI run (aim + fire +
    // local player damage) instead of puppeting it. Gated by PULSAR.ini
    // npcCombatAiLive=1. The EvaluatePackage/PerformAction hooks query
    // IsNpcCombatLive to pass these NPCs' AI through; TickLerp marks membership.
    void SetNpcCombatAiLive(bool on);
    bool NpcCombatAiLiveOn();
    void MarkNpcCombatLive(uint32_t formID, bool on);
    bool IsNpcCombatLive(uint32_t formID);

    // kill-XP-sharing: map a LOCAL victim FormID to the HOST-canonical key
    // both machines agree on. Static-ref NPCs share the same FormID on both sides
    // (returned unchanged). Dynamic (0xFF) clones are looked up through the
    // host→local combat-sync remap (reverse) so the returned key is the host's
    // FormID. Returns 0 when a dynamic victim has no remap entry (i.e. it is a
    // purely-local, un-synced NPC) - the caller then skips sharing. Thread-safe.
    uint32_t LocalRefToHostRef(uint32_t localFormID);

    // Returns snapshot of every TESObjectCELL the engine has loaded, as
    // observed via Actor::Update hook. Replaces the stale-on-1.16.236
    // TESDataHandler.formArrays[kCELL] iteration path. Callers filter stale
    // pointers defensively (SEH-wrap ref-iteration).
    std::vector<RE::TESObjectCELL*> GetActiveCellsSnapshot();

}  // namespace Pulsar::NPCSync
