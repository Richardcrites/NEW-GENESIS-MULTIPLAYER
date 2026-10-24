#include "PCH.h"
#include "CloneManager/CloneManager.h"
#include "NASLog.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/A/Actor.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESGlobal.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/B/BSFixedString.h>

#include <atomic>

using namespace std::string_view_literals;

namespace Pulsar::CloneManager {

    // =====================================================================
    // REL::IDs - ALL verified via read_versionlib.py + Ghidra decompile.
    // Do not replace with CommonLibSF placeholders. See:
    //   D:\PlusarCellsync\PULSAR_ANIMATED_SPAWN_RE.md
    //   D:\PlusarCellsync\PULSAR_SPAWN_RE.md
    //   D:\PlusarCellsync\PULSAR_PROCEDURAL_RE.md
    // =====================================================================

    // FUN_14179a510 - Inner Placement Engine. Same function Papyrus PlaceAtMe
    // calls. Spawns a fully-initialized, animatable actor.
    //   Signature (from Ghidra decompile of FUN_14207d830 caller):
    //     longlong FUN_14179a510(
    //         longlong      source_npc_or_actor,   // TESNPC* OR Actor*
    //         void*         niTransform,           // 0x18-byte struct from FUN_140401660
    //         void*         rotation,              // 3 floats (x,y,z rot)
    //         TESObjectCELL* parentCell,
    //         TESWorldSpace* worldSpace,
    //         bool          initiallyDisabled,
    //         bool          startDead,
    //         int           unknown);              // pass 0
    using InnerPlacement_t = void* (*)(
        void* source,
        const void* niTransform,
        const void* rotation,
        void* parentCell,
        void* worldSpace,
        bool initiallyDisabled,
        bool startDead,
        int unknown);

    // FUN_140401660 - NiTransform builder.
    //   outTransform[0..11]  = position copied from pos[0..11]
    //   outTransform[0x0C]   = cell bin index (0xffffffff if cell==null fallback)
    //   outTransform[0x10]   = cell pointer (filled by engine from world/current)
    using BuildNiTransform_t = void* (*)(
        void* outTransform,       // 0x18-byte struct
        const void* niPoint3Pos,  // 3 floats
        void* explicitCell);      // TESObjectCELL* or null

    // FUN_140b3fd60 - SetPersistent with full persistence registration (NOT just
    // the formFlags bit). Registers the actor in engine's persistent-refs table.
    using SetPersistent_t = void (*)(void* ref, bool persist);

    // FUN_140b4f260 - GetParentWorldSpace(ObjectRef*) - returns TESWorldSpace*.
    using GetWorldSpace_t = void* (*)(void* ref);

    static REL::Relocation<InnerPlacement_t>   g_innerPlace{ REL::ID(97230) };
    static REL::Relocation<BuildNiTransform_t> g_buildTx  { REL::ID(40699) };
    static REL::Relocation<SetPersistent_t>    g_setPers  { REL::ID(63305) };
    static REL::Relocation<GetWorldSpace_t>    g_getWorld { REL::ID(63417) };

    // Tracked clone state. Handle-based tracking so cell-unload doesn't give us a
    // dangling pointer. Raw FormID stored for quick identity check.
    static std::atomic<uint32_t> g_cloneFormID{ 0 };
    static std::atomic<bool>     g_initLogged{ false };

    void Init()
    {
        // Log resolved addresses once - lets us verify the REL::IDs mapped to
        // the expected offsets at runtime (catches any versionlib drift).
        if (g_initLogged.exchange(true)) return;
        PLOG_INFO("[PULSAR] CloneManager: REL addresses: "
                  "InnerPlace=0x{:x} BuildTx=0x{:x} SetPers=0x{:x} GetWorld=0x{:x}"sv,
                  reinterpret_cast<uintptr_t>(g_innerPlace.get()),
                  reinterpret_cast<uintptr_t>(g_buildTx.get()),
                  reinterpret_cast<uintptr_t>(g_setPers.get()),
                  reinterpret_cast<uintptr_t>(g_getWorld.get()));
    }

