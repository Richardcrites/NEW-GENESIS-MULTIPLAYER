# Design documents

One document per feature, written before it was built. They record the
architecture and, more importantly, **why** it ended up that way: what the
constraints were, which approaches were rejected, and what the engine forced.

That reasoning is the part you cannot recover from reading the code. The code
tells you what happens; these tell you why it is not done the obvious way.

## Read these first if you are picking the project up

| Document | Why it matters |
| --- | --- |
| `spacetimedb-backend-design` | The lobby and persistence model as a whole. |
| `lobby-relay-design` | How two players connect without port forwarding. |
| `p2p-hole-punching-design` | Direct connection first, relay as fallback. |
| `profile-serialization-design` | How a character survives across sessions. |
| `coop-downed-redesign-design` | The third attempt at downed and revive. The two earlier ones are here too, and the differences are instructive. |
| `host-authoritative-npc-sync` | Why NPCs are host-owned and what that costs. |
| `port-1.16.244` | What a game update actually requires. Read this before the next one. |

## A warning about line numbers

These documents cite source locations like `NetworkHost.cpp:777`. **Those
numbers are historical and no longer correct**, some by several hundred lines.
The files and symbol names are still right, so search by name rather than
jumping to a line.

## What used to be here

An earlier version of this repository also carried the step-by-step
implementation plans that were written alongside each design document. They were
removed: the work in them is finished, they were four times the size of the
designs, and their file-and-line references had drifted far enough to send a
reader to the wrong place.

They are still in the git history if you ever want them.
