#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace Pulsar::Network
{
    // Per-connection end-to-end crypto channel.
    //
    // Ephemeral ECDH (NIST P-256) key agreement + AES-256-GCM authenticated packet
    // encryption, all via Windows CNG (bcrypt) so there is no third-party dependency.
    // Both peers generate a fresh keypair per connection, exchange PUBLIC keys (safe in
    // the clear), derive the SAME shared key locally, and from then on every packet is
    // encrypted+authenticated. The relay and any network observer only ever see
    // ciphertext: this is end-to-end between the two players, so even the relay operator
    // cannot read gameplay traffic.
    //
    // Wire format of an encrypted packet: [12B nonce][ciphertext][16B GCM tag].
    // Thread-safe: an internal mutex guards the key handle for concurrent send/recv.
    class CryptoChannel
    {
    public:
        CryptoChannel() = default;
        ~CryptoChannel();
        CryptoChannel(const CryptoChannel&) = delete;
        CryptoChannel& operator=(const CryptoChannel&) = delete;

        // Fresh ephemeral keypair for a new connection. Clears any previous state.
        bool Init();

        // Authenticated key agreement: mix a shared secret (delivered out-of-band over
        // the TRUSTED DB/TLS channel, NEVER through the relay) into the key derivation,
        // so the derived key = Hash(secret || ECDH). A man-in-the-middle relay can swap
        // the ECDH public keys but does NOT know this secret, so it can never compute the
        // session key: this defeats an ACTIVE MITM by a malicious relay operator, not just
        // a passive one. Call BEFORE DeriveFromPeer. Empty secret = legacy unauthenticated
        // ECDH (passive-only protection; used only where no DB channel exists, e.g. pure
        // direct-IP play with no relay in the middle).
        void SetAuthSecret(const uint8_t* a_secret, size_t a_len);

        // Our public key (BCRYPT_ECCPUBLIC_BLOB) to hand to the peer.
        std::vector<uint8_t> PublicBlob() const;

        // Derive the shared AES key from the peer's public blob. Ready() == true after.
        bool DeriveFromPeer(const uint8_t* a_peerBlob, size_t a_len);
        bool Ready() const;

        // out = [12B nonce][ciphertext][16B tag]. Returns false on failure.
        bool Encrypt(const uint8_t* a_pt, size_t a_len, std::vector<uint8_t>& a_out);
        // Reverse + verify the auth tag. Returns false (out empty) on tamper/failure.
        bool Decrypt(const uint8_t* a_in, size_t a_len, std::vector<uint8_t>& a_out);

        void Reset();

        // Overhead added per packet (nonce + tag).
        static constexpr size_t kNonceLen = 12;
        static constexpr size_t kTagLen   = 16;
        static constexpr size_t kOverhead = kNonceLen + kTagLen;

        // One-shot primitive self-test: two channels agree a key, GCM round-trips a
        // message, and a tampered ciphertext is rejected. Run once at startup.
        static bool SelfTest();

    private:
        mutable std::mutex   m_mtx;
        void*                m_hAesKey    = nullptr;  // BCRYPT_KEY_HANDLE
        void*                m_hMyKeyPair = nullptr;  // BCRYPT_KEY_HANDLE (ephemeral)
        std::vector<uint8_t> m_pubBlob;
        std::vector<uint8_t> m_authSecret;   // DB-delivered shared secret mixed into the KDF (anti-MITM); empty = none
        bool                 m_ready = false;
    };
}
