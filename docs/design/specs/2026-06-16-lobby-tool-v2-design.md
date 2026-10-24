# PULSAR Lobby Tool v2 - Design Spec

**Datum:** 2026-06-16
**Ziel:** Das Lobby-Tool (`PulsarLobby.exe`) idiotensicher + sicher machen:
Englisch, eindeutige Spielernamen, weniger Tipparbeit (ein Server-Feld + History),
keine öffentliche Host-IP (DDoS-Schutz), plus QoL-Guardrails.

Bestätigte Entscheidungen: **(A)** ein „Server"-Feld, Relay wird automatisch
abgeleitet (`host:7777`), mit Advanced-Override. **(B)** harte Namens-
Eindeutigkeit (Name gehört der Maschinen-Identität; belegt = abgelehnt).

---

## Komponenten

| Komponente | Änderung |
|---|---|
| `lobby/companion-gui/MainForm.cs` | Hauptteil: Englisch, ein Server-Feld + History, Name-Claim-UI, IP-Spalte raus, Guardrails, Status, Persistenz. |
| `lobby/companion/Program.cs` | `register`-Command (+ack ok/taken), Host-IP in relay-Modus NICHT senden, `lobbies.json` ohne Host-IP. |
| `lobby/server/spacetimedb/Lib.cs` | `Account.DisplayName` **unique**; `RegisterAccount` lehnt belegte Namen ab; `CreateLobby` speichert in relay-Modus `HostAddr=""`. |

## 1. Englisch
Alle GUI-Strings (Labels, Buttons, Status, Dialoge, Log-Zeilen der GUI) → Englisch.
Companion/Modul-Logs (Dev-seitig) bleiben wie sind.

## 2. Eindeutiger Spielername (Claim)
- **Modul:** `[Unique] public string DisplayName;` (unique-Index → `ctx.Db.Account.DisplayName.Find(name)`).
  `RegisterAccount(name)`: wenn ein Account mit diesem Namen existiert und eine
  **andere** Identität hat → `throw "name taken"`. Sonst Upsert nach Identität
  (eigener Name/Rename frei). Caps wie bisher (1..32).
- **Companion:** neuer serve-Command `{"action":"register","cmdId","name"}` →
  `RegisterAccount(name)`; Reducer-Event-Status auswerten → `pulsar_ack.json`
  `{ok:true}` bzw. `{ok:false,error:"name taken"}`. (cmdId-Matching wie bei host/join.)
- **GUI:** Feld „Player name" + Button **„Claim name"**. Erfolg → grün
  „✓ You are 'Banso'"; belegt → rot „Name taken - choose another". Name wird
  lokal gemerkt (auto-fill). **Host/Join bleiben gesperrt bis Name geclaimed.**

## 3. Server-Feld + Auto-Relay + History
- **Ein „Server"-Feld** (editierbare ComboBox). Eingabe `host` oder `wss://host`.
  Daraus abgeleitet: `PULSAR_STDB_URI = wss://host`, `PULSAR_RELAY_ADDR = host:7777`.
- **„Advanced"-Checkbox** blendet ein separates **Relay**-Feld (ComboBox) ein, das
  die Ableitung überschreibt (für abweichenden Relay-Host/Port).
- **History + Persistenz:** Config `%LOCALAPPDATA%\PULSAR\lobbygui.json`:
  ```json
  {
    "playerName": "Banso",
    "servers":  ["194-164-171-23.sslip.io", "..."],
    "relays":   ["..."],
    "starfieldPath": "...",
    "autoLaunch": true,
    "advanced": false
  }
  ```
  ComboBoxen zeigen die Listen (neueste zuerst); bei „Daemon starten" wird der
  aktuelle Wert vorn eingereiht (dedupe, cap 8). Beim Start geladen, bei Änderung
  gespeichert.

## 4. Host-IP verstecken (DDoS-Schutz)
- **Root-Fix:** Im **relay-Modus wird die echte Host-IP nie öffentlich gespeichert.**
  Companion ruft `CreateLobby(name, "", max, session)` (statt LAN-IP). Modul:
  wenn `relaySession` gesetzt → `HostAddr=""` speichern (keine `IsValidHostAddr`-
  Pflicht); nur im Direkt/LAN-Modus (kein Relay) bleibt eine valide `host:port`.
- `lobbies.json`: `hostAddr` wird **weggelassen** (Client braucht im relay-Modus
  nur `relaySession`).
- **GUI-Browser:** Spalte „Host (IP)" **entfällt** komplett. Auswahl per Lobby-Name;
  Verbindung läuft über die Session. Niemand (auch kein STDB-Schnüffler) sieht eine
  Host-IP.
- Direkt/LAN-Modus (Relay leer) teilt weiterhin die private LAN-IP - opt-in, geringes Risiko.

## 5. QoL / Idiotensicher
- **Nichts neu tippen:** Name, Server/Relay-History, Starfield-Pfad, Auto-Launch persistent.
- **Starfield auto-detect:** Steam-Pfad / Registry (`sfse_loader.exe`); Browse nur als Fallback.
- **State-Machine / Guardrails:** „Claim name" erst nach Connect aktiv; Host/Join erst
  nach Connect **und** Name-Claim. Hinweise statt Sackgassen („Start the daemon first",
  „Claim a name first"). Eine Lobby gleichzeitig: beim Hosten wird „Host" zu „Close lobby".
- **Friendly errors:** Eingaben validieren (Server-Format); Connect-/Reducer-Fehler als
  Klartext statt Exceptions.
- **Status-Zeile:** „Connected · You are 'Banso'" (farbig). Leere Liste:
  „No open lobbies - host one!".

## Protokoll-Ergänzung (Companion serve)
- `{"action":"register","cmdId":"…","name":"…"}` → ack `{cmdId,ok,error?}`.
- host/join unverändert, aber host sendet im relay-Modus `addr=""` (keine IP).

## Fehlerbehandlung
- Name belegt → ack `{ok:false,error}` → GUI rot, Name nicht geclaimed.
- Connect-Fehler (falscher Server) → GUI-Status rot „Cannot reach server".
- Config korrupt/fehlt → Defaults, neu schreiben (kein Crash).

## Sicherheit
- Host-IP nie öffentlich (relay-Modus). Name an Maschinen-Identität (DPAPI-Token)
  gebunden - kein echtes Konto (OIDC = Tier-2). Relay-Auth/Rate-Limit weiterhin Tier-2.

## Tests
- **Modul:** zwei Identitäten claimen denselben Namen → 2. wird abgelehnt (sql/CLI prüfen).
- **Companion:** `register`-cmd → ack ok; nochmal mit fremdem Namen → ack taken.
  host im relay-Modus → Lobby hat `HostAddr=""` (sql), `lobbies.json` ohne IP.
- **GUI:** baut; manuell - Claim-Flow, History-Persistenz, gesperrte Buttons, keine IP-Spalte.
- **Integration:** voller 2-Spieler-Flow weiter grün (Relay live).

## Out of Scope (später)
- Echte Konten/OIDC (Tier-2), Relay-Token/Rate-Limit.
- Lobby-Passwörter, Freundeslisten, In-Game-GUI (separat).
- Reservierte Namen freigeben/Timeout (Name bleibt an Identität gebunden).
