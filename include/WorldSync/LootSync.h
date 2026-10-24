#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace RE { class TESObjectREFR; }

namespace Pulsar::WorldSync::Loot {

    // Install MinHook on TESObjectREFR::Activate (REL::ID 63380, RVA 0xB47920).
    // On host, filters player loot-pickups and broadcasts PKT_LOOT_PICKUP so
    // the client can despawn the matching REFR on its side.
    void Init();

    // Client-side receiver: called from NetworkHost::OnReceive when a
    // PKT_LOOT_PICKUP arrives. Queues a pickup to run on the game thread.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Key/keycard mirror. TickKeySync (game-thread, ~2s) scans the local
    // player's inventory for KEY (kKEYM) items and broadcasts any new one via
    // PKT_KEY_MIRROR so the peer also gets it - nobody gets stuck at a loading-
    // screen door that needs the key. OnKeyMirrorReceived adds the key to the
    // local player if missing. Idempotent (both sides track seen keys → no echo).
    void TickKeySync();
    void OnKeyMirrorReceived(const std::uint8_t* data, std::size_t length);

    // Drain pending pickups - must run on the game thread (called from
    // Main.cpp's 16ms ProcessPendingTasks tick or similar).
    void DrainPendingPickups();

    // Debug: dump container+player inventory diff while a container-debug
    // trace is active. Called from the game-thread tick. No-op outside the
    // 5s window after a container was activated.
    void TickDebugContainerTrace();

    // M.2 (2026-05-08): Periodic poll of recently-activated containers.
    // Ship cargo (BarterMenu path) doesn't fire BGSInventoryListEvent on
    // transfers, so its diffs only show up via this poll. Call from game-
    // thread tick. Internally rate-limited to 500ms.
    void TickContainerPolling();

    // Shared replay primitive - set the in-flight "this is a replay, don't
    // re-broadcast" guard, then call the engine's original Activate via the
    // MinHook trampoline owned by LootSync. Used by sibling sync modules
    // (DoorSync, future) so they don't need to duplicate the trampoline
    // pointer or the t_replayingFromPacket guard.
    //
    // `args` is ActivateArgs* (32-byte struct) - kept void* in the public
    // header to avoid leaking the internal struct definition.
    void CallOrigActivateAsReplay(RE::TESObjectREFR* refr, void* args);

    // L.7 Plan B - suppress M.2 inventory broadcast hook (and the V24
    // mod-snapshot / V25 grow-skip paths) for the duration of a synthetic
    // engine call. Caller pattern (RAII or manual restore):
    //
    //   const bool prev = Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);
    //   ... do AddObjectToContainer calls that must not re-broadcast ...
    //   Pulsar::WorldSync::Loot::SetReplayingFromPacket(prev);
    //
    // Thread-local - only suppresses on the calling thread. Returns the
    // previous value so the caller can restore exactly.
    bool SetReplayingFromPacket(bool value);
    bool IsReplayingFromPacket();

    // F.2-v2 (2026-06-11): expose the M.2 mod read/apply primitives for the
    // worn-equipment sync. Read the kObjectInstance mod array + sentinel of an
    // item in a_container's inventory / write a cached array back onto it.
    void ReadItemMods(RE::TESObjectREFR* a_container, std::uint32_t a_itemFormID,
                      std::vector<std::uint32_t>& a_outMods, std::uint16_t& a_outSentinel);
    void ApplyItemMods(RE::TESObjectREFR* a_container, std::uint32_t a_itemFormID,
                       const std::vector<std::uint32_t>& a_mods, std::uint16_t a_sentinel);

    // (2026-06-18): NATIVE engine mod-attach for an inventory item. Unlike
    // ApplyItemMods (M.2 overwrite primitive - only rewrites EXISTING array
    // slots, can't allocate/grow, ignores slot compatibility → naked base forms
    // + wrong mods), this drives the engine's own per-entry attach core
    // (FUN_14063de80, RVA 0x63D270 - the crafting/AttachMod path): it
    // allocates the kObjectInstance if absent, GROWS the mod array, and adds
    // each mod into its CORRECT slot from the mod template. Used by the
    // DB-restore path so restored gear (incl. the suit's O2 mod) comes back
    // correct. Game-thread only; SEH-wrapped internally. Returns the number of
    // mods successfully attached. Leaves the other ApplyItemMods callers
    // (clone-equip / corpse-loot) untouched.
    std::uint32_t AttachItemModsNative(RE::TESObjectREFR* a_owner,
                                       std::uint32_t a_itemFormID,
                                       const std::vector<std::uint32_t>& a_mods,
                                       std::uint16_t a_sentinel);

}  // namespace Pulsar::WorldSync::Loot