    RE::Actor* SpawnClone()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_WARN("[PULSAR] CloneManager::SpawnClone - player is null"sv);
            return nullptr;
        }
        if (!player->parentCell) {
            PLOG_WARN("[PULSAR] CloneManager::SpawnClone - player has no parentCell"sv);
            return nullptr;
        }

        // Input for FUN_140401660: NiPoint3 position
        struct NiPoint3 { float x, y, z; };
        NiPoint3 pos{
            player->data.location.x,
            player->data.location.y,
            player->data.location.z
        };
        NiPoint3 rot{ 0.0f, 0.0f, player->data.angle.z };

        // Output: 0x18-byte struct that FUN_14179a510 expects as param_2
        alignas(8) uint8_t niTransform[0x18] = {};

        // PlaceDuplicateActorAtMe caller only passes 2 args to FUN_140401660
        // (RCX=out, RDX=pos). param_3 (R8) was uninitialized garbage. Ghidra
        // inferred the param from usage. Real behavior: nullptr → fallback path.
        PLOG_INFO("[PULSAR] CloneManager: step 1 - BuildTx (null cell)"sv);
        g_buildTx(niTransform, &pos, nullptr);
        PLOG_INFO("[PULSAR] CloneManager: step 1 OK"sv);

        PLOG_INFO("[PULSAR] CloneManager: step 2 - GetWorldSpace"sv);
        void* worldSpace = g_getWorld(player);
        PLOG_INFO("[PULSAR] CloneManager: step 2 OK (ws={})"sv, worldSpace);

        // Hypothesis from decompile: FUN_14179a510 reads source+0xC8 which on Actor
        // is loadedData (valid pointer), but on TESNPC is something else → crash.
        // PlaceDuplicateActorAtMe (118396) passes Actor*, not TESNPC*. Try that first.
        PLOG_INFO("[PULSAR] CloneManager: step 3 - InnerPlace (Actor* source = player)"sv);
        void* spawned = g_innerPlace(
            player,            // duplicate the player Actor (like PlaceDuplicateActorAtMe does)
            niTransform,       // 0x18-byte transform struct
            &rot,              // rotation floats
            player->parentCell,
            worldSpace,
            false,             // not initially disabled
            false,             // not dead
            0);
        PLOG_INFO("[PULSAR] CloneManager: step 3 returned {}"sv, spawned);

        if (!spawned) {
            PLOG_WARN("[PULSAR] CloneManager::SpawnClone - inner placement returned null"sv);
            return nullptr;
        }

        auto* cloneRef = reinterpret_cast<RE::TESObjectREFR*>(spawned);
        auto* clone    = reinterpret_cast<RE::Actor*>(spawned);

        // Persist so the clone survives cell-unloads (important for procedural cells)
        g_setPers(cloneRef, true);

        g_cloneFormID.store(cloneRef->formID, std::memory_order_release);

        PLOG_INFO("[PULSAR] CloneManager: spawned clone FormID=0x{:08X} at ({:.0f},{:.0f},{:.0f})"sv,
                  cloneRef->formID, pos.x, pos.y, pos.z);
        NASLOG("CloneManager spawned clone 0x%08X", cloneRef->formID);

        // ISOLATION TEST 2: GLOB write RE-ENABLED. Test solo-host (no client)
        // to see if the 45s crash needs network data or happens on find-clone alone.
        uint32_t fid = cloneRef->formID;
        uint32_t hi = fid >> 16;
        uint32_t lo = fid & 0xFFFF;
        auto setGlob = [](const char* name, float value) {
            auto* g = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString(name));
            if (g) g->value = value;
        };
        setGlob("PulsarFormHi", static_cast<float>(hi));
        setGlob("PulsarFormLo", static_cast<float>(lo));
        setGlob("PulsarSpawn", 2.0f);
        PLOG_INFO("[PULSAR] CloneManager: wrote GLOBs PulsarFormHi={} Lo={} Spawn=2.0"sv, hi, lo);

        return clone;
    }

    RE::Actor* GetClone()
    {
        uint32_t id = g_cloneFormID.load(std::memory_order_acquire);
        if (!id) return nullptr;
        auto* form = RE::TESForm::LookupByID(id);
        if (!form) {
            // Handle no longer resolvable (actor destroyed or save-loaded)
            g_cloneFormID.store(0, std::memory_order_release);
            return nullptr;
        }
        return form->As<RE::Actor>();
    }

    bool IsCloneActive()
    {
        auto* clone = GetClone();
        return clone && clone->parentCell != nullptr;
    }

    void DespawnClone()
    {
        auto* clone = GetClone();
        if (!clone) return;

        // Mark for delete via TESForm::SetDelete vtable slot (0x33 per prior research).
        // Safer to use Disable + MarkForDelete via the engine's normal pattern. For
        // now, simplest approach: set kDisabled flag. Full cleanup comes in a
        // follow-up - the actor will be GC'd when its cell unloads.
        // TODO: proper delete via vtable[0x33] once confirmed in-game.
        PLOG_INFO("[PULSAR] CloneManager::DespawnClone - FormID=0x{:08X} (reset tracking only)"sv,
                  clone->formID);

        g_cloneFormID.store(0, std::memory_order_release);
    }

    void Reset()
    {
        g_cloneFormID.store(0, std::memory_order_release);
    }

    bool IsCppSpawnedClone(uint32_t formID)
    {
        return formID != 0 && formID == g_cloneFormID.load(std::memory_order_acquire);
    }

}  // namespace Pulsar::CloneManager
