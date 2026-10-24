#pragma once

#include <cstdint>

namespace RE {
    class PlayerCharacter;
    class Actor;
    class TESObjectREFR;
}

namespace Pulsar::CloneManager {

    // Called once at plugin load. Resolves REL::Relocations; safe to call before forms loaded.
    void Init();

    // Spawn a clone of the local player via the engine's inner placement function
    // (FUN_14179a510 / REL::ID 97230). This is the same function Papyrus PlaceAtMe
    // calls internally, so the clone gets a full animation graph, AI process, and
    // proper cell attachment - it will animate like a real NPC.
    //
    // Returns nullptr on failure (invalid player, missing REL::Relocations, or
    // engine-level rejection). On success, returns the spawned Actor and tracks
    // its handle so GetClone() works across cell transitions.
    RE::Actor* SpawnClone();

    // Despawn the current clone, if any. Safe no-op if no clone is tracked.
    void DespawnClone();

    // Get the currently tracked clone, or nullptr if none / destroyed / save-loaded.
    // Handle-based resolution: returns null automatically if the underlying actor
    // was freed, even if the FormID value is still held somewhere.
    RE::Actor* GetClone();

    // True if the engine reports the clone is alive and in a loaded cell.
    bool IsCloneActive();

    // Reset all tracking state. Called on disconnect or before a fresh spawn.
    void Reset();

    // True if the given actor's FormID matches the tracked C++-spawned clone.
    // Used by RemotePlayerManager to skip apply paths that crash on C++-clones.
    bool IsCppSpawnedClone(uint32_t formID);

}  // namespace Pulsar::CloneManager
