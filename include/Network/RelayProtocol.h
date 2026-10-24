#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <string>

// Wire contract shared with the C# relay (lobby/relay/Program.cs). Control packet
// = magic(8) + type(1) + session(16) = 25 bytes; anything else is forwarded.
namespace Pulsar::Network::Relay
{
    inline constexpr std::array<uint8_t, 8> kMagic = { 'P','U','L','S','A','R','L','Y' };
    inline constexpr size_t  kSessionBytes = 16;
    inline constexpr size_t  kControlLen   = 8 + 1 + kSessionBytes; // 25
    inline constexpr uint8_t kTypeRegister = 1;
    inline constexpr uint8_t kTypeJoin     = 2;
    inline constexpr uint8_t kTypePeer     = 3;
    inline constexpr size_t  kPeerLen      = kControlLen + 6; // 25 + IPv4(4) + port(2) = 31

    // Decode 32 hex chars -> 16 bytes. Returns false on bad length/char.
    inline bool DecodeSession(const std::string& hex, uint8_t out[kSessionBytes])
    {
        if (hex.size() != kSessionBytes * 2) return false;
        auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < kSessionBytes; ++i) {
            int hi = nyb(hex[i * 2]), lo = nyb(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            out[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return true;
    }

    inline std::array<uint8_t, kControlLen> MakeControl(uint8_t type, const uint8_t session[kSessionBytes])
    {
        std::array<uint8_t, kControlLen> b{};
        for (size_t i = 0; i < kMagic.size(); ++i) b[i] = kMagic[i];
        b[8] = type;
        for (size_t i = 0; i < kSessionBytes; ++i) b[9 + i] = session[i];
        return b;
    }

    // PEER (relay -> peer): control header + peer's public endpoint.
    // ipNetOrder = IPv4 already in network byte order; port written big-endian.
    inline std::array<uint8_t, kPeerLen> MakePeer(const uint8_t session[kSessionBytes],
                                                  uint32_t ipNetOrder, uint16_t portHostOrder)
    {
        std::array<uint8_t, kPeerLen> b{};
        for (size_t i = 0; i < kMagic.size(); ++i) b[i] = kMagic[i];
        b[8] = kTypePeer;
        for (size_t i = 0; i < kSessionBytes; ++i) b[9 + i] = session[i];
        b[25] = static_cast<uint8_t>(ipNetOrder & 0xFF);
        b[26] = static_cast<uint8_t>((ipNetOrder >> 8) & 0xFF);
        b[27] = static_cast<uint8_t>((ipNetOrder >> 16) & 0xFF);
        b[28] = static_cast<uint8_t>((ipNetOrder >> 24) & 0xFF);
        b[29] = static_cast<uint8_t>((portHostOrder >> 8) & 0xFF); // big-endian
        b[30] = static_cast<uint8_t>(portHostOrder & 0xFF);
        return b;
    }

    // Cheap classify for the ENet intercept: magic + a known control type.
    inline bool IsControlMagic(const uint8_t* d, size_t n)
    {
        if (!d || n < kControlLen) return false;
        for (size_t i = 0; i < kMagic.size(); ++i) if (d[i] != kMagic[i]) return false;
        return d[8] == kTypeRegister || d[8] == kTypeJoin || d[8] == kTypePeer;
    }

    // Parse a PEER datagram. Out: ipNetOrder (-> ENetAddress.host), portHostOrder (-> .port).
    inline bool ParsePeer(const uint8_t* d, size_t n, uint32_t& ipNetOrder, uint16_t& portHostOrder)
    {
        if (n < kPeerLen || !IsControlMagic(d, n) || d[8] != kTypePeer) return false;
        ipNetOrder = static_cast<uint32_t>(d[25]) | (static_cast<uint32_t>(d[26]) << 8) |
                     (static_cast<uint32_t>(d[27]) << 16) | (static_cast<uint32_t>(d[28]) << 24);
        portHostOrder = static_cast<uint16_t>((static_cast<uint16_t>(d[29]) << 8) | d[30]);
        return true;
    }
}
