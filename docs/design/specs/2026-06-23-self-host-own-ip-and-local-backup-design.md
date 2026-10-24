# Self-Host on Own IP + Local Character Backup (Design)

**Status:** design approved in chat 2026-06-23 (user). Post-Alpha-0.1 (Pulsar unified tool, phase "One-Click Local Stack" + backup). Not started.

**Goal:** A player can host a complete PULSAR co-op session on their own machine and IP (LAN or internet), with no dependency on the official cloud DB or the official relay, joinable by a friend via a direct-IP invite. Characters are safely backed up locally (auto rolling backup + manual export/import) and portable between online and local play.

---

## Locked decisions (from chat)

1. **Relay-only exposure, DB stays host-local.** In self-host mode only the relay is reachable from the network (UDP, and the game payload is already E2E-encrypted via). The host's local SpacetimeDB stays bound to localhost and serves only the host's own companion. The joining friend connects only to the relay; their character comes from their own source (their cloud DB if online, or an imported character). Rationale: a raw internet-exposed SpacetimeDB would carry profile data in cleartext and add attack surface; a home user has no domain/cert. Relay-only means only one UDP port to forward and the content is encrypted.
2. **Self-host is strictly private / invite-only.** A self-hosted session never appears in the public lobby list (the public list is fed by the cloud DB; local mode writes only to the local DB). Friends join by sharing an IP-based invite. No "list my game publicly" toggle.
3. **Reach = LAN + Internet.** Both same-network friends (LAN IP) and internet friends (public IP + UDP port-forward) are supported. Internet self-host is an Advanced option with a clear warning and a reachability self-check.
4. **Local backup = auto + manual.** Rolling auto-backup of the profile blob (keep last 10 versions) triggered on the same hook as auto-save (save/disconnect), plus manual Export/Import buttons. Works in both online and local modes.

## Non-goals

- The friend's character living on the host's DB (dedicated-server style). Out of scope; would be a separate, larger design.
- Exposing SpacetimeDB to the internet (deliberately avoided per decision 1).
- N>2 players (engine limit MAX_CLIENTS=1, host+client only).
- Bundling/redistributing SFSE (unchanged: require + check, license).

---

## Current state (verified in code, 2026-06-23)

