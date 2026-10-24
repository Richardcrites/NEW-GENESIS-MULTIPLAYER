# Co-op Downed & Revive - Redesign (Keep-Alive Custom Downed State)

**Date:** 2026-06-06
**Task:** Co-op Downed & Revive (Feature A) - player mortality sync
**Module:** `src/Coop/CoopMortality.cpp`
**Status:** Design approved 2026-06-06, supersedes the death-mask / bleedout approaches.

## Goal

A co-op player who would die instead enters a **downed** state: frozen and
incapacitated but alive, mirrored to the partner. They recover when (a) a partner
revives them, (b) combat ends, or (c) both players are down → game-over reload.
This redesign delivers the **MVP**: a *stable* downed state (no fall-through, no
flip) with a clean stand-up, using combat-end + F10 + both-down. Medpack revive
and a proper downed pose are explicit follow-ups.

## Background - why a redesign (do NOT re-try these)

Five in-game iterations on 2026-06-06 proved the player has **no usable engine
downed-state**:

- **Death corpse (kDead):** the PLAYER is never flagged `kDead` (boolBits 0x...1A2,
  no bit 0x800) - player death is a *reload game-over*, not an NPC corpse. So
  `Actor::Resurrect` is a no-op on the player (it early-returns unless `IsDead()`),
  and there is no engine "revive the player" path. Clearing +0xF8 death bits or
  setting kDead+Resurrect both left the player ragdolled, head through the floor.
- **Bleedout (life-state 7):** the engine **reverts the player out of bleedout to
  alive every frame** (proven: the game-thread read life-state = 0 each frame).
  Forcing bleedout via a setter-hook produced a bleedout↔alive flip ~60/sec that
  physically shredded the ragdoll → through the floor.

Conclusion: stop fighting the engine's death/bleedout. **Never let the player die;
build the downed state ourselves while the player stays `alive` (life-state 0).**

## Architecture - Keep-Alive (approved Approach 1)

The player is held in the normal **alive** life-state the entire time they are
downed. We intercept the engine's attempt to kill the player, suppress it, freeze
the player via control-lock, and clamp HP positive so the engine stops retrying.
Because the player never leaves `alive`, there is no ragdoll, no bleedout flip, and
the reload never arms (the reload keys off the death life-state, which we never
allow for a single down).

State lives in `src/Coop/CoopMortality.cpp`. The bleedout/Resurrect/death-mask code
(`SafeResurrect`, `SafeClearDeathState`, the death→bleedout redirect, the
`g_reviveGuardUntilMs` guard, the death-tick masking for single-down) is removed and
replaced by the components below. The death-tick hook (`FUN_141a5ba50`, REL::ID
102812) is retained ONLY for the both-down game-over reload + load-counter clear.

## Components

### 1. Entry trigger - life-state setter hook (retained, repurposed)
Hook `REL::ID 100787` (the life-state setter, `void __fastcall(Actor*, uint32 enum)`).
For the local player only (`SafeIsLocalPlayer`), when the engine sets a death enum
(1 or 2) and we are NOT both-down:
- Set `g_localDowned` (atomic) on the false→true edge; `SendDownedState(true)`.
- **Swallow** the call - return WITHOUT calling the original setter. The life-state
  is left untouched (stays `alive`/0). No bleedout, no death.
- Repeated death-sets while already downed are likewise swallowed (cheap no-op;
  nothing is re-applied, so there is no flip).

NPCs and non-death sets pass straight through to the original setter unchanged.
HP is NOT raised here (the hook runs on engine worker threads;
`ModActorValue` must run on the game thread) - see component 2.

### 2. HP clamp (game thread, `OnGameThreadTick`)
While `g_localDowned`: clamp Health to a positive floor (`kDownedFloor = 25`) each
frame via `SafeRaiseHealthTo` (ModActorValue kDamage += deficit, the proven path).
With HP > 0 the engine stops issuing player death-sets → fully stable, no spam.

### 3. Control-lock (NEW - the only open RE)
On entry (game thread): disable player controls so the player is frozen
(no movement / attack / activate). On revive: re-enable.
**Open RE:** find the clean engine path - candidates: `RE::PlayerControls` /
`RE::ControlMap` toggle, or the function dialogue/cutscenes use to lock player input.
The implementation plan's first task is to identify and verify this (versionlib +
Ghidra-confirm only if needed; one agent max - see the token-budget rule). If a
clean control-lock is not found quickly, MVP fallback = clamp movement
(SpeedMult 0) + block weapon, documented as a stopgap.

