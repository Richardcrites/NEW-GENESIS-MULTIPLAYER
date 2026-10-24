# Client Progression Persistence - Stage 1: Skills + Perks - Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming)
**Task:** (Stage 1)
**Context:** PULSAR co-op (P2P, host-authoritative). When a client joins, the host's save is bridged onto the client; today only the client's **inventory + O2** are restored on top (via `DirectRestore`, L.7 Plan B). Skills/perks/level come from the host's bridged save, so a joining client temporarily loses their own progression. We keep P2P (no dedicated server); appearance is explicitly out for the alpha.

## Goal
A joining client keeps their own **perks** (which, in Starfield, include skill ranks) across a co-op session - they are captured before the bridge and re-applied after it. Stage 1 is **additive**: the client's own perks are added back; nothing is removed.

## Non-goals (this stage)
- Level/XP (Stage 2), full perk diff / `RemovePerk` (Stage 2), appearance (out for alpha), credits-apply (gated, needs money API), mods (gated,), cross-machine FormID remap (works same-machine; cross-machine reuses the existing T34 mapping later).

## Why extend DirectRestore (not a new path)
Investigation (2026-06-17) confirmed the **authoritative** item-restore is `DirectRestore`: it captures the client's inventory **before** the bridge (`CaptureClientState`, `src/InventorySync/DirectRestore.cpp:445`, invoked from `WorldSync::OnSaveReceived`, `src/WorldSync/WorldSync.cpp:489`) and re-applies it **after** the bridge (`RestoreClientState`, called from `InventorySync::DoApply`, `src/InventorySync/InventorySync.cpp:1469`). It already handles an ActorValue (O2) with the exact capture-pre/apply-post pattern we need. The old M.3 chunk-filter approach ("didn't work right" - same-cell-only, single-snapshot) is superseded and only runs as a subordinate first-pass. Building progression into DirectRestore reuses the proven mechanism and the correct timing; a separate path would risk the same problems M.3 had.

## Engine data (verified against CommonLibSF)
- Player perks: `Actor` has `Perks* perks` at **+0x360** (`RE/A/Actor.h:427`), `perkArrayLock` at **+0x36C**. `Perks` = `{ BSTArray<PerkRankData>* perkRanks; }` (`RE/A/Actor.h:58`). `PerkRankData` = `{ BGSPerk* perk; uint8 currentRank; ... }` (`RE/P/PerkRankData.h`, size 0x10).
- ActorValues: `avStorage` at **+0x260** (`RE/A/Actor.h:395`); `GetActorValue`/`SetActorValue` already used for O2 (`ClientProfile.cpp`, `DirectRestore.cpp` O2 path).
- Apply perk: the engine exposes a perk-add (Papyrus native `Actor.AddPerk(BGSPerk, bool)` - `BSScript__NativeFunction2_Actor_void_BGSPerk_bool_`). The underlying engine `Actor::AddPerk(BGSPerk*, rank)` RVA must be resolved and **Ghidra-verified** before use (project rule: never trust a CLibSF/REL::ID blindly).

## Architecture
Two additive hooks into the existing DirectRestore flow + a few extra fields in the profile blob. No new modules, no new STDB schema (the blob is opaque; the DB just carries more bytes via the existing auto-save → `upsert_profile`).

### Components
1. **Perk snapshot type** (in `DirectRestore` cache + `ProfileData`): `std::vector<PerkEntry>` where `PerkEntry = { uint32_t formID; std::string plugin; uint32_t localID; uint8_t rank; }`. Same plugin/localID (T34) scheme already used for inventory items - raw `formID` is authoritative same-machine; `plugin`/`localID` enable cross-machine resolution later.

2. **Capture (pre-bridge)** - extend `DirectRestore::CaptureClientState()`:
   - Under `perkArrayLock`, walk `player->perks->perkRanks` → record each `{perk->formID, rank}`.
   - Resolve plugin/localID via the existing form→plugin helper used for items.
   - Store into the DirectRestore cache alongside the inventory cache (frozen by the existing `LockCache()`).
   - Captured at the same instant as inventory = still the client's own values (pre-bridge).

3. **Apply (post-bridge, additive)** - extend `DirectRestore::RestoreClientState()`:
   - After the inventory 4-pass completes, for each cached perk: if the live player does NOT already have it at >= the cached rank, call the (Ghidra-verified) engine `AddPerk(perk, rank)`.
   - Additive only: never remove host perks (Stage 1). Gate behind player-ready + the same teardown safety used for the rest of the restore.
   - A `kApplyPerksOnRestore` flag (default true) so it can be toggled off fast if it misbehaves, mirroring the existing `kApply*` flags.

4. **Profile serialization** - add the perk vector to `ProfileData` (`include/Profile/ClientProfile.h`) and its encode/decode (`src/Profile/ClientProfile.cpp`), so perks ride the cloud profile via the existing auto-save . `playerLevel`/`raceName` fields already exist; perks are new.

### Data flow
Join → `OnSaveReceived` → `CaptureClientState` (inventory **+ perks**) → `LockCache` → engine bridge loads host save → `DoApply` → `RestoreClientState` (inventory **+ additive perks**) → player has their own gear **and** their own perks. On disconnect, auto-save  serializes the profile (now incl. perks) → cloud, so it persists across sessions.

## Error handling / safety
- Capture: read-only under `perkArrayLock`; SEH-guard the walk (like `IsPlayerReadyForConnect`).
- Apply: only when the player is ready and not mid-teardown (reuse the existing gates). Additive AddPerk avoids the riskier remove path. `kApplyPerksOnRestore` kill-switch.
- `AddPerk` RVA must be Ghidra-verified before wiring (project rule). If it can't be safely resolved, Stage 1 falls back to capture-only (store perks in the profile, defer apply) so nothing destabilizes.
- FormID: same-machine uses raw `formID`; if a cached perk form doesn't resolve on this machine (cross-machine, different load order), skip it (log) rather than apply a wrong form.

## Testing
- **C# / DB side:** trivial - perks are extra bytes in the opaque blob; the existing auto-save upload + `upsert_profile` path is already verified . A round-trip check (serialize → deserialize ProfileData with perks) in the profile encode/decode.
- **In-game (host + client):** client with known perks joins → after the bridge, confirm the client still has their own perks (log perk count pre-bridge vs post-restore; the existing `TickPostRestoreWatch` +5s/+10s diff is the natural place to also log perks). Verify no crash on apply. Then disconnect → rejoin → perks persist via cloud.
- **No-regression:** inventory + O2 restore must still work unchanged.

## Deploy
DLL change (capture/apply) + ClientProfile serialization change. No STDB schema change (opaque blob) → **no `--delete-data` needed**. Rebuild DLL + deploy (local + NAS); companion/GUI unaffected. Profiles captured before this build simply have no perk data (harmless).

## Out of scope → future stages
- Stage 2: Level/XP, full perk diff (`RemovePerk` host-only perks), skill challenge-progress AVs.
- Separate tracks: credits money-API, mods , appearance, cross-machine accounts (OIDC).
