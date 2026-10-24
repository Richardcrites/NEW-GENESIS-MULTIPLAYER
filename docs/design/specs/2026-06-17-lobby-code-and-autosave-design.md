# Lobby Join-Code + Auto-Save - Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming)
**Context:** PULSAR lobby/persistence (STDB backend + companion `serve` daemon + WinForms GUI + DLL). Two independent QoL improvements requested after the Tier-2 security audit shipped.

## Goal

1. **Lobby Join-Code** - let a player join a friend's lobby by typing a short code, instead of finding it in the public browser. The lobby stays publicly listed; the code is a convenience shortcut.
2. **Auto-Save** - automatically persist the local player's character profile to the cloud on co-op disconnect and on game exit, so progress made during a session is never lost. Auto-restore-on-join already exists and is unchanged.

Architectural guardrail (unchanged): STDB carries only meta/persistence. Real-time co-op stays on ENet/relay. The model is host-authoritative world + per-player character profile restored onto the host's world.

---

## Feature 1 - Lobby Join-Code

### Decision summary
- Listing model: **public by default; code is an additional shortcut** (lobby remains in the browser).
- No new join reducer: because the lobby is public and already in every client's subscription, code→lobby is resolved **client-side in the companion**, then runs the existing join path.

### Module (`lobby/server/spacetimedb/Lib.cs`) - schema change
- Add a public column `string Code` to the `Lobby` table.
- Generate in `CreateLobby` using `ctx.Rng` (deterministic, reducer-safe): 6 characters from the unambiguous alphabet `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no `0/O/1/I`).
- Collision check: regenerate (up to ~5 tries) if an **open** lobby already holds that code. Closed lobbies keep their old code (ignored - only open lobbies are joinable).
- `CloseHostedLobbies` / close paths need no change (Code travels with the row; the status filter excludes closed lobbies from resolution).

### Companion (`lobby/companion/Program.cs`)
- **Host ack carries the code.** After `CreateLobby` commits, read the host's own lobby row from the local cache (`Lobby.Iter()` where `HostIdentity == sender && Status=="open"`) and include `code` in the host ack (`WriteAck` gains an optional `code` field).
- **New command action `joinCode`** (`HandleCommandFile`): read `code` + `passwordHash` from the cmd file, normalize the code (trim + uppercase), find the lobby in the local cache where `Code==code && Status=="open"`. If found, set `s_joinCmd/s_joinLobby/s_joinDirectAddr/s_joinVersion` exactly like the existing `join` action and call `JoinLobby(lobbyId, passwordHash)` - reusing the existing `OnJoinLobby` + `ResolveAwaitedJoin` grant-resolution path verbatim. If not found, `WriteAck(cmdId, false, "code not found")`.
- `WriteLobbies` may optionally include `code` per lobby (harmless - lobby is public); not required for the join path.

### GUI (`lobby/companion-gui/MainForm.cs`)
- **Host:** when the host ack returns a `code`, display it prominently (e.g. a read-only label "Invite code: 7G4K2P") with a "Copy" button (copies the bare code, or a short "Join my PULSAR lobby with code: 7G4K2P" string).
- **Join:** add a single-line "Code" textbox + "Join by code" button next to the existing lobby list. Clicking writes a `joinCode` cmd. If the selected lobby in the list has a password, the existing password prompt applies; for code-join, prompt for a password only if the resolved lobby needs one - simplest: always allow an optional password field, pass its hash (empty hash for open lobbies is already handled).
- Password handling stays orthogonal and unchanged.

### Edge cases
- Code not found / lobby closed between display and join → friendly "code not found / lobby no longer open".
- Whitespace / lowercase input → normalized before lookup.
- Two open lobbies with the same code → prevented at generation; client picks the first match defensively.

---

## Feature 2 - Auto-Save (event-based)

### Decision summary
- Triggers: **co-op disconnect** (primary, reliable, in-DLL) + **game exit** (best-effort, covers solo). No periodic save.
- Auto-restore-on-join is unchanged.
- Version handling unified at the companion layer.

### DLL (`src/Lobby/LobbyHandoff.*`, `src/Network/NetworkHost.*`, `src/Main.cpp`)
- Reuse the existing capture (`SerializeProfile` → write `profile-out`) used by the manual F10 path. Auto-save calls the same capture at:
  - **Co-op disconnect:** hook the existing NetworkHost disconnect/session-end path. This is the main case - loot/credits/items gained during the session are written back.
  - **Game exit:** a best-effort SFSE shutdown hook (or process-exit). If unreliable, disconnect-capture still covers the co-op case.
- Capture is read-only (reads inventory/credits/AVs) but still gated by the existing teardown gates (player valid, not in death/load teardown) to avoid mid-teardown access - same gating discipline as the downed/revive work.
- The DLL does **not** manage a version number (see below).

### Companion (`lobby/companion/Program.cs`)
- In the `serve` loop, **watch `profile-out`** for changes (mtime/length or content hash). On change: read the blob and call `upsert_profile(blob, version)` automatically.
- **Unified version:** the companion sets `version = current unix seconds` on every upload (manual and auto). This makes the module's LWW (`if version <= stored: ignore`) always monotonic without any DLL/local counter. The blob's internal `ProfileData.version` field is display-only and untouched.
- Write a small status (last upload time + ok/err) to the ack/status channel the GUI reads.

### GUI (`lobby/companion-gui/MainForm.cs`)
- Toggle "Auto-save my character" (default ON) persisted in `lobbygui.json`. When off, the GUI signals the companion to skip the profile-out watcher (e.g. via a flag in the cmd/config the daemon reads).
- Show "Last saved: HH:MM" from the companion status.

### Restore (unchanged)
- On join, the companion writes `profile-in` (download), the DLL applies it when the player is ready (`TickApplyProfile`). Optionally surface "restored your character" in the GUI log.

### Edge cases / known limits (out of scope for this work)
- **Credits apply stays gated** (`kApplyCreditsOnRestore=false`) - needs a real money API; tracked separately.
- **Mods re-roll on restore**  - the blob *stores* mods, but apply is a separate fix.
- LWW assumes one player on one machine; concurrent multi-device edits for the same identity can clobber (acceptable for alpha).
- Game-exit capture reliability is a risk; disconnect-capture is the dependable path.

---

## Data flow

**Host with code:** GUI host → companion `CreateLobby` (module generates `Code`) → host ack returns `code` → GUI shows code + Copy.

**Join by code:** GUI "Join by code" → cmd `joinCode{code,passwordHash}` → companion resolves `code`→lobby in local cache → existing `JoinLobby` path → `RelayGrant.Status` → handoff + ack → Starfield launches → ENet via relay.

**Auto-save:** in-game disconnect/exit → DLL `SerializeProfile` → `profile-out` → companion `serve` watcher → `upsert_profile(blob, unixSeconds)` → stored (LWW).

**Auto-restore (unchanged):** join → companion `pull` own profile → `profile-in` → DLL `TickApplyProfile`.

---

## Testing

- **Module (CLI/harness):** `CreateLobby` assigns a 6-char code from the allowed alphabet; two open lobbies never collide; code is readable on the public lobby row. Reuse the local two-connection harness style for any join-by-code path that needs a live lobby.
- **Companion:** `joinCode` resolves a known code to the right lobby and acks ok; unknown code → "code not found". `profile-out` change triggers exactly one `upsert_profile` with a monotonically increasing version.
- **GUI:** host shows the code; Copy puts it on the clipboard; "Join by code" writes the right cmd; auto-save toggle persists and gates the watcher.
- **In-game (user):** host → friend joins by code over the VPS relay; play, disconnect → reconnect later and confirm the character state carried (within the credits/mods limits above).

## Deploy

- Feature 1 changes the schema (`Lobby.Code`) → VPS republish with `spacetime publish pulsar-lobby --delete-data -y` (wipes accounts/lobbies; names re-claimed). Feature 2 needs no schema change. Since F1 wipes anyway, ship both together = exactly one more wipe.
- Regenerate companion bindings after the module change; rebuild companion + GUI; refresh `lobby/_dist/LobbyTools`; push the private deploy repo for the VPS pull.

## Out of scope (future)
- Private/invite-only (code-only, hidden) lobbies - the code column is the groundwork.
- Friends list / recent players, auto-reconnect, deep-link invites.
- Character progression (level/XP/skills), appearance, ships in the blob.
- Credits money API, mods re-roll , periodic auto-save.
