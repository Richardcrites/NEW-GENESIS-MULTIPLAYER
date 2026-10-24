#pragma once

#include <cstdint>
#include <cstddef>

// Co-op Kill-XP-Sharing.
//
// In a shared co-op area, when EITHER player kills an NPC, BOTH players receive
// that kill's XP. Vanilla only awards XP to whoever dealt the killing blow on
// the LOCAL copy of the NPC; a scripted mirror-kill (NPCSync SafeKillActor on the
// non-participant's machine) awards no natural XP, so today the non-participant
// gets nothing. This module closes that gap.
//
// CAPTURE: we hook the engine XP-award primitive FUN_141962310 (RVA 0x1962310).
//   On a KILL award (source==7) to the local player we read the final awarded XP
//   (the function's return value) + the victim, canonicalize the victim to the
//   HOST FormID, record it in a local "natural kills" set, and broadcast it.
// APPLY: on PKT_KILL_XP we award the SAME XP to our own player via the same
//   primitive with source=0xc (no multiplier, and our capture hook ignores 0xc
//   so there is no echo) UNLESS we already earned natural XP for that host-ref
//   (the dedup set) - which is exactly the both-shoot case, so no double-count.
//
// Gated by Config::coopShareKillXp (default on). RE: see
// reference_kill_xp_re memory + docs/design/specs/2026-06-24-coop-kill-xp-sharing-design.md.
namespace Pulsar::Coop::KillXp
{
    // Install the FUN_141962310 hook (MinHook). Safe to call once at plugin load
    // (post-SFSE). Logs the resolved address + prologue bytes for 244 verify.
    void Init();

    // Game-thread: drains queued shared-XP awards received from the peer.
    // Call every frame from the main game-thread tick (bidirectional).
    void OnGameThreadTick();

    // Network thread: a PKT_KILL_XP arrived. Parses + queues; the award itself
    // runs on the game thread (OnGameThreadTick).
    void OnKillXpReceived(const std::uint8_t* a_data, std::size_t a_length);
}