- **DB mode toggle exists** (`AppConfig.DbMode` Online/Local; `SettingsViewModel`; `MainWindowViewModel` branches on it).
- **`LocalStack`** (`lobby/pulsar/Services/LocalStack.cs`) already orchestrates: `spacetime start` (local STDB), best-effort module publish, and `PulsarRelay.exe`. BUT it hardcodes `127.0.0.1` for both the STDB URI and the relay address, so it only works same-PC today.
- **Relay already binds `IPAddress.Any`** on `PULSAR_RELAY_PORT` (default 7777) and supports `PULSAR_RELAY_PUBLIC_HOST`. Binding is not the gap; advertising the right address is.
- **Bundling not done:** `LocalStack` expects an external `spacetime.exe` (path in Settings) and the module SOURCE dir (`lobby/server/spacetimedb`); neither ships in the tool ZIP. Publish flag has an open TODO.
- **No local character backup** in the tool. The only "backup" is the StarfieldCustom.ini `.pulsar.bak`. The profile blob (`profile-out` handoff in `%LOCALAPPDATA%\PULSAR\`) is consume-once; nothing keeps history or lets the user export/import.
- **Profile blob** is the JSON-v2 produced by `SerializeProfile`/`ApplyProfile` (items+mods, credits, O2, perks, skills, level, XP, powers, research). Companion uploads it via `UpsertProfile(blob, version)`.
- **Handoff** carries peer/relay addresses to the DLL already (consume-once, `%LOCALAPPDATA%\PULSAR\`).

---

## Design

### A. Host on own IP

LocalStack stops hardcoding 127.0.0.1 and instead resolves the host's reachable address:

- **STDB** stays `ws://127.0.0.1:3000` (host-local only, never advertised). Unchanged.
- **Relay** keeps binding `0.0.0.0:7777` (already does) but the address handed to the joiner is the host's reachable IP:
  - **LAN:** the primary LAN IPv4 of the active adapter (enumerate, prefer a private 192.168/10/172.16-31 address bound to the default route; skip loopback/APIPA/virtual adapters).
  - **Internet:** the public IPv4, discovered via a lightweight lookup (the official STDB/relay already see the source IP; fall back to a plain "what is my IP" HTTPS endpoint). Cached per session.
- A small `NetworkAddress` service in the tool computes both, with a manual override field (some users know their address better than autodetect).

### B. Direct-IP invite + join

- **Host UI (Co-op hub, when hosting self-hosted):** after the stack is up, show a copyable **invite** that encodes the reachable IP + relay port. Format: a short `pulsar://join/<ip>:<port>` string (human-pasteable, also accepts bare `ip:port`). LAN and internet each get the appropriate IP; if both exist, show LAN by default with an "internet address" expander.
- **Joiner UI:** a **"Join by IP / invite"** field. Accepts `pulsar://join/...` or bare `ip:port`. On join, the tool writes the address into the DLL handoff as the direct connect target (reusing the existing relay/peer handoff path), then launches. No cloud-lobby resolution.
- **DLL/companion:** the existing `Connect`/`SetRelay` + handoff already accept a direct target; the join-by-IP path feeds that target straight through, bypassing the DB lobby lookup.
- Character on the joiner side: their own (cloud DB if online-hybrid, or imported via section D). Session gameplay uses the existing host-save-bridge, unchanged.

### C. Internet specifics

- **Port-forward guidance:** a short in-tool panel: forward UDP `<relayPort>` to this PC, with the detected public IP shown.
- **Reachability self-check:** an optional probe that asks an external helper (or the official relay acting as a STUN-like reflector) to send a UDP packet back to the host's `publicIP:relayPort`; report reachable / not reachable so the host knows before inviting. If no helper is available, degrade to a clear manual checklist.
- **Advanced gate + warning:** internet self-host lives behind Advanced with a one-line honest warning (your relay sees both IPs; you must port-forward). Default recommendation in the tool stays: use the official relay/P2P; self-host for those who want independence.

### D. Local character backup (online + local)

- **Auto rolling backup:** on the existing auto-save trigger (save/disconnect), after a profile blob is produced, copy it into `%LOCALAPPDATA%\PULSAR\backups\<identity-or-"local">\` as `char-<unixtime>.json`. Keep the newest **10**, prune older. Cheap, silent, no UI churn.
- **Manual Export:** "Export character" writes the current profile blob to a user-chosen `.pulsarchar` file (the JSON-v2 blob plus a small header: display name, timestamp, level, source mode) so the Import dialog can show what it is.
- **Manual Import:** "Import character" reads a `.pulsarchar`, shows its header, and on confirm writes it into the active DB (cloud or local) via the existing `UpsertProfile` path (monotone LWW version = now). This is the bridge between online and local characters and the restore path after a DB wipe.
- **Safety:** Import never touches Starfield `.sfs` saves; it only writes the DB profile that co-op applies. Export is read-only.

### E. Bundling (makes local hosting work from a distributed ZIP)

- **Ship in the `pulsar-releases` ZIP:** the `spacetime` binary, a prebuilt/publishable lobby module, and `PulsarRelay.exe`. `package_release.ps1` lays them out next to the tool; `LocalStack` locators find the bundled binary first (then fall back to the Settings path / walk-up for dev).
- **Resolve the publish-flag TODO** in `LocalStack.PublishModuleAsync` against the bundled CLI version (confirm `spacetime publish` flags), so first-run local hosting publishes the module reliably (or ships an already-built module artifact to skip publish entirely on the user machine).
- **Prerequisite check:** verify the SpacetimeDB redistribution license permits bundling the binary before we ship it (same diligence as the SFSE license check). If it does not, fall back to a guided one-time download (like the SFSE/Address-Library links on the Setup page).

---

## Security considerations

- Game traffic is E2E-encrypted  on every path including self-hosted relay, so the relay operator (here, the host) cannot read the joiner's gameplay; and a passive network observer on the internet path sees only ciphertext.
- STDB is never exposed (decision 1), so profile data never crosses the network in self-host mode.
- Internet self-host exposes one UDP port; the host's public IP becomes known to the joiner (inherent to direct connect, same as any P2P).
- No new secret material; no change to the official DB/relay or their security posture.

---

## Open items / assumptions (sensible defaults; flag to change)

- Auto-backup retention = 10 versions; trigger = save/disconnect (same as auto-save).
- Export format = JSON-v2 blob + small header; extension `.pulsarchar`.
- Reachability probe needs a helper endpoint; if none is stood up, ship the manual checklist and treat the live probe as a later add.
- Invite string scheme `pulsar://join/<ip>:<port>`; bare `ip:port` also accepted.

---

## UPDATE 2026-06-23 (after reading the handoff + companion code) - simplified to "pure direct"

Code investigation changed the connection model for the better:

- **The DLL handoff already supports a pure direct connect.** `ReadHandoff` (`src/Lobby/LobbyHandoff.cpp`) reads `role`, `host_addr` (-> hostIP:port), `relay_addr`, `relay_session`. When `relay_addr` is empty and `host_addr` is set, the client connects straight to `hostIP:port` with no relay and no session. The companion already has this "direct lobby (no session needed)" branch (`s_joinDirectAddr`, `WriteHandoff(... relayAddr, relaySession)`).
- **Therefore self-host needs no relay at all.** The relay exists only for NAT traversal when you do not control the network. Self-host means you do: LAN works directly; internet works directly with the UDP game port forwarded (already accepted). So the model is **direct ENet, no relay process, no STDB-for-the-connection**.
- **Consequence for decision 1:** "relay-only exposure" is moot for self-host because there is no relay. Only the game port is exposed, and that traffic is E2E-encrypted . The local STDB stays purely for the host's own character persistence + backup, never on the wire.
- **The invite is just `ip:port`** (no session hex). `NetworkAddress.FormatInvite` is already correct.

Revised flow:
- **Host (self-hosted direct):** companion stages the host's profile, writes handoff `role=host` (DLL StartHosts on the game port, bound to all interfaces); tool shows invite = reachable IP:port. Local STDB optional, only for the host's own character/backup.
- **Join by IP:** companion pulls the joiner's own profile (their cloud DB if online, or an imported character) -> stages `profile-in`, writes handoff `role=client, host_addr=<pasted ip:port>`, empty relay/session; DLL connects directly.
- New companion verbs needed: a "host direct" and "join direct <ip:port>" path that bypass the DB lobby create/resolve but still stage the profile and write the handoff.

This supersedes the relay-centric parts of sections A-C above; backup (D) and bundling (E) are unchanged, except bundling no longer needs to ship the relay for the self-host case (relay stays a separate, official-infra thing).

## Rough component map (for the implementation plan)

- `lobby/pulsar/Services/LocalStack.cs` (advertise reachable IP, bundled-binary locator, publish flag).
- `lobby/pulsar/Services/NetworkAddress.cs` (new: LAN IP, public IP, override).
- `lobby/pulsar/Services/CharacterBackup.cs` (new: rolling backup + export/import over the profile blob).
- `lobby/pulsar/ViewModels/PlayViewModel.cs` + `Views/PlayView.axaml` (invite display, Join-by-IP field).
- `lobby/pulsar/ViewModels/SettingsViewModel.cs` / a Backup view (export/import buttons, retention).
- `lobby/pulsar/Services/DaemonBridge.cs` (direct-IP handoff target path).
- `lobby/companion/Program.cs` (accept direct connect target without cloud-lobby resolve, if needed).
- `tools/package_release.ps1` (bundle spacetime + module + relay).
