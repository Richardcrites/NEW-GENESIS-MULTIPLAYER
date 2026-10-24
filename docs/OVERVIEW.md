# PULSAR: project overview

Two-player co-op for **Starfield**. One player hosts, the other joins, and both
move through the same game session: shared combat, shared loot and progression,
synchronised ships, vehicles, doors, quests, and proximity voice chat.

> **Status: alpha, and no longer actively developed.**
> This repository is published so the work does not disappear. It is playable
> and it was tested by real people, but it has known bugs that break co-op in
> specific situations, and nobody is currently fixing them. If you want to pick
> it up, everything you need is here. See [Taking this over](#taking-this-over).

## Requirements

- **Starfield 1.16.244 on Steam.** The plugin resolves engine functions by
  address ID, and those IDs are tied to the exact game version. Other versions
  will not work until the addresses are re-resolved.
- [SFSE](https://www.nexusmods.com/starfield/mods/106) matching that version.
- Windows. Both players need the same PULSAR version.
- Steam only. That is an SFSE limitation, not a choice.

## How it works

Three processes on each player's machine, plus optional shared infrastructure:

```
  Pulsar.exe            PulsarCompanion.exe          PULSAR.dll
  (Avalonia GUI)        (headless daemon)            (SFSE plugin, in-game)
       |                        |                          |
       |  handoff file          |  localhost loopback      |
       +----------------------->+<------------------------>+
                                |                          |
                     SpacetimeDB lobby            ENet P2P, encrypted
                     (matchmaking only)           (all gameplay traffic)
                                                            |
                                                    peer's PULSAR.dll
```

**The lobby only does matchmaking.** Once two players are matched, all gameplay
traffic runs peer to peer over ENet with ECDH key exchange and AES-GCM, either
directly via NAT hole punching or through a relay when that fails. No gameplay
data touches the lobby server.

**The split of responsibilities** follows one rule: the DLL owns anything that
must run in the game process, the companion owns anything heavy or external, and
the tool owns the UI. Proximity voice is a good example. The companion captures
audio and runs the Opus codec, but push-to-talk is gated inside the DLL, because
a background process cannot read the keyboard while Starfield holds focus.

More detail in [`docs/PULSAR_Technical.md`](PULSAR_Technical.md), and the
design documents under [`docs/design/`](design/) explain *why* things are
built the way they are. Those are worth reading before changing anything.

## What works

Player position, animation and combat sync, shared aggro, host-authoritative
NPCs, downed and revive, inventory and progression persistence across sessions,
quest sync, doors, containers, equipment, ship docking and space flight, ground
vehicles, fast travel with a save-based bridge for procedural cells, kill XP
sharing, time and weather, and proximity voice chat.

## What does not work

Honest list, because you will hit these:

**Breaks co-op**
- Station and POI spawn divergence in space: the client is missing host POIs,
  ends up with a stale position, and appears stuck inside the ship.
- A mid-session bridge does not re-apply the client's profile, so the client can
  end up playing the host's character.
- Host worn gear can persist as an unusable ghost item on the client.
- The client's gravity can stay disabled after a station or space transition.
- Riding a vehicle together crashes the client on entry.

**Visible but survivable**
- Clone flicker on cell changes (despawn and respawn instead of reparenting).
- Explosion and gas knockdown ragdolls both players despite full health.
- Restoring from the database spams one HUD message per item.
- Ship hatch animations do not sync.
- Bidirectional door sync has edge cases when the client leads.

**Unfinished**
- Equipment sync is implemented but disabled: applying equipment to the remote
  clone crashes its animation graph.
- Corpse loot sync is implemented but untested.
- Host-authoritative NPC firing is not done.

## Structural limits

These are not bugs, they are consequences of the design, and worth knowing
before planning anything ambitious:

- **Two players.** A dedicated server and larger sessions were investigated and
  rejected as infeasible: the engine does not run headless, the networking is
  built around a single peer and would need a rewrite of several thousand lines,
  and the engine's actor load does not scale. Four players might be reachable.
  A roleplay server is not.
- **Version locked.** Every Starfield update requires re-resolving the address
  IDs before the plugin loads again.
- **Steam only.**

## Building

See [BUILD.md](../BUILD.md). The short version: you need Visual Studio 2026, vcpkg,
the .NET 8 SDK, and you **must** apply the CommonLibSF patches in `../patches/`
after cloning, or the plugin will not work.

## Taking this over

There is no gatekeeping here. Fork it, publish it, rename it, take it in a
different direction. Nobody has to ask anyone.

If you want somewhere to start, the space POI divergence and the profile
re-apply bug are the two that most often ruin a session.

Before changing anything structural, read the relevant document in
[`docs/design/`](design/). There is one per feature, written before the feature
was built, and it records why the design ended up the way it did and which
approaches were rejected. That reasoning is the part you cannot reconstruct from
the code, and it will keep you from re-walking a dead end. The line numbers
those documents cite have drifted, so search by symbol name instead.

## License

Copyright (C) 2026 ramisotti13-eng

GPL-3.0-or-later, see [LICENSE](../LICENSE), plus the additional terms in
[LICENSE-ADDITIONAL](../LICENSE-ADDITIONAL).

PULSAR links against [CommonLibSF](https://github.com/libxse/CommonLibSF), which
is why GPL is the honest choice here rather than something more permissive.

The additional terms are permitted under Section 7 of the GPL and come down to
three things: keep the attribution to the original project, mark your version as
modified, and do not name your fork plainly "PULSAR" so that people mistake it
for this one. Everything the GPL grants you is untouched. You may use, study,
change, and redistribute this, commercially included. Just say where it came
from.

Third-party components and their licenses are listed in
[THIRD-PARTY.md](../THIRD-PARTY.md).

## Disclaimer

Not affiliated with, endorsed by, or connected to Bethesda Softworks or ZeniMax.
Starfield is their trademark.

This repository contains no Bethesda assets. `PULSAR.esp` and
`PULSAR - Main.ba2` are original work created with the Creation Kit.

Mods carry risk. Back up your saves.
