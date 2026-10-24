# Co-op Kill-XP-Sharing - Design + RE (Task)

> Status: DESIGN / RE COMPLETE. **No code yet** (user: "Schau einfach mal plan das revers aber code noch nichts").
> Engine: Starfield 1.16.236. RE verified in Ghidra (StarfieldRE.gpr).

## Goal

In a shared co-op area, when **either** player kills an NPC, **both** players receive that kill's XP.
Today only the killer gets it; the non-participant gets nothing.

## Why both-shoot already shares XP today (RE'd)

Each machine runs its **own** copy of every NPC. Kill XP is awarded by the engine's death
handler **only to whoever dealt the killing blow on the LOCAL copy** (a natural combat death).

- Both shoot the same NPC → on each machine the local player kills the local copy → **each engine awards its own player** → both get XP. (This is the "already works" case the user saw.)
- One player kills → killer's engine awards the killer. The other machine kills its matched
  copy via our **scripted** mirror (`SafeKillActor`, NPCSync.cpp), and a scripted `Kill()`
  does **not** run the natural kill-XP award → the non-participant gets nothing. **This is the gap.**

## Engine RE (all verified, 1.16.236)

### Kill-XP award chain
- Death handler **`FUN_141908af0`** (RVA `0x1908af0`) - we already hook it (CoopMortality
  `Hooked_DeathHandler`, `kDeathHandlerRVA=0x1908af0`; ABI `void(Actor* victim=param_1, Actor* killer=param_2, void*, void*, byte, char)`).
  Internally it awards kill XP:
  ```c
  FUN_141962310(player /*=DAT_145f4afd0*/, baseKillXp /*XMM1*/, 7 /*KILL source*/, 1 /*applyMult*/,
                victim /*param_5*/, victim /*param_6*/, killerCtx /*param_7*/);
  ```

### The engine XP-award primitive - `FUN_141962310` (RVA `0x1962310`)
```c
uint64_t FUN_141962310(Actor* target, float xp /*XMM1*/, int source, char applyMult,
                       void* p5, void* victim /*p6*/, void* p7);
```
- Entry guard: proceeds iff `xp > 0` OR `source ∈ {10,11}`. (For our award `source=0xc=12`, needs `xp>0`.)
- `applyMult==1` → multiplies `xp` by a level/difficulty factor
  (`(levelFactor * DAT_145ee45a0 + DAT_145ee4580) * xp`, floored at `DAT_144f08578`), fires an
  experience event (`FUN_140c94d90(0x16, ...)` with victim/killer context).
  `applyMult==0` → **adds the raw `xp` unchanged** (what we want for a pre-computed share).
- Then **unconditionally**: `ModActorValue(Experience, xp)` via the ActorValueOwner at
  `target+0x70`, vtable slot `0x28`; the Experience AV form = `*(FUN_14049acc0()+0x100)`.
  Then `FUN_1418d9450(target, source, p5, xp)` = level-up check + HUD "+XP" event dispatch.
- **Return value (RAX low 32 bits) = the final awarded XP** (post-multiplier, rounded).

### XP source enums (3rd arg) observed across the 10 callers
`2`,`3`(quantity/skill),`4`(location discovery),`7`(**KILL** - death handler),`8`,`0xc`(generic `Game.RewardPlayerXP`).
- **`source==7` = kill XP.** Filtering on it isolates kills cleanly.
- `Game.RewardPlayerXP(uint xp, bool)` Papyrus native → handler `FUN_142013030` → `FUN_141962310(player, xp, 0xc, !skip, 0,0,0)`. We award shares with **`source=0xc`** so our own capture hook (which filters `source==7`) never re-captures them → no echo / no recursion.

### Experience AV
Read today in ClientProfile.cpp / DirectRestore.cpp via `LookupByEditorID<ActorValueInfo>("Experience")` + `GetActorValue`. We do NOT need a delta measurement (see "value source").

## Design - `KillXpSync` module (layered on top, does NOT touch combat-mirror)

