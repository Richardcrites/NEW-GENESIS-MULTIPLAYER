# In-Game Lobby GUI (Teil 3) - Native Scaleform - Design

**Datum:** 2026-06-16
**Status:** Design (genehmigt: native Scaleform, In-Game-Hotkey)

## Ziel
Ein per Hotkey im Spiel geöffnetes, **Starfield-natives** Menü zum Browsen/Hosten/Joinen
von Lobbies - das In-Game-Frontend zur bestehenden STDB/Companion-Persistenz.

## Warum native Scaleform (nicht ImGui)
ImGui-D3D12-Overlay ist hier eine Sackgasse: dokumentierter **Hard-Crash beim zweiten
Render-Frame** (HDR-Swapchain R10G10B10A2, GPU/Driver-Crash, **SEH fängt ihn nicht** -
siehe `feedback_imgui_crash`). CommonLibSF hat dagegen die komplette Scaleform-Schicht
gebunden (`ScaleformGFxMovie/Value/FunctionHandler`, `SWFToCodeFunctionHandler`,
`IMenu`, `GameMenuBase`, `UIMessageQueue`, `UI`). Nativ = echter Game-Look, pausiert das
Spiel, native Eingabe, **kein D3D-Konflikt**.

## Architektur
Das Menü ist **Anzeige + Eingabe** in der DLL. Der Companion bleibt der **STDB-Client** und
wird zum **lokalen Daemon** (`serve`-Modus): dauerhaft connected, schreibt die Lobby-Liste
laufend raus, lauscht auf Kommando-Dateien. Die DLL läuft live → sie **treibt den
ENet-Connect selbst** (kein Launcher-Neustart nötig).

```
Companion (serve, subscribed 'lobby')  ──►  pulsar_lobbies.json   (Live-Liste)
        ▲                                              │
        │ pulsar_cmd.json {action,…}                   ▼
   führt Reducer aus  ◄── Scaleform-Menü (DLL): liest Liste, rendert, Klick schreibt cmd
        │
        └─► pull-profile→profile-in + Ack ─► DLL appliet Profil (Gate) + ENet-Connect
```

**Wichtig:** Der Companion-`serve`-Daemon nutzt den **sicheren Lese-Pfad** aus dem
Security-Tier-1-Spec (per-user View/RLS), **nicht** `SELECT * FROM profile`.

## Komponenten / Dateien
| Datei | Verantwortung |
|---|---|
| `Data/Interface/pulsar_lobby.swf` (Create) | Flash-UI: Lobby-Liste, Host/Join/Refresh/Close-Buttons, Status-Text |
| `src/Lobby/LobbyMenu.{h,cpp}` (Create) | custom `IMenu`: registriert sich am UI-Menü-Factory, lädt Movie, pusht Liste via `ScaleformGFxValue`, registriert `SWFToCodeFunctionHandler`-Callbacks (OnHost/OnJoin/OnRefresh/OnClose), schreibt `pulsar_cmd.json` |
| `src/Main.cpp` (Modify) | Hotkey → `UIMessageQueue::AddMessage(LobbyMenu::MENU_NAME, kShow)`; Menü-Registrierung bei Plugin-Load |
| `lobby/companion/Program.cs` (Modify) | neuer `serve`-Modus: subscribed `lobby` (+ secure profile-view), schreibt `pulsar_lobbies.json` laufend, watcht `pulsar_cmd.json` → führt Reducer aus (create/join), `pull-profile`→`profile-in`, schreibt Ack |
| `src/Lobby/LobbyHandoff.{h,cpp}` (Modify) | Connect-Trigger: nach Companion-Ack + gestagtem `profile-in` direkt ENet-Connect zum gewählten `host_addr` initiieren (live, ohne Prozess-Neustart) |

