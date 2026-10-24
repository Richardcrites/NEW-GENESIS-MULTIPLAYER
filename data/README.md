# Game data files

These are the runtime pieces of the mod. They are binaries, so they are checked
in rather than built from this repository.

| File | What it is |
| --- | --- |
| `PULSAR.esp` | The plugin, made with the Creation Kit. Original work, no Bethesda content. |
| `PULSAR - Main.ba2` | Archive holding the compiled Papyrus scripts. |
| `Scripts/*.pex` | The same compiled scripts, shipped loose as well. |

## The master flag

`PULSAR.esp` here is the **editable** plugin, exactly as the Creation Kit writes
it. The plugin that ships to players is the same file with the ESM master flag
set, which is a single bit in the TES4 header at offset `0x08`.

`tools/package_release.ps1` sets that bit at packaging time. The filename stays
`PULSAR.esp` on purpose: existing saves reference that name, and renaming it to
`.esm` would produce a missing-plugin warning for every current player.

Do not commit a flagged copy over this one. Keep the editable version here and
let the packaging step do the flagging.

## Papyrus sources

`scripts/Source/` holds the sources for `PulsarBridge.psc` and
`PulsarFurnSync.psc`. Both are the originals.

**`PulsarRemoteControl.psc` is a reconstruction, and it is untested.** The
original source was lost; only the compiled `Scripts/PulsarRemoteControl.pex`
survives, and that `.pex` is what still ships and still runs. The script is not
optional: it spawns the remote player, so co-op does not work without it.

The reconstruction was rebuilt from three sources that agree with each other:

* The `.pex` itself. Its string table names every function, every local
  variable and every log line, and its debug section gives the line span of
  each function in the lost file.
* The C++ side of the protocol, which is complete (`RemotePlayerManager`,
  `NPCSync`).
* `docs/PULSAR_Technical.md` section 4, which documents the state machine.

What none of that gives back is the body of each function, so the control flow
is inferred. It follows the protocol the C++ side expects, but it has never
been compiled against the Creation Kit and never run in a two player session,
which is the only way to prove it behaves like the `.pex`.

So this is still a gap, just a much smaller one, and it is stated rather than
hidden. Under the GPL the preferred form for modification should be available;
for this one file what you get is a faithful but unverified rebuild next to the
binary that actually works. Do not compile it over the `.pex` and assume
nothing changed. Read it, fix it, test it with two players, then replace the
binary.
