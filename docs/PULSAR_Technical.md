# PULSAR - Technical Architecture

> **Status:** 2026-04-20. Co-op on static cells (cities/stations) stable; proc cells (planets) stable for movement, animation and shooting. NPC planet-sync designed (Phase A/B/C) but not yet implemented.
>
> **Target:** Starfield 1.16.236 + SFSE 0.2.19.
> **DLCs tolerated but not required:** Shattered Space, Terran Armada (handshake exchanges flags, warns on mismatch).

---

## 1. Overview

PULSAR is an asymmetric 2-player co-op mod. One machine runs as **host**, the other as **client**. The host's world/save is transferred to the client at connect, after which both machines run the same save in parallel, synced via UDP (ENet).

Each machine sees:
- Its own local player (driven by the user's input, vanilla engine)
- A "clone" actor representing the other player (driven by incoming network packets)
- The same world state (NPCs, quests, time) - only deltas are synced, not full state

The mod is an **SFSE plugin** (`PULSAR.dll`) plus a small Papyrus bridge (`PULSAR.esp` + `PulsarRemoteControl.psc`). Initial development used Papyrus to drive spawns (because `PlaceAtMe` had to run on the game thread), and this pattern was kept even after C++ gained a Havok-safe game-thread path.

---

## 2. System Architecture

```
┌────────────────────────────────┐          ┌────────────────────────────────┐
│            HOST                │          │            CLIENT              │
│                                │          │                                │
│  Local player (vanilla)        │          │  Local player (vanilla)        │
│       ↓                        │          │       ↓                        │
│  FillAnimationData()           │          │  FillAnimationData()           │
│  + BoneSync::ReadBones()       │          │  + BoneSync::ReadBones()       │
│       ↓                        │          │       ↓                        │
│  PKT_POSITION  ─────────────── ENet ─────────────→ RemotePlayerManager     │
│  PKT_BONES                                         ::RequestSetPosition()  │
│  PKT_FIRE                                          ::RequestSetBones()     │
│  PKT_EQUIP                                                ↓                │
│  PKT_NPC_STATE                                     ProcessPending (60Hz)   │
│                                                          ↓                 │
│  NPCSync (Sarah et al.)                            Clone on planet/city    │
│                                                                            │
│  ←──────────────  same packet types reversed (client → host)  ──────────┐ │
│                                                                         │ │
│  RemotePlayerManager (clone of client) ← ────────────── ENet ←──────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
```

Key structural points:
- **Symmetric** - both sides run the same code paths. Host-ness is just a config flag (`g_config.isHost`).
- **Authoritative local state** - each side owns its own player. No server-side truth; movement on the remote is purely visual.
- **Clone driven by packets** - the "other" player is just an NPC spawned via `PlaceAtMe(player_base)`, then hijacked by our hooks (IMovementDirectControl, PerformAction blocker, vtable-patching for animation).

Subsystems by file:

| Directory | Purpose |
|---|---|
| `src/Main.cpp` | SFSE plugin entry. Installs `AddPermanentTask` callback that drives per-frame work. |
| `src/Network/NetworkHost.cpp` | ENet setup, packet dispatch, connection state. |
| `src/RemotePlayer/RemotePlayerManager.cpp` | The big one (~2300 lines). Clone spawn, position sync, animation, hooks. |
| `src/Animation/BoneSync.cpp` | Reads/applies 19 bone rotations + hip position. |
| `src/NPCSync/NPCSync.cpp` | Side-NPCs (Sarah, etc.) - different from the clone path. |
| `src/Companion/CompanionManager.cpp` | Vasco-style 2nd clone ("the other player as companion"). |
| `src/WorldSync/WorldSync.cpp` | Save transfer on connect. |
| `src/Profile/ClientProfile.cpp` | Client sends its inventory/appearance to host. |
| `src/SaveScanner/SaveScanner.cpp` | Finds most recent save file for transfer. |
| `src/CloneManager/CloneManager.cpp` | C++-only spawn proof-of-concept (auto-disabled; Papyrus path in use). |
| `src/UI/*.cpp` | Pre-launch connect screen + in-game overlay. |
| `src/Debug/*.cpp` | Instrumentation hooks. |

---

## 3. Network Layer

**Transport:** ENet UDP, two channels:
- Channel 0: real-time (position, bones) - unreliable
- Channel 1: large / state (equipment, fire, profile, save, NPC-state) - reliable

**Thread model:** ENet runs on a dedicated network thread. All `enet_peer_send` / `enet_host_service` calls are wrapped in `m_enetMutex` (ENet is not thread-safe - see `feedback_enet_threadsafe.md`).

**Timeouts:**
- Normal: 5 s
- During loading screen (set by WorldSync): 120 s (the client side is frozen during save load)
- Auto-reconnect every 3 s

**Config:** `PULSAR.ini` carries `isHost`, `hostAddress`, `port`. Overridden at runtime by the connect UI.

**Packet types:** `include/Network/PacketTypes.h`. All packed structs (`#pragma pack(1)`).

| ID | Name | Direction | Reliable? | Purpose |
|---|---|---|---|---|
| `PKT_HANDSHAKE` | 'KloBrille' | client→host | ✓ | magic + version + DLC flags |
| `PKT_POSITION` | 0x01 | both | ✗ | pos + rot + anim-var snapshot |
| `PKT_BONES` | 0x02 | both | ✗ | 19 bone quaternions + hips |
| `PKT_EQUIP` | 0x03 | both | ✓ | weapon + armor array |
| `PKT_FIRE` | 0x04 | both | ✓ | fire-action FormID |
| `PKT_CLIENT_PROFILE` | 0x05 | client→host | ✓ | inventory + appearance |
| `PKT_NPC_STATE` | 0x06 | host→client | batched | NPC pos/rot/health |
| `PKT_DROP_ITEM` | 0x07 | both | ✓ | dropped item (currently disabled) |
| `PKT_SAVE_TRANSFER` | 0x70 | host→client | ✓ | raw .sfs bytes at connect |

---

## 4. Spawn Pipeline

**Why the split C++/Papyrus:** `PlaceAtMe` has to run on the main game thread. At project start we used Papyrus (which inherently runs on the game thread); we kept this even after C++ got Havok-safe main-thread execution. A C++-only spawner exists in `CloneManager` as a proof-of-concept but is auto-disabled.

**GLOB-bridge (PULSAR.esp):** PULSAR.esp defines Global variables; C++ writes them, Papyrus reads & reacts.

States in `PulsarSpawn` GLOB:
- `0.0` = despawned
- `1.0` = C++ has requested spawn
- `2.0` = Papyrus has spawned (Actor exists, FormID written to `PulsarFormHi/Lo`)
- `3.0` = C++ has taken over (hooks installed, CharController ready)

**Flow:**

```
C++                                             Papyrus (PulsarRemoteControl.psc)
─────────────────────                           ────────────────────────────────
RequestSpawn()
  → PulsarSpawn = 1.0
                                              OnTimer (50ms):
                                                if PulsarSpawn==1.0:
                                                  ref = PlaceAtMe(playerBase, 1, true, false, false)
                                                  PulsarFormHi/Lo = ref.GetFormID()
                                                  PulsarSpawn = 2.0
                                                  [spawn XMarker for LookAt]
                                                  ref.SetLookAt(marker)

ProcessPending (60Hz):
  FindSpawnedActor()
    if PulsarSpawn >= 2.0 and !m_remoteActor:
      m_remoteActor = LookupByID(hi<<32|lo)
      → install PerformAction + EvaluatePackage hooks
      → CallCreateCharControllerRaw (REL::ID 101052)
      → PulsarSpawn = 3.0
```

**Despawn:** `PulsarSpawn = 0.0` → Papyrus calls `DisableNoWait` + `Delete` → ref GC'd. Our `DoDespawn()` zeros internal state, restores patched vtables.

**ForceRespawn:** despawn → 2 s delay → spawn. Used when state gets wedged.

**Papyrus quirks (see `feedback_papyrus_api.md`):**
- `StartTimer(0.05)` must be called at the TOP of `OnTimer` - if an exception later aborts the body, the next tick still fires.
- `PlaceAtMe` in Starfield has **5 parameters** (not 4 like Skyrim): `(base, count, persist, disabled, deferInit)`.
- `EnsureGlobs()` via `GetFormFromFile("PULSAR.esp", 0x800)` etc. - load-order independent.

---

## 5. Position & Movement Sync

### 5.1 Packet

```cpp
struct PositionData {
    float x, y, z;       // worldspace
    float rotX, rotY, rotZ;
    float speed;         // graph-var "Speed"
    float direction;     // graph-var "Direction"
    float lookHeading;   // "LookHeadingOffset"
    uint16_t animFlags;  // packed: iSyncIdleLocomotion | iSyncSprintState
                         //       | iSyncRelaxReadySneak | IsCompletelyStopped
                         //       | bPlayerMoveStartActive
    AnimVarSnapshot vars; // full int/float graph-var capture (see 6.2)
};
```

Sent unreliable at ~60 Hz from both sides (`Main.cpp` `AddPermanentTask` body, 16 ms gate).

### 5.2 Receive side

`RemotePlayerManager::RequestSetPosition` puts the incoming `RemotePositionData` in `m_pendingPosData` under `m_posMutex`, sets `m_pendingPosition=true`.

`ProcessPending` (on game thread) consumes:
```cpp
if (m_pendingPosition.exchange(false)) {
    std::lock_guard lock(m_posMutex);
    m_targetPosData = m_pendingPosData;
    m_hasPosition = true;
}
```

### 5.3 Apply side - `DoSetPosition()`

Core logic (simplified):
```cpp
// 1. Pin parentCell (UAF fix - T22)
SafePinParentCell(m_remoteActor, player->parentCell);

// 2. Lerp (unless jumping - then snap)
if (isJumping) { lerpX=targetX; lerpY=targetY; lerpZ=targetZ; }
else {
    lerpX += (targetX - lerpX) * 0.02;
    lerpY += (targetY - lerpY) * 0.02;
    lerpZ += (targetZ - lerpZ) * 0.05;
}

// 3. Teleport on huge delta
if (dx²+dy² > 500²) { lerp = target; }

// 4. Pick write mode
if (isProcCell) {
    SafeWriteActorDataLocation(pos);           // +0x8C, every frame
    if (gate_30Hz) SafeHavokBodySetPosition(pos);  // vtable[72], 30 Hz
} else {
    SafeSetActorPosition(pos, rotZ);           // full Actor::SetPosition
}

// 5. DirectControl speed
DirectCtrl.SetSpeed(isJumping ? 0 : targetSpeed);  // vtable[3]
```

### 5.4 IMovementDirectControl

From Actor+0x240 → dispatch-table+0x48 → IMovementDirectControl interface. vtable slots:
- `[1]` Enable
- `[3]` SetSpeed(float)
- `[5]` SetTargetPosition(NiPoint3*, speed)

We use `[3]` to drive locomotion blend (walk/run/sprint based on speed magnitude). The engine's built-in anim graph picks the right locomotion state from speed + direction + sneak-flag.

### 5.5 Proc-cell vs static-cell split

Actor::SetPosition Stage-1 (`FUN_140B4EB30`) re-indexes the refr in the cell-quadrant hashmap. On proc cells this dereferences `refr->parentCell` before acquiring the cell's lock - a **UAF** when proc-gen has destroyed the old cell but our clone still points at it (T22 finding).

**Fix:** every frame, write `clone->parentCell = player->parentCell` (pin). Player is the streaming anchor, their cell is always live. UAF-window closed.

**But:** T22 scenario (c) - freshly allocated proc cell may not have its `cell+0x110` hashmap buckets constructed yet. So in proc cells we still skip Stage-1 by using `data.location`-direct writes + Havok-direct CharController writes. In static cells we use normal `Actor::SetPosition`. See `feedback_procedural_cell_crash.md`.

---

## 6. Animation Pipeline

### 6.1 Two parallel mechanisms

The clone's animation is driven by **two** inputs that the engine's anim graph combines:
1. **Graph variables** (set every frame from network data) - drive locomotion blend, pose, weapon stance.
2. **Graph events** (fired on state transitions via `NotifyAnimationGraphImpl`) - drive jump, sneak, draw.

Plus `IMovementDirectControl::SetSpeed` drives the locomotion-state machine's speed input.

### 6.2 Graph variables captured from host

`FillAnimationData()` in `Main.cpp` / `NetworkHost.cpp` reads host player graph vars:
- Floats: `Speed`, `Direction`, `LookHeadingOffset`, `LookAtPitchVar`, `LookAtHeadingVar`
- Ints: `iSyncIdleLocomotion`, `iSyncSprintState`, `iSyncRelaxReadySneak`, `iSyncJumpState`, `iSyncGunActions`, `iSyncGunDown`
- Bools: `IsCompletelyStopped`, `bPlayerMoveStartActive`, `bHeadTrackingActive`
- Additional: `s_weaponDrawnSticky` (from `actorState2 & 0x3`), `isSneaking` via `REL::ID(100547)`.

### 6.3 Graph variables applied to clone

In `DoSetPosition` (per frame):
```cpp
ForceSetGraphIntDirect(graph, setIntFn, "iSyncGravity", 0);
ForceSetGraphIntDirect(graph, setIntFn, "iSyncWeaponDrawState", m_weaponDrawn ? 1 : 0);
ForceSetGraphIntDirect(graph, setIntFn, "iSyncActorStance", isWeaponReady ? 1 : 0);
ForceSetGraphIntDirect(graph, setIntFn, "iSyncRelaxedReadyAlert", npcRelax);
ForceSetGraphIntDirect(graph, setIntFn, "iSyncGunActions", 0);  // hardcoded - unsafe to sync (crashes in proc cells)
ForceSetGraphIntDirect(graph, setIntFn, "iSyncGunDown", 0);
// ... plus head-tracking flags, sighted, bleedout, swim, partial-body etc.
```

The `iSync*` prefix comes from Starfield's internal multiplayer scaffold (T14 finding): 18 variables intended for ReplayTool / CK-preview sync. We use them because NPC graphs honor them.

### 6.4 Jump sync - the interesting case

Host transitions `iSyncJumpState`:
- Weapon HOLSTERED: `0 → 1 → 0` (state 2 never appears - host graph quirk)
- Weapon DRAWN: `0 → 1 → 2 → 0`

On the clone we detect state changes and fire events:
```cpp
if (netJump == 1) {
    NotifyAnimationGraphImpl("JumpUp");
    s_forceAnimUntil = now + 1500;          // let ActionFall/Land through for 1.5s
    s_syntheticFallAt = now + 250;          // schedule synthetic JumpFall
} else if (netJump == 2) {
    s_syntheticFallAt = 0;                  // cancel synthetic
    NotifyAnimationGraphImpl("JumpFall");
    s_forceAnimUntil = now + 700;
} else if (netJump == 0) {
    s_syntheticFallAt = 0;
    NotifyAnimationGraphImpl("JumpDown");
    s_forceAnimUntil = now + 400;
}

// 250ms after JumpUp, if state 2 didn't arrive, fire synthetic JumpFall
// (fixes the holstered-host case where clone would stay in JumpUp pose = "ball glitch")
if (s_syntheticFallAt > 0 && now >= s_syntheticFallAt) {
    NotifyAnimationGraphImpl("JumpFall");
}
```

**Force-animation window** (`g_forceAnimation` flag + `s_forceAnimUntil` timestamp): while open, `HookedPerformAction` lets Engine actions through on the clone. Needed because during a jump the engine tries to fire `ActionFall`, `ActionLand`, etc. on the clone - without pass-through the graph can't transition.

Window sizes:
- `JumpUp`: 1500 ms (covers full arc)
- `JumpFall`: 700 ms (covers rest of fall + land)
- `JumpDown`: 400 ms
- `Sneak`: 200 ms
- `Draw/Holster`: 500 ms

### 6.5 Graph kickstart

After initial equip+holster, auto-fire a draw→holster cycle with **2000 ms** gap. This triggers the engine's non-instant `ActionInitializeGraphToBaseState` (0x0002FFA9) which brings the graph to a state where jumps animate correctly. The instant version (`ActionInstantInitializeGraphToBaseState`, fires at spawn automatically) alone is insufficient.

500 ms gap was too short - engine didn't register the drawn state before our holster, so the holster became a non-transition and the init never fired.

### 6.6 Pitch / LookAt

Pitch (up-down aim) is NOT a graph var that can be set from outside - the engine computes it from LookAt targets. Our solution:

1. Papyrus spawns an XMarker (FormID `0x3B`) once, calls `SetLookAt(marker)` on the clone each tick.
2. C++ moves the marker every frame via `ExtraDirectAtTarget` on the clone's ExtraDataList.
3. Marker position computed in spherical coords with **Bethesda-forward convention**:
   ```
   forward.x = sin(yaw) * cos(pitch)        // Bethesda: X = sin, not cos
   forward.y = cos(yaw) * cos(pitch)        // Bethesda: Y = cos, not sin
   forward.z = sin(pitch)
   markerPos = clonePos + forward * LOOK_DIST;   // LOOK_DIST = 100
   ```
4. Graph settings in PULSAR.esp: `LookAtOnGain=100`, `fPitchSpeed=100`, `DirectAtPitchMax=1.57`, `bHeadTrackingActive=true`.

See `feedback_pitch_approaches.md` for the 30+ approaches that didn't work.

### 6.7 Bone sync

`BoneSync::ReadBones` captures 19 bone rotations (quaternions) + hip position from host's character at ~20 Hz:
- Root, Pelvis, Spine1/2/3, Neck, Head, Clavicle.L/R, UpperArm.L/R, LowerArm.L/R, Hand.L/R, Thigh.L/R

Sent in `PKT_BONES` (~512 bytes). Client applies via direct `NiAVObject->local.rotation` writes on each bone under the clone's 3D node. This gives pose fidelity beyond what locomotion-blend can produce (e.g. head-tracking another direction, slight lean, arm placement while aiming).

---

## 7. Weapon & Equipment Sync

### 7.1 Equipment

Host player's equipped weapon + armor array is sampled every 2 s, hashed, sent only on change in `PKT_EQUIP`:
```cpp
struct EquipData {
    uint32_t weaponFormID;
    uint32_t armorFormIDs[16];
    uint8_t  armorCount;
};
```

Client applies via `ActorEquipManager::EquipObject` (REL::ID 101949). Direct engine call; our only work is to build the `BGSObjectInstance` with the right `instanceData` (for modded weapons with specific mod slots we pass the inventory instance data).

### 7.2 Weapon drawn state

`m_weaponDrawn` is set from host's `actorState2 & 0x3` with 50 ms debounce. Applied via `DrawWeaponMagicHands(bool)` - the same engine call Papyrus/combat-hooks use for draw/holster. Engine responds with internal `ActionDraw`/`ActionSheathe` which we let through during the force-window.

### 7.3 Weapon swap re-draw

Equipping a new weapon while the old one was drawn: holster old → wait 800 ms → re-draw new. Needed because the engine's equip pipeline expects holstered state to swap cleanly; drawing on top of a swap leaves the clone holding the old weapon's model with the new weapon's animations.

---

## 8. Fire / Combat Sync

### 8.1 Fire-forward (synthesized PerformAction)

When the host player fires, `HookedPerformAction` sees the action (e.g. `ActionFireAuto` = 0x00004A5C), extracts its FormID, sends `PKT_FIRE` to the client.

Client receives → `m_pendingFire = true` + `m_pendingFireAction = formID`.

In `ProcessPending`:
```cpp
if (canFire && m_pendingFire.exchange(false)) {
    auto* actionForm = LookupByID(fireFormID);

    // Build fake actionData struct (the type PerformAction takes)
    alignas(16) uint8_t actionData[0x68]{};
    *(void**)(actionData + 0x00) = BGA_vtable;
    *(Actor**)(actionData + 0x08) = m_remoteActor;  // target = clone
    *(TESForm**)(actionData + 0x18) = actionForm;
    *(void**)(actionData + 0x28) = AO_vtable;

    // Skip ActionSightedRelease (breaks aim pose)
    if (fireFormID == 0x00004A58) return;

    // Set iSyncSighted before fire-actions so first projectile aims forward
    if (fireFormID == 0x00004A5C || fireFormID == 0x00004A5A) {
        ForceSetGraphIntDirect(graph, setInt, "iSyncSighted", 1);
    }

    SafeCallPerformAction(s_origPerformAction, actionData);
}
```

This routes the action through the engine's full fire pipeline: projectile spawn, muzzle flash, recoil, sound. Used to crash on proc cells (see below).

### 8.2 The proc-cell crash and why it's gone

The `skipFireForwardProcCell` gate was added on 2026-04-19 after the engine's fire path crashed ~5 s after the first shot on a planet. At the time we attributed this to "bhkWorld bullet spawn races proc-gen".

T22 later showed the real cause was UAF on `clone->parentCell`. The parentCell-pin (applied every frame) closes the UAF window. Fire-forward was re-enabled on proc cells on 2026-04-20 and works end-to-end - bullets, sound, impact marks all visible.

### 8.3 No friendly-fire / no combat on clone

Design rule: the clone is never a combat target. It doesn't take damage, doesn't flinch, doesn't aggro.

`HookedPerformAction` blocks actions on the clone **unless**:
- action is whitelisted (`ActionInitializeGraphToBaseState`, `ActionInstantInitializeGraphToBaseState`)
- `g_forceAnimation` window is open (jump/sneak/draw/sighted transitions)
- action is an outgoing fire-sync we synthesized ourselves (handled separately)

Incoming damage reactions (flinch, stagger, hit, death) - always blocked. See `feedback_coop_no_combat_clone.md`.

---

## 9. NPC Sync (Phase A - current)

**Separate from the player-clone path.** NPCSync tracks side-NPCs (Sarah, other companions, static scripted NPCs).

### 9.1 Host capture

`HookedActorUpdate` fires for every Actor every tick. For each non-player actor in the same cell as the player with a static FormID:

```cpp
if (a_actor == player) return;
if (a_actor->parentCell != player->parentCell) return;
if ((a_actor->formID & 0xFF000000) == 0xFF000000) return;  // skip dynamic refs

// Capture: pos, rot, health, weaponDrawn, isDead
g_tracked[formID] = snapshot;
```

`OnGameThreadTick` (every 33 ms ≈ 30 Hz) drains up to 16 tracked NPCs per tick, batches into `PKT_NPC_STATE`:
```cpp
struct NPCStateBatchHeader { uint32_t cellFormID; uint16_t count; };
struct NPCStateData {
    uint32_t refFormID, baseFormID;
    float    posX, posY, posZ, rotZ;
    float    health, healthMax;
    uint8_t  flags;  // bit 0: isDead, bit 1: weaponDrawn
};
```

Unreliable for normal updates; reliable when an NPC dies (flags bit 0).

### 9.2 Client apply

`ApplyPendingRemoteState` resolves each received NPC by ref-FormID (or base-FormID fallback for dynamic refs). Currently **skips NPCs in proc cells** - a leftover from the pre-T22 workaround. Marked for removal.

Applied state:
- `Actor::SetPosition(pos, true)` - proper physics-settle
- Angle via `data.angle`
- Health via `SetActorValue`
- `DrawWeaponMagicHands` for weapon state
- `KillActor` for dead flag

Remote-owned actors are added to `g_remoteOwned` - `HookedActorUpdate` suppresses the engine's AI tick for them (TiltedEvolution pattern: if AI runs on top of network state, they fight each other → jitter).

### 9.3 Not yet implemented (Phase B/C)

Planet POI enemies are excluded from current NPCSync because:
- They have 0xFF FormIDs → filtered out by host capture
- Their base TESNPC is also dynamic → can't be resolved on client via `LookupByID`

Design for fixing this: `docs/NPCSync_Phase_B_Design.md`. Summary: snapshot-based sync with `hostRefFormID → localRefFormID` remap table, based on T27 RE findings.

---

## 10. WorldSync (Save Transfer)

**Problem:** host and client need to share the same world state (quest stage, time, relations, proc-cell contents). Easiest solution: transfer the host's entire .sfs save file at connect.

### 10.1 Flow

```
CLIENT                              HOST
  |                                   |
  |--- PKT_HANDSHAKE ('KloBrille') ->|
  |                                   | Liest neuestes Save via SaveScanner
  |<-- PKT_SAVE_TRANSFER (raw .sfs) -|  reliable, 1-2 MB, ENet fragments
  |                                   |
  | Schreibt PULSAR_Session.sfs       |
  | QueueLoadGame(entry)              |
  | → engine load-screen              |
  |                                   | Timeout extended to 120s
  | ... Welt geladen ...              |
  |                                   |
  |--- PKT_CLIENT_PROFILE (inv) ----> | Inventory-Override auf Klon
  |--- PKT_POSITION (per frame) ----> | Normal sync resumes
```

### 10.2 State machine

`WorldSync::State` = `IDLE | WAITING_FOR_SAVE | SAVE_RECEIVED | LOADING | LOADED`.

Client side: position sending is paused while state != LOADED. ENet timeout extended to 120 s so we don't drop connection during the load screen.

### 10.3 Save-load triggers

`BGSSaveLoadManager::queuedTasks + 0x50`:
- bit `0x8` = quicksave
- bit `0x4` = load by filename (reads path from `mgr+0x70`) - confirmed via Ghidra `FUN_141852ab0`

Quicksave bit silent-fails for our use case (loads from save slot, not from a file path we control); we use the filename variant.

### 10.4 Client profile

After world is loaded, client sends `PKT_CLIENT_PROFILE`:
```cpp
struct ClientProfileData {
    uint32_t head_npc_formID;
    uint32_t weapon_formID;
    uint32_t armor_formIDs[16];
    uint8_t  armor_count;
    ...
};
```

Host applies to the clone-of-client so it looks like the actual client player. Inventory-restore is currently disabled (items were getting transferred to the clone; we're not using the inventory yet).

---

## 11. Proc-Cell Handling (the big RE effort)

The biggest distinction between static and procedural cells in Starfield:

| Aspect | Static cell | Procedural cell |
|---|---|---|
| Formation | Stored in plugin files | Generated at runtime from `BGSPlanet::ProcGen` + content trees |
| Lifetime | Stable for session | Torn down when player leaves range |
| Cell FormID | Static (from plugin) | 0xFF-prefix (dynamic) |
| Spawned NPCs | Static FormIDs | 0xFF-prefix (including base TESNPC) |
| parentCell pointer on refs | Stable | **Dangling after teardown** - this was our main crash source |

### 11.1 T22 diagnosis - it was never a thread race

Early hypothesis: "proc-gen worker threads race Actor::SetPosition on the cell-quadrant hashmap." T22's Ghidra RE disproved this:
- Proc-gen runs on the main thread (per-frame ticker from `0x1485fa168` dispatch table)
- Proc-gen takes a different lock (`DAT_14590b198`) than hashmap writers (`cell+0xFC`)
- Proc-gen doesn't touch `cell+0x110`/`cell+0x120` (the hashmap)

The real cause: proc-gen destroys the `TESObjectCELL` object when unloading a chunk. Our clone keeps its stale `refr->parentCell` pointer. Stage-1 of `Actor::SetPosition` dereferences `parentCell` BEFORE acquiring the lock → AV on freed memory.

### 11.2 The fix stack

1. **parentCell-pin** - every `DoSetPosition` frame: `clone->parentCell = player->parentCell`. Player is streaming anchor → their parentCell is always live.
2. **Split-path in proc cells** - T22 scenario (c) remains: a fresh proc cell may have zero-filled bucket arrays. Bypass Stage-1 by writing `data.location` directly + calling `CharController::setPosition` directly (vtable slot 72).
3. **30 Hz Havok rate-limit in split-path** - 10 Hz caused ball-glitch (IK uses stale ground position). 60 Hz caused a crash when combined with `iSyncGunActions` sync. 30 Hz is the Goldilocks zone.
4. **Static cells use `Actor::SetPosition` normally** - parentCell is live, full Stage-1 is safe and gives proper hashmap registration + Havok broadphase wake-up + NiAVObject mirroring.

### 11.3 NPCSync proc-cell status

Currently NPCSync skips proc cells client-side. With the parentCell-pin pattern applicable to NPCs too, this is no longer required - Phase A removal is the next step in NPC sync.

---

## 12. Debug & Logging

**Logger:** `NASLog.h` - REX-style structured logger, tagged `[PULSAR]`. Host log at `%USERPROFILE%/Documents/My Games/Starfield/SFSE/Logs/PULSAR.log`. Client log deployed to `<NAS>/logs/PULSAR.log`.

**Rate-limited periodic logs** (every 2-5 s):
- `Sync: pos=...` - incoming position
- `PITCH: p=... yaw=...` - pitch input
- `SARAH: Spd=... Dir=...` - nearby NPC anim-var probe (debug only)
- `PLAYER: as1=... as2=...` - actorState flags
- `NPCSync: tracked=N captures/3s=M` - NPC tracker stats
- `PKT stats: sent=... recv=... recvPos=...` - network traffic

**One-shot logs:**
- `Equipped weapon 0x...` - weapon change
- `Forced jump: JumpUp/Fall/Down (state=N prev=M)` - jump transitions
- `Forced DrawWeaponMagicHands(bool)` - weapon toggle
- `GraphKickstart: draw fired / holster fired` - init cycle
- `ALLOWED action / BLOCKED action` - PerformAction decisions (logged once per form-ID)
- `RE_NPC ref=... base=...` - NPC tracker for RE (debugging planet NPCs)

**Crash recovery:** Starfield crashes produce `.dmp` files at `<Starfield>/Starfield_DATE.dmp`. Analysis via `local RE notes (Python + `minidump` library) extracts crash RIP/RSP, stack walk, thread context.

---

## 13. Hook Points (reference)

| Name | RVA / REL::ID | Type | Purpose |
|---|---|---|---|
| `HookedPerformAction` | DAT_145FB7BF0 (fn ptr via MinHook) | trampoline | Blocks engine actions on clone; forwards fire to sync |
| `HookedEvaluatePackage` | `REL::ID(150640)` | trampoline | AI-block on clone in static cells, pass-through in proc cells |
| `HookedActorUpdate` | primary Actor vtable `[0]` slot (via RTTI - NOT `VTABLE_Actor[0]`, that's secondary; use `VTABLE_Actor[1]` = `REL::ID 451614`) | vtable-patch | NPC state capture / remote-owned AI suppression |
| `HookedPlayerCtrl` | REL::ID 82252 | trampoline | Read player camera pitch into `g_playerPitch` |
| Clone CC creation | REL::ID 101052 (FUN_14193bed0) | call | Create CharController on newly-spawned clone |
| `PlaceAtMe` | RVA 0x207CFB0 | reference | 5-param Papyrus spawner |
| `SetPosition` | vtable[0x137] on Actor | called directly | Via `SafeSetActorPosition` |
| CharController setPosition | vtable[72] (byte 0x240) | called directly | Via `SafeHavokBodySetPosition` |
| CharController setLinearVelocity | vtable[79] (byte 0x278) | (unused currently) | For velocity-based clone motion |
| `ActorEquipManager::EquipObject` | REL::ID 101949 | called directly | Equipment apply |
| `IsSneaking` | REL::ID 100547 | called directly | Real sneak detection |
| `BGSSaveLoadManager::QueueLoadGame` | via bit 0x4 at queuedTasks+0x50 | flag-set | Save load trigger |

---

## 14. RE Findings (T1-T27 summary)

| Task | Target | Key answer |
|---|---|---|
| T1 | Actor::SetPosition internals | 4-stage function: cell-hashmap update, event fire, Havok body update, NiAVObject mirror. Stage-1 is the UAF site. |
| T2 | TESObjectREFR::SetParentCell | Exists at vtable[0x10C], but direct assignment of parentCell is enough for our use - we don't need proper SetParentCell semantics. |
| T3 | Procedural content manager | Main-thread ticker; content tree at BGSPlanetContentManagerTree. |
| T4 | NiAVObject transform pipeline | Translation at +0x70 in NiAVObject. |
| T5 | ObjectReferenceManager singleton | REFR factory; not currently used directly. |
| T6 | bhkWorld lock | **Does not exist in shipping Starfield.** markForWrite/unmarkForWrite strings stripped. Engine relies on implicit thread-phase contract. |
| T7-T9 | Havok body chain on Actor | Actor+0x228 (aiProc) → +0x08 (middleHigh) → +0x458 (CC) → CC+0x20 body wrapper → +0x20 inner hknpBody. Double-null gate required. |
| T10 | Physics-state-gated animations | Anim graph checks body velocity.z for JumpUp/Apex/JumpFall - confirms why we need Havok writes for jumps. |
| T11 | Graph-var setter functions | Functions resolved for setInt/setFloat via vtable[0x20]/[0x22] - used for iSync* writes. |
| T12 | Graph variable names | 18 iSync\* vars identified, 4 Player-graph-only (silent no-op on NPC). |
| T13 | Constant-var decoding | iSyncRelaxReadySneak values: 0=relax, 1=ready, 2=sneak, 3=alert. |
| T14 | MP-scaffold | Starfield has **iSync\* variable protocol** (real). "Network module" (0x1435-0x1436) is HTTP/Telemetry/Bethesda.net - NOT game-MP. No aktivierbares MP-scaffold. |
| T15 | Network-module analysis | HTTP/libcurl/WinHttp/Xbox Live services. Dead end for us. |
| T16 | NPC graph vars | iSync\* honored on NPC graphs; IsPlayer/IsNPC not registered on NPC graph. |
| T17 | hknpWorld vtable | markForWrite/unmarkForWrite absent. 0 hits on lock-related strings. |
| T18 | HavokCommandQueue | Worker-task wrapper, not a generic "enqueue write" API. |
| T19 | Internal body writers | All 51 engine setPosition callers run without locks - main-thread phase contract. |
| T20 | Cell-hashmap writers | 6 writers besides SetPosition Stage-1. All take `cell+0xFC` lock internally. Caller prologue reads `refr+0x98` unguarded. |
| T22 | Proc-gen vs hashmap | **Proc-gen does NOT race the hashmap.** UAF on `refr->parentCell` is the real bug. |
| T24 | Hashmap readers | All readers take the lock. Stale entries in freed cells are safe. |
| T27 | NPC spawn chain | Dynamic FormID allocator at `FUN_14036BF70` - process-local, non-deterministic. Sync must be snapshot-based. |

Full findings in `local RE notes*_findings.md`.

---

## 15. Known Limitations (as of 2026-04-20)

1. **No Phase B/C NPC sync on planets** - planet enemies visible on each machine separately, not shared. Design ready; implementation pending.
2. **Save-load crash on host** (rare) - reloading a different save while client is connected sometimes crashes host. Single occurrence so far; not root-caused.
3. **0.5 m jump height on planet** - cross-machine Havok rate is 30 Hz; between writes gravity pulls the body down. Bumping to 60 Hz caused a combined failure with iSyncGunActions sync. 30 Hz is the stable sweet spot; full height is a polish task for later.
4. **iSyncGunActions / iSyncGunDown not synced** - setting them on the clone triggers the crash path the fire-forward originally hit. Hardcoded to 0. Shooting animation still works because `iSyncRelaxReadySneak` + PerformAction synthesis drive the same behavior.
5. **ImGui overlay can't run in-game** - D3D12 swapchain hook crashes (see `feedback_imgui_crash.md`). UI uses an external launcher pre-game.
6. **Quicksave transfer via bit 0x8 silent-fails** - using menu-save path (bit 0x4) with filename.
7. **Clone takes no damage / not hostile target** - design decision, not a bug.
8. **Fall damage on clone during jumps (historical)** - fixed by one-shot impulse at state transitions instead of continuous velocity writes.
9. **bhkCharRigidBodyController vtable** - we use `0x144D6B430` (primary), NOT `0x144D693F0` (abstract base). Wrong vtable = null calls.

---

## 16. Build & Deploy

### 16.1 Build

Cmake + vcpkg + CommonLibSF submodule. Build directory: `<repo>/build/vs`.

```bash
cmake --build <repo>/build/vs --config Release --target PULSAR
```

Output: `<repo>/build/vs/Release/PULSAR.dll`.

### 16.2 Deploy - ALWAYS both targets

```bash
cp PULSAR.dll "<Starfield>/Data/SFSE/Plugins/PULSAR.dll"                    # HOST
cp PULSAR.dll "<NAS>/deploy/Data/SFSE/Plugins/PULSAR.dll"     # CLIENT (NAS share)
```

Client reads from the NAS share when launching Starfield.

### 16.3 Logs to check after testing

```
%USERPROFILE%/Documents/My Games/Starfield/SFSE/Logs/PULSAR.log   # host
<NAS>/logs/PULSAR.log                      # client
```

Backups available at `<repo>/logs_backup/` when specifically saved.

---

## 17. Development Workflow Notes

- **Never git push** - local commits only.
- **Test on real hosts every change** - cross-machine co-op can't be unit-tested.
- **SEH everywhere** - raw pointer dereferences on engine data are wrapped in `__try/__except`. Separates plain-C wrappers from C++ functions (C2712 compiler error otherwise).
- **ProcessPending = 60 Hz on SFSE worker pool** - tasks dispatch across 6+ threads in practice. All part of the engine task graph → Havok-safe.
- **Ghidra project at `the local Ghidra project`** - RE scripts in `local RE notes*.java`, run via `analyzeHeadless.bat` pattern in `run_t2.bat`.

---

## 18. Project Status (2026-04-20)

**Stable:**
- Co-op in cities / static cells
- Walking, running, sneaking, jumping on planets
- Weapon draw/holster, equipment sync
- Shooting on planets (bullets, sound, impacts)
- Save transfer at connect
- Sarah Morgan follow-sync in static cells

**In design / next up:**
- Planet NPC sync (Phase A: static NPCs on planet; Phase B: scripted POI)
- Full-height jumps on planet (physics fight - needs iSyncGravity experiment or velocity-based approach)

**Parked:**
- Phase C planet NPC sync (random mobs) - needs T28 RE for template-source-lookup
- 3D drops & loot sync - disabled due to dupe bugs
- Quicksave transfer - silent-fail, using menu-save path instead
- In-game ImGui overlay - D3D12 hook unstable
