# Co-op Revive (Inc4) - Design

**Feature:** Co-op Downed & Revive (Feature A), increment 4 - bringing a downed
player back up.

**Status:** Design approved 2026-05-30. Builds on the confirmed Inc1/Inc3/Inc5 +
V13m work : a downed player's vanilla death-reload is suppressed by
masking the `+0xF8` death bits in `FUN_141a5ba50`; downed state is mirrored over
`PKT_PLAYER_DOWNED 0x97` (atomic `g_remoteDowned`); both-down → controlled
game-over (load last save) with the relaunch-trap neutralized.

## Goal

A single downed player can come back up two ways:
- **A - Medipack revive:** the partner walks to the downed player, holds Activate
  while carrying a healing item; one healing item is consumed and the downed player
  is revived.
- **B - Combat-end auto-revive:** when combat is over, the downed player stands back
  up automatically (no item needed).

Both-down (both players downed at once before any revive) still ends in the existing
game-over (load last save). The revive paths exist to prevent escalation to that
game-over while one player is still up.

## Authority model

Consistent with the rest of Feature A: **the revive is always EXECUTED on the downed
player's own machine.** A partner can only *trigger* a revive over the network; the
downed machine performs the actual state change. This keeps a single owner of each
player's mortality and avoids cross-machine writes to a remote actor.

Shared execution primitive - `ReviveLocalPlayer(float a_hpFraction)` (generalized from
the existing F10 debug revive):
1. Clear the `+0xF8` death/bleedout bits (`SafeClearDeathState`, already exists).
2. Restore Health to `a_hpFraction * permanentMaxHealth` (existing `SafeRaiseHealthTo`
   / `SafeReadMaxHealth`; floor fallback if max unreadable).
3. `g_localDowned = false`.
4. Send `PKT_PLAYER_DOWNED 0` so the peer clears its `g_remoteDowned`.

## Way A - Medipack revive (partner-initiated)

Runs on the **helper's** machine, polled each game-thread frame:

Trigger conditions (all must hold):
- A remote player is currently downed (`g_remoteDowned == true`).
- The helper (local player) is within `kReviveRadius` (~2.5 m) of the downed player's
  clone - clone position via `RemotePlayerManager`.
- The helper is holding the Activate key for `kReviveHoldSeconds` (~2.0 s) continuously
  (accumulated hold time; resets if released or out of range).
- The helper has at least one healing item in inventory.

On success:
1. Remove 1 healing item from the helper's inventory.
2. Send `PKT_PLAYER_REVIVE 0x98 { playerSlot = downed peer's slot }`.

The downed player's machine handles `PKT_PLAYER_REVIVE` in `OnReviveReceived` →
`ReviveLocalPlayer(0.5)` (50% max health - an intentional, item-paid revive).

**Healing item set:** a small fixed list of healing aid base FormIDs (Med Pack,
Trauma Pack, Emergency Kit). The exact base IDs are verified during implementation
(in-game inventory dump / wiki / Ghidra); kept in one named table so it is trivial to
extend.

## Way B - Combat-end auto-revive (local)

Runs on the **downed player's own** machine in `OnGameThreadTick`:
- If `g_localDowned` and `player->IsInCombat() == false` continuously for
  `kCombatClearSeconds` (~1.5 s, debounced against combat-flag flicker)
  → `ReviveLocalPlayer(0.25)` (25% max health - you survived, but barely).

`IsInCombat()` is the CommonLibSF `Actor` virtual (vtable index 0x16C) - no raw RE
needed. **Fallback (if the player's combat flag proves unreliable while in the masked
death state):** treat combat as over when no living nearby actor has
`BOOL_BITS::kSearchingInCombat` set / a non-zero combat target - NPCSync already reads
those fields (`boolBits` + `currentCombatTarget` at actor `+0x298`). The fallback is a
small, self-contained query; which signal wins is decided by the in-game test, but both
are "combat state"-based per the agreed approach.

## Integration with existing downed/both-down logic

- **Single-down** (one player downed): unchanged suppression (death bits masked, no
  reload). The player now leaves the downed state via Way A, Way B, or the existing
  F10 debug revive - all routing through `ReviveLocalPlayer`.
- **Both-down** (both downed before any revive fires): unchanged - both release the
  reload → load last save (game-over), with the V13m relaunch-trap neutralizer keeping
  it crash-free.
- The post-load recovery in `OnGameThreadTick` (life-state reads alive → clear downed)
  stays as the safety net after a game-over load.

## Components / files

- `include/Network/PacketTypes.h`
  - `PKT_PLAYER_REVIVE = 0x98`.
  - `struct PlayerReviveData { uint8_t playerSlot; uint8_t pad[3]; }` (4 bytes,
    `#pragma pack(1)`, `static_assert` size).
- `src/Coop/CoopMortality.cpp`
  - `ReviveLocalPlayer(float hpFraction)` - extract the shared revive body; F10 calls
    it with 0.5.
  - Way B: combat-end poll + debounce in `OnGameThreadTick`.
  - Way A: helper-side detection (proximity + Activate-hold + inventory check),
    healing-item table + inventory check/consume helpers, `SendRevive(slot)`.
  - `OnReviveReceived(data, len)` → `ReviveLocalPlayer(0.5)` (currently an empty stub).
- `src/Network/NetworkHost.cpp` - dispatch `PKT_PLAYER_REVIVE` (bidirectional, like
  `PKT_PLAYER_DOWNED`) → `Mortality::OnReviveReceived`.
- Read-only dependency: `RemotePlayerManager` for the downed clone's world position.

## Tunables (one place, easy to adjust after testing)

- `kReviveRadius` ≈ 2.5 m
- `kReviveHoldSeconds` ≈ 2.0 s
- `kCombatClearSeconds` ≈ 1.5 s
- Revive HP: medpack 0.5 × max, combat-end 0.25 × max
- Activate key: `E` for MVP (rebind/controller is a later polish item)

## Open verify points (resolved during implementation)

1. Healing aid base FormIDs (Med Pack / Trauma Pack / Emergency Kit).
2. `player->IsInCombat()` reliability while in the masked death state - else use the
   nearby-hostile fallback.
3. Activate-key polling vs. rebind/controller, hold radius/duration tuning.
4. `RemotePlayerManager` accessor for the downed clone's position/actor.

## Build order

1. **Way B** first (combat-end auto-revive) - smallest, self-contained, only the
   `IsInCombat` poll + `ReviveLocalPlayer`. Quick to test.
2. **Way A** (medipack revive) - partner interaction + `PKT_PLAYER_REVIVE` + inventory.

## Out of scope (later polish / V2)

- Downed POSE (dedicated companion downed animation) - still deferred, separate RE.
- Revive progress HUD/prompt, bleed-to-death timer, controller/rebind-aware Activate.
- Feature B (clone aggro-draw) - separate plan.
