#include "PCH.h"
#include "SpawnSync/SpawnSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/PluginLookup.h"

#include <windows.h>
#include <REL/Relocation.h>

#include <RE/E/Events.h>
#include <RE/B/BSTEvent.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::SpawnSync {
namespace {

    // ENABLE worker - REL::ID(63386) (RVA 0xB4A9C0 on 244). The function the
    // engine's own EnableFunctor calls: void Enable(TESObjectREFR*, bool fadeIn).
    // Idempotent (no-op if the kDisabled formFlag bit 0x800 is already clear) and
    // SAFE off the Papyrus VM on the game thread (no VM-stack arg). RE deliverable:
    // local RE notes For ACTORS the direct call is
    // correct (the engine's EnableFunctor calls the worker directly, bypassing the
    // Actor enable-vtable override).
    using EnableFn = void (*)(RE::TESObjectREFR*, bool);
    static EnableFn g_enableRefr = nullptr;

    static std::atomic<bool> s_initDone{ false };

    // ---- raw-offset helpers (verified offsets, mirror EventTrace) -----------
    static std::uint32_t SafeRawFormID(const void* obj) noexcept
    {
        if (!obj) return 0;
        __try { return *reinterpret_cast<const std::uint32_t*>(
                    static_cast<const std::uint8_t*>(obj) + 0x28); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    static std::uint8_t SafeRawFormType(const void* obj) noexcept
    {
        if (!obj) return 0;
        __try { return *reinterpret_cast<const std::uint8_t*>(
                    static_cast<const std::uint8_t*>(obj) + 0x2E); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    static void* SafeRefBase(const void* refr) noexcept
    {
        if (!refr) return nullptr;
        __try { return *reinterpret_cast<void* const*>(
                    static_cast<const std::uint8_t*>(refr) + 0x98); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    static std::uint32_t SafeRefCellID(const void* refr) noexcept
    {
        if (!refr) return 0;
        __try {
            auto* cell = *reinterpret_cast<void* const*>(
                static_cast<const std::uint8_t*>(refr) + 0xB0);
            if (!cell) return 0;
            return *reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(cell) + 0x28);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    static std::uint32_t SafePlayerCellID() noexcept
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        return p ? SafeRefCellID(p) : 0;
    }
    // formFlags dword @ REFR+0x20, bit 0x800 = kDisabled. Sets outOk=false if the
    // read raised (torn ptr) so the caller doesn't act on a garbage flag.
    static bool SafeIsDisabled(const void* refr, bool& outOk) noexcept
    {
        outOk = false;
        if (!refr) return false;
        __try {
            std::uint32_t flags = *reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(refr) + 0x20);
            outOk = true;
            return (flags & 0x800u) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    struct PosRot { float x, y, z, rz; bool ok; };
    static PosRot SafeReadPosRot(const void* refr) noexcept
    {
        PosRot p{ 0, 0, 0, 0, false };
        if (!refr) return p;
        __try {
            auto* loc = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(refr) + 0x8C);   // data.location
            p.x = loc[0]; p.y = loc[1]; p.z = loc[2];
            // OBJ_REFR data @ REFR+0x80: angle @ data+0x00 (x/y/z = +0x80/84/88),
            // location @ data+0x0C (+0x8C/90/94), objectReference @ +0x98,
            // BGSInventoryList @ +0xA0. angle.z is +0x88 - NOT +0xA0 (that is the
            // inventory-list pointer; writing it corrupts the actor -> crash).
            p.rz = *reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(refr) + 0x88);   // data.angle.z
            p.ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { p.ok = false; }
        return p;
    }
    static void SafeWritePosRot(void* refr, float x, float y, float z, float rz) noexcept
    {
        if (!refr) return;
        __try {
            auto* loc = reinterpret_cast<float*>(
                static_cast<std::uint8_t*>(refr) + 0x8C);
            loc[0] = x; loc[1] = y; loc[2] = z;
            // angle.z @ +0x88 (see SafeReadPosRot). The old +0xA0 was the
            // BGSInventoryList pointer - writing it crashed the engine in the
            // inventory iterator (REFR+0xA0 guarded list, mov [rbx+0x28]).
            *reinterpret_cast<float*>(
                static_cast<std::uint8_t*>(refr) + 0x88) = rz;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static bool SafeEnable(RE::TESObjectREFR* refr) noexcept
    {
        if (!refr || !g_enableRefr) return false;
        __try { g_enableRefr(refr, false); return true; }   // fadeIn=false (instant)
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- state --------------------------------------------------------------
    struct PendingSpawn { std::uint32_t refFormID; std::uint32_t cellFormID;
                          float x, y, z, rz; };
    static std::mutex                 s_pendingMutex;
    static std::vector<PendingSpawn>  s_pending;

    static std::mutex                          s_enabledRefsMutex;
    static std::unordered_set<std::uint32_t>   s_enabledRefs;   // client dedup

    static std::mutex                              s_hostSentMutex;
    static std::unordered_map<std::uint32_t, std::uint64_t> s_hostSentMs;  // host per-ref debounce

    // ---- host broadcast -----------------------------------------------------
    static void BroadcastSpawn(void* actorPtr, std::uint32_t refFormID,
                               std::uint32_t cellFormID)
    {
        using namespace Pulsar::Network;

        // Per-ref debounce: ActorCellChange ENTER can fire repeatedly when the
        // engine juggles cells; one spawn-broadcast per ref per 5 s is plenty.
        {
            const std::uint64_t now = ::GetTickCount64();
            std::lock_guard g(s_hostSentMutex);
            auto it = s_hostSentMs.find(refFormID);
            if (it != s_hostSentMs.end() && now - it->second < 5000) return;
            s_hostSentMs[refFormID] = now;
        }

        std::uint32_t baseID = 0;
        if (void* base = SafeRefBase(actorPtr)) baseID = SafeRawFormID(base);
        PosRot pr = SafeReadPosRot(actorPtr);

        NpcSpawnData d{};
        d.refFormID   = refFormID;
        d.baseFormID  = baseID;
        d.baseLocalID = NPCSync::PluginLookup::FormIDToLocalID(baseID);
        d.cellFormID  = cellFormID;
        d.posX = pr.x; d.posY = pr.y; d.posZ = pr.z; d.rotZ = pr.rz;
        NPCSync::PluginLookup::FormIDToPluginName(
            baseID, d.basePlugin, sizeof(d.basePlugin));

        std::uint8_t buf[1 + sizeof(NpcSpawnData)]{};
        buf[0] = PKT_NPC_SPAWN;
        std::memcpy(buf + 1, &d, sizeof(d));
        Pulsar::Network::GetSingleton().Send(buf, sizeof(buf), true);  // reliable

        PLOG_INFO(
            "[PULSAR] SpawnSync TX: ref=0x{:08X} base=0x{:08X} plugin='{}' cell=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
            refFormID, baseID, d.basePlugin[0] ? d.basePlugin : "(none)",
            cellFormID, pr.x, pr.y, pr.z);
    }

    // ---- host detection sink ------------------------------------------------
    // ActorCellChangeEvent ENTER (cellA==0 -> cellB) = an actor became active in
    // a cell. On the host, for a PERSISTENT actor ref entering the host player's
    // OWN cell, broadcast a spawn so the client enables the same ref. This is
    // independent of NPCSync's 20-NPC position cap (that cap is why the pirates
    // were never sent before).
    struct SpawnDetectSink
        : public RE::BSTEventSink<RE::ActorCellChangeEvent::Event>
    {
        RE::BSEventNotifyControl ProcessEvent(
            const RE::ActorCellChangeEvent::Event& e,
            RE::BSTEventSource<RE::ActorCellChangeEvent::Event>*) override
        {
            auto& net = Pulsar::Network::GetSingleton();
            if (!net.IsHost() || !net.IsConnected())
                return RE::BSEventNotifyControl::kContinue;

            void*         actorPtr = nullptr;
            std::uint32_t actorFID = 0, cellAID = 0, cellBID = 0;
            __try {
                actorPtr = e.actor.get();
                actorFID = SafeRawFormID(actorPtr);
                cellAID  = SafeRawFormID(e.cellA);
                cellBID  = SafeRawFormID(e.cellB);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // ENTER only (no old cell -> a new cell).
            if (cellAID != 0 || cellBID == 0 || actorFID == 0)
                return RE::BSEventNotifyControl::kContinue;
            // Persistent ref only - FF-dynamic refs can't be resolved cross-machine.
            if ((actorFID & 0xFF000000u) == 0xFF000000u)
                return RE::BSEventNotifyControl::kContinue;
            if (actorFID == 0x14u) return RE::BSEventNotifyControl::kContinue;  // player
            // Only the host player's own cell (the shared area).
            if (cellBID != SafePlayerCellID())
                return RE::BSEventNotifyControl::kContinue;
            // Must be an actor reference (kACHR = 0x4B).
            if (SafeRawFormType(actorPtr) != 0x4B)
                return RE::BSEventNotifyControl::kContinue;

            BroadcastSpawn(actorPtr, actorFID, cellBID);
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static SpawnDetectSink g_spawnSink;

}  // anonymous namespace

// =====================================================================
// Public API
// =====================================================================

void Init()
{
    if (s_initDone.exchange(true)) return;

    // Resolve the Enable worker (REL::ID 63386). REL::ID maps cleanly in the
    // 1.16.244 versionlib (RE-verified), so this resolves; if a future versionlib
    // dropped it, g_enableRefr stays null and SafeEnable degrades to a no-op.
    REL::Relocation<void*> enableTarget{ REL::ID(63386) };
    g_enableRefr = reinterpret_cast<EnableFn>(enableTarget.get());
    PLOG_INFO("[PULSAR] SpawnSync: Enable worker resolved at 0x{:x} (REL::ID 63386)"sv,
              reinterpret_cast<std::uintptr_t>(g_enableRefr));

    // Register the host spawn-detection sink, guarded (degrade to no-op if the
    // address-library mapping is missing, like EventTrace).
    bool ok = false, threw = false;
    __try {
        auto* src = RE::ActorCellChangeEvent::Event::GetEventSource();
        if (src) { src->RegisterSink(&g_spawnSink); ok = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        threw = true;
    }
    if (threw)    PLOG_WARN("[PULSAR] SpawnSync: ActorCellChange RegisterSink threw"sv);
    else if (!ok) PLOG_WARN("[PULSAR] SpawnSync: ActorCellChange GetEventSource null"sv);
    else          PLOG_INFO("[PULSAR] SpawnSync: ActorCellChange sink registered"sv);
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;
    if (!data || length < 1 || data[0] != PKT_NPC_SPAWN) return;
    if (length < 1 + sizeof(NpcSpawnData)) return;

    NpcSpawnData d{};
    std::memcpy(&d, data + 1, sizeof(d));
    {
        std::lock_guard g(s_pendingMutex);
        s_pending.push_back({ d.refFormID, d.cellFormID,
                              d.posX, d.posY, d.posZ, d.rotZ });
    }
    PLOG_INFO("[PULSAR] SpawnSync RX: ref=0x{:08X} cell=0x{:08X} @({:.0f},{:.0f},{:.0f}) queued"sv,
              d.refFormID, d.cellFormID, d.posX, d.posY, d.posZ);
}

void DrainPending()
{
    std::vector<PendingSpawn> work;
    {
        std::lock_guard g(s_pendingMutex);
        if (s_pending.empty()) return;
        work.swap(s_pending);
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        std::lock_guard g(s_pendingMutex);
        s_pending.insert(s_pending.begin(), work.begin(), work.end());
        return;
    }
    const std::uint32_t playerCell = SafePlayerCellID();

    for (auto& s : work) {
        {
            std::lock_guard g(s_enabledRefsMutex);
            if (s_enabledRefs.count(s.refFormID)) continue;  // already handled
        }
        // Co-location gate: only enable NPCs for the cell we're actually in.
        // If the host's cell isn't ours, the engine wouldn't render the ref
        // anyway; drop it (re-broadcast on the host's 5s window covers a later
        // co-location).
        if (s.cellFormID != 0 && playerCell != 0 && s.cellFormID != playerCell)
            continue;

        auto* form = RE::TESForm::LookupByID(s.refFormID);
        if (!form) continue;                              // unknown on this machine
        if (SafeRawFormType(form) != 0x4B) continue;      // not an actor ref
        auto* refr = reinterpret_cast<RE::TESObjectREFR*>(form);

        bool flagsOk = false;
        const bool disabled = SafeIsDisabled(refr, flagsOk);
        if (flagsOk && !disabled) {
            // Already enabled locally (client spawned it itself, e.g. the
            // deterministic crowd) - mark + skip so we never re-touch it.
            std::lock_guard g(s_enabledRefsMutex);
            s_enabledRefs.insert(s.refFormID);
            continue;
        }

        const bool ok = SafeEnable(refr);
        SafeWritePosRot(refr, s.x, s.y, s.z, s.rz);
        {
            std::lock_guard g(s_enabledRefsMutex);
            s_enabledRefs.insert(s.refFormID);
        }
        PLOG_INFO("[PULSAR] SpawnSync: ENABLE {} ref=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
                  ok ? "ok" : "FAILED", s.refFormID, s.x, s.y, s.z);
    }
}

}  // namespace Pulsar::SpawnSync
