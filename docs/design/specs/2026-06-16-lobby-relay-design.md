# PULSAR Lobby Relay - Design Spec

**Datum:** 2026-06-16
**Ziel:** NAT-freies Co-op ohne Portfreigabe. Beide Spieler verbinden sich nur
nach außen zu einem öffentlichen UDP-Relay (auf dem VPS), der die ENet-Pakete
zwischen ihnen per Session durchreicht. Spieler-Erlebnis: Lobby sehen → joinen →
spielen, ohne jede Netzwerk-Config.

---

## Architektur

Transparenter UDP-Relay: ENet bleibt **unverändert** und läuft *darüber*.

```
 Host  ──REGISTER S──▶ [ Relay :UDP ] ◀──JOIN S── Client
 Host ◀═══ ENet ═══▶  Relay  ◀═══ ENet ═══▶  Client
           (Relay paart per Session S und cross-forwardet Datagramme)
```

- Beide Seiten senden **nur ausgehend** → kein eingehender Port, keine Freigabe.
- Aus ENet-Sicht ist der „Peer" jeweils die Relay-Adresse. Der Relay merkt sich
  pro Session die zwei Endpunkte (Host/Client, wie vom NAT gemappt) und reicht
  jedes Datagramm an die Gegenseite weiter.
- Funktioniert für **jeden** NAT-Typ (kein Hole-Punching nötig).

**Warum transparent (statt ENet im Relay zu terminieren):** PULSARs
Host-autoritative Logik erwartet einen direkten Peer. Der Relay arbeitet
*unterhalb* von ENet (reines Datagramm-Forwarding) → keine Änderung an ENet-
Protokoll, Kanälen oder der bestehenden Sync-Logik.

## Wire-Protokoll (Relay ⇆ PULSAR)

Ein UDP-Socket auf dem Relay. Jedes empfangene Datagramm:

1. **Control** wenn es mit dem 8-Byte-Magic `PULSARLY` (0x50 0x55 0x4C 0x53 0x41
   0x52 0x4C 0x59) beginnt UND die exakte Control-Länge hat:
   - `REGISTER` = magic + `0x01` + 16-Byte-Session → setzt `host[S] = src`.
   - `JOIN`     = magic + `0x02` + 16-Byte-Session → setzt `client[S] = src`.
   - Control wird **konsumiert**, nie weitergeleitet (auch Keepalives).
2. **Daten** (alles andere): Quelle nachschlagen → an die Gegenseite der Session
   forwarden. Quelle == `host[S]` → an `client[S]`; Quelle == `client[S]` → an
   `host[S]`. Unbekannte Quelle ohne Control → verwerfen.

Der 8-Byte-Magic + Längen-Check macht eine Kollision mit echten ENet-Paketen
praktisch unmöglich; Control wird daher bei *jedem* Paket geprüft (so werden
Host-Keepalives korrekt konsumiert statt geforwardet).

**Session-Kodierung:** In Lobby/JSON/Handoff steht die Session als **32 Hex-
Zeichen** (text-freundlich). Auf dem Draht (REGISTER/JOIN-Payload) sind es die
**16 dekodierten Bytes**. Die DLL dekodiert Hex→Bytes; der Relay vergleicht roh
und interpretiert den Inhalt nicht.

## Komponenten

### 1. Relay-Daemon - `lobby/relay/` (neues C#-Projekt, net8.0)
- Ein `UdpClient`/Socket auf konfigurierbarem Port (Default **7777/udp**).
- Session-Map `Guid(16B) → { IPEndPoint? host, IPEndPoint? client, lastSeen }`.
- Empfangsschleife: Control parsen (REGISTER/JOIN) oder Daten forwarden.
- Session-Timeout (Default 10 min ohne Traffic → entfernen). Host-Endpoint-
  Wechsel (NAT-Rebind) wird vom nächsten REGISTER aktualisiert.
- Läuft als 3. Service im `lobby/deploy/docker-compose.yml`, UDP-Port direkt
  veröffentlicht (kein TLS - das ist rohes Game-UDP; die Session-ID ist das
  Geheimnis). Eigenes `Dockerfile` (dotnet-runtime) oder `mcr.microsoft.com/
  dotnet/runtime:8.0` + publizierte DLL.

### 2. PULSAR C++ - `src/Network/NetworkHost.cpp`
- **Relay-Modus aktiv**, wenn der Handoff eine Relay-Adresse + Session liefert
  (sonst unveränderter Direkt-Pfad → LAN bleibt wie bisher).
- *Host* (`StartHost`): nach `enet_host_create` die Session aus dem Handoff
  übernehmen; per `enet_socket_send(m_host->socket, relayAddr, REGISTER+S)` beim
  Relay registrieren und alle **~15 s** wiederholen (hält das NAT-Pinhole offen),
  bis ein Client verbunden ist (danach hält ENet-Traffic es offen; Keepalive
  läuft sicherheitshalber weiter).
- *Client* (`Connect`): vor dem Connect `JOIN+S` an den Relay senden (3× über
  ~150 ms, gegen Reordering), dann `enet_host_connect(relayAddr)` **statt** der
  Host-IP. Restliche ENet-Logik unverändert.
- Neue Helper: `RegisterWithRelay(session)`, `JoinRelay(session)`; Reuse von
  ENets Socket via `m_host->socket`.

