#pragma once
#include <cstdint>
#include <cstddef>

namespace Pulsar::Voice
{
    // Owns the localhost UDP loopback to the Companion. Serviced on the network
    // thread. All entry points are self-guarding: any failure logs and disables
    // voice without touching the rest of the DLL.
    class VoiceBridge
    {
    public:
        static void Init();       // bind loopback socket (idempotent)
        static void Shutdown();   // close socket
        static void Service();    // drain inbound loopback (mic frames, settings, hello)

        // Called from NetworkHost::OnReceive when a PKT_VOICE arrives (already
        // decrypted). a_data/a_len is [VoiceFrameHeader][opus] (no type byte).
        static void OnVoicePacket(const uint8_t* a_data, size_t a_len) noexcept;

        // Called from NetworkHost ENet connect/disconnect handlers.
        static void OnPeerUp() noexcept;
        static void OnPeerDown() noexcept;
    };
}
