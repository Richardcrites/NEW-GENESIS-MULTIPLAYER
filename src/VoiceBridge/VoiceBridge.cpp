#include "PCH.h"
#include "VoiceBridge/VoiceBridge.h"
#include "VoiceBridge/VoiceProtocol.h"
#include "VoiceBridge/ProximityGain.h"
#include "Network/NetworkHost.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")   // GetAsyncKeyState (in-process PTT)

namespace Pulsar::Voice
{
    namespace
    {
        SOCKET             g_sock = INVALID_SOCKET;
        sockaddr_in        g_companionAddr{};       // learned from HELLO
        std::atomic<bool>  g_haveCompanion{ false };
        std::atomic<bool>  g_peerConnected{ false };

        // gain-calc settings pushed by the Companion (kTagSettings)
        std::atomic<float> g_dNear{ 600.0f };
        std::atomic<float> g_dFar{ 4000.0f };
        std::atomic<bool>  g_alwaysAudible{ false };
        std::atomic<uint8_t> g_mode{ 0 };   // 0=open, 1=ptt (PTT gated HERE, in-process)
        std::atomic<int>   g_pttKey{ 0 };   // Win32 VK for PTT

        // network-thread-only state
        bool     g_companionLogged = false;   // log "registered" once, not every 2s heartbeat
        bool     g_sessionUpSent   = false;   // one SESSION_UP per peer connection
        uint64_t g_micIn = 0, g_voiceOut = 0, g_voiceIn = 0;  // cumulative flow counters
        float    g_lastGain = 1.0f;
        uint64_t g_lastDiagMs = 0;

        void SendToCompanion(const uint8_t* data, int len)
        {
            if (g_sock == INVALID_SOCKET || !g_haveCompanion.load()) return;
            ::sendto(g_sock, reinterpret_cast<const char*>(data), len, 0,
                     reinterpret_cast<sockaddr*>(&g_companionAddr), sizeof(g_companionAddr));
        }

        void SendSessionUp()
        {
            const uint8_t m = kTagSessionUp; SendToCompanion(&m, 1);
            g_sessionUpSent = true;
        }

        // Outbound: hand a framed [VoiceFrameHeader][opus] to the peer.
        void SendVoiceToPeer(const uint8_t* payload, size_t len)
        {
            Pulsar::Network::GetSingleton().SendVoiceFrame(payload, len);
            ++g_voiceOut;
        }

        // Cumulative flow line every ~2s (dev build only). Tells you exactly which
        // leg is dead: mic-in=0 => mic never opened (PTT gate / capture); voice-in=0
        // with voice-out>0 on the peer => receive/decode; etc.
        void DiagTick()
        {
            const uint64_t now = ::GetTickCount64();
            if (now - g_lastDiagMs < 2000) return;
            g_lastDiagMs = now;
            PLOG_INFO("[VOICE] flow: mic-in={} voice-out={} voice-in={} lastGain={:.2f} peer={} companion={} mode={} ptt=0x{:02X}",
                      g_micIn, g_voiceOut, g_voiceIn, g_lastGain,
                      g_peerConnected.load() ? 1 : 0, g_haveCompanion.load() ? 1 : 0,
                      g_mode.load(), g_pttKey.load());
        }
    }