### 3. STDB-Modul - `lobby/server/spacetimedb/Lib.cs`
- `Lobby` bekommt Feld `RelaySession` (String, 32 Hex-Zeichen = 16 Byte).
- `CreateLobby` nimmt `relaySession` als zusätzliches Argument (vom Companion
  generiert), validiert Länge/Hex. `HostAddr` bleibt für den Direkt/LAN-Fallback
  erhalten (optional).
- Sichtbarkeit unverändert (Lobby ist lesbar; `RelaySession` ist für Joiner
  nötig und kein Account-Secret).

### 4. Companion + Handoff - `lobby/companion/Program.cs`
- Relay-Adresse aus `PULSAR_RELAY_ADDR` (z.B. `relay.example.com:7777`); leer →
  Direkt-Modus (heutiges Verhalten).
- *Host* (`serve`-host-cmd): 16-Byte-Session per RNG erzeugen → an `CreateLobby`
  übergeben → Handoff schreibt `relay_addr` + `relay_session` (statt/zusätzlich
  zu `host_addr`).
- *Client* (join): `relay_session` der Lobby auslesen → Handoff schreibt
  `relay_addr` + `relay_session`.
- `pulsar_lobbies.json` bekommt `relaySession` (für die GUI/den Client).

### 5. GUI - `lobby/companion-gui/MainForm.cs` (klein)
- Optionales Feld **„Relay (Host:Port)"** (Default aus `PULSAR_RELAY_ADDR`); wird
  als Env an den gespawnten Daemon weitergegeben. Leer → Direkt-Modus.

## Datenfluss (end-to-end)
1. Host „Lobby hosten" → Companion erzeugt Session S → `CreateLobby(..., S)` →
   Lobby speichert S → Handoff (`relay_addr`, `relay_session=S`).
2. Starfield (Host) startet → DLL liest Handoff → `enet_host_create` +
   `REGISTER S` an Relay (+Keepalive).
3. Client sieht Lobby (inkl. S) → „Beitreten" → Companion Handoff
   (`relay_addr`, `relay_session=S`).
4. Starfield (Client) startet → DLL `JOIN S` an Relay → `enet_host_connect(relay)`.
5. Relay paart Host/Client von S → forwardet → ENet-Handshake läuft → Co-op.

## Fehlerfälle
- **Kein Join:** Session-Timeout im Relay (10 min) → vergessen.
- **Host trennt:** ENet-Disconnect wie bisher; Relay-Session läuft per Timeout ab.
- **Relay-Neustart:** Sessions weg → laufende Spiele brechen ab, neue Lobbys/
  Joins funktionieren sofort wieder (Lobby-DB ist davon unabhängig).
- **NAT-Rebind des Hosts:** nächstes Keepalive-REGISTER aktualisiert `host[S]`.
- **Relay nicht erreichbar:** DLL-Log + bestehender Reconnect-Pfad; kein Crash.

## Konfiguration
- Relay: `PULSAR_RELAY_PORT` (Default 7777). Compose veröffentlicht `7777/udp`.
- Companion/DLL: `PULSAR_RELAY_ADDR` (`host:port`); leer = Direkt-Modus.
- Firewall (VPS): zusätzlich **UDP 7777** öffnen.

## Sicherheit
- Session-ID = 128-bit Zufall = das Beitritts-Geheimnis (nicht ratbar). Reicht
  für v1 (Tier 1). Echte Relay-Token/Auth = später (Tier 2, zusammen mit OIDC).
- Der Relay sieht nur opake UDP-Bytes (ENet-Payload), interpretiert nichts.
- Reducer validiert `relaySession` (Länge/Hex), Caps wie gehabt.

## Entscheidungen (v1)
- **Relay nur wenn konfiguriert** (`PULSAR_RELAY_ADDR` gesetzt); sonst
  unveränderter Direkt/LAN-Pfad. (Löst „immer Relay vs. LAN-Direkt": LAN-Nutzer
  ohne Relay-Config bleiben direkt + latenzarm; Internet-Nutzer setzen die Relay-
  Adresse.)
- Relay als eigenes C#-Projekt im selben Docker-Compose.
- ENet-Protokoll bleibt unangetastet (Relay arbeitet darunter).

## Tests
- **Relay-Unit:** zwei lokale UDP-Sockets simulieren Host/Client → REGISTER/JOIN
  → Datagramm-Roundtrip + Cross-Forwarding + Timeout.
- **PULSAR-C++:** Smoke - Host registriert (REGISTER im Relay-Log), Client joint,
  ENet-Handshake über Relay steht (`peer slot 1` / Clone wie beim LAN-Test).
- **Integration:** 2 Spieler übers Internet, beide hinter NAT, **ohne**
  Portfreigabe → Co-op läuft.

## Hauptrisiko (zuerst validieren)
`REGISTER`/`JOIN` über ENets eigenen Socket (`m_host->socket` + `enet_socket_send`)
und dass ENet die vom Relay geforwardeten Pakete (Quelle = Relay-Adresse) als
seinen Peer akzeptiert. → Als allererstes isoliert testen (Relay-Unit + ein
Minimal-ENet-Smoke), bevor der Rest gebaut wird.

## Out of Scope (später)
- NAT-Hole-Punching für direktes P2P (Latenz-Optimierung) mit Relay-Fallback.
- Relay-Token-Auth / Rate-Limiting (Tier 2).
- Automatische LAN-Direkt-Erkennung (heute: Relay-Adresse leer lassen).
- Mehrere gleichzeitige Relay-Sessions/Last-Tests bei vielen Lobbys.
