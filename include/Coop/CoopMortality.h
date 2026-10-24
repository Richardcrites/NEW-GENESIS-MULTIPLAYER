#pragma once
#include <cstdint>
#include <cstddef>

namespace RE { class TESObjectREFR; }

namespace Pulsar::Coop::Mortality
{
    void Init();
    void OnGameThreadTick();   // game thread, ~1000x/sec; gated internally

    // True while the remote partner (= their clone on THIS machine) is downed
    // (bleedout pose, life-state mirrored from PKT_PLAYER_DOWNED). Other modules
    // gate clone-mutating ops on this: e.g. EquipSync's apply (DoEquipArmor/Weapon)
    // must NOT run while the clone is downed - its 3D/graph update resets the
    // bleedout/knee pose (confirmed: armor sync blocked the downed animation).
    bool IsRemoteDowned();

    // True while THIS (local) player is downed (HP hit 0 → bleedout). EquipSync
    // gates its SEND on this: while locally downed, do NOT scan/broadcast our worn
    // equipment - the equip read/refresh disturbs the local bleedout pose.
    bool IsLocalDowned();

    // called by ShipCombatSync when the (shared) ship's hull reaches 0.
    // The ship explodes but the player isn't killed (no death to intercept), so
    // route the local player into the downed state directly (sets local-downed +
    // notifies the peer). Both machines call it → OnGameThreadTick's both-downed
    // game-over reloads BOTH sides - same scheme as a both-down death. Idempotent;
    // no-op if not connected or already downed. Game thread.
    void TriggerShipDowned();

    // Game thread, called from the Activate-core hook (LootSync, REL::ID 63380).
    // Returns true when the activation was "revive the downed partner-clone" and
    // was consumed (caller must skip the original = no talk dialog on the clone).
    bool OnActivateMaybeRevive(RE::TESObjectREFR* a_self, RE::TESObjectREFR* a_activator);

    // ENet-thread receivers (queue only, applied on the game thread).
    void OnDownedReceived(const std::uint8_t* a_data, std::size_t a_length);
    void OnReviveReceived(const std::uint8_t* a_data, std::size_t a_length);
    void OnCombatStatusReceived(const std::uint8_t* a_data, std::size_t a_length);
}
