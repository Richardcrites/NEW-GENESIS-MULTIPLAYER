# Co-op Downed & Revive (Player Mortality Sync) - Design

**Status:** Design approved 2026-05-25 (brainstorming). Implementation = next session (includes an RE discovery phase). This is **Feature A** of the co-op-combat-mortality area; **Feature B (clone aggro-draw)** is a separate follow-up spec.

## Goal

Make co-op player death meaningful and shared. Each player is a full, independent player (own skills, HP, weapons) and takes NPC damage on their **own** machine (separate HP pools - already working). When a player would die, they instead enter a **downed** state (Starfield's vanilla death = game-over/reload is intercepted); their partner can **revive** them. The downed/revive state is mirrored to the player's clone on the other machine. If both players are downed at once, it's a real game-over.

## Context / current state

- Co-op clones: each player is mirrored on the other machine as a clone (`RemotePlayerManager`). Position/anim/fire-actions already sync. Per `memory/feedback_coop_no_combat_clone`, the clone currently takes no hit reactions and is not a combat target - this feature deliberately extends that (the clone now reflects downed/death, driven by the **real** remote player's state, not local host-side damage → no false deaths).
- Each machine already runs its own NPCs that attack the local player; the local player already loses HP and (vanilla) dies. Confirmed in testing: the client gets NPC aggro and takes damage.
- Combat Sync V2-Step-1 (shipped, earlier commits ) gives us the RE'd engine kill primitive `SafeKillActor` (FUN_141fcb950 @ RVA 0x1FCB950) and the packet/dispatch patterns to reuse.

## Architecture - Essential/Bleedout approach

Use the engine's own essential-actor **bleedout** as the downed state instead of building one from scratch:

1. **Death intercept:** flag both players `kEssential` (`Actor` bool/flag). In Bethesda engines, essential actors at 0 HP enter bleedout (collapse, incapacitated, no game-over) rather than dying. We get the downed state largely for free.
2. **Downed detection:** detect the local player's bleedout/downed transition (a bleedout flag or "knocked/incapacitated" state + health ≤ 0 while still alive). Edge-triggered.
3. **Downed sync:** on local downed-enter, broadcast `PKT_PLAYER_DOWNED` → the remote machine puts the player's clone into a downed pose (bleedout/ragdoll/down-anim) so the partner sees them on the ground at their position.
4. **Revive:** the partner approaches the downed clone and triggers an interaction (activate / hold-key prompt) → broadcast `PKT_PLAYER_REVIVE` → on the downed player's own machine, end bleedout + restore partial HP (the real revive). The clone then stands back up (revive echoes back via the cleared downed state).
5. **Both downed:** if both players are downed simultaneously → real game-over for both (let vanilla death proceed / force it, e.g. clear essential or `SafeKillActor`-equivalent), both reload → rejoin via the existing save-bridge flow.

**Authority:** a player's HP, downed, and revive are authoritative on **their own** machine. The other machine only mirrors the result onto the clone. No cross-machine player-damage forwarding (separate HP pools). This avoids false deaths (clone can never die from host-side NPC fire - only from the real remote player's downed/death state).

## Components

| Unit | Responsibility |
|---|---|
| Death-intercept | Set `kEssential` on both players at session start; verify bleedout engages. |
| Downed detector | Per-tick (or event) detection of local player downed-enter / revive / death; edge-triggered, one-shot per transition. |
| Downed sync (send) | Broadcast `PKT_PLAYER_DOWNED` / `PKT_PLAYER_REVIVE` with player id + state. |
| Clone downed apply (recv) | Put/clear the remote player's clone in the downed pose on receipt. |
| Revive interaction | Detect the local player activating a downed clone → send `PKT_PLAYER_REVIVE`. |
| Both-downed handler | Detect both-downed → real game-over path. |

## Packets (new)

- `PKT_PLAYER_DOWNED` - {playerId/sourcetag, downed=1/0 or enter/exit, optional pos}. Either side → peer.
- `PKT_PLAYER_REVIVE` - {targetPlayer} - peer instructs the downed player's machine to revive (or local revive → broadcast the resulting cleared state). Exact direction settled in the plan.

(IDs: next free after 0x96 = 0x97/0x98. Own framing like the combat packets.)

## RE discovery needed (first implementation step - discovery-first, per project convention)

1. **Does `kEssential` give the PLAYER a bleedout/downed state** (vs. vanilla game-over)? Players aren't normally essential - verify in-game + Ghidra (the death/game-over trigger and the bleedout branch). The whole approach hinges on this; if it doesn't hold, fall back to approach 2 (hook the death function) or 3 (HP-floor + custom state machine).
2. **How to read the downed/bleedout state** (flag/offset) for detection.
3. **How to end bleedout + restore HP** (the revive primitive) - likely an AV restore + a clear-bleedout call; RE if no clean API.
4. **Downed pose on the clone** - can we drive a bleedout/ragdoll pose on the clone, or reuse `SafeKillActor`-style ragdoll without true death?

## Out of scope (this spec)

- **Feature B - clone aggro-draw** (making local NPCs target/engage the remote clone so aggro splits between both players). Separate spec/plan after A.
- Continuous player HP-bar sync between machines (separate HP pools by design).
- Downed-state polish (revive progress bar, downed timer/bleed-to-death, UI prompts) - V2 of this feature.

## Verification

No unit-test harness (project convention) - verified in two-machine co-op + logs:
1. Player taken to 0 HP by NPCs enters downed (no game-over), clone shows downed on the partner's screen.
2. Partner revives → downed player back up with partial HP, clone stands.
3. Both downed → game-over/reload for both.
Log prefix e.g. `[DOWNED]`.

## Decomposition reminder

This area = **A: Downed/Revive (this spec)** → then **B: clone aggro-draw**. Each its own spec → plan → build. Start with A.