    void VoiceBridge::Init()
    {
        if (g_sock != INVALID_SOCKET) return;
        WSADATA wsa;
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { PLOG_WARN("[VOICE] WSAStartup failed"); return; }
        g_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock == INVALID_SOCKET) { PLOG_WARN("[VOICE] socket() failed"); return; }
        u_long nb = 1; ::ioctlsocket(g_sock, FIONBIO, &nb);  // non-blocking
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_port = htons(kLoopbackPort);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            PLOG_WARN("[VOICE] bind 127.0.0.1:{} failed (voice disabled)", kLoopbackPort);
            ::closesocket(g_sock); g_sock = INVALID_SOCKET; return;
        }
        PLOG_INFO("[VOICE] loopback bound on 127.0.0.1:{}", kLoopbackPort);
    }

    void VoiceBridge::Shutdown()
    {
        if (g_sock != INVALID_SOCKET) { ::closesocket(g_sock); g_sock = INVALID_SOCKET; }
        g_haveCompanion.store(false);
    }

    void VoiceBridge::Service()
    {
        if (g_sock == INVALID_SOCKET) return;
        uint8_t buf[1500];
        for (;;)
        {
            sockaddr_in from{}; int fromLen = sizeof(from);
            int n = ::recvfrom(g_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) break;  // WSAEWOULDBLOCK when drained
            const uint8_t tag = buf[0];
            if (tag == kTagHello) {
                g_companionAddr = from;
                g_haveCompanion.store(true);
                if (!g_companionLogged) { PLOG_INFO("[VOICE] companion registered"); g_companionLogged = true; }
                // Robustness: if the peer connected BEFORE the companion registered,
                // OnPeerUp's SESSION_UP was dropped (no companion address yet). Send
                // it now so a late-registering companion still starts capture.
                if (g_peerConnected.load() && !g_sessionUpSent) {
                    SendSessionUp();
                    PLOG_INFO("[VOICE] session-up (re)sent on companion register");
                }
            } else if (tag == kTagAudio && n > 1) {
                ++g_micIn;
                // PTT is gated HERE because the DLL is in-process with Starfield and
                // GetAsyncKeyState works; the Companion (background) can't see keys
                // while the game holds focus. Open-mic (mode 0) forwards everything.
                bool forward = true;
                if (g_mode.load() == 1) {
                    const int vk = g_pttKey.load();
                    forward = (vk != 0) && ((::GetAsyncKeyState(vk) & 0x8000) != 0);
                }
                if (forward) SendVoiceToPeer(buf + 1, static_cast<size_t>(n - 1));
            } else if (tag == kTagSettings && n >= 1 + 9) {
                float dn, df; std::memcpy(&dn, buf + 1, 4); std::memcpy(&df, buf + 5, 4);
                g_dNear.store(dn); g_dFar.store(df); g_alwaysAudible.store(buf[9] != 0);
                if (n >= 1 + 14) {
                    g_mode.store(buf[10]);
                    int k; std::memcpy(&k, buf + 11, 4); g_pttKey.store(k);
                }
            }
        }
        DiagTick();
    }

    void VoiceBridge::OnPeerUp() noexcept
    {
        g_peerConnected.store(true);
        SendSessionUp();
        PLOG_INFO("[VOICE] peer up -> session-up sent (companion={})", g_haveCompanion.load() ? 1 : 0);
    }

    void VoiceBridge::OnPeerDown() noexcept
    {
        g_peerConnected.store(false);
        g_sessionUpSent = false;
        const uint8_t m = kTagSessionDown; SendToCompanion(&m, 1);
        PLOG_INFO("[VOICE] peer down -> session-down sent");
    }

    void VoiceBridge::OnVoicePacket(const uint8_t* a_data, size_t a_len) noexcept
    {
        if (a_len == 0 || a_len > 1200) return;
        ++g_voiceIn;
        float gain = 1.0f;
        __try
        {
            float local[3] = { 0, 0, 0 }, peer[3] = { 0, 0, 0 };
            uint32_t localCell = 0, peerCell = 0;

            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (pc) {
                const auto& loc = pc->data.location;   // same read as the PKT_POSITION broadcast
                local[0] = loc.x; local[1] = loc.y; local[2] = loc.z;
                localCell = pc->parentCell ? pc->parentCell->formID : 0u;
            }
            auto& rpm = Pulsar::RemotePlayer::GetRemotePlayerManager();
            peer[0] = rpm.GetNetX(); peer[1] = rpm.GetNetY(); peer[2] = rpm.GetNetZ();
            peerCell = rpm.GetRemoteCellID();

            gain = ComputeProximityGain(local, peer, localCell, peerCell,
                                        g_alwaysAudible.load(), g_dNear.load(), g_dFar.load());
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { gain = g_alwaysAudible.load() ? 1.0f : 0.0f; }

        g_lastGain = gain;
        uint8_t out[1500];
        out[0] = kTagAudio;
        std::memcpy(out + 1, &gain, 4);
        std::memcpy(out + 5, a_data, a_len);
        SendToCompanion(out, static_cast<int>(5 + a_len));
    }
}