### 4. Revive - `ReviveLocalPlayer(hpFraction, reason)` (simplified)
1. `g_localDowned = false`; `SendDownedState(false)`.
2. Restore Health to `hpFraction * permanentMax` (floor fallback).
3. Re-enable controls (component 3).
No life-state change, no Resurrect - the player was always alive.
Callers: Way B combat-end (existing manual `highActorHandles` hostile scan, revive
at 0.25), F10 debug (revive at 0.5).

### 5. Both-down → game-over
When `g_localDowned && g_remoteDowned` (checked in `OnGameThreadTick` and the
death-tick hook): stop the HP clamp, force real death via the original setter
(`SafeSetLifeStateRaw(player, deathEnum)`), and clear the engine load counter
(0x5E794DC) every frame so the imminent reload avoids the relaunch+int3 trap
(V13m). The vanilla death-tick reload then arms → both sides load their last save.
The setter hook's `!bothDowned` condition lets the forced death through.

### 6. Net-sync (existing, unchanged)
`PKT_PLAYER_DOWNED 0x97` (`PlayerDownedData{downed,slot}`), bidirectional, reliable.
`g_remoteDowned` mirrors the peer. Post-load recovery: on a fresh load with the
player alive, clear stale `g_localDowned`/`g_remoteDowned`.

## Data flow

```
HP→0  → engine SetLifeState(player, death)
      → [setter hook] swallow + g_localDowned=true + PKT_PLAYER_DOWNED(1)   (life-state stays alive)
      → [game thread] HP clamp to 25 + DISABLE controls                      (frozen, stable)
partner/peer ← PKT_PLAYER_DOWNED(1) → g_remoteDowned=true
revive (Way B combat-end / F10):
      → ReviveLocalPlayer: g_localDowned=false + PKT_PLAYER_DOWNED(0) + HP restore + ENABLE controls
both down (g_localDowned && g_remoteDowned):
      → stop clamp + force real death + clear load counter → vanilla reload (both)
```

## MVP scope

- **In:** stable keep-alive downed state, control-lock, HP clamp, combat-end
  auto-revive (Way B, existing), F10 debug revive, both-down game-over, net-sync.
- **Step 2 (next plan):** medpack revive via the Activate hook (F.7, REL::ID 63380)
  - branch when the local player activates the downed remote clone → SendRevive.
  Replaces the proven-broken `GetAsyncKeyState(E)` poll (E is engine-bound to
  Activate → opens the "Reden" dialog).
- **Polish (later):** a real downed pose/animation (MVP = upright frozen);
  first-person vignette; both-down packet-delay latch (~6s recv-queue backlog).

## Out of scope
Continuous HP-bar cross-sync (separate pools by design). Clone aggro-draw
(Feature B). Revive UI bar / bleed-to-death timer. Quest/scripted-death handling
(co-op downed overrides player death generally).

## Error handling
All engine pointer derefs / virtual calls stay `__try/__except`-wrapped
(no C++ destructor objects inside `__try`). The setter hook filters to the local
player and passes everything else through untouched - NPC death/combat-sync must be
unaffected. ENet sends are thread-safe (calls from the worker-thread hook are OK).

## Testing (manual, in-game - no unit-test framework for engine behavior)
1. One player down → **frozen upright, NO fall-through**, controls locked; partner
   sees the clone standing still. Log: single `entered downed` (no per-frame spam).
2. Combat ends (all hostiles dead) → auto-revive, controls return, player moves.
3. F10 while downed (keep one enemy alive so Way B holds off) → revive.
4. Both down → both reload last save, no host crash.
5. Regression: NPC/enemy death still works normally (setter hook player-only).

## Risks
- **Control-lock RE** is the main unknown; MVP fallback documented (component 3).
- HP clamp must actually stop the engine death-spam (high confidence; HP>0 removes
  the death trigger). If not, swallowing alone already keeps life-state alive.
- "Frozen upright" may look odd - accepted for MVP; pose is later polish.
