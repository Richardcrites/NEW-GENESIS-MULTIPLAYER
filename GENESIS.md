# Star Wars Genesis co-op (PULSAR fork)

This repository is a modified version of PULSAR, changed so it can run together with the
**Star Wars Genesis** Wabbajack modlist for Starfield.

Based on PULSAR by ramisotti13-eng
https://github.com/ramisotti13-eng/pulsar

The upstream snapshot is the first commit in this repository
(`ramisotti13-eng/pulsar-tool@48f58d3`). Every Genesis change is a separate commit after
it, so `git diff <first commit>..HEAD` shows exactly what this fork changes.

## Before you start: game version

PULSAR's SFSE plugin only runs on **Starfield 1.16.244** (Steam). It finds engine
functions by address, and those addresses change with every game update.

Genesis pins its own Starfield version in the Wabbajack "Stock Game" folder. **Check that
version first.** Open `<Genesis folder>\Stock Game\Starfield.exe` → Properties → Details
→ Product version.

* **1.16.244:** go on to Setup.
* **Anything else:** the plugin will refuse to load (see `CompatibleVersions` in
  `src/Main.cpp`). You either need a Genesis build for 1.16.244, or someone has to port
  PULSAR's addresses to the Genesis version (see `docs/design/specs/2026-06-24-port-1.16.244.md`
  for how the last port was done). Nothing else in this guide helps until this matches.

## Setup (both players)

Both players need the **same Genesis version** with the **same profile and load order**.
PULSAR shows a warning in the lobby browser when the mod lists differ.

1. Install Genesis with Wabbajack as usual. Start it once from Mod Organizer 2 to make
   sure it works on its own, then **close MO2**.
2. Start the PULSAR tool from this fork. On **Co-op → Game & launch**, click **Browse...**
   and pick `ModOrganizer.exe` in your Genesis folder (not `sfse_loader.exe`).
   The tool reads `ModOrganizer.ini` and switches to Mod Organizer mode:
   * The game path becomes the Genesis Stock Game folder.
   * The load order is the active MO2 profile's `plugins.txt`.
   * The game is launched through MO2's SFSE executable entry, so the Genesis files are
     actually loaded.
   A status line under the path shows the profile and executable entry it found.
3. With MO2 still **closed**, go to **Setup**, click **Install mod**, then **Fix all**. This
   copies PULSAR into the Stock Game `Data` folder and enables `PULSAR.esp` in the
   Genesis profile's `plugins.txt` (a `.pulsar.bak` backup is made first). If the profile
   uses its own game INI files, the INI fixes go to the profile's `StarfieldCustom.ini`.
4. Host and join as described in [README.md](README.md). Let the PULSAR tool launch the
   game. It starts it through MO2 for you, and it closes your lobby when the game exits.

Start from a **fresh save made inside Genesis**. Saves from vanilla Starfield or from a
different Genesis version will not line up.

## What this fork changes

| Area | Problem with Genesis | Change |
| --- | --- | --- |
| Tool: launching | Wabbajack lists live in MO2's virtual file system. PULSAR started `sfse_loader.exe` directly, so the game came up without any Genesis files. | Picking `ModOrganizer.exe` launches `ModOrganizer.exe "moshortcut://:<SFSE entry>"`. (`lobby/pulsar/Services/Mo2Instance.cs`) |
| Tool: load order | MO2 overwrites `%LOCALAPPDATA%\Starfield\Plugins.txt` with its profile on every launch. PULSAR.esp was enabled there and so got dropped, and the lobby's mod check read the wrong list. | In MO2 mode, `plugins.txt` and `StarfieldCustom.ini` are read and written in the MO2 profile. |
| Tool: game folder | Install and Setup targeted the Steam folder. Genesis runs from its own Stock Game copy. | The game path is taken from `gamePath` in `ModOrganizer.ini`. |
| Plugin: NPC spawn | The Papyrus side looks forms up by plugin-local ID in official plugins only. For a Genesis NPC it spawned an unrelated vanilla form and disabled an unrelated vanilla ref. | The C++ side checks what Papyrus would resolve to and skips the request unless it is the intended form. The client then keeps its own copy of that NPC. (`src/NPCSync/NPCSync.cpp`, `PluginLookup::PapyrusFindFormInAnyPlugin`) |

## Known limits

* **Untested in game.** The tool changes build. The plugin change has not been compiled
  here: it needs MSVC, CommonLibSF and the patches in `patches/` on Windows (see
  [BUILD.md](BUILD.md)). None of it has been run with two players on Genesis yet.
* **Genesis-only NPCs are not position-synced.** When the host's NPC comes from a Genesis
  plugin and the client has no matching local NPC, the client just doesn't see the host's
  copy. A full fix means passing full FormIDs to Papyrus, which means changing
  `scripts/Source/PulsarRemoteControl.psc` and recompiling it with the Creation Kit. That
  source is a reconstruction (see `data/README.md`), so the change has to be tested with
  two players before replacing the shipped `.pex`.
* **Upstream says it was never tested with other mods.** Genesis is a total conversion, so
  expect problems in areas upstream already lists as rough: space, docking, vehicles,
  quests. Before reporting a bug, check whether it also happens in vanilla PULSAR.
* Only **portable** MO2 instances are detected, which is what Wabbajack installs.
* The official PULSAR lobby and relay are paid for until the end of 2026. After that,
  self-host them (see README).
