#include "PCH.h"
#include "NPCSync/NPCSync.h"
#include "HookGuard.h"
#include "NASLog.h"

#include <RE/A/Actor.h>
#include <RE/A/ActorValueInfo.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiUpdateData.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESGlobal.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESForm.h>
#include <RE/B/BSFixedString.h>
#include <Windows.h>

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/LocomotionApply.h"
#include "NPCSync/PluginLookup.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "WorldSync/LootSync.h"   // corpse-loot sync: ReadItemMods/ApplyItemMods + replay guard
#include "WorldSync/WorldSync.h"

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <cstring>

using namespace std::string_view_literals;

namespace Pulsar::NPCSync {

    extern bool IsHostMode();  // defined below

    // ===================================================================
    // Combat Sync V1 - DISCOVERY PROBE (2026-05-25). Read-only, no behavior
    // change. Logs per-NPC Health + IsDead on change, on BOTH host and client,
    // keyed by FormID. Goal: in a ground fight, compare HOST vs CLIENT logs for
    // the SAME FormID. Key question: when the CLIENT shoots an NPC, does the
    // HOST's authoritative NPC lose health (= hits already count) or not (= the
    // gap Combat Sync V2 must close)? Also reveals whether the client's puppet
    // health is network-mirrored or locally simulated, and where death fires.
    // ===================================================================
    static RE::ActorValueInfo* CombatProbe_HealthAV()
    {
        static RE::ActorValueInfo* s_av =
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("Health"));
        return s_av;
    }

    // SEH-isolated read - no C++ destructibles inside __try.
    static bool CombatProbe_SafeRead(RE::Actor* a_actor, RE::ActorValueInfo* a_av,
                                     float& a_cur, float& a_max, bool& a_dead)
    {
        __try {
            a_cur  = a_actor->GetActorValue(*a_av);
            a_max  = a_actor->GetPermanentActorValue(*a_av);
            a_dead = a_actor->IsDead();
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static std::mutex                                                g_combatProbeMu;
    static std::unordered_map<std::uint32_t, std::pair<float, bool>> g_combatProbeLast;

    static void CombatProbeObserve(RE::Actor* a_actor)
    {
        if (!a_actor) return;
        const std::uint32_t fid = a_actor->formID;
        if (fid == 0x14) return;  // skip local player

        auto* av = CombatProbe_HealthAV();
        if (!av) return;

        float cur = 0.0f, max = 0.0f;
        bool  dead = false;
        if (!CombatProbe_SafeRead(a_actor, av, cur, max, dead)) return;
        if (max <= 0.0f) return;  // no health pool → not a combat actor

        bool  doLog   = false;
        float prevCur = cur;
        {
            std::lock_guard lk(g_combatProbeMu);
            auto it = g_combatProbeLast.find(fid);
            if (it == g_combatProbeLast.end()) {
                g_combatProbeLast.emplace(fid, std::make_pair(cur, dead));
                doLog = true;  // first sight
            } else {
                prevCur = it->second.first;
                const float d = (cur > prevCur) ? (cur - prevCur) : (prevCur - cur);
                if (d > 0.5f || dead != it->second.second) {
                    it->second = std::make_pair(cur, dead);
                    doLog = true;
                }
            }
        }
        if (doLog) {
            PLOG_INFO(
                "[PULSAR] [COMBAT-PROBE] {} NPC 0x{:08X} health={:.0f}/{:.0f} "
                "dead={} (delta={:+.0f})"sv,
                IsHostMode() ? "HOST" : "CLIENT",
                fid, cur, max, dead ? 1 : 0, cur - prevCur);
        }
    }

    // SEH-wrapped SetPosition+angle write for NPCs. __try can't live in a
    // function that has C++ objects requiring unwinding (std::vector etc).
    // Used in static cells where Actor::SetPosition is safe.
    static bool SafeNpcSetPosition(RE::Actor* actor, const RE::NiPoint3A& target, float rotZ)
    {
        __try {
            actor->SetPosition(target, true);
            actor->data.angle.z = rotZ;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Phase D variant: SetPosition with updateCharController=FALSE.
    // Runs Stage 1 (cell-refmap update) + Stage 2 (event fire) +
    // Stage 4 (NiAVObject.local.translate). SKIPS Stage 3 Havok
    // body teleport. Goal: move the mesh via Stage 4 without breaking
    // rendering via Havok broadcast.
    static bool SafeNpcSetPositionNoCC(RE::Actor* actor, const RE::NiPoint3A& target, float rotZ)
    {
        __try {
            actor->SetPosition(target, false);
            actor->data.angle.z = rotZ;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-wrapped cell-prefix check. Same rationale as the clone's
    // IsActorInProcCell in RemotePlayerManager.cpp: proc-gen reshuffles
    // parentCell pointers, an unlocked deref races that. Returns 1 if proc, 0 else.
    static int IsNpcInProcCell(RE::Actor* a_actor)
    {
        __try {
            auto* cell = a_actor ? a_actor->parentCell : nullptr;
            if (!cell) return 0;
            return ((cell->formID & 0xFF000000) == 0xFF000000) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Pin NPC->parentCell = player->parentCell. Same UAF fix as clone (T22):
    // when proc-gen tears down a POI chunk, the old CELL* is freed but the NPC
    // still holds a stale pointer; stage-1 of Actor::SetPosition derefs it.
    // The player is the streaming anchor - its parentCell is live every frame.
    // Plain-C function because __try can't coexist with C++ unwinding (C2712).
    static int SafePinNpcParentCell(RE::Actor* actor, RE::TESObjectCELL* tgtCell)
    {
        __try {
            if (!actor || !tgtCell) return 0;
            if (actor->parentCell != tgtCell) actor->parentCell = tgtCell;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Layer-2: raw data.location write at actor+0x8C. Used in proc cells
    // where Actor::SetPosition would touch the racing cell-refmap.
    static bool SafeNpcWriteDataLocation(RE::Actor* actor, const RE::NiPoint3A& pos, float angleZ)
    {
        __try {
            auto* locPtr = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(actor) + 0x8C);
            locPtr[0] = pos.x;
            locPtr[1] = pos.y;
            locPtr[2] = pos.z;
            actor->data.angle.z = angleZ;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Layer-3 inner: NiAVObject local.translate write + UpdateTransforms.
    // Raw buffer for NiUpdateData avoids C++ unwinding inside __try (C2712).
    static void UnsafeNpcWriteNodeTranslate(RE::NiAVObject* node, const RE::NiPoint3A& pos)
    {
        __try {
            auto* t = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(node) + 0x70);
            t[0] = pos.x;
            t[1] = pos.y;
            t[2] = pos.z;

            alignas(8) uint8_t updateBuf[0x60]{};
            *reinterpret_cast<uint32_t*>(updateBuf + 0x38) = 0x10;
            *reinterpret_cast<uint32_t*>(updateBuf + 0x3C) = 1;
            *reinterpret_cast<uint32_t*>(updateBuf + 0x40) = 1;
            node->UpdateTransforms(reinterpret_cast<RE::NiUpdateData*>(updateBuf));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Layer-3 outer: acquire loadedData read-lock, then call inner writer.
    // No __try here - BSAutoReadLock needs C++ unwinding.
    static void SafeNpcWriteNode(RE::Actor* actor, const RE::NiPoint3A& pos)
    {
        auto guard = actor->loadedData.LockRead();
        const auto* data = *guard;
        if (!data || !data->data3D) return;
        UnsafeNpcWriteNodeTranslate(data->data3D.get(), pos);
    }

    // Plain-C SEH helper - reads NiAVObject.world.translate (offset +0xB0).
    // Caller holds any necessary locks on the data3D pointer; this function
    // just does the raw 12-byte read under SEH in case node has been freed.
    static void SafeReadNodeWorld(RE::NiAVObject* node, float out[3])
    {
        __try {
            auto* wt = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(node) + 0xB0);
            out[0] = wt[0]; out[1] = wt[1]; out[2] = wt[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = 0; out[1] = 0; out[2] = 0;
        }
    }

    // Force-write NiAVObject.world.translate (offset +0xB0). Needed when
    // engine's UpdateWorldData delegates to collisionObject (T4 Section 3:
    // flags2 bit 11 clear + collisionObject non-null path) and skips the
    // normal `world = parent.world * local` recompute. SYNC-DIAG (Phase D
    // v7) confirmed this happens on dynamic-ref proc-gen NPCs: data.location
    // tracks host target but mesh.world stays stuck at spawn position.
    // Writing world directly on each apply tick keeps the renderer aligned
    // with host - any engine rewrite via Update-delegation will be
    // overwritten by our next 30 Hz tick.
    static void SafeWriteNodeWorld(RE::NiAVObject* node, const RE::NiPoint3A& pos)
    {
        __try {
            auto* wt = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(node) + 0xB0);
            wt[0] = pos.x; wt[1] = pos.y; wt[2] = pos.z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    static void ForceMeshWorldTranslate(RE::Actor* actor, const RE::NiPoint3A& pos)
    {
        auto guard = actor->loadedData.LockRead();
        const auto* data = *guard;
        if (!data || !data->data3D) return;
        SafeWriteNodeWorld(data->data3D.get(), pos);
    }

    // Trigger recursive downward-pass (NiNode::Update slot 73) with
    // `flags2` bit 11 SET so the collision-delegation branch is skipped
    // (T4 Section 3). That propagates `world = parent.world * local`
    // through every skeleton bone - fixing the case where root.world
    // is at target but child bones' world stays at spawn.
    //
    // Raw NiUpdateData buffer layout:
    //   +0x38 = flags  (0x10 = default CommonLibSF-style)
    //   +0x3C = flags2 (0x800 = skip collision delegation, bit 11 set)
    //   +0x40 = flags3 (stamp, any non-zero value)
    static void SafeForceDownwardPass(RE::NiAVObject* node)
    {
        __try {
            alignas(8) uint8_t buf[0x60]{};
            *reinterpret_cast<uint32_t*>(buf + 0x38) = 0x10;
            *reinterpret_cast<uint32_t*>(buf + 0x3C) = 0x800;  // bit 11 = skip collision delegation
            *reinterpret_cast<uint32_t*>(buf + 0x40) = 1;
            auto* vt = *reinterpret_cast<uintptr_t**>(node);
            using UpdateFn = void(*)(RE::NiAVObject*, void*);
            reinterpret_cast<UpdateFn>(vt[73])(node, buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    static void ForceSkeletonWorldPropagate(RE::Actor* actor)
    {
        auto guard = actor->loadedData.LockRead();
        const auto* data = *guard;
        if (!data || !data->data3D) return;
        SafeForceDownwardPass(data->data3D.get());
    }

    // Proc-cell data-only write. We USED to also call SafeNpcWriteNode which
    // writes NiAVObject.local.translate - but that's WORLD coords written into
    // the node's LOCAL space. If the parent node's transform isn't identity
    // (common in proc cells), the renderer multiplies:
    //     world_draw = parent_matrix * (ourWorldX, ourWorldY, ourWorldZ)
    // which puts the mesh at some random off-screen position. Result: NPC has
    // has3D=true + target registered + HUD shows LVL/HP, but the mesh is
    // rendered nowhere the user can see → "ghost NPCs".
    //
    // Fix: only write data.location (actor+0x8C). The engine's per-frame
    // transform rebuild reads data.location as world-space and builds the
    // correct NiAVObject transforms from it.
    static bool SafeNpcProcCellWrite(RE::Actor* actor, const RE::NiPoint3A& pos, float angleZ)
    {
        return SafeNpcWriteDataLocation(actor, pos, angleZ);
    }

    // Phase E Task 3: SEH-wrapped graph-variable readers for NPCs.
    // Actor inherits from IAnimationGraphManagerHolder - the same virtual
    // getters used by Main.cpp::FillAnimationData for the Player. NPCs can
    // be in partial-spawn state (animationGraphManager null, graph vtable
    // torn mid-reinit), so wrap each read in __try. Plain-C helpers because
    // C2712 forbids __try in functions with C++ destructor-bearing locals,
    // and HookedActorUpdate holds std::lock_guard / std::vector on its stack.
    //
    // BSFixedString is a POD ref-counted pointer, but its ctor/dtor touch a
    // shared atomic - safe to construct on the caller side and hand in by
    // pointer. We accept a pointer to avoid any C++ ctor inside the __try.
    static int SafeNpcGetGraphFloat(RE::Actor* actor, const RE::BSFixedString* varName, float* outVal)
    {
        __try {
            float tmp = 0.0f;
            bool ok = actor->GetGraphVariableImplFloat(*varName, tmp);
            *outVal = tmp;
            return ok ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *outVal = 0.0f;
            return -1;
        }
    }

    static int SafeNpcGetGraphInt(RE::Actor* actor, const RE::BSFixedString* varName, int32_t* outVal)
    {
        __try {
            int32_t tmp = 0;
            bool ok = actor->GetGraphVariableImplInt(*varName, tmp);
            *outVal = tmp;
            return ok ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *outVal = 0;
            return -1;
        }
    }

    // =====================================================================
    // Papyrus-trampoline NPC spawn (T33 design)
    // =====================================================================
    // T30 showed FUN_14179A510 can't accept TESNPC* source (reads +0x98
    // unconditionally, gets garbage, crashes). T33 found FUN_14207D3C0
    // (PlaceActorAtMe) - the proper path - but it's Papyrus-VM-framed, not a
    // clean C++ entry point.
    //
    // Solution: drive Papyrus's PlaceAtMe from C++ via GLOB signalling, same
    // pattern the clone spawn and item drops already use.
    //
    // Protocol (reusing PulsarDrop GLOBs which were reserved but never used):
    //   C++ writes baseFormID to PulsarDropHi/Lo.
    //   C++ sets PulsarSpawn = 10.0 (NPC spawn request).
    //   Papyrus on spawnVal==10.0 calls player.PlaceAtMe(base, 1, false, false, false),
    //     writes resulting REFR's FormID back to PulsarDropHi/Lo,
    //     sets PulsarSpawn = 11.0 (done).
    //   C++ polls PulsarSpawn in the apply loop; on 11.0 reads the result
    //     FormID, adds to g_npcRemap, resets PulsarSpawn = 2.0 and DropHi/Lo = 0.
    //   Only ONE spawn in flight at a time (serialized via g_spawnInFlight).

    // Remap: host's refFormID → client's local spawned refFormID.
    static std::unordered_map<uint32_t, uint32_t> g_npcRemap;
    static std::mutex g_npcRemapMutex;

    // Once we initiate a spawn for a hostRefID (even if it later fails),
    // never retry. Prevents leak spam and state-machine deadlocks.
    static std::unordered_set<uint32_t> g_spawnAttempted;
    static std::mutex g_spawnAttemptedMutex;

    // Single-slot state machine for the Papyrus trampoline. 0 = idle,
    // otherwise = hostRefFormID of the in-flight spawn request.
    static std::atomic<uint32_t> g_spawnInFlightHostRef{ 0 };
    static std::atomic<uint64_t> g_spawnInFlightDeadline{ 0 };  // GetTickCount64() abort time

    // GLOB helpers - LookupByEditorID returns nullptr in weird save states;
    // callers check.
    static RE::TESGlobal* ResolveGlob(const char* name)
    {
        return RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString(name));
    }

    static void WriteGlob(const char* name, float v)
    {
        if (auto* g = ResolveGlob(name)) g->value = v;
    }

    static float ReadGlob(const char* name)
    {
        if (auto* g = ResolveGlob(name)) return g->value;
        return 0.0f;
    }

    // Actor::Update signature - verified in Ghidra (see D:\PlusarCellsync\PULSAR_NPC_UPDATE_RE.md):
    //   VTABLE_Actor[0] (REL::ID 451610) is a SECONDARY sub-object vtable (COL offset=16, for
    //   Actor-as-TESObjectREFR subobject). Only companions reach slot 0x13F there.
    //   VTABLE_Actor[1] (REL::ID 451614) is the PRIMARY Actor vtable (COL offset=0).
    //   Slot 0x13F on the primary is a 2-insn thunk to vftable[0x197] - the real Actor::Update.
    //   The main per-frame NPC loop (FUN_141A86730) dispatches through this primary slot.
    using ActorUpdate_t = void(*)(RE::Actor*, float);
    static ActorUpdate_t g_origActorUpdate = nullptr;

    // Host-side NPC tracking. Key = refFormID, updated from Actor::Update hook,
    // drained by OnGameThreadTick() send loop (Task 4).
    struct TrackedNPC {
        uint32_t baseFormID = 0;       // TESNPC template - may be 0xFF-prefix (dynamic, per-process clone of a leveled list)
        uint32_t sourceBaseFormID = 0; // Static source template from baseTemplateForm (TESNPC+0x130, T28). 0 if base is already static. Used by client for cross-machine spawn.
        uint32_t sourceLocalID    = 0; // Flag-aware plugin-local portion of the static source (T34). Only valid when sourcePlugin[0] is set.
        char     sourcePlugin[Pulsar::Network::NPC_SOURCE_PLUGIN_MAX] = {}; // Resolved once from host's TESDataHandler. Stable string across the session.
        float    posX = 0, posY = 0, posZ = 0;
        float    rotZ = 0;
        float    speed = 0.0f;
        float    direction = 0.0f;
        uint8_t  moveFlags = 0;
        float    health = 0, healthMax = 0;
        bool     isDead = false;
        bool     weaponDrawn = false;
        uint64_t lastSentTick = 0;   // GetTickCount64() of last send
        uint64_t lastCapturedTick = 0; // GetTickCount64() of last HookedActorUpdate capture - used for staleness prune (V4)
        bool     everSent = false;
    };
    static std::unordered_map<uint32_t, TrackedNPC> g_tracked;
    static std::mutex g_trackedMutex;

    // Combat Sync V2 Step 1 - host-side queue of client-forwarded damage.
    // Filled on the ENet thread by OnDamageReceived; drained + applied on the
    // game thread in OnGameThreadTick (SetActorValue must run on game thread).
    static std::vector<Pulsar::Network::NpcDamageData> g_pendingDamage;
    static std::mutex g_pendingDamageMutex;

    // Client-side AI suppression set - declared here so HookedActorUpdate can see it.
    // See detailed comment at full definition site further down.
    static std::unordered_set<uint32_t> g_remoteOwned;
    static std::mutex g_remoteOwnedMutex;

    // P1: tracked NPCs whose AI we block (EvaluatePackage + PerformAction)
    // so DirectControl can drive them as puppets. SEPARATE from g_remoteOwned so
    // it ONLY feeds the AI-block hooks via IsRemoteOwned - not the capture/update
    // logic that also reads g_remoteOwned. Populated only when npcDirectControl=1.
    static std::unordered_set<uint32_t> g_npcDcBlocked;
    static std::mutex g_npcDcBlockedMutex;

    // P3 "combat-live": NPCs whose engine combat AI we LET RUN (so they aim +
    // fire visibly + damage the local player) instead of puppeting them. Gated by
    // PULSAR.ini npcCombatAiLive=1; populated per-frame for weaponDrawn remote NPCs.
    // The EvaluatePackage + PerformAction hooks override their AI-block for these.
    static std::unordered_set<uint32_t> g_npcCombatLive;
    static std::mutex                    g_npcCombatLiveMutex;
    static std::atomic<bool>             g_npcCombatAiLiveFlag{ false };

    // Apply-loop "first-time-seen" set. Separate from g_remoteOwned because we
    // add remoteOwned at spawn-DONE (to protect our own spawn from anti-dup).
    // `firstSync` semantics = "first time ApplyPending* runs its position write
    // for this actor" - needed for the force-SetPosition-on-first-sync path
    // that triggers the engine's 3D-stream. Using g_remoteOwned.insert().second
    // for firstSync meant firstSync was always false after we pre-inserted.
    static std::unordered_set<uint32_t> g_firstSynced;
    static std::mutex g_firstSyncedMutex;

    // Combat Sync V2 Step 1 - client-side damage forwarding state, keyed by
    // LOCAL matched actor FormID.
    //   baseline       = the Health value we've already accounted for. Local
    //                    combat only ever lowers Health, so a read below
    //                    baseline = new client-dealt damage to forward.
    //   clientEngaged  = set true the first time we see a local drop. Once set,
    //                    we STOP applying continuous host Health to this actor
    //                    (death only) so the host's lagged "still alive" value
    //                    can't heal a client-killed enemy and cause us to
    //                    re-forward the same damage. Self-classifies enemies
    //                    vs. companions without any host-side flag.
    //   killApplied    = guards the one-shot SetActorValue(0) on the isDead edge.
    struct DamageFwdState {
        float baseline       = -1.0f;   // -1 = uninitialised
        bool  clientEngaged  = false;
        bool  killApplied    = false;   // host→client: we already killed our local matched enemy on host's isDead
        bool  killForwarded  = false;   // client→host: we already forwarded a KILL because our local enemy died (V2-Step-1d)
    };
    static std::unordered_map<uint32_t, DamageFwdState> g_dmgFwd;
    static std::mutex g_dmgFwdMutex;

    // ===================================================================
    // Corpse-loot sync (2026-06-11) - each machine rolls NPC loot with its
    // OWN RNG, so corpse contents diverge between host and client (the
    // F.1.e take-sync then warns+skips on items the peer doesn't have).
    // Host = authority: ONE PKT_CORPSE_LOOT per corpse, sent ~1s after the
    // death edge (the engine appends death items at kill time - an
    // immediate capture would miss them). Client wipes its matched corpse
    // and applies the host list incl. mod arrays (epics render identically).
    // ===================================================================
    struct CorpseItemResolved {
        uint32_t formID = 0;          // receiver-local resolved FormID
        int32_t  count  = 0;
        uint16_t modSentinel = 0xFFFF;
        std::vector<uint32_t> mods;   // receiver-local resolved mod FormIDs
    };
    // Host: corpses whose loot broadcast is scheduled / already sent.
    static std::unordered_map<uint32_t, uint64_t> g_corpseLootDueMs;  // refID → send-at tick
    static std::unordered_set<uint32_t>           g_corpseLootSent;
    // Client: received loot waiting for the matched local corpse.
    static std::unordered_map<uint32_t, std::vector<CorpseItemResolved>> g_corpseLootPending;
    static std::mutex g_corpseLootMutex;

    // Plain-C SEH inventory walk: {formID, total count} per entry. Offsets
    // verified (L.7): invList @refr+0xA0, dataSize@+0x28, dataPtr@+0x30;
    // BGSInventoryItem stride 0x28 (object@0, stacks.size@+0x10, data@+0x18);
    // Stack stride 0x10 (extra@0, count@+0x08). TESForm.formID @ +0x28.
    struct CorpseInvEntry { uint32_t formID; int32_t count; };
    static int SafeCollectInventory(RE::TESObjectREFR* a_refr,
                                    CorpseInvEntry* a_out, int a_max)
    {
        __try {
            auto* pb = reinterpret_cast<uint8_t*>(a_refr);
            auto* invList = *reinterpret_cast<uint8_t**>(pb + 0xA0);
            if (!invList) return 0;
            uint32_t entryCount = *reinterpret_cast<uint32_t*>(invList + 0x28);
            auto* entries = *reinterpret_cast<uint8_t**>(invList + 0x30);
            if (!entries || entryCount == 0 || entryCount > 100000) return 0;
            int n = 0;
            for (uint32_t i = 0; i < entryCount && n < a_max; ++i) {
                uint8_t* entry = entries + i * 0x28;
                auto* obj = *reinterpret_cast<uint8_t**>(entry + 0x00);
                if (!obj) continue;
                uint32_t formID = *reinterpret_cast<uint32_t*>(obj + 0x28);
                uint32_t stackCount = *reinterpret_cast<uint32_t*>(entry + 0x10);
                auto* stacksData = *reinterpret_cast<uint8_t**>(entry + 0x18);
                if (!stacksData || stackCount == 0 || stackCount > 1000) continue;
                int32_t total = 0;
                for (uint32_t s = 0; s < stackCount; ++s) {
                    total += *reinterpret_cast<int32_t*>(stacksData + s * 0x10 + 0x08);
                }
                if (total <= 0 || !formID) continue;
                a_out[n].formID = formID;
                a_out[n].count  = total;
                ++n;
            }
            return n;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // Client: wipe the matched local corpse and apply the host's loot list.
    // Game thread, AFTER the corpse is dead (the engine appends its own death
    // items at kill - applying earlier would mix both rolls). Replay guard
    // keeps the M.2 inventory sinks from re-broadcasting our writes.
    static void ApplyCorpseLootIfPending(uint32_t a_hostRefID, RE::Actor* a_corpse)
    {
        std::vector<CorpseItemResolved> items;
        {
            std::lock_guard clk(g_corpseLootMutex);
            auto it = g_corpseLootPending.find(a_hostRefID);
            if (it == g_corpseLootPending.end()) return;
            items = std::move(it->second);
            g_corpseLootPending.erase(it);
        }
        const bool prev = Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);

        // Wipe the local roll - negative AddObjectToContainer is the proven
        // DirectRestore removal primitive.
        CorpseInvEntry cur[48]{};
        const int n = SafeCollectInventory(a_corpse, cur, 48);
        for (int i = 0; i < n; ++i) {
            auto* f = RE::TESForm::LookupByID(cur[i].formID);
            if (!f) continue;
            a_corpse->AddObjectToContainer(reinterpret_cast<RE::TESBoundObject*>(f),
                nullptr, -cur[i].count, nullptr, RE::ITEM_TRANSFER_REASON::kNone);
        }

        // Apply the host's authoritative list incl. mod arrays + sentinels.
        int applied = 0;
        for (const auto& e : items) {
            auto* f = RE::TESForm::LookupByID(e.formID);
            if (!f) continue;
            a_corpse->AddObjectToContainer(reinterpret_cast<RE::TESBoundObject*>(f),
                nullptr, e.count, nullptr, RE::ITEM_TRANSFER_REASON::kNone);
            if (!e.mods.empty() || e.modSentinel != 0xFFFF) {
                Pulsar::WorldSync::Loot::ApplyItemMods(
                    static_cast<RE::TESObjectREFR*>(a_corpse), e.formID,
                    e.mods, e.modSentinel);
            }
            ++applied;
        }
        Pulsar::WorldSync::Loot::SetReplayingFromPacket(prev);
        PLOG_INFO("[CORPSE-LOOT] applied {} host items to local corpse 0x{:08X} (wiped {} local entries)"sv,
                  applied, a_corpse->formID, n);
    }

    // V5 civilian snapshot host-side. LOCAL-tier (filtered) NPCs in the
    // 100 m sight radius get periodic position broadcasts so the client can
    // correct AI-divergence drift without the bandwidth cost of full sync.
    struct CivilianSnapshot {
        float    posX, posY, posZ;
        float    rotZ;
        uint64_t lastCapturedTick;  // GetTickCount64() - used for staleness prune + throttle
    };
    static std::unordered_map<uint32_t, CivilianSnapshot> g_civilianSnapshots;
    static std::mutex                                     g_civilianSnapshotsMutex;

    // Client-side queue for incoming civilian snapshots, drained on game thread.
    static std::vector<Pulsar::Network::CivilianSnapshotData> g_pendingCivilians;
    static std::mutex                                         g_pendingCiviliansMutex;

    static std::atomic<uint32_t> g_dbgCivSent{ 0 };
    static std::atomic<uint32_t> g_dbgCivApplied{ 0 };
    static std::atomic<uint32_t> g_dbgCivSkipDrift{ 0 };

    // Plain-C SEH read of pos+rotZ. Same C2712 pattern as elsewhere - caller
    // owns C++ destructor scope.
    static int SafeReadActorPosRot(RE::Actor* a,
                                   float* outX, float* outY, float* outZ,
                                   float* outRotZ)
    {
        __try {
            *outX    = a->data.location.x;
            *outY    = a->data.location.y;
            *outZ    = a->data.location.z;
            *outRotZ = a->data.angle.z;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Forward decl - defined alongside the tier classifier later in the file.
    static int SafeReadActorFormID(RE::Actor* actor, uint32_t* outFid);

    // Capture LOCAL-tier civilian within view radius for periodic broadcast.
    // Called from HookedActorUpdate's IsNPCWeSync-false branch (host-side only,
    // because client never reaches that branch's surrounding code).
    static void MaybeRecordCivilianSnapshot(RE::Actor* actor)
    {
        if (!actor) return;

        uint32_t refFormID = 0;
        if (!SafeReadActorFormID(actor, &refFormID)) return;
        if (refFormID == 0) return;
        // Static-ref only - dynamic 0xFF refs differ per-machine, can't
        // direct-LookupByID on client. Crowd ambient civilians (proc-spawned
        // from leveled lists) generally have static bases anyway, but their
        // REFR is dynamic; we skip those for V5.
        if ((refFormID & 0xFF000000) == 0xFF000000) return;

        const uint64_t now = GetTickCount64();
        // Per-actor throttle: HookedActorUpdate fires at 60 Hz × N actors.
        // 250 ms TTL is fresh enough for the 500 ms broadcast cadence.
        {
            std::lock_guard lock(g_civilianSnapshotsMutex);
            auto it = g_civilianSnapshots.find(refFormID);
            if (it != g_civilianSnapshots.end() &&
                now - it->second.lastCapturedTick < 250)
            {
                return;
            }
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        float x, y, z, rotZ;
        if (!SafeReadActorPosRot(actor, &x, &y, &z, &rotZ)) return;

        // Tighter than IsNPCWeSync's 400 m: civilians outside the 100 m
        // visible radius aren't worth resyncing - both sides will redrift
        // when player gets close again, snapshot kicks back in.
        constexpr float kRadiusSq = 100.0f * 100.0f;
        const float dx = x - player->data.location.x;
        const float dy = y - player->data.location.y;
        if (dx*dx + dy*dy > kRadiusSq) return;

        CivilianSnapshot s{ x, y, z, rotZ, now };
        {
            std::lock_guard lock(g_civilianSnapshotsMutex);
            g_civilianSnapshots[refFormID] = s;
        }
    }

    // Latch-set for "actor has has3D=true and stays that way". HookedActorUpdate
    // uses this to fully skip Update once 3D is in - without the latch we'd
    // skip forever starting from has3D=false, which means the engine's load
    // queue never gets pumped and 3D never loads → invisible NPCs.
    static std::unordered_set<uint32_t> g_has3DReady;
    static std::mutex g_has3DReadyMutex;

    // =====================================================================
    // T40 - PROC-GEN DETERMINISM CHECK
    // =====================================================================
    // Hypothesis: Host + Client have identical save-state (WorldSync loaded
    // host.sfs on client). If Starfield's proc-gen is deterministic on
    // {save-state + cellHash + worldSeed}, both machines natively spawn
    // the SAME enemies at the SAME positions.
    //
    // If true → we throw out PlaceAtMe-sync (Phase B/C) entirely, drop
    // the anti-dup, drop g_remoteOwned - and only sync delta state
    // (health, death, aggro) between already-identical native actors.
    // No more scene-graph attachment issues, no more dynamic-ref
    // mismatches, no more PlaceAtMe flood crash.
    //
    // Collection: every HookedActorUpdate call on a proc-cell actor in
    // player's current cell records {baseFormID, position}. Dumped every
    // 10s and on cell-change (one-shot per cell) in a single sorted log
    // line so host/client dumps are byte-comparable modulo per-machine
    // refFormID.
    struct ProcDumpEntry {
        uint32_t baseFormID;       // Direct base (TESNPC) FormID. Dynamic (0xFF) for proc-gen clones.
        uint32_t effectiveBaseID;  // Stable cross-machine ID:
                                   // If baseFormID is static → same as baseFormID
                                   // If baseFormID is dynamic → TESNPC+0x130 (T28 source template)
                                   // This is what host+client should agree on.
        float    pos[3];
    };
    static std::unordered_map<uint32_t, ProcDumpEntry> g_procDumpTable;
    static std::mutex g_procDumpMutex;
    static std::atomic<uint32_t> g_procDumpLastCell{ 0 };

    // Active-cells registry - every HookedActorUpdate tick inserts the
    // actor's parentCell pointer. Union = all cells the engine has loaded,
    // regardless of whether they're the local player's parentCell. VehicleSync
    // (and any future code needing cross-cell iteration) queries this via
    // GetActiveCellsSnapshot(). Replaces formArrays[kCELL] which is empty
    // at runtime on 1.16.236.
    static std::unordered_set<RE::TESObjectCELL*> g_activeCells;
    static std::mutex g_activeCellsMutex;

    std::vector<RE::TESObjectCELL*> GetActiveCellsSnapshot()
    {
        std::lock_guard lock(g_activeCellsMutex);
        return std::vector<RE::TESObjectCELL*>(g_activeCells.begin(), g_activeCells.end());
    }

    // Plain-C SEH wrapper - no smart-pointers, no std::lock_guard, no C++
    // unwinding allowed. Reads raw offsets per CommonLibSF layout:
    //   TESForm+0x28 = formID (NOT 0x20 - that's formFlags)
    //   TESObjectREFR+0xB0 = parentCell*
    //   TESObjectREFR+0x98 = objectReference (TESBoundObject* via NiPointer raw)
    //   TESObjectREFR+0x8C = data.location (3 floats)
    //   TESNPC+0x130 = baseTemplateForm (T28 RE - static source for 0xFF clones)
    static int SafeProcDumpGather(RE::Actor* a,
                                  uint32_t& refOut,
                                  uint32_t& cellIDOut,
                                  ProcDumpEntry& eOut)
    {
        __try {
            if (!a) return 0;
            auto* base = reinterpret_cast<uint8_t*>(a);
            auto* cell = *reinterpret_cast<uint8_t**>(base + 0xB0);
            if (!cell) return 0;
            cellIDOut = *reinterpret_cast<uint32_t*>(cell + 0x28);
            refOut = *reinterpret_cast<uint32_t*>(base + 0x28);
            auto* baseObj = *reinterpret_cast<uint8_t**>(base + 0x98);
            eOut.baseFormID = baseObj ? *reinterpret_cast<uint32_t*>(baseObj + 0x28) : 0;
            // Effective (cross-machine-stable) base:
            //   Dynamic base (0xFF-prefix) → follow +0x130 to static source template
            //   Static base                → same as direct base
            eOut.effectiveBaseID = eOut.baseFormID;
            if (baseObj && (eOut.baseFormID & 0xFF000000) == 0xFF000000) {
                auto* srcTpl = *reinterpret_cast<uint8_t**>(baseObj + 0x130);
                if (srcTpl) {
                    uint32_t srcID = *reinterpret_cast<uint32_t*>(srcTpl + 0x28);
                    if (srcID != 0) eOut.effectiveBaseID = srcID;
                }
            }
            auto* locPtr = reinterpret_cast<float*>(base + 0x8C);
            eOut.pos[0] = locPtr[0];
            eOut.pos[1] = locPtr[1];
            eOut.pos[2] = locPtr[2];
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Counter: how many times HookedActorUpdate saw any actor with
    // non-null parentCell, regardless of cell match. Helps debug why
    // PROCDUMP count=0 - if this counter is also 0 the hook isn't
    // firing, if it's high but PROCDUMP is 0 the filter is too strict.
    static std::atomic<uint64_t> g_procDumpHookSeen{ 0 };
    static std::atomic<uint64_t> g_procDumpProcCellSeen{ 0 };

    // Distance-only filter v3 - drops proc-cell requirement since v2
    // showed procCellActors=0 even with 17000+ hook calls/10s. Either
    // parentCell offset is wrong for some subclass of Actor, or the
    // engine routes proc-gen NPCs through a non-0xFF cell somehow.
    // Just collect everything within 400m of player, log their cells
    // separately so we can see what cells they're ACTUALLY in.
    struct ProcDumpEntryV3 : ProcDumpEntry {
        uint32_t cellFormID;
    };
    static std::unordered_map<uint32_t, ProcDumpEntryV3> g_procDumpTableV3;

    static void SafeProcDumpCollect(RE::Actor* a)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || a == player) return;

        uint32_t refFormID = 0, cellFormID = 0;
        ProcDumpEntry e{};
        if (!SafeProcDumpGather(a, refFormID, cellFormID, e)) return;
        g_procDumpHookSeen.fetch_add(1, std::memory_order_relaxed);

        if (cellFormID == 0) return;  // no cell assigned
        g_procDumpProcCellSeen.fetch_add(1, std::memory_order_relaxed);

        // Distance-based filter (engine active-ref loading radius ~400m).
        float pX = player->data.location.x;
        float pY = player->data.location.y;
        float pZ = player->data.location.z;
        float dx = e.pos[0] - pX, dy = e.pos[1] - pY, dz = e.pos[2] - pZ;
        float d2 = dx*dx + dy*dy + dz*dz;
        constexpr float kRadius = 400.0f;
        if (d2 > kRadius * kRadius) return;

        ProcDumpEntryV3 v3{};
        v3.baseFormID = e.baseFormID;
        v3.effectiveBaseID = e.effectiveBaseID;
        v3.pos[0] = e.pos[0]; v3.pos[1] = e.pos[1]; v3.pos[2] = e.pos[2];
        v3.cellFormID = cellFormID;

        std::lock_guard lk(g_procDumpMutex);
        g_procDumpTableV3[refFormID] = v3;
    }

    static void ProcDumpEmit(const char* role)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;
        uint32_t cellID = player->parentCell->formID;

        std::vector<std::pair<uint32_t, ProcDumpEntryV3>> entries;
        {
            std::lock_guard lk(g_procDumpMutex);
            entries.reserve(g_procDumpTableV3.size());
            for (auto& [ref, e] : g_procDumpTableV3) entries.emplace_back(ref, e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second.baseFormID != b.second.baseFormID)
                          return a.second.baseFormID < b.second.baseFormID;
                      return a.second.pos[0] < b.second.pos[0];
                  });

#ifndef PULSAR_HARDENED
        std::string list;
        list.reserve(entries.size() * 64);
        for (auto& [ref, e] : entries) {
            list += std::format("[b=0x{:08X} c=0x{:08X} p=({:.0f},{:.0f},{:.0f})] ",
                                e.baseFormID, e.cellFormID,
                                e.pos[0], e.pos[1], e.pos[2]);
        }
        uint64_t hooked = g_procDumpHookSeen.exchange(0);
        uint64_t withCell = g_procDumpProcCellSeen.exchange(0);
        PLOG_INFO("[PULSAR] PROCDUMP {} cell=0x{:08X} count={} hookCalls={} cellActors={} playerPos=({:.0f},{:.0f},{:.0f}) {}"sv,
                  role, cellID, entries.size(), hooked, withCell,
                  player->data.location.x, player->data.location.y, player->data.location.z,
                  list);
#endif
    }

    static void ProcDumpTick(bool isHost)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;
        uint32_t cellID = player->parentCell->formID;

        // Cell change → emit final dump of previous cell, clear table.
        uint32_t prev = g_procDumpLastCell.exchange(cellID);
        if (prev != 0 && prev != cellID) {
            ProcDumpEmit(isHost ? "HOST-final" : "CLIENT-final");
            std::lock_guard lk(g_procDumpMutex);
            g_procDumpTableV3.clear();
            return;
        }

        // Periodic emit every 10s while in the same cell.
        static uint64_t s_lastEmitH = 0;
        static uint64_t s_lastEmitC = 0;
        uint64_t& lastEmit = isHost ? s_lastEmitH : s_lastEmitC;
        uint64_t now = GetTickCount64();
        if (now - lastEmit > 10000) {
            lastEmit = now;
            ProcDumpEmit(isHost ? "HOST" : "CLIENT");
        }
    }

    // SEH-wrapped read of TESNPC's baseTemplateForm pointer at offset 0x130.
    // T28 Ghidra RE verified this offset for Starfield 1.16.236 (the clone
    // factory FUN_140CDBB20 writes the source-template pointer there).
    // When a TESNPC has a dynamic (0xFF-prefix) FormID, it's a per-process
    // clone from a leveled-list resolve; the real cross-machine-stable
    // identity lives at +0x130 → TESNPC/LVLN from an ESM.
    static uint32_t SafeReadBaseTemplateFormID(RE::TESNPC* npc)
    {
        __try {
            auto* tpl = *reinterpret_cast<RE::TESForm**>(
                reinterpret_cast<uintptr_t>(npc) + 0x130);
            if (!tpl) return 0;
            return tpl->formID;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // =====================================================================
    // V2 Civilian Filter  - tier classifier
    // =====================================================================
    // Goal: drop civilian/crowd NPCs from sync entirely. Engine simulates
    // AI deterministically on both sides - civilian NPCs at the same
    // spawn points run the same wander/sit/converse packages naturally.
    // We only sync NPCs whose state would visibly diverge between host
    // and client:
    // Companions       (kPlayerTeammate)
    // In-combat actors (kSearchingInCombat / currentCombatTarget != 0)
    // Aggressive NPCs  (kAttackOnSight)
    // Dying            (kDead - broadcast death once, then drop)
    //
    // Classification cached per-actor for 1s to avoid repeated reads on
    // a hot path (HookedActorUpdate fires per-actor per-frame). On cell
    // change the cache clears so a fresh evaluation runs against the
    // new NPC population.
    enum class NPCTier : uint8_t { kLocal, kTracked };

    struct TierCacheEntry {
        NPCTier  tier;
        uint64_t evaluatedMs;
    };
    static std::unordered_map<uint32_t, TierCacheEntry> g_tierCache;
    static std::mutex                                   g_tierCacheMutex;

    static std::atomic<uint64_t> g_dbgFilteredCivilian{ 0 };
    static std::atomic<uint64_t> g_dbgPromotedTracked{ 0 };

    // Plain-C SEH read of Actor.boolBits (+0x208), currentCombatTarget
    // (+0x298), boolFlags (+0x378). Avoids virtuals during cell-teardown.
    static int SafeReadActorTierFields(RE::Actor* a,
                                       uint32_t* outBoolBits,
                                       uint32_t* outCombatTarget,
                                       uint32_t* outBoolFlags)
    {
        __try {
            auto* base = reinterpret_cast<uint8_t*>(a);
            *outBoolBits     = *reinterpret_cast<uint32_t*>(base + 0x208);
            *outCombatTarget = *reinterpret_cast<uint32_t*>(base + 0x298);
            *outBoolFlags    = *reinterpret_cast<uint32_t*>(base + 0x378);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Plain-C SEH read of TESForm.formID (+0x28). Same offset as everywhere
    // else in this file; extracted here so ClassifyNPC's body stays clean
    // of __try (lock_guard would trip C2712 otherwise).
    static int SafeReadActorFormID(RE::Actor* actor, uint32_t* outFid)
    {
        __try {
            *outFid = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(actor) + 0x28);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static NPCTier ClassifyNPC(RE::Actor* actor)
    {
        constexpr uint64_t kTTLMs = 1000;
        uint32_t fid = 0;
        if (!SafeReadActorFormID(actor, &fid)) return NPCTier::kLocal;
        if (fid == 0) return NPCTier::kLocal;

        const uint64_t nowMs = GetTickCount64();
        {
            std::lock_guard lk(g_tierCacheMutex);
            auto it = g_tierCache.find(fid);
            if (it != g_tierCache.end() && nowMs - it->second.evaluatedMs < kTTLMs) {
                return it->second.tier;
            }
        }

        uint32_t boolBits = 0, combatTarget = 0, boolFlags = 0;
        if (!SafeReadActorTierFields(actor, &boolBits, &combatTarget, &boolFlags)) {
            return NPCTier::kLocal;
        }

        // BOOL_BITS layout (Actor.h):
        //   kDead              = 1 << 11
        //   kSearchingInCombat = 1 << 14
        //   kPlayerTeammate    = 1 << 26
        //   kGuard             = 1 << 30
        constexpr uint32_t kBitDead              = 1u << 11;
        constexpr uint32_t kBitSearchingInCombat = 1u << 14;
        constexpr uint32_t kBitPlayerTeammate    = 1u << 26;
        constexpr uint32_t kBitGuard             = 1u << 30;
        // BOOL_FLAGS layout:
        //   kAngryWithPlayer = 1 << 11
        //   kAttackOnSight   = 1 << 15
        //   kEssential       = 1 << 18
        //   kProtected       = 1 << 19
        constexpr uint32_t kFlagAngryWithPlayer  = 1u << 11;
        constexpr uint32_t kFlagAttackOnSight    = 1u << 15;
        constexpr uint32_t kFlagEssential        = 1u << 18;
        constexpr uint32_t kFlagProtected        = 1u << 19;

        NPCTier tier = NPCTier::kLocal;
        if      (boolBits  & kBitPlayerTeammate)    tier = NPCTier::kTracked;
        else if (boolBits  & kBitSearchingInCombat) tier = NPCTier::kTracked;
        else if (boolBits  & kBitDead)              tier = NPCTier::kTracked;
        else if (boolBits  & kBitGuard)             tier = NPCTier::kTracked;
        else if (boolFlags & kFlagAttackOnSight)    tier = NPCTier::kTracked;
        else if (boolFlags & kFlagAngryWithPlayer)  tier = NPCTier::kTracked;
        else if (boolFlags & kFlagEssential)        tier = NPCTier::kTracked;
        else if (boolFlags & kFlagProtected)        tier = NPCTier::kTracked;
        else if (combatTarget != 0)                 tier = NPCTier::kTracked;

        {
            std::lock_guard lk(g_tierCacheMutex);
            g_tierCache[fid] = TierCacheEntry{ tier, nowMs };
        }
        if (tier == NPCTier::kTracked) {
            g_dbgPromotedTracked.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_dbgFilteredCivilian.fetch_add(1, std::memory_order_relaxed);
        }
        return tier;
    }

    static bool IsNPCWeSync(RE::Actor* a_actor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        if (a_actor == player) return false;

        // Exclude the PULSAR remote-player clone (the client's avatar that
        // we spawned on host via PlaceAtMe(playerBase)). Without this, the
        // host sees the clone as a dynamic actor with base=0x7 (player) and
        // broadcasts it as an NPC - client then spawns a copy glued to its
        // own player position.
        //
        // Additional defense: explicitly reject PlayerBase (0x7) as a base
        // even if GetRemoteActor() hasn't been assigned yet - early in the
        // session the clone can be in g_tracked before RemotePlayer has
        // captured it. Either match drops.
        if (auto* remote = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor()) {
            if (a_actor == remote) return false;
        }
        if (auto baseForm = a_actor->GetBaseObject()) {
            if (baseForm->formID == 0x00000007) return false;
        }

        auto* playerCell = player->parentCell;
        if (!playerCell) return false;

        // Distance-based sync instead of strict cell-match. Starfield planet
        // surfaces partition into many small proc cells - a POI at 200m has
        // a different parentCell than the player even though both are loaded.
        // Use a radius that matches the engine's active-ref loading range.
        constexpr float kSyncRadiusXY_sq = 400.0f * 400.0f;
        const float dx = a_actor->data.location.x - player->data.location.x;
        const float dy = a_actor->data.location.y - player->data.location.y;
        if (dx * dx + dy * dy > kSyncRadiusXY_sq) return false;

        // V2 Civilian Filter : drop crowd/civilian NPCs that the
        // engine simulates deterministically on both sides anyway. Only
        // companions, in-combat actors, and dying actors continue past
        // this gate.
        if (ClassifyNPC(a_actor) == NPCTier::kLocal) return false;

        return true;
    }

    // Diagnostic kept minimal now that hook is verified working on primary vtable.
    // Counter of in-cell captures for a periodic status log.
    static std::atomic<uint32_t> g_dbgSameCellCaptures{0};

    // Cached ActorValueInfo for Health - resolved once when the game world is available.
    // Looked up lazily on first hook call because forms aren't loaded at plugin init.
    static std::atomic<RE::ActorValueInfo*> g_healthAV{ nullptr };

    // Pacify-caches: Aggression=0 + Confidence=0 make a remote-owned NPC
    // ignore/flee rather than attack. Applied once per remote-owned actor
    // on first-sync - proper T35 AI-freeze is the real fix, this is the
    // stop-gap so client doesn't die to invisible enemies.
    static std::atomic<RE::ActorValueInfo*> g_aggressionAV{ nullptr };
    static std::atomic<RE::ActorValueInfo*> g_confidenceAV{ nullptr };

    // SEH-wrapped ActorValue set. Plain-C (no C++ unwinding) so the SEH guard
    // works. SetActorValue can fault if the actor is mid-cell-teardown.
    static int SafeSetActorValue(RE::Actor* actor, RE::ActorValueInfo* av, float v)
    {
        __try {
            actor->SetActorValue(*av, v);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // V2-Step-1b kill primitive. RE'd (Ghidra 2026-05-25): the Papyrus Actor.Kill
    // native (binder NativeFunction1<Actor,void,Actor*>) routes through the inner
    // engine kill FUN_141fcb950(self, killer, notSilent, VM*, stackID). It runs the
    // invulnerability check (VM/stackID are used ONLY in the "is invulnerable" error
    // path - safe to pass null for a normal, already-damaged enemy) then the real
    // kill FUN_1419088b0. No REL::ID exists for it (not in Address Library) → raw
    // RVA via REL::Offset, same pattern as MoveTo (Main.cpp:94) / SaveBuffer fns.
    // notSilent=true = normal Kill (vs KillSilent which passes 0 as arg 3).
    using KillActorFn = void(*)(RE::Actor* self, RE::Actor* killer, bool notSilent,
                                void* vm, std::uint32_t stackID);
    static REL::Relocation<KillActorFn> g_killActorFn{ REL::Offset{ 0x1FC82E0 } };

    static int SafeKillActor(RE::Actor* actor, RE::Actor* killer)
    {
        __try {
            g_killActorFn(actor, killer, true, nullptr, 0);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Anti-dup: client-side set of non-remote-owned dynamic-ref NPCs in a proc
    // cell that we've already Disable()'d once. Prevents retry-spam and the
    // engine's delayed state-transitions from making us re-disable every frame.
    static std::unordered_set<uint32_t> g_clientDisabledNatives;
    static std::mutex g_clientDisabledNativesMutex;

    // "First seen" tick per actor FormID. Gates anti-dup so we don't disable
    // our OWN fresh spawn in the race window between PlaceAtMe (engine creates
    // the ref, Actor::Update hook fires) and our Papyrus DONE callback
    // registering it in g_remoteOwned. Actor must have been around ≥ grace
    // window before we consider it a non-remote native.
    static std::unordered_map<uint32_t, uint64_t> g_clientFirstSeenTick;
    static std::mutex g_clientFirstSeenTickMutex;
    static constexpr uint64_t kAntiDupGraceMs = 2000;

    // SEH-wrapped "disable-ish": teleport the actor underground. Combined with
    // adding to g_remoteOwned (our Actor::Update skip set), this produces the
    // "disappeared" effect - no visible, no AI, no combat - without touching
    // the engine's formFlags or triggering Disable cleanup paths (which we
    // saw corrupts our own fresh spawns).
    static int SafeActorTeleportAway(RE::Actor* actor)
    {
        __try {
            auto* loc = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(actor) + 0x8C);
            loc[0] = 0.0f;
            loc[1] = 0.0f;
            loc[2] = -100000.0f;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Phase D: HookedActorUpdate is pure pass-through. No remote-owned
    // skip (would freeze transforms), no anti-dup (natives are WANTED
    // since both machines deterministically proc-gen the same NPCs).
    // The collector still runs so g_procDumpTableV3 populates for the
    // matcher. AI suppression for matched NPCs happens via Aggression=0
    // set in the apply loop - cleaner than Update-skip hacks.
    static constexpr bool kT40PassThrough = true;

    // Phase D: find a local native actor that corresponds to a host-NPC
    // state packet. Matching uses (effectiveBaseID + position proximity).
    //
    // `hostEffectiveID` priority:
    //   1. Host's sourceBaseFormID (T28 - static template for dynamic 0xFF clones)
    //   2. Else host's baseFormID (for static-ref NPCs like Sarah)
    //
    // Client's effectiveBaseID is computed the same way in SafeProcDumpGather.
    // Both machines agree on the static TESNPC template FormID, which is
    // stable because it's loaded from the same .esm file.
    //
    // `claimed`: set of client refFormIDs already matched in this apply pass.
    // Prevents two host-NPCs from remoting to the same local actor (which
    // would cause tele-jitter as each tick re-sets position between two
    // different host-targets).
    static uint32_t FindLocalMatch(uint32_t hostEffectiveID,
                                   float hostX, float hostY, float hostZ,
                                   const std::unordered_set<uint32_t>& claimed)
    {
        if (hostEffectiveID == 0) return 0;
        constexpr float kMatchRadius2 = 50.0f * 50.0f;
        uint32_t bestRef = 0;
        float bestDist2 = kMatchRadius2;
        std::lock_guard lk(g_procDumpMutex);
        for (auto& [ref, e] : g_procDumpTableV3) {
            if (e.effectiveBaseID != hostEffectiveID) continue;
            if (claimed.count(ref)) continue;  // exclusivity: already matched
            float dx = e.pos[0] - hostX, dy = e.pos[1] - hostY, dz = e.pos[2] - hostZ;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < bestDist2) {
                bestDist2 = d2;
                bestRef = ref;
            }
        }
        return bestRef;
    }

    // Phase E Task 0 probe helper: walk actor->aiProcess->middleHigh->charController
    // and return the cc pointer, vtable pointer, and rigid-body pointer (cc+0x20).
    // Plain-C - __try can't coexist with C++ unwinding (C2712). Returns 1 on
    // successful deref (cc may still be nullptr if the chain has a gap - that's
    // a valid state during early-init/cell-transition, not an AV).
    static int SafeProbeCharController(RE::Actor* a_actor,
                                       void** out_cc,
                                       void*** out_vt,
                                       uintptr_t* out_body)
    {
        __try {
            auto u = reinterpret_cast<uintptr_t>(a_actor);
            uintptr_t aiProc = *reinterpret_cast<uintptr_t*>(u + 0x228);
            uintptr_t mh = aiProc ? *reinterpret_cast<uintptr_t*>(aiProc + 0x08) : 0;
            void* cc = mh ? *reinterpret_cast<void**>(mh + 0x458) : nullptr;
            void** vt = cc ? *reinterpret_cast<void***>(cc) : nullptr;
            uintptr_t bw = cc ? *reinterpret_cast<uintptr_t*>(
                reinterpret_cast<uintptr_t>(cc) + 0x20) : 0;
            *out_cc = cc;
            *out_vt = vt;
            *out_body = bw;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // 2026-05-03 - engine-crash absorber for the original Actor::Update.
    // Crash signature: ACCESS VIOLATION read at NULL+0x08 inside Starfield.exe
    // RVA 0x00566BF5, hit ~3min after dock when client sits in vanilla station
    // with FF-prefix Frontier crew clones still in update list.
    // Hypothesis: a procedural NPC's internal state (aiProcess / parentCell /
    // animationGraphManager) was torn down by cell-unload, but the actor stays
    // in the engine's per-frame update queue. Engine reads a_actor->internalPtr
    // and dereferences vtable[1] on it → null deref.
    // We can't fix the engine's internal state. We CAN absorb the SEH and log
    // the offending actor once so we have RE evidence next session.
    // Helper is split: the C++ object work (mutex / unordered_set / formatting)
    // lives in NPCSyncLogCaughtUpdate so SafeOrigActorUpdate stays POD-only and
    // C2712-clean (no destructor-bearing locals in same scope as __try).
    static void NPCSyncLogCaughtUpdate(std::uint32_t a_fid)
    {
        static std::mutex                            s_mu;
        static std::unordered_set<std::uint32_t>     s_logged;
        bool first = false;
        {
            std::lock_guard lk(s_mu);
            first = s_logged.insert(a_fid).second;
        }
        if (first) {
            PLOG_WARN(
                "[PULSAR] NPCSync SAFE-UPDATE caught engine crash for actor "
                "0x{:08X} - stale clone or torn internal state. Skipping this "
                "tick; engine retry or unregister will follow."sv, a_fid);
        }
    }
    static void SafeOrigActorUpdate(RE::Actor* a_actor, float a_delta) noexcept
    {
        if (!g_origActorUpdate) return;
        __try {
            g_origActorUpdate(a_actor, a_delta);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::uint32_t fid = 0;
            __try {
                if (a_actor) {
                    fid = *reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<std::uint8_t*>(a_actor) + 0x28);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            NPCSyncLogCaughtUpdate(fid);
        }
    }

    static void HookedActorUpdate(RE::Actor* a_actor, float a_delta)
    {
        // HookGuard: bail to engine immediately during save-load + cell-stream
        // chaos. PULSAR-side state (cell registry, proc dump table, lerp targets)
        // holds Actor* that engine save-load invalidates → UAF on the next read.
        // HookGuard::Suspend is set in WorldSync::OnSaveReceived, auto-resumed
        // 5s after WorldSync hits LOADED + loadMenu closed.
        if (!Pulsar::HookGuard::IsSafe()) {
            SafeOrigActorUpdate(a_actor, a_delta);
            return;
        }

        extern bool IsHostMode();  // defined below - avoids Config include churn

        // T40: collect proc-cell actor state BEFORE any skip/anti-dup logic
        // runs. This captures the raw natives that the engine's own proc-gen
        // spawned, regardless of what we do with them afterward.
        SafeProcDumpCollect(a_actor);

        // Combat Sync V1 discovery probe - runs for ALL actors on BOTH sides
        // (before the host/client split below), read-only health/death logging.
        CombatProbeObserve(a_actor);

        // Track every active cell we encounter via any actor's parentCell.
        // Cheap: unordered_set.insert is O(1), and HookedActorUpdate fires at
        // engine tick rate for all active actors. Stale cell pointers get
        // pruned lazily when GetActiveCellsSnapshot callers use them.
        if (a_actor && a_actor->parentCell) {
            std::lock_guard lock(g_activeCellsMutex);
            g_activeCells.insert(a_actor->parentCell);
        }

        // Phase E CLIENT dispatch:
        //   * Always call origUpdate - the engine needs to tick the anim
        //     graph, propagate mesh.world.rotation from the skeleton, update
        //     the Havok body, and apply root-motion translate. Full-skip (a
        //     previous attempt) caused NPCs to hover above the ground with
        //     frozen rotation and running-in-place anim because none of
        //     those propagations ran.
        //   * AFTER origUpdate, TickLerp overrides position for tracked
        //     actors so host authority wins. AI decisions are separately
        //     blocked by HookedEvaluatePackage via IsRemoteOwned(formID)
        // without that block, Sarah's companion AI would keep trying
        //     to walk toward the CLIENT player, fighting our TickLerp.
        if (!IsHostMode()) {
            // Pre-tick: write graph vars BEFORE origUpdate so the anim-graph
            // tick in the same frame reads our Speed/Direction/iState values
            // post-origUpdate writes lagged by 1 frame and empirically the
            // walk transition never committed.
            if (a_actor) Pulsar::NPCSync::PreTickWriteGraphVars(a_actor);
            if (a_actor) Pulsar::NPCSync::ReadGraphSnapshot(a_actor, "PRE");

            SafeOrigActorUpdate(a_actor, a_delta);

            // Post-tick probe BEFORE our re-write: shows what origUpdate
            // left behind. If Speed=0 here while PRE said Speed=4.95, the
            // engine's AI/MovementController is overwriting our writes
            // during its tick - then the anim-graph tick (also during
            // origUpdate) would have read the reset value, not ours.
            if (a_actor) Pulsar::NPCSync::ReadGraphSnapshot(a_actor, "MID");

            // Post-tick: position override (plus rotation lerp + drift snap
            // when needed). Graph vars re-written here too as safety for
            // the next-frame anim tick - cheap and idempotent.
            if (a_actor) Pulsar::NPCSync::TickLerp(a_actor, 0.25f);

            if (a_actor) Pulsar::NPCSync::ReadGraphSnapshot(a_actor, "POST");
            return;
        }
        // HOST: fall through to tracking code below - we still need
        // the host to capture + stream NPC state to the client.

        // CLIENT: skip the original Update for actors that are being driven by
        // remote state. Their position/health comes from the network; running
        // AI on top causes jitter (AI moves actor → network snaps back → loop).
        // Pattern borrowed from TiltedEvolution's HookActorProcess.
        if (!IsHostMode() && a_actor) {
            bool remote = false;
            {
                std::lock_guard lock(g_remoteOwnedMutex);
                remote = g_remoteOwned.count(a_actor->formID) > 0;
            }
            if (remote) {
                // Full skip of original Update. Earlier variant "let Update
                // run while has3D=false so 3D loads" caused a render-thread
                // NULL deref during bone/transform iteration (Starfield+0x30B350E,
                // dump e54734b7-…-dmp, 2026-04-21 23:50) - the engine walks
                // bone arrays on actors whose 3D state is half-initialized.
                // Reverted to full skip; visible-invisible trade-off handled
                // properly in T35 (flag-based AI freeze, not update-skip).
                return;
            }

            // Anti-dup: client's save-load + own proc-gen leave NATIVE
            // enemies in the proc cell - duplicates of the host's NPCs that
            // we already mirror via PlaceAtMe. User sees double mobs, client
            // AI runs on invisible-to-host natives, damage propagates
            // wrongly. Auto-disable every 0xFF-prefix NPC in the player's
            // proc cell that we didn't spawn ourselves.
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (a_actor != player && player && player->parentCell &&
                (a_actor->formID & 0xFF000000) == 0xFF000000)
            {
                // Not ours, and dynamic-ref. Must also be in a PROC cell AND
                // the PLAYER's current cell (don't disable NPCs elsewhere).
                auto* npcCell = a_actor->parentCell;
                if (npcCell && npcCell == player->parentCell &&
                    (npcCell->formID & 0xFF000000) == 0xFF000000)
                {
                    // Skip if it's our remote-player-clone (it also has
                    // dynamic-ref + is in player's cell).
                    bool isOurClone = false;
                    if (auto* rc = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor()) {
                        isOurClone = (a_actor == rc);
                    }
                    if (!isOurClone) {
                        // Grace period: first time we see this actor, record
                        // the tick and LET IT LIVE. This window lets our own
                        // Papyrus spawn finish + the Apply loop register the
                        // new ref in g_remoteOwned. Without this, we disable
                        // our own fresh spawn and it crashes under subsequent
                        // SetPosition calls.
                        uint64_t nowT = GetTickCount64();
                        uint64_t firstSeenT = 0;
                        {
                            std::lock_guard lk(g_clientFirstSeenTickMutex);
                            auto [it, inserted] =
                                g_clientFirstSeenTick.try_emplace(a_actor->formID, nowT);
                            firstSeenT = it->second;
                        }
                        if (nowT - firstSeenT < kAntiDupGraceMs) {
                            // Still in grace window - run original Update so
                            // the actor gets its normal per-frame tick while
                            // we decide. If it becomes remote-owned during
                            // the grace window, the early-return above fires
                            // on subsequent ticks.
                            SafeOrigActorUpdate(a_actor, a_delta);
                            return;
                        }

                        bool firstTime;
                        {
                            std::lock_guard lk(g_clientDisabledNativesMutex);
                            firstTime = g_clientDisabledNatives.insert(a_actor->formID).second;
                        }
                        if (firstTime) {
                            if (SafeActorTeleportAway(a_actor)) {
                                // Also add to g_remoteOwned so this hook
                                // returns fast on subsequent frames for the
                                // same actor (AI suppressed, no work).
                                {
                                    std::lock_guard lk(g_remoteOwnedMutex);
                                    g_remoteOwned.insert(a_actor->formID);
                                }
                                PLOG_INFO("[PULSAR] NPCSync: anti-dup hid native proc-cell NPC 0x{:08X} cell=0x{:08X} (grace elapsed)"sv,
                                          a_actor->formID, npcCell->formID);
                            }
                        }
                        return;
                    }
                }
            }
        }

        // Always call original first so game state stays consistent
        SafeOrigActorUpdate(a_actor, a_delta);

        if (!a_actor) return;

        // Only the host captures state. Client NPCs are driven by incoming packets.
        if (!IsHostMode()) return;

        // 2026-04-20 RE-tracker: log EVERY actor we see on the host (once per
        // form), before any filter. Goal: discover what NPCs exist on planets
        // (runtime FormID vs base FormID vs editorID) so we can design a
        // proper sync path for 0xFF dynamic enemies. Only logs once per form
        // ID to avoid spam. Tagged "RE_NPC" for easy grep.
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (a_actor && a_actor != player && a_actor->parentCell) {
                static std::unordered_set<uint32_t> s_seenForms;
                static std::mutex s_seenMutex;
                uint32_t refFormID = a_actor->formID;
                bool firstSeen = false;
                {
                    std::lock_guard lock(s_seenMutex);
                    if (s_seenForms.insert(refFormID).second) firstSeen = true;
                }
                if (firstSeen) {
                    uint32_t baseFormID = 0;
                    const char* npcEditorID = "?";
                    if (auto* npc = a_actor->GetNPC()) {
                        baseFormID = npc->formID;
                        const char* eid = npc->GetFormEditorID();
                        if (eid && eid[0]) npcEditorID = eid;
                    }
                    uint32_t cellFormID = a_actor->parentCell->formID;
                    bool isProc = (cellFormID & 0xFF000000) == 0xFF000000;
                    bool isDynRef = (refFormID & 0xFF000000) == 0xFF000000;
                    // V3 : also dump tier-decision fields to verify
                    // classifier sees what we expect for named NPCs.
                    uint32_t bb = 0, ct = 0, bf = 0;
                    SafeReadActorTierFields(a_actor, &bb, &ct, &bf);
                    NPCTier classified = ClassifyNPC(a_actor);
                    PLOG_INFO("[PULSAR] RE_NPC ref=0x{:08X}{} base=0x{:08X} '{}' cell=0x{:08X}{} dist={:.0f} bits=0x{:08X} flags=0x{:08X} ct=0x{:08X} tier={}"sv,
                        refFormID, isDynRef ? "(DYN)" : "",
                        baseFormID, npcEditorID,
                        cellFormID, isProc ? "(PROC)" : "(STATIC)",
                        std::sqrt(
                            (a_actor->data.location.x - player->data.location.x) *
                            (a_actor->data.location.x - player->data.location.x) +
                            (a_actor->data.location.y - player->data.location.y) *
                            (a_actor->data.location.y - player->data.location.y)),
                        bb, bf, ct,
                        classified == NPCTier::kTracked ? "TRACKED" : "local");
                }
            }
        }

        if (!IsNPCWeSync(a_actor)) {
            // Retroactive cleanup: if this actor was tracked before (e.g.
            // the remote-player clone captured before RemotePlayer took
            // ownership), drop it now so the send-loop stops broadcasting.
            const uint32_t fid = a_actor->formID;
            {
                std::lock_guard lock(g_trackedMutex);
                g_tracked.erase(fid);
            }
            // V5 civilian periodic snapshot (LOCAL-tier in view radius).
            MaybeRecordCivilianSnapshot(a_actor);
            return;
        }
        g_dbgSameCellCaptures.fetch_add(1, std::memory_order_relaxed);

        // Snapshot current state
        float posX = a_actor->data.location.x;
        float posY = a_actor->data.location.y;
        float posZ = a_actor->data.location.z;
        float rotZ = a_actor->data.angle.z;

        // Health lookup via cached ActorValueInfo. First call resolves + caches.
        float health = 0.0f, healthMax = 0.0f;
        auto* healthAV = g_healthAV.load(std::memory_order_acquire);
        if (!healthAV) {
            healthAV = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("Health");
            if (healthAV) {
                g_healthAV.store(healthAV, std::memory_order_release);
            }
        }
        if (healthAV) {
            health    = a_actor->GetActorValue(*healthAV);
            healthMax = a_actor->GetPermanentActorValue(*healthAV);
        }
        // V2-Step-1c: IsDead() polling MISSES the death (a dead actor leaves the
        // per-frame update loop before the flag flips, so this capture rarely sees
        // it). Effective Health goes <=0 and stays negative for many frames before
        // teardown - use that as the death signal so the host reliably broadcasts
        // isDead → client kills its matched enemy.
        bool  isDead    = a_actor->IsDead() || (healthAV && health <= 0.0f);
        bool  weaponDrawn = (a_actor->actorState2 & 0x3) >= 2;

        // Phase E Task 3: sample locomotion graph vars for NPC sync.
        // Mirrors the read path used by Main.cpp::FillAnimationData for the
        // Player - GetGraphVariableImpl{Float,Int} are virtuals on
        // IAnimationGraphManagerHolder, so the SAME call works on any Actor.
        // SEH-guarded via plain-C helpers because NPCs can be mid-spawn with
        // a partially-initialized animation graph (null graph manager, torn
        // vtable during re-init) and HookedActorUpdate has C++ destructor
        // locals (C2712 forbids mixing __try with those).
        float speed = 0.0f;
        float direction = 0.0f;
        int32_t sprintState = 0;
        // P3 fire-sync: also sample the host NPC's combat/fire graph state so
        // the client can mirror the aim/fire anim (its AI is blocked → it never
        // fires on its own). gunActions pulses on weapon fire; sighted/iState mark
        // the combat-ready aim stance (idle iState=10, combat seen as 17).
        int32_t gunActions = 0, gunDown = 0, sighted = 0, iState = 0;
        {
            static const RE::BSFixedString kSpeedVar("Speed");
            static const RE::BSFixedString kDirVar("Direction");
            static const RE::BSFixedString kSprintVar("iSyncSprintState");
            static const RE::BSFixedString kGunActVar("iSyncGunActions");
            static const RE::BSFixedString kGunDownVar("iSyncGunDown");
            static const RE::BSFixedString kSightedVar("iSyncSighted");
            static const RE::BSFixedString kIStateVar("iState");
            SafeNpcGetGraphFloat(a_actor, &kSpeedVar, &speed);
            SafeNpcGetGraphFloat(a_actor, &kDirVar, &direction);
            SafeNpcGetGraphInt(a_actor, &kSprintVar, &sprintState);
            SafeNpcGetGraphInt(a_actor, &kGunActVar, &gunActions);
            SafeNpcGetGraphInt(a_actor, &kGunDownVar, &gunDown);
            SafeNpcGetGraphInt(a_actor, &kSightedVar, &sighted);
            SafeNpcGetGraphInt(a_actor, &kIStateVar, &iState);
        }

        uint8_t moveFlags = 0;
        if (speed > 5.0f)     moveFlags |= 0x1;  // bit 0 = moving
        if (sprintState != 0) moveFlags |= 0x2;  // bit 1 = sprinting
        if (gunActions != 0)  moveFlags |= 0x4;  // bit 2 = firing (P3)
        if (sighted != 0 || weaponDrawn) moveFlags |= 0x8;  // bit 3 = combat-aim stance
        // [FIRE-DBG] host-side combat-var snapshot (3s throttle) so we can tune the
        // exact graph values that drive the client fire/aim anim.
        if (weaponDrawn || gunActions || sighted) {
            static std::unordered_map<uint32_t, uint64_t> s_fireDiag;
            uint64_t nowF = GetTickCount64();
            auto& lf = s_fireDiag[a_actor->formID];
            if (nowF - lf > 3000) {
                lf = nowF;
                PLOG_INFO("[FIRE-DBG] host NPC=0x{:08X} iState={} gunActions={} gunDown={} sighted={} wpnDrawn={} -> mf=0x{:02X}"sv,
                          a_actor->formID, iState, gunActions, gunDown, sighted, weaponDrawn ? 1 : 0, moveFlags);
            }
        }

        // Mitigation for Task #91 host-combat crash (render-thread bone-NULL
        // deref at Starfield+0x30B350E): actively-dying / already-dead actors
        // are in a torn-down state where their bone/transform arrays go
        // through weird partial states. Our per-frame capture reading
        // actor->IsDead + bone arrays can race with the render thread.
        // Drop the actor from tracking IMMEDIATELY when it dies so we stop
        // poking at it during the engine's teardown window.
        if (isDead) {
            const uint32_t fid = a_actor->formID;
            std::lock_guard lock(g_trackedMutex);
            auto it = g_tracked.find(fid);
            if (it != g_tracked.end()) {
                // Mark dead for the next send so client can kill its clone,
                // THEN the send-loop will erase. But we also stop updating
                // position from here on - capture below is skipped.
                it->second.isDead = true;
            }
            return;
        }

        // Capture base form ID - the TESNPC template - so the client can
        // resolve this actor even when the runtime FormID doesn't match.
        uint32_t baseFormID = 0;
        uint32_t sourceBaseFormID = 0;
        if (auto* npc = a_actor->GetNPC()) {
            baseFormID = npc->formID;
            // If the base itself is dynamic (per-process clone), walk the
            // baseTemplateForm pointer at +0x130 to find the static source
            // template that lives in the ESM and is identical on both sides.
            if ((baseFormID & 0xFF000000) == 0xFF000000) {
                sourceBaseFormID = SafeReadBaseTemplateFormID(npc);
            }
        }

        // Phase E probe: log CharController layout for first 5 unique base IDs.
        // __try lives in a helper (SafeProbeCharController) because this
        // function has C++ objects with destructors (C2712).
        static std::unordered_set<uint32_t> s_probedBases;
        static std::atomic<int> s_probedCount{0};
        if (baseFormID != 0 && s_probedCount.load() < 5) {
            bool insert = false;
            {
                std::lock_guard lk(g_trackedMutex);
                insert = s_probedBases.insert(baseFormID).second;
            }
            if (insert) {
                void* cc = nullptr;
                void** vt = nullptr;
                uintptr_t bw = 0;
                int rc = SafeProbeCharController(a_actor, &cc, &vt, &bw);
                if (rc == 1) {
                    PLOG_INFO("[PULSAR] E-probe base=0x{:08X} actor=0x{:08X} cc={} vt={} body={}"sv,
                        baseFormID, a_actor->formID,
                        (void*)cc, (void*)vt, (void*)bw);
                    s_probedCount.fetch_add(1);
                } else {
                    PLOG_WARN("[PULSAR] E-probe SEH for base=0x{:08X}"sv, baseFormID);
                }
            }
        }

        // Skip NPCs for which we have no static handle: both base and
        // source are dynamic (or unreadable). Rare but possible.
        const bool baseStatic   = (baseFormID != 0) && ((baseFormID & 0xFF000000) != 0xFF000000);
        const bool sourceStatic = (sourceBaseFormID != 0) && ((sourceBaseFormID & 0xFF000000) != 0xFF000000);
        if (!baseStatic && !sourceStatic) return;

        // Resolve the source plugin once per (baseFormID, sourceBaseFormID)
        // pair - TESFile.fileName pointer is stable for the session, so we
        // only re-resolve if the effective static-base FormID changed.
        uint32_t effectiveStaticID =
            ((baseFormID & 0xFF000000) == 0xFF000000 && sourceBaseFormID)
                ? sourceBaseFormID
                : baseFormID;

        std::lock_guard lock(g_trackedMutex);
        auto& entry = g_tracked[a_actor->formID];
        const bool idChanged = (entry.baseFormID != baseFormID) ||
                               (entry.sourceBaseFormID != sourceBaseFormID);
        entry.baseFormID = baseFormID;
        entry.sourceBaseFormID = sourceBaseFormID;
        if (idChanged) {
            entry.sourceLocalID = 0;
            entry.sourcePlugin[0] = '\0';
            if (effectiveStaticID != 0 &&
                (effectiveStaticID & 0xFF000000) != 0xFF000000)
            {
                // Cache plugin-name by effectiveStaticID globally. Many NPCs
                // in a POI share the same TESNPC/LVLN base form (proc-gen
                // clones of one template) - without this cache each NPC's
                // first-sight triggered a full DataHandler walk, which under
                // POI load meant dozens of walks/second contending with
                // engine mod-list access. Reduces the race surface suspected
                // in Task #91.
                struct PluginEntry {
                    char     name[Pulsar::Network::NPC_SOURCE_PLUGIN_MAX];
                    uint32_t localID;
                    bool     resolved;
                };
                static std::unordered_map<uint32_t, PluginEntry> s_pluginCache;
                static std::mutex s_pluginCacheMutex;

                PluginEntry pe{};
                bool cacheHit = false;
                {
                    std::lock_guard lk(s_pluginCacheMutex);
                    auto it = s_pluginCache.find(effectiveStaticID);
                    if (it != s_pluginCache.end()) {
                        pe = it->second;
                        cacheHit = true;
                    }
                }
                if (!cacheHit) {
                    pe.resolved = Pulsar::NPCSync::PluginLookup::FormIDToPluginName(
                        effectiveStaticID, pe.name, sizeof(pe.name));
                    pe.localID = pe.resolved
                        ? Pulsar::NPCSync::PluginLookup::FormIDToLocalID(effectiveStaticID)
                        : 0;
                    std::lock_guard lk(s_pluginCacheMutex);
                    s_pluginCache[effectiveStaticID] = pe;
                }
                if (pe.resolved) {
                    std::memcpy(entry.sourcePlugin, pe.name,
                                Pulsar::Network::NPC_SOURCE_PLUGIN_MAX);
                    entry.sourceLocalID = pe.localID;
                }
            }
        }
        entry.posX = posX; entry.posY = posY; entry.posZ = posZ;
        entry.rotZ = rotZ;
        entry.speed = speed;
        entry.direction = direction;
        entry.moveFlags = moveFlags;
        entry.health = health; entry.healthMax = healthMax;
        entry.isDead = isDead;
        entry.weaponDrawn = weaponDrawn;

        // Corpse-loot sync: on the death EDGE (host only), schedule the
        // one-shot inventory broadcast for ~1s later (engine appends death
        // items at kill time; OnGameThreadTick drains the due entries).
        if (isDead && IsHostMode()) {
            std::lock_guard clk(g_corpseLootMutex);
            if (g_corpseLootSent.insert(a_actor->formID).second) {
                g_corpseLootDueMs[a_actor->formID] = GetTickCount64() + 1000;
                PLOG_INFO("[CORPSE-LOOT] death-edge scheduled ref=0x{:08X} (send in 1s)"sv,
                          a_actor->formID);
            }
        }
        entry.lastCapturedTick = GetTickCount64();  // V4 staleness prune
        // lastSentTick + everSent preserved across captures - updated by Task 4 send loop

        // Phase E smoke diag: once per 3s per-actor, log sampled values.
        // ~30 Hz capture would flood the log otherwise. No __try in scope here
        // (std::lock_guard above means C++ unwinding is already active).
        {
            static std::unordered_map<uint32_t, uint64_t> s_diagTick;
            uint64_t nowMs = GetTickCount64();
            auto& last = s_diagTick[a_actor->formID];
            if (nowMs - last > 3000) {
                last = nowMs;
                PLOG_INFO("[PULSAR] E-sample NPC=0x{:08X} spd={:.1f} dir={:.2f} mf=0x{:02X}"sv,
                    a_actor->formID, speed, direction, moveFlags);
            }
        }
    }

    // Small helper so we don't drag Network singleton into this hot path's header deps.
    // Implemented below, checks the existing Network::NetworkHost::IsHost() flag.
    bool IsHostMode();

    bool Init()
    {
        // Hook Actor::Update via vtable-slot swap on the PRIMARY Actor vtable.
        // VTABLE_Actor[1] = REL::ID(451614), vfunc index 0x13F (offset 0x9F8).
        REL::Relocation<uintptr_t*> vtable{ REL::ID(451614) };
        constexpr size_t kUpdateVfuncIndex = 0x13F;

        uintptr_t* slot = vtable.get() + kUpdateVfuncIndex;

        DWORD oldProt = 0;
        if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
            PLOG_WARN("[PULSAR] NPCSync: VirtualProtect failed, hook NOT installed"sv);
            return false;
        }

        g_origActorUpdate = reinterpret_cast<ActorUpdate_t>(*slot);
        *slot = reinterpret_cast<uintptr_t>(&HookedActorUpdate);

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(uintptr_t), oldProt, &ignored);

        PLOG_INFO("[PULSAR] NPCSync: Actor::Update hooked at slot {} (orig=0x{:x})"sv,
                  static_cast<void*>(slot),
                  reinterpret_cast<uintptr_t>(g_origActorUpdate));
        return true;
    }

    // Client → host: forward `dmg` of client-dealt damage for the host enemy
    // identified by hostRefFormID (the g_npcRemap key). Reliable: cumulative
    // deltas, a dropped packet = lost damage. Cheap (9 bytes); fires a few
    // times/sec per actively-shot enemy during combat.
    static void SendNpcDamage(uint32_t hostRefFormID, float dmg, bool kill = false)
    {
        if (hostRefFormID == 0) return;
        if (!kill && dmg <= 0.0f) return;
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        using namespace Pulsar::Network;
        uint8_t buf[1 + sizeof(NpcDamageData)];
        buf[0] = PKT_NPC_DAMAGE;
        NpcDamageData d{};
        d.hostRefFormID = hostRefFormID;
        d.damage        = dmg;
        d.kill          = kill ? 1 : 0;
        std::memcpy(&buf[1], &d, sizeof(d));
        net.Send(buf, sizeof(buf), /*reliable=*/true);

        if (kill) {
            PLOG_INFO("[COMBAT-FWD] client -> host KILL hostRef=0x{:08X}"sv, hostRefFormID);
            return;
        }
        static uint64_t s_lastLog = 0;
        uint64_t now = GetTickCount64();
        if (now - s_lastLog > 1000) {
            s_lastLog = now;
            PLOG_INFO("[COMBAT-FWD] client -> host hostRef=0x{:08X} dmg={:.1f}"sv,
                      hostRefFormID, dmg);
        }
    }

    void OnGameThreadTick()
    {
        // T40: PROCDUMP tick fires on BOTH host and client. Has3D-check
        // not needed - we just want raw positions of cell-native actors.
        ProcDumpTick(IsHostMode());

        if (!IsHostMode()) return;

        // Combat Sync V2 Step 1 - apply client-forwarded damage to the host's
        // authoritative enemies. The host's own per-tick capture then broadcasts
        // the reduced Health + isDead back via PKT_NPC_STATE (existing path).
        {
            std::vector<Pulsar::Network::NpcDamageData> dmgBatch;
            {
                std::lock_guard lk(g_pendingDamageMutex);
                dmgBatch.swap(g_pendingDamage);
            }
            if (!dmgBatch.empty()) {
                RE::ActorValueInfo* healthAV = g_healthAV.load(std::memory_order_acquire);
                if (!healthAV) {
                    healthAV = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("Health");
                    if (healthAV) g_healthAV.store(healthAV, std::memory_order_release);
                }
                for (const auto& d : dmgBatch) {
                    if (!healthAV) break;
                    RE::TESForm* form = RE::TESForm::LookupByID(d.hostRefFormID);
                    if (!form) continue;
                    RE::Actor* actor = form->As<RE::Actor>();
                    if (!actor && form->Is(RE::FormType::kACHR))
                        actor = reinterpret_cast<RE::Actor*>(form);
                    if (!actor || !actor->parentCell) continue;

                    RE::PlayerCharacter* hostPlayer = RE::PlayerCharacter::GetSingleton();

                    // V2-Step-1d: explicit KILL - the client's local copy of this
                    // enemy died. Kill the host's matched enemy outright, bypassing
                    // the damage-vs-regen race (some enemies regen faster than the
                    // forwarded net damage arrives, e.g. 0xFF0152AA oscillating 26..36).
                    if (d.kill) {
                        bool wasDead = actor->IsDead();
                        if (!wasDead) SafeKillActor(actor, hostPlayer);
                        PLOG_INFO("[COMBAT-FWD] host KILL-forward hostRef=0x{:08X} wasDead={}"sv,
                                  d.hostRefFormID, wasDead);
                        continue;
                    }

                    // V2-Step-1b: apply via the kDamage AV-modifier path (engine
                    // damage notification → triggers death/ragdoll/flags), NOT a
                    // blunt SetActorValue which leaves the actor alive at <=0 HP
                    // (test 14:17 showed 0x dead=1, health went to -117). kDamage
                    // accumulates the negative delta, so no read-modify-write.
                    float before = actor->GetActorValue(*healthAV);
                    actor->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *healthAV,
                                         -d.damage, hostPlayer);
                    float after = actor->GetActorValue(*healthAV);

                    // V2-Step-1b: ModActorValue reduces health but does NOT trigger
                    // the death handler (test 14:46 confirmed). When effective health
                    // crosses <=0, fire the RE'd engine kill so the host enemy actually
                    // dies (ragdoll + kDead) → the host's capture then broadcasts isDead.
                    bool killed = false;
                    if (after <= 0.0f && !actor->IsDead()) {
                        killed = (SafeKillActor(actor, hostPlayer) != 0);
                    }

                    PLOG_INFO("[COMBAT-FWD] host applied hostRef=0x{:08X} dmg={:.1f} health {:.1f} -> {:.1f} dead={} killFired={}"sv,
                              d.hostRefFormID, d.damage, before, after, actor->IsDead(), killed);
                }
            }
        }

        // Corpse-loot sync - drain due one-shot inventory broadcasts (host).
        {
            std::vector<uint32_t> dueRefs;
            {
                std::lock_guard clk(g_corpseLootMutex);
                const uint64_t now = GetTickCount64();
                for (auto it = g_corpseLootDueMs.begin(); it != g_corpseLootDueMs.end();) {
                    if (now >= it->second) {
                        dueRefs.push_back(it->first);
                        it = g_corpseLootDueMs.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (uint32_t refID : dueRefs) {
                RE::TESForm* form = RE::TESForm::LookupByID(refID);
                if (!form) {
                    PLOG_INFO("[CORPSE-LOOT] drain ref=0x{:08X} - form gone (despawned)"sv, refID);
                    continue;
                }
                RE::Actor* corpse = form->As<RE::Actor>();
                if (!corpse && form->Is(RE::FormType::kACHR))
                    corpse = reinterpret_cast<RE::Actor*>(form);
                if (!corpse) {
                    PLOG_INFO("[CORPSE-LOOT] drain ref=0x{:08X} - not an actor (type=0x{:02X})"sv,
                              refID, static_cast<uint8_t>(form->GetFormType()));
                    continue;
                }

                CorpseInvEntry inv[Pulsar::Network::kCorpseLootMaxItems]{};
                const int n = SafeCollectInventory(
                    corpse, inv, Pulsar::Network::kCorpseLootMaxItems);
                if (n <= 0) {
                    PLOG_INFO("[CORPSE-LOOT] drain ref=0x{:08X} - inventory empty (n=0)"sv, refID);
                    continue;
                }

                std::vector<uint8_t> buf;
                buf.reserve(1 + sizeof(Pulsar::Network::CorpseLootHeader)
                            + n * (sizeof(Pulsar::Network::CorpseLootItem)
                                   + 8ull * sizeof(Pulsar::Network::LootDropMod)));
                buf.push_back(static_cast<uint8_t>(Pulsar::Network::PKT_CORPSE_LOOT));
                Pulsar::Network::CorpseLootHeader hdr{};
                hdr.hostRefFormID = refID;
                hdr.itemCount     = static_cast<uint8_t>(n);
                const auto* hp = reinterpret_cast<const uint8_t*>(&hdr);
                buf.insert(buf.end(), hp, hp + sizeof(hdr));

                for (int i = 0; i < n; ++i) {
                    std::vector<uint32_t> mods;
                    uint16_t sentinel = 0xFFFF;
                    Pulsar::WorldSync::Loot::ReadItemMods(
                        static_cast<RE::TESObjectREFR*>(corpse), inv[i].formID,
                        mods, sentinel);
                    if (mods.size() > Pulsar::Network::kEquipV2MaxModsPerItem)
                        mods.resize(Pulsar::Network::kEquipV2MaxModsPerItem);

                    Pulsar::Network::CorpseLootItem w{};
                    w.base        = inv[i].formID;
                    w.localID     = PluginLookup::FormIDToLocalID(inv[i].formID);
                    PluginLookup::FormIDToPluginName(inv[i].formID, w.plugin, sizeof(w.plugin));
                    w.count       = inv[i].count;
                    w.modCount    = static_cast<uint8_t>(mods.size());
                    w.modSentinel = sentinel;
                    const auto* wp = reinterpret_cast<const uint8_t*>(&w);
                    buf.insert(buf.end(), wp, wp + sizeof(w));
                    for (uint32_t modID : mods) {
                        Pulsar::Network::LootDropMod m{};
                        m.base    = modID;
                        m.localID = PluginLookup::FormIDToLocalID(modID);
                        PluginLookup::FormIDToPluginName(modID, m.plugin, sizeof(m.plugin));
                        const auto* mp = reinterpret_cast<const uint8_t*>(&m);
                        buf.insert(buf.end(), mp, mp + sizeof(m));
                    }
                }

                Pulsar::Network::GetSingleton().Send(buf.data(), buf.size(), true);
                PLOG_INFO("[CORPSE-LOOT] TX ref=0x{:08X} items={} bytes={}"sv,
                          refID, n, static_cast<uint32_t>(buf.size()));
            }
        }

        // Periodic diagnostic - run even while not connected so we can verify the hook
        // standalone. Kept at the top so an early-return doesn't swallow it.
        uint64_t tickNow = GetTickCount64();
        static uint64_t s_lastLog = 0;
        if (tickNow - s_lastLog > 3000) {
            s_lastLog = tickNow;
            size_t n;
            {
                std::lock_guard lock(g_trackedMutex);
                n = g_tracked.size();
            }
            size_t cacheSz;
            {
                std::lock_guard lk(g_tierCacheMutex);
                cacheSz = g_tierCache.size();
            }
            uint32_t captures   = g_dbgSameCellCaptures.exchange(0, std::memory_order_relaxed);
            uint64_t civilians  = g_dbgFilteredCivilian.exchange(0, std::memory_order_relaxed);
            uint64_t promotions = g_dbgPromotedTracked.exchange(0, std::memory_order_relaxed);
            PLOG_INFO("[PULSAR] NPCSync: tracked={} captures/3s={} tierEvals/3s civilian={} tracked={} cache={}"sv,
                      n, captures, civilians, promotions, cacheSz);
        }

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;
        uint32_t cellFormID = player->parentCell->formID;

        // V4 cell-change-clear removed. New Atlantis spans many cells
        // (Concourse 0x0001259C, MAST 0x00014CB7, Spaceport 0x000151E9, …) -
        // player crossing internal boundaries triggered g_tracked.clear()
        // every few seconds. Each clear stopped broadcasts for ~100ms while
        // entries re-populated, manifesting on the client as visible NPC
        // despawn-flicker. Replaced with per-tick staleness prune below:
        // entries with no HookedActorUpdate capture in the last 5 s are
        // dropped, which is the same effect (drop entries from the previous
        // cell) without the cliff-edge timing.
        constexpr uint64_t kStaleMs = 5000;
        {
            uint64_t now2 = GetTickCount64();
            std::lock_guard lock(g_trackedMutex);
            for (auto it = g_tracked.begin(); it != g_tracked.end(); ) {
                if (it->second.lastCapturedTick != 0 &&
                    now2 - it->second.lastCapturedTick > kStaleMs)
                {
                    it = g_tracked.erase(it);
                } else {
                    ++it;
                }
            }
        }

        constexpr uint64_t kSendIntervalMs = 33;   // ~30 Hz per NPC
        constexpr size_t   kMaxPerBatch    = 16;
        uint64_t now = GetTickCount64();

        // Collect up to kMaxPerBatch due-to-send NPCs under lock
        std::vector<std::pair<uint32_t, TrackedNPC>> toSend;
        toSend.reserve(kMaxPerBatch);
        {
            std::lock_guard lock(g_trackedMutex);
            for (auto& [formID, entry] : g_tracked) {
                if (entry.everSent && now - entry.lastSentTick < kSendIntervalMs) continue;
                toSend.emplace_back(formID, entry);
                entry.lastSentTick = now;
                entry.everSent = true;
                if (toSend.size() >= kMaxPerBatch) break;
            }
        }

        if (!toSend.empty()) {
            using namespace Pulsar::Network;
            size_t packetSize = 1 + sizeof(NPCStateBatchHeader)
                              + toSend.size() * sizeof(NPCStateData);
            std::vector<uint8_t> buf(packetSize);
            buf[0] = PKT_NPC_STATE;

            NPCStateBatchHeader hdr{};
            hdr.cellFormID = cellFormID;
            hdr.count      = static_cast<uint16_t>(toSend.size());
            std::memcpy(&buf[1], &hdr, sizeof(hdr));

            auto* entries = reinterpret_cast<NPCStateData*>(&buf[1 + sizeof(hdr)]);
            bool anyDead = false;
            for (size_t i = 0; i < toSend.size(); ++i) {
                auto& [formID, src] = toSend[i];
                auto& dst = entries[i];
                dst.refFormID        = formID;
                dst.baseFormID       = src.baseFormID;
                dst.sourceBaseFormID = src.sourceBaseFormID;
                dst.sourceLocalID    = src.sourceLocalID;
                std::memcpy(dst.sourcePlugin, src.sourcePlugin,
                            Pulsar::Network::NPC_SOURCE_PLUGIN_MAX);
                dst.posX = src.posX; dst.posY = src.posY; dst.posZ = src.posZ;
                dst.rotZ = src.rotZ;
                dst.speed = src.speed;
                dst.direction = src.direction;
                dst.moveFlags = src.moveFlags;
                dst.health    = src.health;
                dst.healthMax = src.healthMax;
                dst.flags = (src.isDead ? 0x1 : 0) | (src.weaponDrawn ? 0x2 : 0);
                if (src.isDead) anyDead = true;
            }

            // Position/health packets unreliable (next cycle re-sends anyway).
            // Dead NPCs need reliable delivery so the client actually kills the actor.
            net.Send(buf.data(), buf.size(), anyDead);

            // After the final reliable send, drop dead NPCs from tracking - no
            // further updates needed for a corpse.
            if (anyDead) {
                std::lock_guard lock(g_trackedMutex);
                for (auto& [formID, data] : toSend) {
                    if (data.isDead) g_tracked.erase(formID);
                }
            }
        }

        // V5 civilian-snapshot broadcast at 2 Hz. Slim packet, no health,
        // no anim - just position. Lets client correct AI-divergence drift on
        // shared static-ref civilians while keeping bandwidth negligible.
        constexpr uint64_t kCivIntervalMs   = 500;
        constexpr size_t   kCivMaxPerBatch  = 64;   // 64 × 20 B = 1280 B payload, fits in any MTU
        constexpr uint64_t kCivStaleMs      = 5000;
        static uint64_t s_lastCivSendMs = 0;
        if (now - s_lastCivSendMs >= kCivIntervalMs) {
            s_lastCivSendMs = now;

            std::vector<std::pair<uint32_t, CivilianSnapshot>> civs;
            civs.reserve(kCivMaxPerBatch);
            {
                std::lock_guard lock(g_civilianSnapshotsMutex);
                // Prune stale (engine despawned the civilian or player walked away)
                for (auto it = g_civilianSnapshots.begin(); it != g_civilianSnapshots.end(); ) {
                    if (now - it->second.lastCapturedTick > kCivStaleMs) {
                        it = g_civilianSnapshots.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto& [refID, s] : g_civilianSnapshots) {
                    civs.emplace_back(refID, s);
                    if (civs.size() >= kCivMaxPerBatch) break;
                }
            }

            if (!civs.empty()) {
                using namespace Pulsar::Network;
                size_t pktSize = 1 + sizeof(CivilianSnapshotHeader)
                               + civs.size() * sizeof(CivilianSnapshotData);
                std::vector<uint8_t> buf(pktSize);
                buf[0] = PKT_CIVILIAN_SNAPSHOT;

                CivilianSnapshotHeader hdr{};
                hdr.cellFormID = cellFormID;
                hdr.count      = static_cast<uint16_t>(civs.size());
                std::memcpy(&buf[1], &hdr, sizeof(hdr));

                auto* entries = reinterpret_cast<CivilianSnapshotData*>(&buf[1 + sizeof(hdr)]);
                for (size_t i = 0; i < civs.size(); ++i) {
                    auto& [refID, s] = civs[i];
                    entries[i].refFormID = refID;
                    entries[i].posX = s.posX;
                    entries[i].posY = s.posY;
                    entries[i].posZ = s.posZ;
                    entries[i].rotZ = s.rotZ;
                }

                net.Send(buf.data(), buf.size(), false);  // unreliable, next 500 ms resyncs anyway
                g_dbgCivSent.fetch_add(static_cast<uint32_t>(civs.size()), std::memory_order_relaxed);
            }
        }
    }

    // Client-side: queue of incoming updates, drained on game thread.
    struct PendingUpdate {
        uint32_t refFormID;         // Host runtime ref (often 0xFF - won't resolve locally)
        uint32_t baseFormID;        // Host's direct base - may be 0xFF (clone of leveled list)
        uint32_t sourceBaseFormID;  // Static source template (T28, +0x130). HOST-LOCAL formID. 0 if base is static.
        uint32_t sourceLocalID;     // Plugin-local portion - flag-aware (regular=24, medium=16, light=12 bits). 0 if no sourcePlugin.
        char     sourcePlugin[Pulsar::Network::NPC_SOURCE_PLUGIN_MAX]; // Plugin filename owning the static source on host. Empty => fall back to legacy direct-ID path.
        float    posX, posY, posZ;
        float    rotZ;
        float    speed;
        float    direction;
        uint8_t  moveFlags;
        float    health, healthMax;
        bool     isDead;
        bool     weaponDrawn;
    };

    // Client-side: resolved cache mapping host runtime FormID → local Actor FormID.
    // Populated lazily by BaseForm+Position search. Avoids re-scanning per packet.
    static std::unordered_map<uint32_t, uint32_t> g_clientIdCache;

    // Client-side: set of local Actor FormIDs that we've successfully applied a
    // remote update to. Used by the Actor::Update hook to SKIP the original AI
    // processing for these actors - otherwise the host-driven position fights
    // with the client AI's movement decisions 80 times per second (visible
    // jitter on Sarah and any other synced companion).
    // (declaration moved above HookedActorUpdate so the hook can reference it)
    static std::vector<PendingUpdate> g_pending;
    static std::mutex g_pendingMutex;

    void OnPacketReceived(const uint8_t* a_data, std::size_t a_length)
    {
        using namespace Pulsar::Network;
        if (a_length < 1 + sizeof(NPCStateBatchHeader)) return;

        NPCStateBatchHeader hdr{};
        std::memcpy(&hdr, a_data + 1, sizeof(hdr));

        size_t expected = 1 + sizeof(NPCStateBatchHeader)
                        + static_cast<size_t>(hdr.count) * sizeof(NPCStateData);
        if (a_length < expected) {
            PLOG_WARN("[PULSAR] NPCSync: truncated batch ({} < {})"sv, a_length, expected);
            return;
        }

        auto* entries = reinterpret_cast<const NPCStateData*>(
            a_data + 1 + sizeof(NPCStateBatchHeader));

        std::lock_guard lock(g_pendingMutex);
        g_pending.reserve(g_pending.size() + hdr.count);
        for (uint16_t i = 0; i < hdr.count; ++i) {
            const auto& e = entries[i];
            PendingUpdate u{};
            u.refFormID        = e.refFormID;
            u.baseFormID       = e.baseFormID;
            u.sourceBaseFormID = e.sourceBaseFormID;
            u.sourceLocalID    = e.sourceLocalID;
            std::memcpy(u.sourcePlugin, e.sourcePlugin, Pulsar::Network::NPC_SOURCE_PLUGIN_MAX);
            u.sourcePlugin[Pulsar::Network::NPC_SOURCE_PLUGIN_MAX - 1] = '\0';
            u.posX = e.posX; u.posY = e.posY; u.posZ = e.posZ;
            u.rotZ = e.rotZ;
            u.speed = e.speed;
            u.direction = e.direction;
            u.moveFlags = e.moveFlags;
            u.health    = e.health;
            u.healthMax = e.healthMax;
            u.isDead    = (e.flags & 0x1) != 0;
            u.weaponDrawn = (e.flags & 0x2) != 0;
            g_pending.push_back(u);
        }
    }

    // Host-side: client forwarded combat damage for a host-matched NPC.
    // ENet thread - only queues; applied on the game thread in OnGameThreadTick.
    void OnDamageReceived(const uint8_t* a_data, std::size_t a_length)
    {
        using namespace Pulsar::Network;
        if (a_length < 1 + sizeof(NpcDamageData)) return;
        NpcDamageData d{};
        std::memcpy(&d, a_data + 1, sizeof(d));
        if (d.hostRefFormID == 0) return;
        if (d.kill == 0 && d.damage <= 0.0f) return;  // kill packets carry damage=0
        std::lock_guard lk(g_pendingDamageMutex);
        g_pendingDamage.push_back(d);
    }

    // Corpse-loot sync: parse + cross-plugin-resolve on the ENet thread, queue
    // keyed by HOST refID; ApplyPendingRemoteState applies it on the game
    // thread once the matched local corpse is dead.
    void OnCorpseLootReceived(const uint8_t* a_data, std::size_t a_length)
    {
        using namespace Pulsar::Network;
        if (!a_data || a_length < 1 + sizeof(CorpseLootHeader)) return;
        CorpseLootHeader hdr{};
        std::memcpy(&hdr, a_data + 1, sizeof(hdr));
        if (hdr.hostRefFormID == 0 || hdr.itemCount > kCorpseLootMaxItems) return;

        std::size_t off = 1 + sizeof(CorpseLootHeader);
        std::vector<CorpseItemResolved> items;
        items.reserve(hdr.itemCount);
        for (uint8_t i = 0; i < hdr.itemCount; ++i) {
            if (off + sizeof(CorpseLootItem) > a_length) return;  // malformed
            CorpseLootItem w{};
            std::memcpy(&w, a_data + off, sizeof(w));
            off += sizeof(w);
            if (w.modCount > kEquipV2MaxModsPerItem) return;
            if (off + static_cast<std::size_t>(w.modCount) * sizeof(LootDropMod) > a_length) return;

            w.plugin[sizeof(w.plugin) - 1] = '\0';
            uint32_t resolved = 0;
            if (w.plugin[0] != '\0') {
                resolved = PluginLookup::ResolveCrossPlugin(w.plugin, w.localID);
            }
            if (!resolved) resolved = w.base;

            CorpseItemResolved e{};
            e.formID      = resolved;
            e.count       = w.count;
            e.modSentinel = w.modSentinel;
            e.mods.reserve(w.modCount);
            for (uint8_t mi = 0; mi < w.modCount; ++mi) {
                LootDropMod m{};
                std::memcpy(&m, a_data + off, sizeof(m));
                off += sizeof(m);
                m.plugin[sizeof(m.plugin) - 1] = '\0';
                uint32_t rm = 0;
                if (m.plugin[0] != '\0') {
                    rm = PluginLookup::ResolveCrossPlugin(m.plugin, m.localID);
                }
                if (!rm) rm = m.base;
                e.mods.push_back(rm);
            }
            if (e.formID && e.count > 0) items.push_back(std::move(e));
        }

        PLOG_INFO("[CORPSE-LOOT] RX hostRef=0x{:08X} items={}"sv,
                  hdr.hostRefFormID, static_cast<uint32_t>(items.size()));
        std::lock_guard clk(g_corpseLootMutex);
        g_corpseLootPending[hdr.hostRefFormID] = std::move(items);
    }

    // V5 receive periodic civilian position resync from host.
    void OnCivilianSnapshotReceived(const uint8_t* a_data, std::size_t a_length)
    {
        using namespace Pulsar::Network;
        if (a_length < 1 + sizeof(CivilianSnapshotHeader)) return;

        CivilianSnapshotHeader hdr{};
        std::memcpy(&hdr, a_data + 1, sizeof(hdr));

        const size_t expected = 1 + sizeof(CivilianSnapshotHeader)
                              + static_cast<size_t>(hdr.count) * sizeof(CivilianSnapshotData);
        if (a_length < expected) {
            PLOG_WARN("[PULSAR] CivilianSnapshot: truncated batch ({} < {})"sv, a_length, expected);
            return;
        }

        auto* entries = reinterpret_cast<const CivilianSnapshotData*>(
            a_data + 1 + sizeof(CivilianSnapshotHeader));

        std::lock_guard lock(g_pendingCiviliansMutex);
        g_pendingCivilians.reserve(g_pendingCivilians.size() + hdr.count);
        for (uint16_t i = 0; i < hdr.count; ++i) {
            g_pendingCivilians.push_back(entries[i]);
        }
    }

    void ApplyPendingRemoteState()
    {
        if (IsHostMode()) return;

        // Phase D: apply runs on client - matcher-based (no PlaceAtMe).
        // Find local native NPC by (baseFormID + position-proximity),
        // apply position/health from host packet.

        // Only apply NPC state once we're fully loaded into the host's world.
        // During WAITING_FOR_SAVE / SAVE_RECEIVED / PENDING_TRIGGER / LOADING,
        // the client's actors are in transient states (loading screen, partial
        // cell load). Calling SetPosition/SetActorValue on them crashes.
        if (Pulsar::WorldSync::GetState() != Pulsar::WorldSync::State::LOADED) {
            // Drop any queued packets so we don't replay stale updates after LOADED fires.
            {
                std::lock_guard lock(g_pendingMutex);
                g_pending.clear();
            }
            {
                std::lock_guard lock(g_pendingCiviliansMutex);
                g_pendingCivilians.clear();
            }
            return;
        }

        // V5 drain civilian-snapshot queue. Independent of g_pending -
        // civilians arrive at 2 Hz from host, only carry pos+rotZ+refFormID,
        // are direct-LookupByID (static-ref only). Drift threshold prevents
        // hot-snap on actors that are already close enough.
        {
            std::vector<Pulsar::Network::CivilianSnapshotData> civWork;
            {
                std::lock_guard lock(g_pendingCiviliansMutex);
                civWork.swap(g_pendingCivilians);
            }
            if (!civWork.empty()) {
                int civApplied = 0, civSkipDrift = 0, civNoForm = 0;
                constexpr float kSnapDriftSq = 5.0f * 5.0f;  // only correct if > 5 m
                for (auto& c : civWork) {
                    auto* form = RE::TESForm::LookupByID(c.refFormID);
                    if (!form) { civNoForm++; continue; }
                    auto* civActor = form->As<RE::Actor>();
                    if (!civActor && form->Is(RE::FormType::kACHR)) {
                        civActor = reinterpret_cast<RE::Actor*>(form);
                    }
                    if (!civActor || !civActor->parentCell) { civNoForm++; continue; }

                    const float ddx = c.posX - civActor->data.location.x;
                    const float ddy = c.posY - civActor->data.location.y;
                    const float ddz = c.posZ - civActor->data.location.z;
                    if (ddx*ddx + ddy*ddy + ddz*ddz < kSnapDriftSq) {
                        civSkipDrift++;
                        continue;
                    }
                    RE::NiPoint3A target(c.posX, c.posY, c.posZ);
                    SafeNpcSetPosition(civActor, target, c.rotZ);
                    civApplied++;
                }
                g_dbgCivApplied.fetch_add(civApplied, std::memory_order_relaxed);
                g_dbgCivSkipDrift.fetch_add(civSkipDrift, std::memory_order_relaxed);

                static uint64_t s_lastCivLog = 0;
                uint64_t nowT = GetTickCount64();
                if (nowT - s_lastCivLog > 5000) {
                    s_lastCivLog = nowT;
                    PLOG_INFO("[PULSAR] CivilianSnapshot apply: batch={} applied={} skipDrift={} noForm={}"sv,
                              civWork.size(), civApplied, civSkipDrift, civNoForm);
                }
            }
        }

        // Swap out the pending queue to minimise lock hold time
        std::vector<PendingUpdate> work;
        {
            std::lock_guard lock(g_pendingMutex);
            work.swap(g_pending);
        }
        if (work.empty()) return;

        // Cache Health form for SetActorValue calls
        auto* healthAV = g_healthAV.load(std::memory_order_acquire);
        if (!healthAV) {
            healthAV = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("Health");
            if (healthAV) g_healthAV.store(healthAV, std::memory_order_release);
        }

        int applied = 0, skippedNoForm = 0, skippedNoCell = 0, spawned = 0;

        for (auto& u : work) {
            // Resolve actor with LookupByID(refFormID) primary: works for
            // static-ref NPCs (Sarah, companions, named quest NPCs) that
            // exist on both host and client. After the WorldSync save
            // transfer, the client's Sarah has loaded 3D in the planet cell
            // (confirmed: user sees her). We just need to drive her position
            // from host packets and neutralize her local AI.
            //
            // Spawn fallback (for Phase C dynamic 0xFF refs that don't exist
            // on client) is currently disabled - FUN_14179A510 returns null
            // for TESNPC source. Needs T30 RE before re-enable.
            RE::Actor* actor = nullptr;

            // First: check remap (covers any previously-spawned local clones)
            uint32_t localRefID = 0;
            {
                std::lock_guard lk(g_npcRemapMutex);
                auto it = g_npcRemap.find(u.refFormID);
                if (it != g_npcRemap.end()) localRefID = it->second;
            }
            if (localRefID) {
                RE::TESForm* form = RE::TESForm::LookupByID(localRefID);
                if (form) {
                    actor = form->As<RE::Actor>();
                    // Fallback: if As<Actor>() fails despite form being real
                    // (possible if the engine hasn't fully initialized the
                    // class hierarchy yet), use the form directly - our
                    // SEH-wrapped SetPosition calls it via Actor's vtable
                    // regardless, and if it's not an Actor the SEH catches.
                    if (!actor && form->Is(RE::FormType::kACHR)) {
                        actor = reinterpret_cast<RE::Actor*>(form);
                    }
                }
                // Only erase remap if form is DEFINITIVELY gone - not on a
                // transient As<Actor> cast failure. Keep retrying.
                if (!actor && !form) {
                    static uint64_t s_lastRemapGoneLog = 0;
                    uint64_t nowT = GetTickCount64();
                    if (nowT - s_lastRemapGoneLog > 3000) {
                        s_lastRemapGoneLog = nowT;
                        PLOG_WARN("[PULSAR] NPCSync: remap gone hostRef=0x{:08X} localRef=0x{:08X} - form lookup returns null"sv,
                                  u.refFormID, localRefID);
                    }
                    {
                        std::lock_guard lk(g_npcRemapMutex);
                        g_npcRemap.erase(u.refFormID);
                    }
                    // Allow a fresh spawn attempt. Previously g_spawnAttempted kept
                    // the entry forever after the first successful spawn, so once
                    // the engine GC'd a clone we'd never bring it back - stuck at
                    // applied=3 noForm=8 even though the host still sends updates.
                    {
                        std::lock_guard lk(g_spawnAttemptedMutex);
                        g_spawnAttempted.erase(u.refFormID);
                    }
                } else if (!actor) {
                    static uint64_t s_lastCastFailLog = 0;
                    uint64_t nowT = GetTickCount64();
                    if (nowT - s_lastCastFailLog > 3000) {
                        s_lastCastFailLog = nowT;
                        PLOG_WARN("[PULSAR] NPCSync: remap cast-fail hostRef=0x{:08X} localRef=0x{:08X} formType={} - keeping remap, retry next tick"sv,
                                  u.refFormID, localRefID,
                                  form ? (int)form->GetFormType() : -1);
                    }
                }
            }

            // 2026-04-20: native LookupByID(refFormID) primary path REMOVED.
            // Reason: on planets, Sarah's (and other companions') native refs
            // exist on client but have has3D=false because their home cells
            // (Frontier interior etc.) aren't loaded. Driving them with
            // SetPosition writes positions nobody can see.
            //
            // New unified path: every host-tracked NPC gets a fresh local
            // clone via Papyrus PlaceAtMe. The native ref keeps doing its
            // own thing (unloaded); our local clone is what the client sees.

            // Phase D: if no remap, find a local native actor matching
            // (effectiveBaseID + spawn-position). T40 verified proc-gen is
            // deterministic - same save → same NPCs at (almost) same
            // positions on both machines. We don't spawn anything
            // client-side: the engine already did that natively.
            //
            // Matcher uses SOURCE TEMPLATE (T28 static base) when available,
            // since dynamic clone FormIDs differ per-machine. Host sends
            // sourceBaseFormID for dynamic-base NPCs; static-base NPCs
            // (Sarah etc.) match directly via baseFormID.
            if (!actor) {
                uint32_t hostEffective = (u.sourceBaseFormID != 0) ? u.sourceBaseFormID : u.baseFormID;
                // Build claimed-set from the existing remap so each local
                // native can only be matched to ONE host-NPC. Prevents
                // tele-jitter when multiple host enemies share a base
                // (e.g. 4× src=0x00259DC6) and the nearest local is the
                // same for two of them.
                std::unordered_set<uint32_t> claimed;
                {
                    std::lock_guard lk(g_npcRemapMutex);
                    claimed.reserve(g_npcRemap.size());
                    for (auto& [hr, lr] : g_npcRemap) claimed.insert(lr);
                }
                uint32_t matchRef = FindLocalMatch(hostEffective, u.posX, u.posY, u.posZ, claimed);
                if (matchRef == 0) {
                    // Engine hasn't proc-gen'd this NPC yet, or it's out of
                    // our 400m collection range. Skip - retry when it
                    // appears in g_procDumpTableV3 via HookedActorUpdate.
                    skippedNoForm++;
                    continue;
                }
                // Try resolving BEFORE writing remap - if LookupByID fails,
                // don't commit (would trap us in remap-gone-erase-rematch loop).
                RE::Actor* matchedActor = nullptr;
                if (auto* form = RE::TESForm::LookupByID(matchRef)) {
                    matchedActor = form->As<RE::Actor>();
                    if (!matchedActor && form->Is(RE::FormType::kACHR)) {
                        matchedActor = reinterpret_cast<RE::Actor*>(form);
                    }
                }
                if (!matchedActor) {
                    // matchRef is stale (actor destroyed between PROCDUMP
                    // capture and apply). Evict from candidate table and
                    // skip this tick - next match attempt sees a fresh pool.
                    std::lock_guard lk(g_procDumpMutex);
                    g_procDumpTableV3.erase(matchRef);
                    skippedNoForm++;
                    continue;
                }
                actor = matchedActor;
                {
                    std::lock_guard lk(g_npcRemapMutex);
                    g_npcRemap[u.refFormID] = matchRef;
                }
                // NOTE: no g_remoteOwned.insert - with Phase D passthrough,
                // engine Update runs natively for ALL actors on client. The
                // remote-owned set is legacy from c5afce5 PlaceAtMe path.
                PLOG_INFO("[PULSAR] NPCSync: MATCHED hostRef=0x{:08X} base=0x{:08X} src=0x{:08X} hostPos=({:.0f},{:.0f},{:.0f}) -> localRef=0x{:08X}"sv,
                          u.refFormID, u.baseFormID, u.sourceBaseFormID,
                          u.posX, u.posY, u.posZ, matchRef);
            }

            // Dead Papyrus-trampoline spawn code below retained for now
            // (unused). Phase D strip will remove entirely once matcher
            // is proven on-console. Guarded on `false` to dead-strip at
            // link time without disturbing the file layout for review.
            if (false) {
                const uint32_t inFlight = g_spawnInFlightHostRef.load(std::memory_order_acquire);
                const float curSpawnState = ReadGlob("PulsarSpawn");

                // (1) Completion polling - in-flight spawn ready to collect?
                if (inFlight == u.refFormID && curSpawnState == 11.0f) {
                    // Papyrus splits signed int32 FormIDs across two GLOBs
                    // using signed division: `hi = formID / 65536; lo = formID - hi*65536`.
                    // The correct decode is the INVERSE signed arithmetic:
                    // `formID = (int32)hi * 65536 + (int32)lo` - same pattern
                    // as RemotePlayerManager::ResolveLookMarker (line ~1041).
                    // Earlier bit-shift reassembly `(hi<<16)|(lo&0xFFFF)`
                    // silently corrupts 0xFF-prefix IDs (signed negative) -
                    // e.g. hi=-254 lo=-63631 → real 0xFEFFF071 but shift-based
                    // decode gave 0xFF020771 (diff of +0x0300 = lo-byte bits
                    // from high-half negative sign-extension).
                    float fHi = ReadGlob("PulsarDropHi");
                    float fLo = ReadGlob("PulsarDropLo");
                    uint32_t newLocalID = static_cast<uint32_t>(
                        static_cast<int32_t>(fHi) * 65536 + static_cast<int32_t>(fLo));

                    PLOG_INFO("[PULSAR] NPCSync: raw GLOBs Drop={:.1f}/{:.1f} → localRef=0x{:08X}"sv,
                              fHi, fLo, newLocalID);

                    // Clear trampoline slot and restore PulsarSpawn to 3.0
                    // (C++-takeover steady-state for player-clone handling).
                    WriteGlob("PulsarDropHi", 0.0f);
                    WriteGlob("PulsarDropLo", 0.0f);
                    WriteGlob("PulsarSpawn", 3.0f);
                    g_spawnInFlightHostRef.store(0, std::memory_order_release);

                    if (newLocalID != 0) {
                        {
                            std::lock_guard lk(g_npcRemapMutex);
                            g_npcRemap[u.refFormID] = newLocalID;
                        }
                        // Mark remote-owned IMMEDIATELY so the anti-dup hook
                        // doesn't disable our own fresh spawn in the window
                        // between Papyrus DONE and first-sync apply. Without
                        // this, HookedActorUpdate (running next frame) would
                        // see a dynamic-ref actor in the proc cell that
                        // isn't yet in g_remoteOwned and Disable() it.
                        {
                            std::lock_guard lk(g_remoteOwnedMutex);
                            g_remoteOwned.insert(newLocalID);
                        }
                        spawned++;
                        PLOG_INFO("[PULSAR] NPCSync: Papyrus spawn DONE hostRef=0x{:08X} base=0x{:08X} -> localRef=0x{:08X}"sv,
                                  u.refFormID, u.baseFormID, newLocalID);

                        // Chain-request: disable the native ref - only if
                        // the host ref is static (same FormID exists on
                        // client). For dynamic refs (POI-spawned mobs), no
                        // client-side native exists, skip.
                        if ((u.refFormID & 0xFF000000) != 0xFF000000) {
                            // Hand Papyrus the flag-aware plugin-local ID so
                            // its plugin-iteration (DoDisableNative) works for
                            // DLC/ESL refs, not just Starfield.esm.
                            uint32_t refLocalID =
                                Pulsar::NPCSync::PluginLookup::FormIDToLocalID(u.refFormID);
                            WriteGlob("PulsarDropHi", static_cast<float>((refLocalID >> 16) & 0xFFFF));
                            WriteGlob("PulsarDropLo", static_cast<float>(refLocalID & 0xFFFF));
                            WriteGlob("PulsarSpawn", 12.0f);
                            PLOG_INFO("[PULSAR] NPCSync: queued disable-native hostRef=0x{:08X} localID=0x{:08X}"sv,
                                      u.refFormID, refLocalID);
                        }

                        // Resolve actor immediately so we can apply position this tick
                        if (auto* form = RE::TESForm::LookupByID(newLocalID)) {
                            actor = form->As<RE::Actor>();
                            if (!actor && form->Is(RE::FormType::kACHR)) {
                                actor = reinterpret_cast<RE::Actor*>(form);
                            }
                        }
                    } else {
                        PLOG_WARN("[PULSAR] NPCSync: Papyrus spawn FAILED (null result) hostRef=0x{:08X} base=0x{:08X}"sv,
                                  u.refFormID, u.baseFormID);
                        // Leave g_spawnAttempted set so we don't retry
                    }
                }
                // (2) Initiation - start a new spawn if slot is free and
                //     Papyrus is idle. Accept both 2.0 (Papyrus-fallback
                //     movement) and 3.0 (C++-takeover steady-state). After
                //     initial clone spawn + C++ LookupByID, state sits at
                //     3.0 - that's the normal case we hit during gameplay.
                else if (inFlight == 0 && (curSpawnState == 2.0f || curSpawnState == 3.0f)) {
                    bool alreadyTried = false;
                    {
                        std::lock_guard lk(g_spawnAttemptedMutex);
                        alreadyTried = !g_spawnAttempted.insert(u.refFormID).second;
                    }
                    if (alreadyTried) { skippedNoForm++; continue; }

                    // Pick the effective host-side static base:
                    // baseFormID is dynamic (0xFF) → use T28 source template
                    // otherwise                    → use baseFormID directly
                    const bool needsSource =
                        ((u.baseFormID & 0xFF000000) == 0xFF000000) && (u.sourceBaseFormID != 0);
                    const uint32_t hostStaticFormID = needsSource ? u.sourceBaseFormID : u.baseFormID;

                    // Decide what plugin-local ID to hand to Papyrus.
                    // Prefer the flag-aware host value (T34); it is plugin-encoded
                    // (12/16/24-bit masked correctly for ESL/medium/regular) and
                    // identical across load orders since the plugin file is the
                    // same binary on both machines.
                    // Fallback: legacy modulo-strip for old host versions that
                    // didn't send sourceLocalID.
                    uint32_t spawnLocalID = 0;
                    if (u.sourceLocalID != 0) {
                        spawnLocalID = u.sourceLocalID;
                    } else if (hostStaticFormID != 0 &&
                               (hostStaticFormID & 0xFF000000) != 0xFF000000) {
                        // Local-ID extraction without flag info - safe for
                        // regular-ESM (all vanilla + SS DLC) but wrong for ESL.
                        spawnLocalID = hostStaticFormID & 0xFFFFFFu;
                    }
                    if (spawnLocalID == 0) {
                        PLOG_WARN("[PULSAR] NPCSync: Papyrus spawn FAIL-NO-LOCALID hostRef=0x{:08X} base=0x{:08X} source=0x{:08X} plugin='{}'"sv,
                                  u.refFormID, u.baseFormID, u.sourceBaseFormID,
                                  u.sourcePlugin[0] ? u.sourcePlugin : "(empty)");
                        skippedNoForm++;
                        continue;
                    }

                    // Sanity probe - if the host told us the plugin name AND
                    // this machine doesn't have it loaded, log once per 3s
                    // so the user knows why the spawn is going to fail.
                    // Papyrus iterates known plugins anyway, so this is
                    // informational, not a hard-skip.
                    if (u.sourcePlugin[0] != '\0') {
                        uint32_t probe = Pulsar::NPCSync::PluginLookup::ResolveCrossPlugin(
                            u.sourcePlugin, u.sourceLocalID);
                        if (probe == 0) {
                            static uint64_t s_lastNoPluginLog = 0;
                            uint64_t nowT = GetTickCount64();
                            if (nowT - s_lastNoPluginLog > 3000) {
                                s_lastNoPluginLog = nowT;
                                PLOG_WARN("[PULSAR] NPCSync: plugin '{}' not loaded on client - hostRef=0x{:08X} spawn will likely fail"sv,
                                          u.sourcePlugin, u.refFormID);
                            }
                        }
                    }

                    // Write flag-aware plugin-local ID + trigger Papyrus.
                    // Papyrus iterates {Starfield.esm, ShatteredSpace.esm, …} until
                    // one GetFormFromFile returns non-None.
                    WriteGlob("PulsarDropHi", static_cast<float>(spawnLocalID >> 16));
                    WriteGlob("PulsarDropLo", static_cast<float>(spawnLocalID & 0xFFFF));
                    // Target position for post-spawn engine-native SetPosition.
                    // Papyrus `newRef.SetPosition(x,y,z)` right after PlaceAtMe
                    // triggers cell-reattach + 3D-stream AT the target cell.
                    // Without this, PlaceAtMe spawns at player position then
                    // our subsequent raw data.location writes move the actor
                    // but engine has already deprioritized 3D-load because
                    // actor was "far" at spawn time → has3D=false forever.
                    // Reuses PulsarPosX/Y/Z GLOBs (normally player-clone pos);
                    // brief ~100ms transient during spawn is harmless.
                    WriteGlob("PulsarPosX", u.posX);
                    WriteGlob("PulsarPosY", u.posY);
                    WriteGlob("PulsarPosZ", u.posZ);
                    g_spawnInFlightHostRef.store(u.refFormID, std::memory_order_release);
                    g_spawnInFlightDeadline.store(GetTickCount64() + 3000, std::memory_order_release);
                    WriteGlob("PulsarSpawn", 10.0f);

                    PLOG_INFO("[PULSAR] NPCSync: Papyrus spawn START hostRef=0x{:08X} base=0x{:08X} source=0x{:08X} plugin='{}' localID=0x{:08X}"sv,
                              u.refFormID, u.baseFormID, u.sourceBaseFormID,
                              u.sourcePlugin[0] ? u.sourcePlugin : "(empty)",
                              spawnLocalID);
                    // Don't apply this tick - wait for Papyrus to complete
                    skippedNoForm++;
                    continue;
                }
                // (3) Otherwise: either another hostRef is in-flight (wait)
                //     or Papyrus is mid-operation (wait). Skip this tick.
                else {
                    // Deadline check - if in-flight for >3s, Papyrus got stuck,
                    // abandon and let the state recover.
                    if (inFlight != 0 && GetTickCount64() > g_spawnInFlightDeadline.load(std::memory_order_acquire)) {
                        PLOG_WARN("[PULSAR] NPCSync: Papyrus spawn TIMEOUT hostRef=0x{:08X} - resetting slot"sv, inFlight);
                        WriteGlob("PulsarSpawn", 3.0f);
                        WriteGlob("PulsarDropHi", 0.0f);
                        WriteGlob("PulsarDropLo", 0.0f);
                        g_spawnInFlightHostRef.store(0, std::memory_order_release);
                    }
                    // Throttled diagnostic so we can see why initiation isn't firing
                    static uint64_t s_lastWaitLog = 0;
                    uint64_t nowT = GetTickCount64();
                    if (nowT - s_lastWaitLog > 3000) {
                        s_lastWaitLog = nowT;
                        PLOG_INFO("[PULSAR] NPCSync: spawn-wait hostRef=0x{:08X} base=0x{:08X} inFlight=0x{:08X} curSpawn={:.1f}"sv,
                                  u.refFormID, u.baseFormID, inFlight, curSpawnState);
                    }
                    skippedNoForm++;
                    continue;
                }
            }

            if (!actor) { skippedNoForm++; continue; }

            // Phase D v10 ISOLATION: parent-cell pin DISABLED. Was needed
            // for PlaceAtMe'd clones whose cell got nulled by proc-gen.
            // Native actors manage their own parentCell - pinning could
            // corrupt engine state.

            if (!actor->parentCell) { skippedNoCell++; continue; }
            applied++;

            // Phase D v10 ISOLATION: disable ALL side-effects on matched
            // actors. No g_remoteOwned insert, no pacify, no nothing.
            // Match stays registered in g_npcRemap. User test will tell
            // us whether native NPCs render when we don't touch them.
            bool firstSync = false;
            {
                std::lock_guard lock(g_firstSyncedMutex);
                firstSync = g_firstSynced.insert(actor->formID).second;
            }

            if (firstSync) {
                // Diagnostic on first sync: is the actor's 3D actually loaded?
                // If has3D=false the SetPosition writes land on an unloaded
                // ref - invisible to the player. Indicates we need proper
                // cell migration (MoveTo or SetParentCell via vtable), not
                // just a pointer pin. Uses BSAutoReadLock → can't __try here,
                // but the lock+check itself is engine-safe.
                bool has3D = false;
                uint32_t npcCell = 0, playerCell = 0;
                {
                    auto guard = actor->loadedData.LockRead();
                    const auto* data = *guard;
                    has3D = (data && data->data3D);
                }
                if (actor->parentCell) npcCell = actor->parentCell->formID;
                if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                    if (p->parentCell) playerCell = p->parentCell->formID;
                }
                PLOG_INFO("[PULSAR] NPCSync: first-sync hostRef=0x{:08X} actor=0x{:08X} npcCell=0x{:08X} playerCell=0x{:08X} has3D={} tgt=({:.0f},{:.0f},{:.0f})"sv,
                          u.refFormID, actor->formID, npcCell, playerCell,
                          has3D, u.posX, u.posY, u.posZ);
            }

            RE::NiPoint3A target(u.posX, u.posY, u.posZ);

            // Diagnostic: compare data.location (what HUD reads) vs
            // NiAVObject.world.translate (what renderer draws). If they
            // diverge → transform-propagation bug. Rate-limited 1/2s,
            // rotating actors so we see different NPCs.
            static uint64_t s_lastSyncDiagTick = 0;
            static uint32_t s_lastSyncDiagActor = 0;
            uint64_t nowTD = GetTickCount64();
            bool doDiag = (nowTD - s_lastSyncDiagTick > 2000)
                       && (actor->formID != s_lastSyncDiagActor);
            float preData[3] = { actor->data.location.x, actor->data.location.y, actor->data.location.z };
            float preMesh[3] = { 0, 0, 0 };
            RE::NiAVObject* preNode = nullptr;
            if (doDiag) {
                auto guard = actor->loadedData.LockRead();
                const auto* data = *guard;
                if (data && data->data3D) preNode = data->data3D.get();
            }
            if (preNode) SafeReadNodeWorld(preNode, preMesh);

            // Phase E - host-velocity-driven locomotion.
            //
            // Replaces the Phase D v14 throttled-SetPosition heuristic
            // (teleport-on-drift) with a proper locomotion pipeline:
            // ApplyLocomotion writes anim-graph vars (Speed/Direction/
            // iSyncIdleLocomotion/iSyncSprintState), sets Havok linear
            // velocity on the CharController, lerps rotZ, and only snaps
            // via SetPosition when positional drift exceeds ~2 m. This
            // both gives smooth remote NPC motion and keeps the Havok
            // broadcast rate low (snap only on meaningful drift), which
            // was the root mitigation for the 0x30B350E bone-array race.
            //
            // We still pre-check has3D: graph-var + velocity writes on
            // un-attached 3D are pointless, and SetPosition in that state
            // was the original crash vector. Skip entirely without 3D.
            bool has3DNow = false;
            {
                auto guard = actor->loadedData.LockRead();
                const auto* data = *guard;
                has3DNow = (data && data->data3D.get() != nullptr);
            }
            if (!has3DNow) {
                if (doDiag) {
                    PLOG_INFO("[PULSAR] NPCSync SKIP no-3D actor=0x{:08X}"sv, actor->formID);
                }
                continue;
            }

            // Phase E: host-velocity-driven locomotion.  ApplyLocomotion
            // handles the graph-var writes, Havok velocity, rotation lerp,
            // and SetPosition snap (only when drift exceeds ~2 m), replacing
            // the Phase D v14 throttled-SetPosition heuristic.
            Pulsar::NPCSync::LocomotionInput li{};
            li.targetPos   = RE::NiPoint3(u.posX, u.posY, u.posZ);
            li.targetRotZ  = u.rotZ;
            li.speed       = u.speed;
            li.direction   = u.direction;
            li.moveFlags   = u.moveFlags;
            li.weaponDrawn = u.weaponDrawn;
            Pulsar::NPCSync::ApplyLocomotion(actor, li);

            if (doDiag) {
                s_lastSyncDiagTick = nowTD;
                s_lastSyncDiagActor = actor->formID;
                float postData[3] = { actor->data.location.x, actor->data.location.y, actor->data.location.z };
                float postMesh[3] = { 0, 0, 0 };
                RE::NiAVObject* postNode = nullptr;
                {
                    auto guard = actor->loadedData.LockRead();
                    const auto* data = *guard;
                    if (data && data->data3D) postNode = data->data3D.get();
                }
                if (postNode) SafeReadNodeWorld(postNode, postMesh);

                float mDx = postMesh[0] - target.x, mDy = postMesh[1] - target.y, mDz = postMesh[2] - target.z;
                float meshDrift = std::sqrt(mDx*mDx + mDy*mDy + mDz*mDz);
                PLOG_INFO("[PULSAR] NPCSync SYNC-DIAG actor=0x{:08X} tgt=({:.1f},{:.1f},{:.1f}) preData=({:.1f},{:.1f},{:.1f}) postData=({:.1f},{:.1f},{:.1f}) preMeshW=({:.1f},{:.1f},{:.1f}) postMeshW=({:.1f},{:.1f},{:.1f}) meshDrift={:.1f}m"sv,
                          actor->formID, target.x, target.y, target.z,
                          preData[0], preData[1], preData[2],
                          postData[0], postData[1], postData[2],
                          preMesh[0], preMesh[1], preMesh[2],
                          postMesh[0], postMesh[1], postMesh[2],
                          meshDrift);
            }

            // Health + Death + client→host damage forward (Combat Sync V2 Step 1).
            if (healthAV) {
                constexpr float kDmgEps = 0.5f;
                float curHealth = actor->GetActorValue(*healthAV);

                DamageFwdState st;
                {
                    std::lock_guard lk(g_dmgFwdMutex);
                    auto& ref = g_dmgFwd[actor->formID];
                    if (ref.baseline < 0.0f) ref.baseline = curHealth;  // first sight
                    st = ref;  // snapshot for use below
                }

                // (1) Detect NEW client-dealt damage = a drop below baseline.
                // V2-Step-1e (2026-06-11): ATTRIBUTION GATE - forward only when
                // the LOCAL player actually attacked within the last 3s (attack
                // actions noted by the PerformAction hook). Without the gate,
                // health drops caused by the partner-CLONE's replayed (visual)
                // projectiles or by NPC-vs-NPC fights were misread as client
                // damage and forwarded → the peer's own shots double-counted.
                if (curHealth < st.baseline - kDmgEps) {
                    const uint64_t lastAtk =
                        Pulsar::RemotePlayer::GetLastLocalPlayerAttackMs();
                    const bool attributable =
                        lastAtk != 0 && (GetTickCount64() - lastAtk) < 3000;
                    // [COMBAT-DBG] every detected health-drop + the attribution
                    // decision. atkAge=999999 → the local-player attack hook never
                    // fired (= "my attacks don't register"); GATED → drop seen but
                    // not credited to us (clone/NPC-vs-NPC) so not forwarded.
                    const uint64_t atkAge =
                        (lastAtk == 0) ? 999999ull : (GetTickCount64() - lastAtk);
                    PLOG_INFO("[COMBAT-DBG] client-hit local=0x{:08X} host=0x{:08X} drop={:.1f} (hp {:.1f}->{:.1f}) atkAge={}ms -> {}"sv,
                              static_cast<uint32_t>(actor->formID), u.refFormID,
                              st.baseline - curHealth, st.baseline, curHealth, atkAge,
                              attributable ? "FORWARD" : "GATED(not-ours)");
                    if (attributable) {
                        float delta = st.baseline - curHealth;
                        SendNpcDamage(u.refFormID, delta);
                        std::lock_guard lk(g_dmgFwdMutex);
                        auto& ref = g_dmgFwd[actor->formID];
                        ref.baseline      = curHealth;   // baseline only descends
                        ref.clientEngaged = true;        // self-classify as a combat enemy
                        st = ref;
                    } else {
                        // Not ours - descend the baseline silently (no forward,
                        // no engage). Host authority keeps correcting health for
                        // non-engaged enemies anyway.
                        std::lock_guard lk(g_dmgFwdMutex);
                        auto& ref = g_dmgFwd[actor->formID];
                        ref.baseline = curHealth;
                        st = ref;
                    }
                }

                // (1b) V2-Step-1d: our LOCAL copy of this enemy died (client's real
                // weapons killed it) but the host's copy may still be alive - host
                // regen can outpace the forwarded net damage so it never reaches 0
                // (seen: 0xFF0152AA oscillating 26..36, never dying). Forward an
                // explicit KILL so the host kills its matched enemy outright now.
                if (st.clientEngaged && !st.killForwarded &&
                    (curHealth <= 0.0f || actor->IsDead())) {
                    SendNpcDamage(u.refFormID, 0.0f, /*kill=*/true);
                    // RAGDOLL/LOOT FIX: the AI-blocked DirectControl puppet does NOT
                    // run the engine death handler when our weapons drop it to 0 HP -
                    // it stays standing + unlootable until the host round-trip kills
                    // it (the observed bug). Force the RE'd kill on our LOCAL copy now
                    // so it ragdolls + becomes lootable immediately. TickLerp's IsDead
                    // check then drops the position-pin and releases DirectControl.
                    bool wasDead = actor->IsDead();
                    bool localKilled = false;
                    if (!wasDead) {
                        // #ragdoll-fix: unpin the DirectControl puppet BEFORE the kill
                        // so the engine death handler ragdolls a FREE body (a pinned-
                        // upright body dies without a ragdoll = "dead but standing").
                        Pulsar::NPCSync::ReleaseNpcDirectControl(
                            static_cast<uint32_t>(actor->formID));
                        localKilled = (SafeKillActor(actor, RE::PlayerCharacter::GetSingleton()) != 0);
                        // #ragdoll - hold the body free through the async death gap.
                        Pulsar::NPCSync::MarkNpcKillPending(
                            static_cast<uint32_t>(actor->formID), 1500);
                    }
                    PLOG_INFO("[COMBAT-DBG] client-KILL local=0x{:08X} host=0x{:08X} curHP={:.1f} wasDead={} localKillFired={} dead={}"sv,
                              static_cast<uint32_t>(actor->formID), u.refFormID,
                              curHealth, wasDead, localKilled, actor->IsDead());
                    std::lock_guard lk(g_dmgFwdMutex);
                    g_dmgFwd[actor->formID].killForwarded = true;
                    st.killForwarded = true;
                    // #ragdoll-fix: latch killApplied only once the actor is ACTUALLY
                    // dead. If the kill didn't take this tick (3D/havok not ready),
                    // leave it unlatched so the host-mirror branch retries next tick.
                    if (actor->IsDead()) {
                        g_dmgFwd[actor->formID].killApplied = true;
                        st.killApplied = true;
                    }
                }

                // (2) Apply host authority.
                if (u.isDead) {
                    // V2-Step-1c: host says this enemy is dead. SetActorValue(0)
                    // does NOT kill (proven 14:46) - fire the RE'd engine kill so
                    // the local matched enemy actually ragdolls. Covers host-killed
                    // enemies the client never shot (the reported gap). One-shot via
                    // killApplied; skip if the client's own combat already killed it.
                    if (!st.killApplied && !actor->IsDead()) {
                        // #ragdoll-fix: unpin the DirectControl puppet FIRST so the
                        // engine kill ragdolls a FREE body, then kill. Latch
                        // killApplied only on confirmed death so a kill that didn't
                        // take (actor 3D/havok not ready) is retried next tick.
                        Pulsar::NPCSync::ReleaseNpcDirectControl(
                            static_cast<uint32_t>(actor->formID));
                        SafeKillActor(actor, RE::PlayerCharacter::GetSingleton());
                        // #ragdoll - hold the body free through the async death gap.
                        Pulsar::NPCSync::MarkNpcKillPending(
                            static_cast<uint32_t>(actor->formID), 1500);
                        PLOG_INFO("[COMBAT-DBG] client mirrored host-KILL local=0x{:08X} host=0x{:08X} engaged={} dead={}"sv,
                                  static_cast<uint32_t>(actor->formID), u.refFormID,
                                  st.clientEngaged ? 1 : 0, actor->IsDead());
                        if (actor->IsDead()) {
                            std::lock_guard lk(g_dmgFwdMutex);
                            g_dmgFwd[actor->formID].killApplied = true;
                        }
                    }
                    // Corpse-loot sync: once the local matched corpse is actually
                    // dead, swap its locally-rolled inventory for the host's
                    // authoritative list (one-shot - pending entry is consumed).
                    if (actor->IsDead()) {
                        ApplyCorpseLootIfPending(u.refFormID, actor);
                    }
                } else if (!st.clientEngaged) {
                    // Not engaged (companions / NPCs the client hasn't shot): keep
                    // continuous host-Health authority; track host's value as baseline
                    // so a host-driven drop (host shooting it) is NOT misread as
                    // client damage.
                    if (std::abs(curHealth - u.health) > 0.5f) {
                        actor->SetActorValue(*healthAV, u.health);
                    }
                    std::lock_guard lk(g_dmgFwdMutex);
                    g_dmgFwd[actor->formID].baseline = u.health;
                }
                // engaged + alive: skip health writes - local combat sim drives it.
            }
        }

        // Periodic diagnostic - every 3s summarize apply stats
        static uint64_t s_lastApplyLog = 0;
        uint64_t now = GetTickCount64();
        if (now - s_lastApplyLog > 3000) {
            s_lastApplyLog = now;
            PLOG_INFO("[PULSAR] NPCSync: apply batch={} applied={} spawned={} noForm={} noCell={}"sv,
                      work.size(), applied, spawned, skippedNoForm, skippedNoCell);
            // [COMBAT-DBG] big-picture combat state: how many host enemies are
            // matched to a local copy (remap), how many we've engaged (= our shots
            // landed + attributed), how many host-kills we've mirrored. remap=0
            // while shooting → matching is the problem; engaged=0 → attribution/
            // attack-hook is the problem.
            size_t remapN = 0, engagedN = 0, killAppliedN = 0;
            { std::lock_guard lk(g_npcRemapMutex); remapN = g_npcRemap.size(); }
            {
                std::lock_guard lk(g_dmgFwdMutex);
                for (auto& [fid, s] : g_dmgFwd) {
                    if (s.clientEngaged) ++engagedN;
                    if (s.killApplied)   ++killAppliedN;
                }
            }
            PLOG_INFO("[COMBAT-DBG] summary remap={} engaged={} killApplied={}"sv,
                      remapN, engagedN, killAppliedN);
        }
    }

    void Reset()
    {
        {
            std::lock_guard lock(g_trackedMutex);
            g_tracked.clear();
        }
        {
            std::lock_guard lock(g_pendingMutex);
            g_pending.clear();
        }
        {
            std::lock_guard lock(g_remoteOwnedMutex);
            g_remoteOwned.clear();
        }
        {
            std::lock_guard lock(g_npcDcBlockedMutex);
            g_npcDcBlocked.clear();
        }
        {
            std::lock_guard lock(g_npcRemapMutex);
            g_npcRemap.clear();
        }
        {
            std::lock_guard lock(g_spawnAttemptedMutex);
            g_spawnAttempted.clear();
        }
        {
            std::lock_guard lock(g_dmgFwdMutex);
            g_dmgFwd.clear();
        }
        {
            std::lock_guard lock(g_pendingDamageMutex);
            g_pendingDamage.clear();
        }
        {
            std::lock_guard lock(g_corpseLootMutex);
            g_corpseLootDueMs.clear();
            g_corpseLootSent.clear();
            g_corpseLootPending.clear();
        }
        {
            std::lock_guard lk(g_tierCacheMutex);
            g_tierCache.clear();
        }
        {
            std::lock_guard lock(g_civilianSnapshotsMutex);
            g_civilianSnapshots.clear();
        }
        {
            std::lock_guard lock(g_pendingCiviliansMutex);
            g_pendingCivilians.clear();
        }
        g_clientIdCache.clear();
    }

    // Public AI-freeze check. Called from RemotePlayerManager hooks to
    // suppress engine-AI on locally-spawned NPC clones (they must only
    // follow host-driven SetPosition writes, not default companion/combat
    // packages). Membership in g_remoteOwned is the authoritative flag.
    // kill-XP-sharing: canonicalize a local victim FormID to the host key.
    // Static refs (non-0xFF) share their FormID across machines → returned as-is.
    // Dynamic (0xFF) clones live in the host→local remap; reverse it to recover
    // the host FormID. No remap entry for a dynamic victim = a purely-local,
    // un-synced NPC → return 0 so the caller skips sharing.
    uint32_t LocalRefToHostRef(uint32_t localFormID)
    {
        if (localFormID == 0) return 0;
        // Static ref: same FormID on both sides.
        if ((localFormID & 0xFF000000u) != 0xFF000000u) return localFormID;
        // Dynamic clone: reverse the host→local remap (value == localFormID).
        std::lock_guard lk(g_npcRemapMutex);
        for (const auto& [hostRef, localRef] : g_npcRemap) {
            if (localRef == localFormID) return hostRef;
        }
        return 0;
    }

    bool IsRemoteOwned(uint32_t formID)
    {
        if (formID == 0) return false;
        // Phase E 2026-04-23 REVERT: do NOT block EvaluatePackage for tracked
        // NPCs. Blocking the AI stopped Sarah from following client (good)
        // but ALSO stopped the anim-graph state machine from receiving
        // Action events (ActionIdleWalk, ActionRun etc.) → animations never
        // committed, NPCs stayed in static pose even with correct graph
        // vars. Trade-off accepted: Sarah's companion AI will path toward
        // the CLIENT player locally, but our TickLerp overrides her world
        // position every tick so she ends up where the host has her.
        // Anim cycle plays courtesy of the AI's own walk-event dispatch.
        {
            std::lock_guard lock(g_remoteOwnedMutex);
            if (g_remoteOwned.find(formID) != g_remoteOwned.end()) return true;
        }
        // P1: DirectControl-puppet NPCs also get the AI-block (packages +
        // actions) so the engine AI stops fighting the host-driven sync.
        std::lock_guard lk(g_npcDcBlockedMutex);
        return g_npcDcBlocked.find(formID) != g_npcDcBlocked.end();
    }

    void MarkNpcAiBlocked(uint32_t formID, bool on)
    {
        if (formID == 0) return;
        std::lock_guard lk(g_npcDcBlockedMutex);
        if (on) g_npcDcBlocked.insert(formID);
        else    g_npcDcBlocked.erase(formID);
    }

    bool IsNpcAiBlocked(uint32_t formID)
    {
        if (formID == 0) return false;
        std::lock_guard lk(g_npcDcBlockedMutex);
        return g_npcDcBlocked.find(formID) != g_npcDcBlocked.end();
    }

    // V36: scripted-scene suspension membership (see header note). A tracked NPC
    // is "in scene" for kSceneWindowMs after each scene line it speaks. The
    // window is refreshed continuously while the scene keeps talking (the client
    // local scene fires lines once the puppet is released, so it self-sustains),
    // and lapses shortly after the scene ends so normal sync resumes.
    static std::unordered_map<uint32_t, uint64_t> g_npcInScene;       // formID -> last scene-line ms
    static std::mutex                             g_npcInSceneMutex;
    static constexpr uint64_t                     kSceneWindowMs = 30000;

    void MarkNpcInScene(uint32_t formID)
    {
        if (formID == 0) return;
        std::lock_guard lk(g_npcInSceneMutex);
        g_npcInScene[formID] = ::GetTickCount64();
    }

    bool IsNpcInScene(uint32_t formID)
    {
        if (formID == 0) return false;
        std::lock_guard lk(g_npcInSceneMutex);
        auto it = g_npcInScene.find(formID);
        if (it == g_npcInScene.end()) return false;
        return (::GetTickCount64() - it->second) <= kSceneWindowMs;
    }

    // P3 combat-live: toggle + per-NPC membership + query (read by the
    // EvaluatePackage/PerformAction hooks to LET the combat AI through).
    void SetNpcCombatAiLive(bool on)
    {
        g_npcCombatAiLiveFlag.store(on, std::memory_order_relaxed);
        PLOG_INFO("[PULSAR] [P3] NPC combat-AI-live {}"sv, on ? "ENABLED (ini)" : "off");
    }
    bool NpcCombatAiLiveOn() { return g_npcCombatAiLiveFlag.load(std::memory_order_relaxed); }

    void MarkNpcCombatLive(uint32_t formID, bool on)
    {
        if (formID == 0) return;
        std::lock_guard lk(g_npcCombatLiveMutex);
        if (on) g_npcCombatLive.insert(formID);
        else    g_npcCombatLive.erase(formID);
    }
    bool IsNpcCombatLive(uint32_t formID)
    {
        if (formID == 0) return false;
        std::lock_guard lk(g_npcCombatLiveMutex);
        return g_npcCombatLive.find(formID) != g_npcCombatLive.end();
    }

}  // namespace Pulsar::NPCSync


namespace Pulsar::NPCSync {

    bool IsHostMode()
    {
        return Pulsar::Network::GetSingleton().IsHost();
    }

}
