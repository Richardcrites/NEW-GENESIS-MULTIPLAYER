#pragma once

#include <cstddef>
#include <cstdint>

namespace Pulsar::WorldSync::Acti {

    // F.7 V1 (2026-04-26): Activator (kACTI) replay-sync. Piggybacks on the
    // Activate-hook in LootSync (the hook is domain-agnostic). Symmetric
    // to F.3 DoorSync - same dispatcher → packet → cell-find → replay flow.
    void Init();

    // Sender entry - called from LootSync's HookedActivate post-orig when
    // the activation was a player-press on a kACTI REFR. Broadcasts
    // PKT_ACTI_STATE. consumedFormID/Count carry the inventory delta the
    // sender detected across the orig-Activate call (0 if nothing was
    // consumed, e.g. switches/buttons; non-zero for power-conduits etc).
    void OnHostActivate(std::uint32_t refID, std::uint32_t baseID,
                        float px, float py, float pz, bool result,
                        std::uint32_t consumedFormID,
                        std::int32_t  consumedCount);

    // Receiver entry - called from NetworkHost::OnReceive on PKT_ACTI_STATE
    // arrival. Resolves cross-plugin base FormID and queues for next tick.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Drain - game-thread. Find local kACTI REFR by (resolved-base,
    // position) and replay Activate via LootSync::CallOrigActivateAsReplay.
    void DrainPending();

}  // namespace Pulsar::WorldSync::Acti
