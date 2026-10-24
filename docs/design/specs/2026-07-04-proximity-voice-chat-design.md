# Proximity Voice Chat (In-Game Voice) - Design

**Status:** Spec approved 2026-07-04. Implementation is Post-Beta (open co-op-breaking
bugs///take priority). Tracks task / issue.

## Motivation

Viele Interessenten haben keinen Mitspieler, der Starfield besitzt. Mit In-Game-Voice
koennte man einer fremden Lobby beitreten und einfach reden, ohne getrennten Discord-Call.
Der Mehrwert gegenueber Discord ist das Raeumliche: die Stimme des anderen wird mit der
Distanz leiser und ueber Zell-/Worldspace-Grenzen stumm, so dass Reden Teil der Spielwelt
wird. Ein per-Nutzer Global-Schalter ("immer hoerbar") deckt den reinen Quatsch-Fall ab,
wenn man sich in-game trennt.

**Verdict (machbar?):** Ja. Die schweren 80 Prozent stehen schon: der NAT-traversierte,
verschluesselte Peer-Transport (ENet + ECDH/AES-GCM + Relay-Fallback) und beide
Spielerpositionen samt Zellen liegen bereits in-process mit ~60 Hz vor. Was fehlt ist rein
der Audio-Stack: Capture, Opus-Codec, Jitter-Buffer, Wiedergabe.

## Entscheidungen (vom User bestaetigt 2026-07-04)

- Spec jetzt, Bauen Post-Beta.
- Audio-Stack lebt im Companion (C#), Transport laeuft ueber die DLL, Tool bietet die
  Konfiguration.
- **Reichweiten-Modell: Proximity + Global-Taste.** Standard ist raumbezogen (leiser mit
  Distanz, stumm bei Zellwechsel). Zusaetzlich ein per-Nutzer Empfaenger-Schalter
  "immer hoerbar" (plus optional belegbare Taste), der die Gain-Rechnung ueberspringt.
- **Umfang: nur Voice-Chat.** Kein oeffentlicher Lobby-Browser / Matchmaking. Fremde finden
  sich weiter ueber Lobby-Code / Server-Liste wie bisher.
- Modi: Push-to-Talk (mit belegbarer Taste) UND Open-Mic, im Tool umschaltbar. PTT ist der
  hervorgehobene Fall aus dem User-Wunsch.

## Architektur-Ueberblick

Drei Prozesse, Trennung nach bestehender Projekt-Philosophie (Companion besitzt das
Schwere/Externe, DLL besitzt das Engine-Nahe, Tool besitzt die UI):

```
 [Mikro] -> Companion (C#)                          Companion (C#) -> [Lautsprecher]
             NAudio capture 48kHz mono                     NAudio render
             VAD / PTT-Gate (WH_KEYBOARD_LL)               Jitter-Buffer ~80ms
             Concentus Opus encode                         Concentus Opus decode
                   | localhost UDP                          ^ Gain anwenden
                   v                                        | localhost UDP
             PULSAR.dll VoiceBridge  --EncSend(ch2,unrel)-->  PULSAR.dll VoiceBridge
                                     <--Gain-prepend--------  (rechnet Proximity-Gain)
             ============ ENet (verschluesselt, NAT/Relay) ============
                   \________ zum Peer und zurueck ________/
```

Der Companion macht alles Audio-Seitige mit reifen Managed-Libraries und ist crash-isoliert
vom Spiel. Die DLL bekommt nur opake Voice-Frames per localhost und leitet sie ueber den
vorhandenen verschluesselten ENet-Relay-Kanal weiter. Die Proximity-Lautstaerke berechnet
die DLL, weil sie beide Positionen in-process hat, und haengt sie an jeden eingehenden
Frame. Der Relay-Server braucht keine Aenderung: Voice fliesst als weitere ENet-Daten ueber
dieselbe Session. Da MAX_CLIENTS = 1 ist, ist Voice genau host <-> 1 client.

## Komponenten und Dateien

### 1. DLL VoiceBridge (neu, C++) - Transport + Proximity

- **Neu:** `src/VoiceBridge/VoiceBridge.cpp`, `include/VoiceBridge/VoiceBridge.h`
- Oeffnet einen reinen UDP-Loopback-Socket auf `127.0.0.1:<fixedPort>` (getrennt von ENet).
  Companion sendet/empfaengt dort die opaken Opus-Frames.
- Outbound: Loopback-Frame empfangen, als `PKT_VOICE` framen,
  `NetworkHost::EncSend(2, data, len, /*reliable=*/false)` an den Peer. Verschluesselung
  passiert automatisch in `EncSend` (`src/Network/NetworkHost.cpp:689`).
- Inbound: wird von `OnReceive` aufgerufen (Punkt 2), berechnet Proximity-Gain, prependet
  den Gain (float) und schickt das Paket per Loopback an den Companion.
- Proximity-Gain (reuse vorhandener Muster, kein neues RE noetig):
  - Lokale Position: `RE::PlayerCharacter::GetSingleton()->data.location` (raw +0x8C/90/94),
    wie in `src/Main.cpp` beim Positions-Broadcast.
  - Peer-Position: `RemotePlayerManager::GetRemotePlayerManager().m_targetPosData`
    (`include/RemotePlayer/RemotePlayerManager.h:250`). Exakte Feldnamen (x/y/z) pinnt die
    Plan-Phase.
  - Zell-Mute: `GetRemoteCellID()` (`RemotePlayerManager.h:95`) gegen
    `player->parentCell->formID`. Interior-Mismatch => Gain 0.
  - Kurve: `gain = clamp01((dFar - dist) / (dFar - dNear))`, Mute bei Zellwechsel.
    `dNear`/`dFar` aus den Settings (Defaults: dNear = 600 units voll, dFar = 4000 units
    stumm; ~70 units = 1 m, Werte in der Plan-Phase feinjustierbar).
  - **Global-Toggle:** Vor der Rechnung ein lokales Flag `voiceAlwaysAudible` aus der Config
    pruefen. Ist es gesetzt, Gain = 1.0 (Rechnung uebersprungen). Reiner Empfaenger-Schalter,
    kostet nichts und braucht keine Peer-Absprache.
- Init in `src/Main.cpp` (nach `EventTrace::Init`), Service auf dem Netzwerk-Thread in der
  bestehenden `NetworkLoop`, kein eigener Thread noetig.
- Alles in SEH-Guards / `noexcept`-Helfern, konsistent mit der bestehenden DLL-Haertung.

### 2. DLL Transport-Anbindung (Edits, klein)

- **`include/Network/PacketTypes.h`:** `PKT_VOICE = 0xA4` (verifiziert frei; hoechste
  belegte ID ist aktuell 0xA3 = PKT_NPC_SPAWN). Optionaler Mini-Header `{uint16 seq; uint8 flags}`
  vor dem Opus-Blob.
- **`include/Network/NetworkHost.h`:** `NUM_CHANNELS` von 2 auf 3 (Kanal 2 = Voice,
  unreliable). Protokoll-Aenderung, daher Versions-Bump; beide Peers muessen matchen.
- **`src/Network/NetworkHost.cpp`:** in `OnReceive`, im Custom-Handler-Block vor dem
  PacketReader-Switch, ein `if (type == PKT_VOICE) { VoiceBridge::OnVoicePacket(...); return; }`.
  Entschluesselung ist an dieser Stelle bereits passiert.

### 3. Companion VoiceEngine (neu, C#) - Audio

- **Neu:** `lobby/companion/Voice/VoiceEngine.cs`, `lobby/companion/Voice/VoiceBridgeClient.cs`,
  `lobby/companion/Voice/PttHook.cs`
- Libraries: **NAudio** (WASAPI Capture + Render + Geraete-Enumeration) und **Concentus**
  (reiner Managed-Opus, kein nativer Build). Beide net8/Windows-tauglich.
- Capture-Pfad: WASAPI capture (20 ms Frames, 48 kHz mono), VAD/Noise-Gate oder PTT-Gate,
  Concentus encode, `VoiceBridgeClient` sendet per localhost UDP an die DLL.
- Playback-Pfad: localhost UDP empfaengt `{gain float}{seq}{opus}` von der DLL,
  Jitter-Buffer (~60-100 ms, Reorder per seq, Drop bei Stau), Concentus decode, Gain
  anwenden, WASAPI render.
- **PTT:** globaler `WH_KEYBOARD_LL` Hook im Companion (`PttHook.cs`), funktioniert auch
  waehrend Starfield Fokus hat (Discord-Prinzip). Im PTT-Modus ist die Capture gegatet.
  Fallback bei Raw-Input-Spielen: DLL erkennt die Taste (hat Input-Hooks) und schickt den
  PTT-Zustand per Loopback. In v1 nur der einfache Hook-Weg.
- Einbindung in die bestehende `serve`-Schleife (`lobby/companion/Program.cs`), Start/Stop
  am Lebenszyklus des Spielprozesses UND an "Peer-Verbindung steht". Voice-Threads sind
  eigene Worker.

### 4. Tool VoiceChat-Reiter (neu, C#) - Konfiguration

- **Neu:** `lobby/pulsar/Views/VoiceChatView.axaml` + `ViewModels/VoiceChatViewModel.cs`,
  plus Nav-Eintrag in `Views/MainWindow.axaml` (Muster wie die bestehenden Reiter Keybinds,
  Servers, Setup, Settings).
- Steuerelemente:
  - Voice an/aus
  - Modus (Open-Mic / Push-to-Talk)
  - **PTT-Taste belegen** (nutzt die vorhandene Capture-Logik aus `KeybindsView.axaml`)
  - Mic-Geraet (Dropdown aus NAudio-Enumeration), Mic-Test/Pegel
  - Eingangs- und Ausgangslautstaerke
  - Proximity-Reichweite (`dNear`/`dFar`)
  - **Global "immer hoerbar" Umschalter + optional belegbare Taste dafuer**
- Persistenz/Propagation: Tool schreibt `pulsar_voice.json` nach `%LOCALAPPDATA%\PULSAR`
  (gleicher Ordner wie der bestehende Handoff-Contract). Der Companion liest die Datei bei
  Aenderung. Das langsame ~1s-Polling ist nur fuer Settings, nie fuer Audio.

## Wire-Protokolle

- **Loopback (Companion <-> DLL), UDP 127.0.0.1:**
  - Companion -> DLL (outbound mic): `[2B seq][1B flags][opus]`
  - DLL -> Companion (inbound peer): `[4B gain float][2B seq][1B flags][opus]`
- **ENet (DLL <-> Peer-DLL), Kanal 2, unreliable:**
  - `[1B PKT_VOICE][2B seq][1B flags][opus]`, in `EncSend` automatisch verschluesselt.

## Proximity-Modell (v1)

- Distanz-Attenuation `dNear`..`dFar` (Default 600..4000 units, ~70 units/m).
- Harter Mute bei Interior-Zell-Mismatch oder unterschiedlichem Worldspace.
- Global-Toggle je Empfaenger ueberschreibt beides mit Gain 1.0.
- Spaeter (nicht v1): Stereo-Panning nach relativer Peilung, Tiefpass-Muffle durch Waende,
  mehrere Sprecher (relevant erst bei mehr als 2 Spielern).

## Libraries / Abhaengigkeiten

- Companion: NuGet `NAudio`, `Concentus` (beide rein managed bzw. Windows-WASAPI).
- DLL: nichts Neues. Nur Winsock-Loopback (Plattform) und der vorhandene ENet/Crypto-Stack.
- Relay/DB: keine Aenderung.

## Fehlerbehandlung

- Kein Peer / keine Verbindung: VoiceEngine idle, kein Capture, kein Socket-Traffic.
- Companion-Absturz: Spiel + Sync laufen weiter (Prozess-Isolation), Voice faellt aus.
- DLL-Loopback-Socket belegt: VoiceBridge loggt und deaktiviert Voice, Rest laeuft.
- Fehlende Peer-Position oder unbekannte Zelle: Gain 0 (stumm) statt Absturz.
- Opus-Decode-Fehler auf einem Frame: Frame droppen, Jitter-Buffer laeuft weiter.
- Alle DLL-Pfade in SEH-Guards; ungueltige Frames werden verworfen, nie interpretiert.

## Build-Reihenfolge (Post-Beta, phasenweise testbar)

1. **Spike Transport:** PKT_VOICE + Kanal 2 + VoiceBridge-Loopback; ein Test pingt
   Dummy-Bytes Companion -> DLL -> Peer -> Companion. Verifiziert die End-zu-End-Strecke
   ohne Audio.
2. **Audio-Loop:** NAudio capture/render + Concentus encode/decode lokal im Companion
   (Loopback auf sich selbst), Latenz + Qualitaet messen.
3. **Voll verkabeln:** Capture -> Opus -> Loopback -> ENet -> Peer -> Loopback -> Decode ->
   Render. Open-Mic erst ohne Proximity.
4. **Proximity:** Gain-Berechnung in der DLL anhaengen, Zell-Mute, Reichweiten-Kurve,
   Global-Toggle.
5. **Settings-UI:** VoiceChat-Reiter, `pulsar_voice.json`, Mic-Auswahl, Modus-Umschalter,
   PTT-Hotkey, Pegel, Global-Toggle.
6. **Polish:** Jitter-Buffer-Tuning, Noise-Gate/VAD, Versions-Bump, Hardening-Check, beide
   Configs bauen (Dev + hardened).

## Risiken / offene Punkte

- **Echo bei Open-Mic ohne Headset.** v1: Noise-Gate + Headset-Empfehlung. AEC (z.B. WebRTC)
  ist out-of-scope fuer v1, optional spaeter.
- **Bandbreite Relay:** Opus ~24 kbit/s pro Stream, vernachlaessigbar neben Spiel-Traffic.
  Trotzdem im Relay-Load-Reporting mitbeobachten.
- **Protokoll-Bump:** Kanal-Anzahl-Aenderung macht alte Clients inkompatibel, sauber ueber
  Versions-Gate behandeln (Handshake-Version).
- **PTT-Hook vs Raw-Input:** WH_KEYBOARD_LL kann von Raw-Input-Spielen umgangen werden.
  Fallback ueber DLL-Input-Erkennung dokumentiert, in v1 nicht gebaut.
- **Companion-Lebenszyklus:** Voice nur aktiv waehrend der Spielprozess laeuft und eine
  Peer-Verbindung steht; an Connect/Disconnect koppeln.

## Verifikation

- Phase 1: Dummy-Roundtrip-Zaehler in beiden PULSAR.log (Dev-Build) bestaetigt Strecke
  Companion -> DLL -> Peer -> Companion.
- Phase 2/3: zwei echte Maschinen, Stimme hoerbar, Latenz subjektiv messen (Ziel deutlich
  unter Discord-Faktor, da Direct-P2P).
- Phase 4: einer laeuft vom anderen weg, Lautstaerke faellt; durch eine Tuer in eine andere
  Zelle, stumm; zurueck, wieder hoerbar; Global-Toggle an, immer voll hoerbar.
- Phase 5: Tool-Settings wirken live (Mic-Wechsel, Modus, Reichweite, PTT-Taste), ohne
  Neustart der DLL.
- Abschluss: Release-Build mit `tools/verify_hardening.ps1` sauber, beide Configs bauen.
