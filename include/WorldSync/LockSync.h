#pragma once

#include <cstddef>
#include <cstdint>

namespace RE { class TESObjectREFR; }

namespace Pulsar::WorldSync::Lock {

    // Resolves the engine's ExtraDataList::GetByType helper (REL::ID 37878)
    // used to walk REFR+0xC8 for an ExtraLock entry. LootSync resolves
    // the same symbol; we resolve it independently so the module stays
    // standalone.
    void Init();

    // Sender entry - called from LootSync's HookedActivate after the
    // engine's original Activate has returned, when the activation was a
    // player-pressed E on a TESObjectCONT or TESObjectDOOR REFR. We
    // probe REFR_LOCK->flags and broadcast PKT_LOCK_STATE if the flag
    // transitioned since the last time we saw it (typ. locked YES→no
    // after a successful lockpick mini-game).
    void OnHostActivate(RE::TESObjectREFR* refr,
                        std::uint32_t refID, std::uint32_t baseID,
                        std::uint8_t baseType,
                        float px, float py, float pz);

    // F.3.c: Container open-sync entry. Called from LootSync's
    // HookedActivate post-orig for player-triggered kCONT activations.
    // Broadcasts PKT_CONTAINER_OPEN if Activate succeeded and the
    // container is currently unlocked. Locked containers are excluded
    // (F.3.b LockSync covers the post-pick auto-open path).
    void OnHostContainerOpen(RE::TESObjectREFR* refr,
                              std::uint32_t refID, std::uint32_t baseID,
                              float px, float py, float pz, bool result);

    // Receiver entry - called from NetworkHost::OnReceive when a
    // PKT_LOCK_STATE arrives. Resolves the cross-plugin base FormID and
    // queues a PendingLock for the next game-thread tick.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Receiver entry for PKT_CONTAINER_OPEN. Mirrors OnPacketReceived but
    // queues a PendingContainerOpen instead.
    void OnContainerOpenReceived(const std::uint8_t* data, std::size_t length);

    // Drain - must run on the game thread. Walks pending locks AND
    // pending container opens; for locks writes REFR_LOCK->flags directly,
    // for opens calls Papyrus SetOpen(true) on the matched local REFR.
    void DrainPending();

}  // namespace Pulsar::WorldSync::Lock
