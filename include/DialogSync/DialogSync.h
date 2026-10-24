#pragma once

#include <cstddef>
#include <cstdint>

namespace Pulsar::DialogSync {

    // L.8 Phase 3 V1 - sink PlayerInDialogueChanged::Event @ RVA 0x597A298
    // and TESOnPCDialogueTargetEvent @ RVA 0x5978698. Both are first-class
    // BSTEventSource singletons confirmed via SFSE GameEvents.h. On state
    // transitions we broadcast PKT_DIALOG_START / PKT_DIALOG_END so both
    // sides know who is in dialogue and with whom. V1 RX is log-only;
    // outcomes already propagate via Phase 1 stage sync.
    void Init();

    void Shutdown();

    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    void Reset();

    // V4 - drain pending PKT_DIALOG_VOTE replays. Must run on game thread
    // (TESTopicInfo::Fire touches BSTEventSource::Notify and ExtraDataList,
    // both game-thread-only). Wired in Main.cpp game-thread tick.
    void DrainPending();

}  // namespace Pulsar::DialogSync
