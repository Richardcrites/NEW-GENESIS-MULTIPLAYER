#pragma once

#include <cstdint>

#include <RE/N/NiPoint.h>

namespace RE { class Actor; }

namespace Pulsar::NPCSync {

    // Host-driven locomotion frame for a non-owned Actor.
    //
    //  targetPos   - world-space target (cm).
    //  targetRotZ  - world yaw in radians.
    //  speed       - host forward speed (Starfield units/s, matches the "Speed"
    //                graph var scale).
    //  direction   - body-relative heading offset in radians, matches the
    //                "Direction" graph var ([-pi, pi]).
    //  moveFlags   - bit 0 = moving, bit 1 = sprinting.
    //  weaponDrawn - host weapon-draw state; retained for future use (sprint
    //                overrides etc.), currently informational.
    struct LocomotionInput {
        RE::NiPoint3 targetPos;
        float        targetRotZ;
        float        speed;
        float        direction;
        uint8_t      moveFlags;
        bool         weaponDrawn;
    };

    // Apply one frame of host-driven locomotion to a non-owned Actor.
    //   1. Writes graph vars (Speed, Direction, iSyncIdleLocomotion,
    //      iSyncSprintState) via the anim-graph's direct vtable.
    //   2. Writes Havok linear velocity on the CharController (vtable slot 79 /
    //      byte 0x278), gated on the same two-level attach check used for the
    //      Player clone.
    //   3. Lerps rotZ toward target (factor 0.2).
    //   4. Snaps via Actor::SetPosition only when positional drift exceeds
    //      the threshold (~2 m).
    //
    // Safe on Actors with partial/absent anim graphs and un-attached Havok
    // bodies (all inner writes are SEH-guarded).
    //
    // Returns true if the actor was non-null and the function ran.
    bool ApplyLocomotion(RE::Actor* actor, const LocomotionInput& in);

    // Per-tick position interpolation toward the last-known host target.
    // Called from the client HookedActorUpdate AFTER g_origActorUpdate so we
    // overwrite whatever the local AI / navmesh moved the actor to. Without
    // this, remote-driven NPCs visibly teleport at packet rate (5-30 Hz)
    // because nothing moves them between incoming updates.
    //
    //   factor - fraction of the remaining delta to consume this tick
    //            (e.g. 0.25 gives ~75 % convergence over 5 ticks at 60 Hz).
    //
    // A no-op for actors with no stored target (i.e. not tracked by us).
    // SEH-wrapped internally; safe during cell transitions / actor teardown.
    void TickLerp(RE::Actor* actor, float factor);

    // KANDIDAT 2 (de-risk): enable/disable the per-NPC IMovementDirectControl
    // path at runtime (set from PULSAR.ini npcDirectControl, or env PULSAR_NPC_DC=1).
    // When on, TickLerp drives tracked NPCs via the engine's locomotion instead of
    // the data.location lerp + SetPosition snap.
    void SetNpcDirectControl(bool on);

    ///ragdoll-fix: immediately release a tracked NPC's DirectControl pin +
    // AI-block. Call BEFORE SafeKillActor on a host-synced NPC so the engine death
    // handler ragdolls a FREE body instead of one pinned upright (the intermittent
    // "dead but standing / no ragdoll" freeze). Idempotent + SEH-safe; no-op if
    // formID isn't a DirectControl puppet.
    void ReleaseNpcDirectControl(uint32_t formID);

    // #ragdoll - mark an NPC kill-pending for durationMs. The engine kill is async
    // (IsDead lags several frames); while pending, ApplyLocomotion + TickLerp stop
    // driving/pinning it so the body stays free and ragdolls when death lands. Call
    // right after SafeKillActor on a host-synced NPC.
    void MarkNpcKillPending(uint32_t formID, uint64_t durationMs);

    // A/B diag (PULSAR.ini npcNoImc=1): skip IMovementDirectControl entirely and
    // drive tracked NPCs via graph-vars + mesh-move only. Isolates whether IMC's
    // SetSpeed is the "walk turns into jog after settle" cause.
    void SetNpcNoImc(bool on);

    // True iff we hold a live LerpTarget for this formID. Used by the client
    // HookedActorUpdate to decide whether to skip the engine's own Actor::Update
    // (tracked = we drive everything, AI must not override). Mirrors the
    // g_remoteOwned pattern the player clone uses.
    bool IsTracked(uint32_t formID);

    // Pre-tick graph-var write. Called BEFORE g_origActorUpdate so the
    // engine's anim-graph tick in the same frame reads our values. Writing
    // graph vars AFTER origUpdate lags the anim blend tree by one frame and
    // empirically stops the walk-cycle from committing (the transition gate
    // keeps seeing idle values at read time).
    void PreTickWriteGraphVars(RE::Actor* actor);

    // Diagnostic readback for the three-point timing probe. `label` is a
    // short string ("PRE", "MID", "POST") tagged in the log so we can spot
    // engine-overwrites between our writes and the anim tick.
    void ReadGraphSnapshot(RE::Actor* actor, const char* label);

    // Dispatch animation-graph state-transition events (moveStart / moveStop
    // / runStart / sprintStart). Graph vars alone don't commit the walk
    // state for native NPCs whose local AI isn't dispatching events - the
    // engine's state-machine gate waits for an explicit transition signal.
    // Tracks last state per-actor so events fire only on state change.
    void NotifyMovementEvent(RE::Actor* actor, float speed, uint8_t moveFlags);

}
