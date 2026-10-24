# Lobby Password - Design Spec (secure)

**Datum:** 2026-06-16
**Ziel:** Optionales Passwort pro Lobby - **echter** Schutz: ohne korrektes
Passwort bekommt man die Relay-Session nicht und kann sich nicht verbinden.

## Kernidee (warum „echt")
Die `RelaySession` ist der eigentliche Zugang zum Host (der Relay paart rein nach
Session, kennt keine Mitgliedschaft). Damit ein Passwort wirklich schützt, darf
die Session **nicht öffentlich** sein: sie wird in einer **privaten Tabelle**
gehalten und erst **nach erfolgreicher passwort-geprüfter Verifizierung** an den
Joiner ausgeliefert (per-user View).

**Wichtig:** Relay-Protokoll + DLL bleiben unverändert. Es ändert sich nur, *wie
der Companion die Session bekommt*: bisher aus der öffentlichen Lobby-Tabelle,
künftig aus der per-user View `my_relay` nach dem Join.

## Hashing
Companion hasht clientseitig (SHA-256 mit festem App-Salt) → 64 Hex. Host und
Joiner hashen identisch; der Server vergleicht nur Hashes. Klartext verlässt nie
die Maschine.

## Modul (`Lib.cs`)
- `Lobby` (öffentlich): **`RelaySession` entfällt** (wird privat); **neu**
  `public bool HasPassword;`. Rest unverändert (HostAddr ist im Relay eh "").
- **Neue private Tabelle** (server-only):
  ```csharp
  [SpacetimeDB.Table(Accessor = "LobbyRelay")]
  public partial struct LobbyRelay { [PrimaryKey] public ulong LobbyId; public string Session; public string Hash; }
  ```
- **Type + per-user View** (Joiner/Host lesen nur ihre Sessions):
  ```csharp
  [SpacetimeDB.Type] public partial struct RelayInfo { public ulong LobbyId; public string Session; }
  [SpacetimeDB.View(Accessor = "MyRelay", Public = true)]
  public static List<RelayInfo> MyRelay(ViewContext ctx) {
      // für jede LobbyRelay-Zeile: Caller == Host ODER Mitglied → Session aufnehmen
  }
  ```
- `CreateLobby(name, hostAddr, maxPlayers, relaySession, passwordHash)`:
  validiert (relaySession leer/32 hex, passwordHash leer/64 hex); `HasPassword =
  passwordHash != ""`; Lobby einfügen → `LobbyId` aus dem Insert-Rückgabewert →
  wenn `relaySession != "" || passwordHash != ""` eine `LobbyRelay`-Zeile
  {LobbyId, relaySession, passwordHash} einfügen.
- `JoinLobby(lobbyId, passwordHash)`: wenn `HasPassword` → `LobbyRelay` per
  LobbyId finden, `Hash` vergleichen; ungleich/fehlt → `throw "wrong password"`.
  Sonst (offen) wie bisher. Dann Mitglied hinzufügen → danach liefert die
  `MyRelay`-View dem Joiner die Session.
- `HandleDisconnect` (+ Lobby-Schließen): die `LobbyRelay`-Zeile der geschlossenen
  Lobby löschen.

## Companion (`Program.cs`)
- `HashPw(pw)`: leer→leer; sonst SHA-256(salt+pw) als 64 Hex.
- Subscribe zusätzlich `SELECT * FROM my_relay`.
- **host-cmd** liest `password` → `HashPw` → `CreateLobby(..., session, pwHash)`.
  Host-Handoff nutzt die selbst erzeugte Session direkt (kein Server-Read).
- **join-cmd wird asynchron**: liest `password`→`HashPw`; merkt
  `pending = {cmdId, lobbyId, addr(=Lobby.HostAddr), version}`; ruft
  `JoinLobby(lobbyId, pwHash)`.
  - `OnJoinLobby`-Event: Status `Failed` → ack `{ok:false,"wrong password"}`,
    pending verwerfen. Status `Committed` → pending als „awaiting session" markieren.
  - serve-Loop pollt `c.Db.MyRelay` nach `lobbyId` (Timeout ~5 s): Session
    gefunden → Handoff(client, addr, version, relay, session) + ack ok. Direkt-
    Modus (keine Session/`my_relay`-Eintrag) → Handoff mit `addr` + ack ok.
- `lobbies.json`: zusätzlich `hasPassword` (bool); weiterhin **ohne** Session/IP.

## GUI (`MainForm.cs`)
- Host-Formular: optionales **„Password"**-Feld (leer = offene Lobby) → ins host-cmd.
- Browser: zusätzliche Spalte/Marker **🔒** bei `hasPassword`-Lobbys; ListView-Tag
  trägt {LobbyId, HasPassword}.
- **Join:** hat die gewählte Lobby ein PW → kleiner **Passwort-Prompt** (eigener
  Mini-Modal-Dialog) → ins join-cmd. Falsch → „Wrong password"-Meldung.

## Fehler/Edge
- Leeres PW = offene Lobby. Falsches PW → JoinLobby wirft → ack error → GUI.
- Direkt/LAN-Modus (kein Relay): HostAddr ist öffentlich → PW dort nur
  best-effort (Relay ist der echte, geschützte Pfad). Dokumentieren.
- Schema-Change → VPS-Republish mit `--delete-data`.

## Tests
- Modul: PW-Lobby anlegen; `MyRelay` als Nicht-Mitglied leer; falscher Hash →
  JoinLobby-Fehler; richtiger Hash → Join ok + `MyRelay` liefert Session (CLI/sql).
- Companion lokal: host mit PW → `hasPassword=true`; join falsch → ack error;
  join richtig → ack ok + Handoff trägt Session (aus my_relay).
- **Regression:** offene Relay-Lobby (kein PW) join → Co-op-Flow weiter ok
  (Session jetzt aus my_relay statt öffentlicher Tabelle).
- GUI: baut; manuell - PW-Feld, 🔒-Marker, Join-Prompt.

## Out of Scope
- Rate-Limit auf Fehlversuche, Passwort-Stärke, PW-Reset (Tier-2).