## Daten (`%LOCALAPPDATA%\PULSAR\`)
- **Neu:** `pulsar_lobbies.json` (Companion→DLL, Live-Liste: `[{lobbyId,name,status,hostAddr,memberCount,maxPlayers}]`),
  `pulsar_cmd.json` (DLL→Companion, `{action:"host"|"join", lobbyId?, name?, addr?, max?}`),
  `pulsar_ack.json` (Companion→DLL, `{cmdId, ok, error?, hostAddr?}`).
- **Bestehend, wiederverwendet:** `handoff`, `profile-in/out`, `companion.token`.

## Datenfluss
**Join:** Companion-`serve` schreibt `lobbies.json` → Hotkey öffnet Menü, rendert Liste →
Klick **Join(id)** → DLL schreibt `cmd{join, lobbyId, addr}` → Companion: `join_lobby` +
`pull-profile`→`profile-in`, schreibt `ack{ok, hostAddr}` → DLL: `TickApplyProfile`-Gate
appliet `profile-in`, startet ENet-Connect zu `hostAddr`.

**Host:** Klick **Host** → `cmd{host,name,addr,max}` → Companion `register_account` +
`create_lobby` + bleibt connected (Keep-Alive hält Lobby offen) → schreibt `ack{ok}` →
DLL geht in Host-Rolle. F10→`profile-out`→push bleibt wie gehabt.

## AS↔C++-Brücke
- `SWFToCodeFunctionHandler` für Button-Callbacks (Flash-Klick → C++).
- `ScaleformGFxValue` um die Lobby-Liste als Array ins Movie zu pushen + Status-Text zu setzen.
- Menü-Lifecycle: `IMenu`-Subklasse, Registrierung am Menü-Factory der `UI`-Singleton,
  Open/Close via `UIMessageQueue`.

## Fehlerbehandlung
- Companion offline → `lobbies.json` fehlt/alt → Menü **öffnet trotzdem**, Status
  „Companion offline", leere Liste.
- `cmd` geschrieben, aber kein `ack` in N s → Status „Timeout/Fehler", Menü bleibt nutzbar.
- `.swf` fehlt → Menü-Open scheitert sauber (Log, **kein Crash**); Movie-Pointer/Value-Push
  defensiv guarden (Movie not ready).
- Alle Datei-IO best-effort, Stale-Dateien consume-once wie beim bestehenden Handoff.

## Tests
- **Headless zuerst:** Companion-`serve` cmd-watch-Loop gegen lokales STDB -
  `pulsar_cmd.json` schreiben → Reducer-Aufruf + `lobbies.json`/`profile-in`/`ack` asserten.
  Voller Datei-Round-Trip ohne Spiel prüfbar.
- **In-Game (User):** Hotkey öffnet Menü, Liste rendert, Host/Join-Klick schreibt cmd,
  Connect läuft; Profil appliet beim Join.

## ⚠️ Risiko / Task 1 = RE-Spike (vor dem Bauen)
Drei echte Unbekannte zuerst klären:
1. **Menü-Registrierung** - Factory-RVA + `MENU_NAME`-Mechanismus, gegen **Ghidra**
   verifizieren (CLibSF-Scaleform-Bindings vorher prüfen - wir hatten schon falsche
   Dock-Bindings; `feedback_clibsf_dock_bindings_wrong`).
2. **`.swf`-Lade-Pfad** (vermutlich `Data/Interface/`) + **ActionScript-Version** Starfields.
3. **Authoring-Toolchain:** JPEXS/ffdec (open-source) vs. Flex-SDK - minimales Test-Movie,
   das einen C++-Callback auslöst, bevor die echte UI gebaut wird.

## Bewusst raus (YAGNI v1)
Keine Passwörter/private Lobbies, kein In-Menü-Profil-Editing, kein Chat, keine Avalonia-GUI
(Companion bleibt headless Daemon).

## Abhängigkeit
Setzt den sicheren Lese-Pfad aus `2026-06-16-lobby-security-tier1-design.md` voraus →
Security-Tier-1 zuerst (oder zeitgleich).
