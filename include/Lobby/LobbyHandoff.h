#pragma once

#include <cstdint>
#include <string>

// Teil 2C - DLL glue between the PULSAR companion (Teil 2B) and the game.
// The companion writes %LOCALAPPDATA%\PULSAR\pulsar_handoff.json (role + host
// address) and pulsar_profile_in.json (the Teil-1 profile blob to apply), then
// launches Starfield. The DLL reads them at load to drive host/client + connect
// target, applies the stored profile after the player is ready, and writes
// pulsar_profile_out.json on save for the companion to upload.
//
// ALL functions are no-ops if no handoff/profile files exist, so normal play
// (no companion) is completely unaffected.
namespace Pulsar::Lobby
{
    struct Handoff
    {
        bool        found   = false;
        std::string role;            // "host" | "client"
        std::string hostIP;          // parsed from "ip:port" (direct/LAN fallback)
        uint16_t    port    = 0;
        uint32_t    version = 0;
        std::string relayAddr;       // "host:port" of the relay; empty = direct mode
        std::string relaySession;    // 32 hex chars; empty = direct mode
        std::string cryptoSecret;    // per-lobby E2E auth secret (anti-MITM), delivered via the
                                     // trusted DB; mixed into the key derivation. Empty = none.
    };

    // %LOCALAPPDATA%\PULSAR\ (created if missing). Shared with the companion.
    std::string LobbyDir();

    // Read pulsar_handoff.json. Handoff.found == false if the file is absent.
    Handoff ReadHandoff();

    // Game-thread tick. Once the player is ready, apply pulsar_profile_in.json
    // via Pulsar::Profile::ApplyProfile (one-shot). No-op if no profile-in file.
    void TickApplyProfile();

    // Serialize the live profile to pulsar_profile_out.json for the companion
    // to upload. Safe to call on save/logout.
    void WriteProfileOut();

    // Auto-save: RequestAutoSave() is thread-safe (sets a flag); TickAutoSave()
    // does the actual capture on the game thread (safe-point gated + debounced).
    // NOTE (A unification): TickAutoSave is gated on
    // !DirectRestore::HasBridgedThisSession() - a disconnect after a co-op
    // session leaves the player in the host-merged world, so capturing there
    // would re-poison the DB (Bug A). The clean profile is maintained by
    // TickCleanProfileCapture below instead.
    void RequestAutoSave();
    void TickAutoSave();

    // A unification Phase 3 - clean DB-profile auto-capture. Game-thread tick.
    // Captures the live player profile to pulsar_profile_out.json (which the
    // companion uploads to the DB) ONLY while no host bridge has occurred this
    // launch (DirectRestore::HasBridgedThisSession() == false) - i.e. from the
    // client's OWN loaded save, which is host-free. Fires on the player-ready
    // false→true transition (a fresh save-load) and at a slow steady-state
    // cadence so solo progression travels to the next session. This replaces
    // the disconnect-time capture as the authoritative-profile source and makes
    // a freshly-saved clean loadout land in the DB automatically (no F10).
    // Skips the write when the serialized blob is unchanged (no DB spam).
    void TickCleanProfileCapture();
}
