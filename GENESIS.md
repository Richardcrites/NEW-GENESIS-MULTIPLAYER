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
2. Get the tool: open this repo's **Actions** tab on GitHub, open the latest green
   **build** run, and download **Genesis-Coop-Windows** at the bottom. Unzip it anywhere
   except inside the Starfield or Genesis folders, keeping the `mod` folder next to
   `Pulsar.exe`, then run `Pulsar.exe`. On **Co-op → Game & launch**, click **Browse...**
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
4. Connect. Let the PULSAR tool launch the game: it starts it through MO2 for you.

Start from a **fresh save made inside Genesis**. Saves from vanilla Starfield or from a
different Genesis version will not line up.

## Connecting: Play on LAN or by IP

These sources have no lobby server address built in (upstream's release doesn't either),
so the status line reads **Offline: no lobby server**. Host codes and the public game
list need a lobby server. Direct play does not:

**Host:**
1. On the **Co-op** page, open **Play on LAN or by IP (advanced)**.
2. Same house or network: leave **Host over the internet** unticked.
   Over the internet: tick it, and forward **UDP port 7777** on your router to your PC.
   Also allow `Starfield.exe` through Windows Firewall when Windows asks.
3. Click **Host on my IP**. Starfield launches. Copy the address the tool shows
   (for example `203.0.113.7:7777`) and send it to your friend.
4. Load your Genesis save and wait in game.

**Joiner:**
1. Same expander. Paste the host's address into **Join a friend by IP** and click
   **Join by IP**. Starfield launches.
2. Load a Genesis save. Both players must be **in game with a save loaded**, not at the
   main menu. Then the host's game is sent over. That can take a while, so let it finish.

Press **F8** in game to regroup at the host.

Offline, your character is backed up locally (`%LOCALAPPDATA%\PULSAR\pulsar_profile_mirror_offline.json`)
and loaded the next time you join by IP. To get host codes back, host a lobby server
yourself (`lobby/deploy`, see [BUILD.md](BUILD.md)) and enter its address under
**Settings → Official server URI**.

## What this fork changes

| Area | Problem with Genesis | Change |
| --- | --- | --- |
| Tool: launching | Wabbajack lists live in MO2's virtual file system. PULSAR started `sfse_loader.exe` directly, so the game came up without any Genesis files. | Picking `ModOrganizer.exe` launches `ModOrganizer.exe "moshortcut://:<SFSE entry>"`. (`lobby/pulsar/Services/Mo2Instance.cs`) |
| Tool: load order | MO2 overwrites `%LOCALAPPDATA%\Starfield\Plugins.txt` with its profile on every launch. PULSAR.esp was enabled there and so got dropped, and the lobby's mod check read the wrong list. | In MO2 mode, `plugins.txt` and `StarfieldCustom.ini` are read and written in the MO2 profile. |
| Tool: game folder | Install and Setup targeted the Steam folder. Genesis runs from its own Stock Game copy. | The game path is taken from `gamePath` in `ModOrganizer.ini`. |
| Plugin: NPC spawn | The Papyrus side looks forms up by plugin-local ID in official plugins only. For a Genesis NPC it spawned an unrelated vanilla form and disabled an unrelated vanilla ref. | The C++ side checks what Papyrus would resolve to and skips the request unless it is the intended form. The client then keeps its own copy of that NPC. (`src/NPCSync/NPCSync.cpp`, `PluginLookup::PapyrusFindFormInAnyPlugin`) |
| Plugin: items and world state | When the host named a plugin the client didn't load, every receive path fell back to the host's raw FormID. On the client that ID belongs to some other mod, so loot, container takes, keys, equipment, corpse loot, consumed items and weather could become the wrong form. Your saved character's items could do the same when a mod went missing. | `PluginLookup::ResolveFromPeer` only uses the raw ID when it really belongs to the same plugin locally. Anything else is dropped with a log line. |
| Tool: mod-match check | Only reported mods the joiner was missing. A long Genesis list is cut to 4 KB by the lobby, so the tail was never compared. | Also reports mods only the joiner has and compares mod counts. It says when the host list was cut short, and also warns when one side is vanilla. |
| Tool + companion: no lobby server | No lobby address ships in the sources, so the companion quit on the connect error. Then even "Play on LAN or by IP", which never needs the server, was greyed out. | The companion stays up offline for direct host/join, keeps a local character backup, and rejects lobby-only actions with a clear message. The status line says Offline. |
| Tool: MO2 safety | Editing the profile while MO2 is open gets undone when MO2 exits. A profile switched in MO2 wasn't noticed. | Install and Fix wait until MO2 is closed. `ModOrganizer.ini` is re-read when it changes. `PULSAR.esp` is also added to `loadorder.txt`. |

## Known limits

* **Untested in game.** The tool changes build and pass a scripted test against a fake
  MO2 install. The plugin changes have not been compiled
  here: that needs MSVC, CommonLibSF and the patches in `patches/` on Windows (see
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
