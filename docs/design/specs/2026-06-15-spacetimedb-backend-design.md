# SpacetimeDB VPS-Backend + Companion-App - Design (Teil 2)

> **Status:** Design / Spec. Nächster Schritt = Implementierungsplan (writing-plans). Implementierung erst nach Stabilisierung der laufenden Co-op-Arbeit ().
> **Datum:** 2026-06-15

## Ziel

Ein selbst-gehostetes SpacetimeDB-Backend, das **Spieler-Accounts (stabile Identität)**, **persistente Charakter-Profile** und **Lobby-/Server-Browser-Vermittlung** liefert - ohne den Echtzeit-Game-Sync anzufassen. Der Game-Traffic bleibt zu 100 % ENet-P2P host↔client. SpacetimeDB ist nur Orchestrierung + Storage.

## Nicht-Ziele (bewusst ausgeschlossen)

- **Echtzeit-Game-Sync über STDB.** Positionen/Anims/Combat/Loot bleiben ENet-P2P. STDB-Latenz (WebSocket über VPS) + fehlendes standalone-C++-SDK machen das untauglich.
- **In-Game-Lobby-UX** (ImGui-Server-Browser, auto-save-load-Flow im Spiel) - das ist **Teil 3**, separate Spec.
- **Profil-Serialisierung** (Inv/Equip/Credits/O2 → Blob) - das ist **Teil 1**, separate Spec. Diese Spec setzt sie als definierte Schnittstelle voraus (siehe „Schnittstelle zu Teil 1").
- **NAT-Traversal / Relay.** v1 setzt Port-Forward bzw. direkte erreichbare Host-IP voraus. NAT-Punch/Relay = später (Teil 3).
- **Echte Accounts mit Login (OIDC).** v1 nutzt anonyme stabile Identity. OIDC ist der dokumentierte Upgrade-Pfad.

## Architektur-Entscheidungen (vom User bestätigt 2026-06-15)

| Entscheidung | Wahl | Begründung |
|---|---|---|
| Scope dieser Spec | STDB-Backend + Companion | „Das" worüber wir reden; Profil-Serialisierung separat. |
| Companion-Sprache | C# / .NET | Reifstes STDB-Client-SDK (Unity-erprobt), einfache Windows-GUI, schnelle Iteration. |
| Deployment | Self-host VPS (Docker) | Voller Kontrolle, deckt VPS-Plan, kein Energy-Pricing. |
| Identität | Anonyme stabile Identity | Einfachster Start für Co-op-Mod; Profil hängt an der STDB-Identity. |
| DLL ↔ Companion-Kopplung | **Ansatz A - Launcher-zentriert, Datei-Handoff** | Kleinster DLL-Eingriff, saubere Prozessgrenze, kein IPC-Protokoll. Forward-kompatibel zu Live-Bridge (Teil 3). |

### Warum Ansatz A (Launcher-Handoff) statt Live-Socket-Bridge

Der Companion ist ein **Vor-Spiel-Launcher**, kein mitlaufender IPC-Partner. Begründung: (1) kleinster Eingriff in die DLL - nur Datei lesen/schreiben, kein Protokoll; (2) saubere Prozessgrenze - Spiel und Launcher haben unabhängige Lifecycles; (3) deckt sich exakt mit der bestehenden Lobby-Planung („Launcher macht save-load + connect; V3-Connect-Gate ist forward-compatible"). Die Live-Socket-Variante (Ansatz B) ist Teil-3-Material und bleibt durch das Datei-Format vorwärtskompatibel erreichbar. Ein nativer C++-STDB-Client (Ansatz C) wurde verworfen: kein standalone-C++-SDK → BSATN-Protokoll selbst bauen und pflegen, hoher Aufwand/Risiko.

## Architektur-Überblick

```
Launcher (C#/.NET, STDB-Client)              STDB-Modul (VPS / Docker)
  Login → Identity ───────────────────────►   account
  Browse / Join lobby ◄────────────────────   lobby / lobby_member   (Subscriptions = Live-Browser)
  Fetch / Upload profile ◄────────────────►   profile (opakes Blob + version)
        │  Handoff-Datei {role, host_addr, blob, version}      ▲ Reducer (ctx.sender-autorisiert)
        ▼  startet Starfield.exe                                │
  PULSAR-DLL (C++)
    ── liest Handoff → DirectRestore (Profil) / NetworkHost::Connect(host_addr)
    ── ENet-P2P-Game-Sync  (UNVERÄNDERT)
    ── Save/Logout → Out-Datei {blob, version++}  → Launcher lädt per upsert_profile hoch
```

**Kernprinzip:** STDB berührt nie den Echtzeit-Pfad. Die einzige Brücke zwischen Launcher-Welt und Spiel-Welt sind zwei Dateien (In-Handoff, Out-Profil).

## Komponenten

### Komponente 1 - STDB-Modul (Server-Logik auf VPS)

Sprache: Rust **oder** C# (im Implementierungsplan zu fixieren; Rust ist der STDB-Default, C# erlaubt Code-Teilung mit dem Companion). Läuft als WASM in der self-hosted STDB-Instanz (`spacetime start` / Docker).

**Tabellen**

| Tabelle | Felder | Anmerkung |
|---|---|---|
| `account` | `identity` (PK), `display_name`, `created_at` | Eine Zeile pro STDB-Identity. |
| `profile` | `identity` (PK/FK→account), `blob` (bytes), `version` (u32), `updated_at` | Ein Profil je Account in v1. `blob` ist **opak** (von Teil 1 serialisiert). |
| `lobby` | `lobby_id` (PK, auto), `host_identity` (FK), `host_addr` (string „ip:port"), `name`, `status` (enum open/in_game/closed), `max_players`, `created_at` | |
| `lobby_member` | `lobby_id` (FK), `member_identity` (FK), `joined_at` | Wer ist in welcher Lobby. |

**Reducer** (alle autorisieren über `ctx.sender`):

| Reducer | Effekt | Autorisierung |
|---|---|---|
| `register_account(display_name)` | Account-Zeile anlegen/aktualisieren | sender = eigene Identity |
| `upsert_profile(blob, version)` | Profil schreiben; nur wenn `version > vorhandene version` (last-writer-wins) | nur eigenes Profil |
| `create_lobby(name, host_addr, max_players)` | Lobby anlegen, `host_identity = sender`, `status = open` | jede authentifizierte Identity |
| `update_lobby_status(lobby_id, status)` | Status setzen (open/in_game/closed) | nur `host_identity == sender` |
| `join_lobby(lobby_id)` | `lobby_member`-Zeile anlegen | sender; nur wenn Lobby `open` und nicht voll |
| `leave_lobby(lobby_id)` | `lobby_member`-Zeile entfernen | sender |

**Lifecycle-Hooks:** STDB liefert `client_connected` / `client_disconnected`. Bei `client_disconnected` werden alle Lobbys mit `host_identity == getrennte Identity` auf `closed` gesetzt und ihre `lobby_member` entfernt → **keine Stale-Lobbys, kein Scheduler nötig**.

**Storage-Agnostik:** Das `blob` ist für STDB nur Bytes + `version`. Dadurch bleibt der Storage swappbar - Phase-1 (lokale Host-Datei) und Phase-2 (STDB) unterscheiden sich nur darin, *wo* das Blob landet, nicht in seinem Format.

### Komponente 2 - Companion-App (C# / .NET, Windows)

Eine Anwendung mit zwei Rollen:

- **Host-Modus:** Account sicherstellen → eigene öffentliche `ip:port` ermitteln/eingeben → `create_lobby` → Handoff `{role: host}` schreiben → `Starfield.exe` starten. Nach Spielstart kann der Launcher `update_lobby_status(in_game)` setzen (Heuristik: Out-Datei erscheint / Timer; Detail im Plan).
- **Client-Modus:** Account sicherstellen → `lobby`-Tabelle abonnieren (Live-Server-Browser) → Lobby wählen → `join_lobby` + eigenes `profile.blob` ziehen → Handoff `{role: client, host_addr, blob, version}` schreiben → `Starfield.exe` starten.
- **Profil-Upload (beide Rollen):** `FileSystemWatcher` auf die Out-Datei; bei Änderung `{blob, version}` lesen und `upsert_profile` aufrufen.

GUI-Framework (Avalonia vs. WinUI vs. WPF) wird im Implementierungsplan entschieden. STDB-Identity-Token persistiert das C#-SDK lokal.

### Komponente 3 - DLL-Glue (C++, neu in PULSAR: `LobbyHandoff.{h,cpp}`)

Minimaler Eingriff, nutzt bestehende Pfade:

- **Beim Start:** Handoff-Datei lesen → `{role, host_addr, profile_blob, version}`. `profile_blob` an den bestehenden **DirectRestore**-Pfad geben (ersetzt den lokalen Capture). `host_addr` an `NetworkHost::Connect` (Client) bzw. ENet-Host starten (Host).
- **Bei Save/Logout:** Profil über die **Teil-1-Schnittstelle** serialisieren → Out-Datei schreiben `{blob, version+1}`.
- **Robustheit:** Handoff fehlt/korrupt → **Fallback auf heutiges lokales Verhalten** (lokaler Capture, manueller Connect). Kein neuer Crash-Pfad; defense in depth.

## Schnittstelle zu Teil 1 (Profil-Serialisierung)

Diese Spec setzt zwei storage-agnostische Funktionen voraus, die Teil 1 liefert:

```cpp
// Serialisiert den aktuellen Spieler-Zustand (Inventar inkl. Mods, Equip, Credits, O2) in ein opakes Blob.
std::vector<uint8_t> SerializePlayerProfile();

// Wendet ein zuvor serialisiertes Blob auf den Spieler an (über den bestehenden DirectRestore-Pfad).
bool ApplyPlayerProfile(std::span<const uint8_t> blob);
```

Mods werden über die M.2-Primitives (`ReadInventoryItemMods` / `SafeRestoreModArrayOnInventoryItem`) serialisiert, FormIDs über das T34-Plugin-Encoding (cross-machine-stabil). Das Blob-Format selbst ist Teil-1-Eigentum; Teil 2 behandelt es opak.

## Datenflüsse

**Host eröffnet Lobby**
1. Launcher → `create_lobby(name, public_addr, max)` → `status = open`.
2. Handoff `{role: host}` → `Starfield.exe` startet → DLL startet ENet-Host.
3. (optional) Launcher → `update_lobby_status(in_game)`.

**Client joint**
1. Launcher → Server-Browser (abonnierte `lobby`-Tabelle) → wählt Lobby.
2. `join_lobby(id)` + `profile.blob` ziehen.
3. Handoff `{role: client, host_addr, blob, version}` → `Starfield.exe` startet.
4. DLL: eigenen Save laden → `NetworkHost::Connect(host_addr)`.
5. Host wendet `blob` via DirectRestore an (Profil ersetzt lokalen Capture).

**Profil persistieren**
1. In-Game-Save/Logout → DLL `SerializePlayerProfile()` → Out-Datei `{blob, version+1}`.
2. Launcher-`FileSystemWatcher` → `upsert_profile(blob, version)` (last-writer-wins per `version`).

## Fehlerbehandlung & Edge-Cases

| Fall | Verhalten |
|---|---|
| STDB unerreichbar | Launcher zeigt „offline"; Fallback auf manuelles Direct-IP-Connect ohne Profil-Sync (= heutiges Verhalten). |
| `version`-Konflikt (offline woanders editiert) | Last-writer-wins per `version` in v1. Dokumentiertes Limit. |
| Host-IP falsch / NAT | Connect schlägt fehl, im Game-Log sichtbar (bestehend). NAT-Punch/Relay = Teil 3. |
| Handoff-Datei fehlt/korrupt | DLL → Fallback auf lokalen Capture (kein neuer Crash). |
| Companion crasht mid-game | Spiel läuft auf ENet weiter (entkoppelt); Profil-Upload nur verzögert bis Neustart. |
| Identity-Token-Verlust | Profil-Verlust (Limit von „anonym"). Mitigation = OIDC-Upgrade. |

## Sicherheit / Autorisierung

Anonyme stabile Identity: die STDB-`Identity` ist die Credential (Token vom SDK lokal gespeichert). Alle Reducer autorisieren über `ctx.sender` - ein Client kann nur sein **eigenes** Profil schreiben und nur seine **eigene** Lobby ändern. Bewusstes v1-Limit: Token-Verlust = Profil-Verlust (kein Recovery); OIDC ist der dokumentierte Upgrade.

## Teststrategie

- **STDB-Modul:** Reducer gegen lokale `spacetime start`-Instanz; Tabellen-State nach jedem Reducer-Call asserten (register → Account existiert; upsert mit kleinerer version → abgelehnt; create/join → korrekte Member-Zeilen; disconnect-Hook → Lobby closed).
- **Companion:** Integrationstests gegen lokale Instanz - Roundtrips register/upsert/create/join; `FileSystemWatcher`-Upload-Pfad.
- **DLL-Glue:** Handoff-Read/Write-Roundtrip; Fallback wenn Datei fehlt/korrupt.
- **End-to-End (manuell):** Host öffnet Lobby → Client browst + joint → Profil im Spiel angewandt → Save → Re-Login zeigt persistiertes Profil; STDB-offline → Fallback auf Direct-IP.

## Abhängigkeiten & Reihenfolge

1. **Teil 1 (Profil-Serialisierung)** muss zuerst existieren - liefert `SerializePlayerProfile` / `ApplyPlayerProfile`. Phase-1-Storage kann eine lokale Host-Datei sein; diese Spec (Teil 2) tauscht den Storage gegen STDB.
2. **Teil 2 (diese Spec)** baut STDB-Modul + Companion + DLL-Glue.
3. **Teil 3 (In-Game-Lobby-UX, optional)** kann später den Datei-Handoff durch eine Live-Socket-Bridge ersetzen - durch das Handoff-Format vorwärtskompatibel.

## Offene Punkte für den Implementierungsplan

- STDB-Modul-Sprache: Rust (Default) vs. C# (Code-Teilung mit Companion).
- Companion-GUI-Framework: Avalonia vs. WinUI vs. WPF.
- Genaue Heuristik für `update_lobby_status(in_game)` (Out-Datei-Erscheinen vs. Timer vs. DLL-Signal).
- Pfad/Format der Handoff- und Out-Dateien (JSON vs. binär; Speicherort relativ zu Starfield-Data).
