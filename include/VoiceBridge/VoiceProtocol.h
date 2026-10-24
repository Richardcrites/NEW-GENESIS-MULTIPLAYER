#pragma once
#include <cstdint>
#include <cstddef>

namespace Pulsar::Voice
{
    constexpr uint16_t kLoopbackPort = 27900;   // DLL binds 127.0.0.1:27900
    constexpr int      kVoiceChannel = 2;       // ENet channel for PKT_VOICE
    constexpr size_t   kMaxOpusBytes = 400;     // 48kHz mono 20ms VOIP frame ceiling

    // First byte of every loopback datagram. The audio tag is reused in both
    // directions; the layouts differ (peer->Companion audio has a 4-byte gain
    // prefix, mic->DLL audio does not). Direction is unambiguous per socket.
    enum LoopbackTag : uint8_t
    {
        kTagAudio       = 0x01,  // Companion<->DLL audio frame
        kTagSessionUp   = 0x02,  // DLL->Companion: ENet peer connected
        kTagSessionDown = 0x03,  // DLL->Companion: ENet peer disconnected
        kTagSettings    = 0x04,  // Companion->DLL: {float dNear; float dFar; uint8 alwaysAudible}
        kTagHello       = 0x10,  // Companion->DLL: register endpoint
    };
}
