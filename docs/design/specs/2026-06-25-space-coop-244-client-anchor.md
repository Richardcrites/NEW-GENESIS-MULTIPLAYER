# Space-Co-op auf 1.16.244: Client driftet im All 

**Datum:** 2026-06-25
**Status:** IMPLEMENTIERT + DEPLOYED (Build [space-bodycommit-v1], Host+NAS, 2026-06-25), TEST OFFEN. Nicht committed.
Impl: `SafeCommitHavokTransform` (FlightSync.cpp, ruft body-data SetTransform 0x23B8840 = memcpy+World-Commit) im
Tick-Apply statt nur memcpy, Body-Fallback g_playerBhkBody bei vt[8]-stale (Unseat), passive-gated, config
`coopSpaceBodyCommit` (default an). Edits: Config.h, FlightSync.cpp (+#include Config.h), Main.cpp build-tag.

## >>> FINALE DIAGNOSE (Nacht-RE 2026-06-25, Ghidra Starfield244 + Subagent) <<<
Detail-Belege: `local RE notes + `FINDINGS_agent.md`.
- **KEIN Port-/Adress-Bug.** Alle 8 Flug/PMO/Marker RVAs via port_map korrekt; vt[8] (Havok-Rotations-Hook)
  236↔244 byte-strukturell IDENTISCH (vt8_236.txt vs vt8_244.txt). Rotations-/Render-Sync intakt.
- **Wurzel:** Der Carry eines im Schiff STEHENDEN Spielers braucht auf 244 den expliziten Havok-**World-Commit**:
  body `vt[0x260]` = SetTransform → schreibt [body+0x4D0]+0x50.. und committet via FUN_14235DCE0 →
  hknp-world `world_vt[0x88]` unter Write-Lock (+0xB20). Unser Sync macht NUR (a) Render-Cache-Override (vt[8] +0xa0)
  und (b) `SafeWriteHavokState` memcpy [body+0x4D0]+0x50 - **nie den World-Commit**. → Schiff-Node rotiert,
  Physik trägt den stehenden Spieler nicht. Zusätzlich stoppt der PMO-Hijack bei Unseat (Engine feuert das
  PMO-Event nur solange der Spieler den aktiven bhkCharacterStateSpaceship hält = seated/pilot).
  Auf 236 propagierte die Engine den Cache noch an stehende Chars; 244 verlangt den Commit.
- **FIX (Option A, robust):** Passive Seite ruft pro `Tick()` (in-space + Host-Transform empfangen) den echten
  Setter auf `g_playerBhkBody` (Position-Match, hängt NICHT an vt[8]):
  `(*(void(__fastcall**)(void*,const float*))((*(void***)body)[0x260/8]))(body, transform16)`.
  vtable-Slot-Index 0x260 ist versionsunabhängig (kein RVA). Translation=0 lassen (Cockpit-Anker), nur Rotation.
  active-pilot-Gate behalten (nur passive Seite treibt). Neue Fn `SafeCommitHavokTransform(body,t16)` statt/zus.
  zum memcpy in `SafeWriteHavokState` (FlightSync.cpp:1244). Option B (PMO-Hijack breiter) geht NICHT -
  unseated Client erzeugt kein PMO-Event.
- **Risiko:** vt[0x260] nimmt den Havok-Write-Lock; aus Game-Thread-Tick rufen, nicht während Engine-Step.
  In-Game-Iteration nötig (Concurrency/Jitter). Config-gated, SEH-safe bauen. Build [space-bodycommit-v1].
---

**Symptom (User):** „Drehen ist synct aber nur die POI-Station im All dreht sich. Ich kann
nach vorne fliegen, fliegt der Client langsam nach hinten und andersrum." User besteht: lief
auf 236 ohne Probleme.
**Modell (User-bestätigt):** EIN Schiff, Host pilotiert, Client = Passagier (kein 2. Schiff).

## Beweis (Logs Session 00:09, Host=C:\... , Client=NAS <NAS>/PULSAR/logs)

1. **Schiff-Sync = Rotation-only, by design.**
   - Host `V3 send (active pilot)`: `t=(0.00,0.00,0.00)` IMMER, nur `row0` (Rotation) wandert.
   - Client `V3 tick-apply`: Rotation 1:1 korrekt (pre/wrote == Host row0).
   - Ursache: Starfield cockpit-fixes Rendering → Schiff-Mesh sitzt am Welt-Ursprung,
     die WELT bewegt sich. `ship NiAVObject world.translate` ist daher ~immer 0.
     → „Drehen synct, POI dreht sich" = exakt erwartetes Rotation-only-Verhalten.
   - Echte Schiff-Translation existiert im Transform NICHT (≙/Park, Arch-Limit).

2. **Client ist NICHT verankert.** Client `pilotDiag` durchgehend:
   `isHost=0 inSpace=1 hasShip=1 seated=0 furn=0x00000000 detPilot=0`.

3. **seated-Detektion ist KORREKT auf 244 (kein Port-Bug).** Host `pilotDiag`:
   `seated=1 furn=0x000D87EA`. Also `MiddleHighProcessData+0x428` (furn-handle) stimmt auf 244.
   → Das `seated=0` des Clients ist ECHT, kein Misread.

4. **FlightSync DUMP (Diagnose, NICHT der Sync-Pfad):** Client liest Garbage
   (`reticleX=2.6e12`, `aimX=48`, `spd=0`) weil Client nicht pilotiert - irrelevant für den Bug.

## Root Cause

Der Client ist im All **nicht im Sitz verankert** (`seated=0`). Mit nur synchronisierter
Schiff-**Rotation** (Translation architektonisch = 0) rotiert das Schiff-Mesh um einen
frei schwebenden Spieler → genau das „langsame Wegdriften".

**Warum lief 236:** Save-Bridge-Load ließ den Client im (vom Host-Save geerbten) Pilot-Sitz
SITZEN (genau dafür existiert die L.5 „Unseat post-load"-Logik: „client's player may end
up seated as pilot"). Verankert + Rotation-Sync = passte; fehlende Translation fiel im tiefen
All nicht auf. **Auf 244** kommt der Client gar nicht mehr verankert aus dem Bridge-Load
(`furn=0` ab dem ersten Diag) → schwebt → driftet.

→ KEINE Adress-Port-Regression (Rotation-Port + seated-Offset beide ok). Regression =
Sitz-/Verankerungs-Zustand kommt aus dem Save-Bridge-Load auf 244 nicht mehr durch.

## Fix-Plan

**Ziel:** Client im All zuverlässig im Cockpit-Sitz verankern → Rotation-Sync trägt ihn
(236-Gefühl), kein Drift. Host bleibt Pilot.

**Primitive vorhanden:**
- `SafeSnapInto(actor, furn)` = `Actor::SnapIntoInteraction` REL::ID 97243 (VehicleSync.cpp:974).
  ID-basiert → auf 244 via versionlib korrekt aufgelöst.
- Cell-FURN-Scan-Muster (VehicleSync `ScanNPCSyncCells` / SeatedState-Statemachine).
- Host-Pilot-Yield existiert: `IsLocalDeterministicPilot` weicht zurück solange Host
  PKT_SHIP_TRANSFORM sendet (FlightSync.cpp ~1459).

**Schritte:**
1. Neuer periodischer Client-Tick `TickClientCockpitAnchor` (gated `coopSpaceCockpitAnchor`, default an):
   wenn `IsConnected && !IsHost && inSpace && hasShip && furn==0 && hostIsActivePilot`:
   a. Cockpit-Sitz-FURN im `player->parentCell` finden (nächstes FURN zum Player; Fallback
      `LookupByID` des vom Host gesendeten Sitz-FormID, siehe unten).
   b. `SafeSnapInto(player, seatFurn)`. Retry alle ~1s bis `furn!=0` (Sitz-Ref evtl. erst
      nach Cell-Load geladen). Max N Versuche, dann aufgeben + loggen.
2. **V3-Rotation-Apply auf seated-only gaten** (Sicherheitsnetz): solange Client `furn==0`,
   KEINE Rotation auf das Client-Schiff schreiben → kein Drift im Worst Case (statt schlimmer).
   Sobald verankert → Rotation läuft wieder.
3. Host sendet seinen Sitz-FormID im PKT_SHIP_TRANSFORM (oder separat) mit, damit der Client
   denselben Sitz per FormID auflösen kann (robuster als Nearest-Scan). Host furn war
   `0x000D87EA` - prüfen ob das ein stabiler Form/Ref ist, der auf dem Client per
   `LookupByID` denselben Sitz liefert; sonst Nearest-FURN-Scan.

**Risiko / Vorsicht:** Sitzen/Aufstehen ist ein dokumentiertes Minenfeld (L.5 hat viele
destruktive Clears wieder zurückgerollt: stuck-in-chair, slide-out, no-UI). SnapInto in den
PILOT-Sitz kann dem Client einen Flug-Controller/HUD geben → Konflikt. Daher:
- Hinter Config-Flag, SEH-safe, mit viel Logging.
- Client-Pilot-Input/HUD weiter unterdrücken solange Host pilotiert (bestehende Lockout/
  Yield-Mechanik dauerhaft statt 8s, solange Host sendet).
- In-Game-Iteration nötig (nicht in einem context-erschöpften One-Shot).

## UPDATE 2026-06-25 (2. Session-Logs 00:36 + Code-Vergleich alt/neu)

User-Report neu: „nur der Quest-Marker bewegt sich, den Fehler hatten wir schon einmal";
besteht auf Port-Regression → „vergleich alte mit jetziger Version".

**Code-Vergleich PORT  durchgeführt - Flug-RVAs sind KORREKT.**
Per `port_map.py` (Oracle) verifiziert, alle 6 flight/marker-Swaps stimmen (code-244-RVA ==
port_map(236-RVA via REL::ID)):
- BhkSpaceshipVt8 0x23BA880→0x23B8270 (ID 125837), MarkerNotifyFn 0x1A1ED40→0x1A1C2F0 (102598),
  marker-Singleton 0x61EA160→0x61E2340 (938443), marker-EvtSource 0x5982220→0x597A220 (839133),
  FlightInputProc 0x68F440→0x68E6D0 (49520), PMOFire 0x2115260→0x2112320 (119862).
- Struct-Offsets (0x428/0xB8/0x60/0xB0) im Port NICHT angefasst (korrekt).
→ **Der Port hat den Flug-Pfad NICHT kaputt-geswappt.** „nur Marker bewegt sich" ist KEIN
falscher RVA. Havok-Body wird auch sauber acquired (`M.1 V2 player-ship body acquired (sticky)`),
ist NICHT regressed. Rotation synct weiter korrekt (V3 tick-apply trackt Host row0).

**Timeline-Befund Client (00:36, beweist die Wurzel):**
- 00:36:18 Client in Space (seated=0).
- 00:36:20 Client SITZT (seated=1, furn=0x000D87EA == HOST-furn, detPilot=1, active=1) -
  erbt Host-Pilot-State aus Save-Bridge.
- 00:36:22.695 **feuert**: „client seated post-load - 1-shot clear" → HUD+TakeoffMenu kHide,
  8s FlightInput-LOCKOUT, „user must hold E ~2s to unseat".
- 00:36:23-28 Client seated=1 **active=0** (Lockout nullt Input) = **EXAKT der korrekte
  Passagier-Zustand**: verankert + Host pilotiert.
- Danach (bis 00:37:29) Client seated=0 → User hat E gehalten (wie vom Flow verlangt) →
  **unverankert** → „nur Marker bewegt sich"/Drift.

**Wurzel bestätigt + verfeinert:** Der Flow lässt den Client den Sitz VERLASSEN (Hold-E).
Sobald unverankert, trägt ihn die Rotation-only-Sync nicht mehr → Welt/POI steht, nur HUD-Marker
läuft. Der Sitz-FormID **0x000D87EA ist auf beiden Maschinen identisch** (statischer Cockpit-Sitz)
→ direkt wiederverwendbar. KEINE Adress-Regression; der „Trigger" durchs Update war ein
geänderter Post-Bridge-Sitz-Zustand, nicht eine Adresse.

**Fix (verfeinert) - Passagier bleibt sitzen statt aufstehen:**
1. Bevorzugt: Flow für den Same-Ship-Passagier-Fall NICHT mehr zum Aufstehen drängen.
   Client im seated-passive Zustand HALTEN (Lockout/Yield aktiv solange Host PKT_SHIP_TRANSFORM
   sendet, statt fix 8s). Das ist genau der korrekte Zustand 00:36:23-28.
2. Robust: wenn Client trotzdem seated=0 (User hält E) → `TickClientCockpitAnchor` re-seatet via
   `SafeSnapInto` in den Cockpit-Sitz (Ref via stabilem furn-FormID 0x000D87EA / LookupByID,
   Fallback Nearest-FURN). Config `coopSpaceKeepPassengerSeated` default an, SEH-safe.
3. Sicherheitsnetz: V3-rotation-apply nur wenn seated → kein Drift falls re-seat scheitert.
ACHTUNG Minenfeld (//viele zurückgerollte Sitz-Clears) → In-Game-Iteration nötig,
NICHT als context-erschöpfter One-Shot bauen.
