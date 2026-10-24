# Host-Authoritative NPC Sync (Locomotion-Anim + Combat) - Design / Beta

**Status:** Spec. Foundation (position/mesh) shipped + gated; this is the
"last mile" for faithful NPC sync. Not Alpha-blocking.

## Problem
On the client, host-driven NPCs (incl. follower companions like Sarah) don't
look right in combat:
- They **float** - body glides to the host position but feet barely shuffle
  (locomotion anim doesn't fully commit).
- **Shooting isn't synced** - each side's local AI decides independently when an
  NPC fires; client shots aren't seen by host and vice-versa (and not all shots).
- **Strafe / special poses** (cover, reload, melee) don't match the host.

## Root cause (reverse-engineered 2026-06-17)
The engine commits the walk/run anim only when the anim state-machine receives
**Action events** (ActionIdleWalk / ActionRun …), which are normally dispatched
by the NPC's **AI** as it moves. This creates a hard trade-off that every past
attempt hit by doing only ONE half:

| Approach tried | Result |
|---|---|
| Let AI run (current, reverted state) | Anim plays (AI dispatches events) BUT AI **fights**: follows client, runs its own combat, holds aim-stance that gates locomotion off → float |
| Block `EvaluatePackage` (AI) - NPCSync 2026-04-23 | NPC stops fighting BUT anim **never commits** (no Action events) → static/float |
| `AIProcess+0xE7=1` (MC::Update no-op) - 2026-04-24 | Actors **vanish** (flag consumed by other subsystems) |
| Self-dispatch `moveStart`/`moveStop` (NotifyMovementEvent) | Not rich enough to commit the locomotion state without the AI |
| DirectControl `SetSpeed` on NPC, **AI still on**  | Glides but T-pose (DirectControl ⇄ running AI conflict) |

### The key insight
The **player clone is the proof it can work**: it is a smooth, animated puppet.
It does **both halves together**:
1. **Blocks `EvaluatePackage`** (HookedEvaluatePackage, static cells) → no AI fight.
2. **Drives `IMovementDirectControl::SetSpeed`** → the engine's locomotion system
   plays the walk/run anim *without* needing AI-dispatched Action events.

**No NPC attempt ever combined block-AI + DirectControl.** Each did one half and
hit the conflict. The untried, promising path is the clone's full recipe.

## Proposed design
Replicate the clone's puppet recipe for tracked NPCs, host-authoritative:

### A. Locomotion (anim + position)
- **Block AI** for tracked NPCs via the existing `HookedEvaluatePackage`
  (`IsRemoteOwned`), **static cells only** - proc cells MUST pass through
  (documented silent-death crash otherwise, RPM:111).
- **Enable `IMovementDirectControl` + per-frame `SetSpeed`(groundSpeed)** so the
  engine plays the matching walk/run cycle with no AI (clone vt[1]=Enable,
  vt[3]=SetSpeed; per-NPC state).
- **Position** stays on the shipped Havok-CC-body writer (`SafeNpcHavokBodySetPos`
  + data.location) - no `Actor::SetPosition` (resets anim). Keep ground-speed +
  heading from the derived velocity.
- Proper **DirectControl release** on untrack / death / cell-change (the de-risk
  left this open; needed so NPCs don't freeze after combat).

### B. Combat / actions (host → client)
With AI blocked, the NPC won't fire on its own → combat must be host-driven:
- Host detects its NPC's **fire / reload / power-attack** action and sends an
  event (extend the existing NPC packet or a new PKT). Client replays it as a
  **cosmetic** action (fire anim + muzzle; damage stays host-authoritative - same
  pattern as Combat Sync clone-shots `AttackDamageMult=0`).
- Tie into **Combat Sync (/)** - the kill/damage path already exists;
  this adds the *visible firing cadence*.

### C. (Optional, fidelity) Host anim-state passthrough
If derived locomotion still mismatches strafe/special poses: host reads the real
graph vars / action state of its NPC and sends them; client writes 1:1.

## Known dead-ends (do NOT repeat)
- AI-block in **proc cells** → silent-death crash (must pass through there).
- `AIProcess+0xE7=1` → actors vanish.
- `Actor::SetPosition` per-frame → resets anim state-machine (use Havok-body).
- `bGraphDriven=1` → froze the anim graph.

## Open RE
- Exact **fire/attack action** function on the host NPC to hook/read (Ghidra:
  weapon-fire dispatch on Actor / combat controller) for B.
- Whether NPC DirectControl behaves like the clone's when AI is *also* blocked
  (the combo is untested) - primary risk to validate first.
- DirectControl Enable/Disable vtable indices for clean release (vt[1]=Enable
  known; Disable index to confirm).

## Phasing
1. **P1 - validate the combo:** block-AI (static) + DirectControl SetSpeed on
   tracked NPCs, gated behind the existing `npcDirectControl` ini. Watch for
   vanish / proc-cell crash / anim. This is the make-or-break experiment.
2. **P2 - release lifecycle** (untrack/death/cell) so nothing freezes.
3. **P3 - combat action sync** (host fire → client cosmetic), tie into.
4. **P4 - optional host anim-state passthrough** for strafe/special poses.

## Current shipped state (foundation, gated `npcDirectControl=1`)
- Havok-CC-body mover (smooth position, no anim reset) - `a847289`.
- Ground-speed-driven graph-var + NotifyMovementEvent anim re-assert.
- `=0` → legacy path (animated via AI, jitters) - safe Alpha default.