### Capture (both host & client) - hook `FUN_141962310` @ `0x1962310`
1. Call original → `ret` (final awarded XP).
2. If `coopShareKillXp` && connected && `source==7` && `target==PlayerCharacter::GetSingleton()`
   && `param_6` is a valid Actor that is **not** the player:
   - `victim = (Actor*)param_6`; `finalXp = (uint32)ret`.
   - `hostRef = isHost ? victim->formID : g_npcRemap.localToHost(victim->formID)`
     (reuse the combat-sync remap; if the client has no mapping → skip, NPC not in synced set).
   - `g_myNaturalKills.insert(hostRef)` (bounded recent-set; this is our double-count guard).
   - Broadcast **`PKT_KILL_XP { uint32 hostRefFormID; float xp; }`** (reliable).
- Verify-first: read the prologue bytes of `0x1962310` at install (codebase practice; raw RVA like the kill fn / death handler, no clean REL::ID expected).

### Apply (receiver) - on `PKT_KILL_XP{ hostRef, xp }`
- If `!coopShareKillXp` → ignore.
- If `g_myNaturalKills` contains `hostRef` → **skip** (I already earned natural XP for this NPC). (erase to bound memory)
- Else → queue a **game-thread** award:
  `FUN_141962310(player, xp, 0xc, /*applyMult=*/0, 0, 0, 0)` - SEH-wrapped.
  Gives the exact same XP the killer got, plus the HUD "+XP" and level-up check.

### Why the `g_myNaturalKills` (host-ref keyed) guard prevents double-counting
- One kills: non-participant has no entry → awards once. ✓
- Both kill (each their own copy): each records the **host-ref** in its own set, each sends
  `PKT_KILL_XP{hostRef}`; each receiver finds the host-ref in its own set → skips → both keep
  only their natural XP. ✓  (Key must be host-canonical on BOTH sides - client remaps before
  record AND before send.)
- Residual race (an in-flight bullet lands a natural kill a few ms after a shared award was
  applied) → tiny double; **accepted for Alpha** (user is XP-tolerant). Optional hardening:
  a short "suppress-natural" window per host-ref.

## Interaction with host-shared level (/) - IMPORTANT
- closed "by design": in co-op the client takes the **host's level** (loads host save);
  Level/XP are **not** DB-round-tripped. Kill-XP-sharing is a **live-session** feature: it makes
  both players gain XP together during play. It does **not** change the persistence decision.
- Self-consistency bonus: with full sharing, **every** kill in the shared area grants **both**
  players that kill's XP exactly once → identical total kill XP → they level in lockstep → no
  level cap between players. This reinforces's goal rather than fighting it.
- The Experience AV is treated by the engine as the running progress value used by the level-up
  check; we add to it via the same primitive the engine uses, so local level-ups trigger normally.

## Config
- New `bool coopShareKillXp = true;` in Config.h (default on; gives an off switch).

## Risks / open items (resolve during in-game iteration)
- Confirm `param_6`==victim at runtime (decompile says param_5==param_6==dying actor); fall back to param_5 if needed.
- Confirm `source==7` fires only for genuine player-credited kills and NOT for our scripted
  `SafeKillActor` mirror (user observation says mirror gives no XP → expected). Add a one-shot
  diag log on first capture/apply to verify on both machines.
- Hook safety on `FUN_141962310`: low-frequency (XP events only), game-thread; SEH-wrap the
  award call (it dispatches a BSTEventSource notify via `FUN_1418d9450`/`FUN_1422cb8f0`, the same
  dispatcher we have SEH-wrapped elsewhere).
- Companion/turret kills (player delta via companion): `source==7 target==player` only fires when
  the player is credited; companion-only kills won't share (acceptable).

## Out of scope (Alpha)
- Host-authoritative canonical XP for all kills (each killer-measured value is fine; levels equal).
- Ship/space-combat kills (has its own path).
- DB persistence of shared XP (stays per).

## Verify BOTH build tags before any sync-test diagnosis ([[feedback_verify_both_builds]]).
