#include "PCH.h"
#include "WorldSync/GrabSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/NPCSync.h"           // GetActiveCellsSnapshot()
#include "NPCSync/PluginLookup.h"

#include <windows.h>
#include <REL/Relocation.h>
#include <RE/B/BSTEvent.h>
#include <RE/E/Events.h>
#include <RE/G/GameVM.h>
#include <RE/N/NiPoint.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::WorldSync::Grab {
namespace {

    // ---- Engine RVAs (verified via Ghidra 2026-04-25, Starfield 1.16.236)
    // FUN_140B9A650 - lazy-init wrapper, returns &singleton @ 0x145977F18
    // FUN_140B9A6C0 - lazy-init wrapper, returns &singleton @ 0x145977F40
    // V2a confirmed only Source A fires for player E-Hold (B is something
    // else - NPC grab? Workshop? - left registered for future logging).
    constexpr std::uintptr_t kRVA_GetGrabSrcA = 0xB99E70;
    constexpr std::uintptr_t kRVA_GetGrabSrcB = 0xB99EE0;

    using GetGrabSrcFn = void* (__fastcall*)();
    static GetGrabSrcFn g_getGrabSrcA = nullptr;
    static GetGrabSrcFn g_getGrabSrcB = nullptr;

    // ObjectReference.SetMotionType native - REL::ID(118463). Identified via
    // VehicleSync's RE pass: only motionType ∈ {1,2} is accepted; 1=Stationary
    // (freezes Havok body, SetPosition no-ops), 2=Dynamic (lets SetPosition
    // writes stick). For grabbed items on receiver we force-Dynamic so the
    // engine doesn't physics-override our per-tick position writes.
    using SetMotionType_t = char(__fastcall*)(void*, std::uint32_t,
                                                RE::TESObjectREFR*,
                                                std::int32_t, bool);
    static REL::Relocation<SetMotionType_t> g_setMotionType{ REL::ID(118463) };

    static int SafeSetMotionType(RE::TESObjectREFR* ref,
                                  std::int32_t motionType,
                                  bool allowActivate) noexcept
    {
        if (!ref) return 0;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return 0;
        auto* vm = gameVM->GetVM();
        if (!vm) return 0;
        __try {
            return g_setMotionType(vm, 0, ref, motionType, allowActivate);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // V3 - fall-on-release via "drop from height" trick.
    //   Starfield's Papyrus base has NO ApplyHavokImpulse / impulse natives
    //   (Bethesda removed them; verified PapyrusBase + Game/Debug/ObjRef.psc).
    //   The latent-native bypass (V3 attempt 1) deadlocked because the engine
    //   call expects a VM stack frame to register its async callback against.
    //
    //   Workaround: nudge the body upward by kReleaseLiftZ then activate
    //   Dynamic motion. Havok integrates from the new position with gravity,
    //   the body wakes (allowActivate=true) and falls naturally. Lift small
    //   enough to be visually invisible but large enough to escape the sleep
    //   epsilon (~0.1u empirically across havok-port games).
    constexpr float kReleaseLiftZ = 0.5f;

    // Position-broadcast cadence while holding. 30 Hz = 33 ms - trades a
    // bit more bandwidth for smoother motion than the 20 Hz BoneSync.
    constexpr std::int64_t kTickIntervalMs = 33;

    // Receiver-side guard: if a position-update packet arrives for a REFR
    // that hasn't moved more than this distance (squared, world units)
    // since last apply, skip the SetPosition to reduce engine churn.
    // Lowered to 0.0025 (~0.05u) - items move sub-unit during hold.
    constexpr float kMinMoveDist2 = 0.0025f;

    // ---- Local raw-offset helpers ---------------------------------------
    //
    // 2026-04-29: NULL-only check insufficient. Crash dump
    // Starfield_04-28-22-48.dmp showed `e.target.get() == 0xFFFFFFFFFFFFFFFF`
    // (sentinel from a freed/just-deallocated REFR) - engine still fired
    // TESGrabReleaseEvent for an NPC's stale grab target while host was
    // walking through cells solo. The original `if (!form)` passed the -1
    // through and we crashed reading [rdi+0x28].
    //
    // Valid Windows user-mode pointers are in [0x10000, 0x7FFFFFFFFFFF).
    // Anything outside is kernel space, sentinels, or unmapped low pages.
    static bool IsLikelyValidPtr(const void* p) noexcept
    {
        const auto a = reinterpret_cast<std::uintptr_t>(p);
        return a >= 0x10000ULL && a < 0x7FFFFFFFFFFFULL;
    }

    static std::uint32_t RawFormID(const void* form) noexcept
    {
        if (!IsLikelyValidPtr(form)) return 0;
        return *reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(form) + 0x28);
    }

    static std::uint8_t RawFormType(const void* form) noexcept
    {
        if (!IsLikelyValidPtr(form)) return 0;
        return *reinterpret_cast<const std::uint8_t*>(
            static_cast<const std::uint8_t*>(form) + 0x2E);
    }

    static void ReadRefrPos(const RE::TESObjectREFR* refr,
                            float& x, float& y, float& z) noexcept
    {
        x = y = z = 0.0f;
        if (!IsLikelyValidPtr(refr)) return;
        auto* p = reinterpret_cast<const float*>(
            reinterpret_cast<const std::uint8_t*>(refr) + 0x8C);
        x = p[0]; y = p[1]; z = p[2];
    }

    // ---- Raw-vtable SetPosition for non-Actor REFRs ---------------------
    // VTABLE[0] slot 0x89 + raw fallback writes to data.location AND niNode
    // transforms. Diagnostic showed (V2c iter 4): vt[0x89] writes data.loc
    // (post==target) but visual stays frozen. So the visible 3D node's
    // world transform isn't being synced from data.loc by vt[0x89] for
    // grabbable items. We write it ourselves.
    //
    // Layout (CommonLibSF verified):
    //   refr+0x8C  = OBJ_REFR.data.location (3 floats)
    //   refr+0xB8  = loadedData (BSGuarded ptr at +0)
    //   loadedData+0x08 = data3D (NiAVObject*)
    //   data3D+0x40 = local NiTransform   (rotate@+0, translate@+0x30)
    //   data3D+0x80 = world NiTransform
    //   data3D+0xC0 = previousWorld NiTransform
    static int SafeRefSetPosition(RE::TESObjectREFR* ref,
                                  float x, float y, float z) noexcept
    {
        if (!ref) return 0;
        __try {
            using SetPosFn = void(__fastcall*)(void*, const RE::NiPoint3&, bool);
            auto** vt = *reinterpret_cast<void***>(ref);
            auto fn = reinterpret_cast<SetPosFn>(vt[0x89]);
            RE::NiPoint3 p(x, y, z);
            fn(ref, p, true);  // true = teleport Havok body too

            auto* f = reinterpret_cast<std::uint8_t*>(ref);

            // Backup: data.location write.
            *reinterpret_cast<float*>(f + 0x8C) = x;
            *reinterpret_cast<float*>(f + 0x90) = y;
            *reinterpret_cast<float*>(f + 0x94) = z;

            // niNode transform writes - force the visual to follow.
            void* loadedDataPtr = *reinterpret_cast<void**>(f + 0xB8);
            if (loadedDataPtr) {
                auto* ld = reinterpret_cast<std::uint8_t*>(loadedDataPtr);
                void* data3D = *reinterpret_cast<void**>(ld + 0x08);
                if (data3D) {
                    auto* d3 = reinterpret_cast<std::uint8_t*>(data3D);
                    // local.translate @ +0x40 + 0x30
                    *reinterpret_cast<float*>(d3 + 0x70) = x;
                    *reinterpret_cast<float*>(d3 + 0x74) = y;
                    *reinterpret_cast<float*>(d3 + 0x78) = z;
                    // world.translate @ +0x80 + 0x30
                    *reinterpret_cast<float*>(d3 + 0xB0) = x;
                    *reinterpret_cast<float*>(d3 + 0xB4) = y;
                    *reinterpret_cast<float*>(d3 + 0xB8) = z;
                    // previousWorld.translate @ +0xC0 + 0x30 - prevents
                    // motion-blur lerp from "snap" to old position.
                    *reinterpret_cast<float*>(d3 + 0xF0) = x;
                    *reinterpret_cast<float*>(d3 + 0xF4) = y;
                    *reinterpret_cast<float*>(d3 + 0xF8) = z;
                }
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // ---- Cell-scan matcher ---------------------------------------------
    static RE::TESObjectREFR* SafeFindRefByBaseInCell(void* cell,
                                                       std::uint32_t resolvedBase,
                                                       std::uint8_t  baseType,
                                                       float px, float py, float pz,
                                                       float radius) noexcept
    {
        __try {
            if (!cell) return nullptr;
            auto* cb = reinterpret_cast<std::uint8_t*>(cell);
            std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = radius * radius;
            for (std::uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(ref) + 0x98);
                if (!base) continue;
                if (baseType != 0 && RawFormType(base) != baseType) continue;
                if (RawFormID(base) != resolvedBase) continue;
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<std::uint8_t*>(ref) + 0x8C);
                float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = ref;
                }
            }
            return best;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // ---- Sender state - currently held REFR + tick clock ----------------

    struct HeldState {
        RE::TESObjectREFR* refr        = nullptr;
        std::uint32_t      refID       = 0;
        std::uint32_t      baseID      = 0;
        std::uint8_t       baseType    = 0;
        // Last position we broadcast - for the t==0 send always go.
        float              lastPx = 0, lastPy = 0, lastPz = 0;
        bool               firstSent = false;
    };
    static std::mutex                            s_heldMutex;
    static HeldState                             s_held;
    static std::chrono::steady_clock::time_point s_lastSendTp;

    // ---- Receiver state -------------------------------------------------

    struct PendingGrab {
        std::uint32_t resolvedBase;
        std::uint8_t  baseType;
        std::uint8_t  grabbed;
        float         px, py, pz;
    };
    static std::mutex                s_pendingMutex;
    static std::vector<PendingGrab>  s_pending;

    // Last-applied REFR per (resolvedBase + initial-pos cell key) - so a
    // stream of position-update packets can re-target the same REFR
    // without repeating the full cell-scan each tick. Keyed by base
    // FormID alone (a cell typically has at most one item with a given
    // base being grabbed at a time; if collisions appear we'd add a
    // position prefix to the key). Cleared on grabbed=0.
    struct ActiveTarget {
        RE::TESObjectREFR* refr = nullptr;
        float lastPx = 0, lastPy = 0, lastPz = 0;
        bool  motionForced = false;  // true after SetMotionType(2,Dynamic) ran once
    };
    static std::mutex                                          s_activeMutex;
    static std::unordered_map<std::uint32_t, ActiveTarget>     s_active;

    // ---- Wire encoding ---------------------------------------------------

    static void SendGrabPacket(std::uint32_t refID, std::uint32_t baseID,
                                std::uint8_t baseType, std::uint8_t grabbed,
                                float px, float py, float pz)
    {
        using namespace Pulsar::Network;

        GrabStateData d{};
        d.refFormID    = refID;
        d.baseFormID   = baseID;
        d.baseLocalID  = NPCSync::PluginLookup::FormIDToLocalID(baseID);
        d.posX         = px;
        d.posY         = py;
        d.posZ         = pz;
        d.baseFormType = baseType;
        d.grabbed      = grabbed;
        NPCSync::PluginLookup::FormIDToPluginName(
            baseID, d.basePlugin, sizeof(d.basePlugin));

        std::uint8_t buf[1 + sizeof(GrabStateData)]{};
        buf[0] = PKT_GRAB_STATE;
        std::memcpy(buf + 1, &d, sizeof(d));

        auto& net = Pulsar::Network::GetSingleton();
        // Reliable for grabbed=0 (release must not be lost), unreliable for
        // position updates (next packet overwrites stale data anyway).
        bool reliable = (grabbed == 0);
        net.Send(buf, sizeof(buf), reliable);
    }

    // ---- Sink class (shared between Source A & B for symmetry) ----------

    class GrabSink : public RE::BSTEventSink<RE::TESGrabReleaseEvent> {
    public:
        const char*       label;
        std::atomic<int>  fireCount{ 0 };
        bool              isActive;  // A is the active path; B kept logging-only

        GrabSink(const char* l, bool active) : label(l), isActive(active) {}

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESGrabReleaseEvent& e,
            RE::BSTEventSource<RE::TESGrabReleaseEvent>* /*src*/) override
        {
            int n = fireCount.fetch_add(1) + 1;

            auto* refr = e.target.get();
            // 2026-04-29: validate pointer before any deref. Engine fires
            // grab-events for stale (freed) targets during cell-tear-down
            // payload's NiPointer._ptr can be sentinel -1.
            if (!IsLikelyValidPtr(refr)) refr = nullptr;
            std::uint32_t refID = RawFormID(refr);
            std::uint32_t baseID = 0;
            std::uint8_t  baseType = 0;
            float rx = 0, ry = 0, rz = 0;
            if (refr) {
                auto* baseRaw = *reinterpret_cast<std::uint8_t* const*>(
                    reinterpret_cast<const std::uint8_t*>(refr) + 0x98);
                baseID   = RawFormID(baseRaw);
                baseType = RawFormType(baseRaw);
                ReadRefrPos(refr, rx, ry, rz);
            }

            if (n <= 50) {
                PLOG_INFO(
                    "[PULSAR] GrabSync src={} grabbed={} target=0x{:08X} "
                    "base=0x{:08X} type=0x{:02X} pos=({:.1f},{:.1f},{:.1f}) (#{})"sv,
                    label, e.grabbed ? 1 : 0,
                    refID, baseID, baseType, rx, ry, rz, n);
            }

            // Only Source A drives the network sync (V2a verified).
            if (!isActive || !refr) return RE::BSEventNotifyControl::kContinue;

            auto& net = Pulsar::Network::GetSingleton();
            bool connected = net.IsConnected();

            if (e.grabbed) {
                {
                    std::lock_guard g(s_heldMutex);
                    s_held.refr      = refr;
                    s_held.refID     = refID;
                    s_held.baseID    = baseID;
                    s_held.baseType  = baseType;
                    s_held.lastPx    = rx;
                    s_held.lastPy    = ry;
                    s_held.lastPz    = rz;
                    s_held.firstSent = false;
                }
                s_lastSendTp = std::chrono::steady_clock::now();
                if (connected) {
                    SendGrabPacket(refID, baseID, baseType, 1, rx, ry, rz);
                    std::lock_guard g(s_heldMutex);
                    s_held.firstSent = true;
                }
            } else {
                {
                    std::lock_guard g(s_heldMutex);
                    s_held.refr     = nullptr;
                    s_held.refID    = 0;
                    s_held.baseID   = 0;
                    s_held.baseType = 0;
                }
                if (connected) {
                    SendGrabPacket(refID, baseID, baseType, 0, rx, ry, rz);
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    static GrabSink s_sinkA{ "A", true  };
    static GrabSink s_sinkB{ "B", false };

}  // namespace

// =====================================================================
// Public API
// =====================================================================

void Init()
{
    g_getGrabSrcA = reinterpret_cast<GetGrabSrcFn>(
        REL::Offset{ kRVA_GetGrabSrcA }.address());
    g_getGrabSrcB = reinterpret_cast<GetGrabSrcFn>(
        REL::Offset{ kRVA_GetGrabSrcB }.address());

    void* rawA = nullptr;
    void* rawB = nullptr;
    __try { rawA = g_getGrabSrcA(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { rawA = nullptr; }
    __try { rawB = g_getGrabSrcB(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { rawB = nullptr; }

    PLOG_INFO("[PULSAR] GrabSync: GetEventSrcA=0x{:X} -> singleton 0x{:X}"sv,
              reinterpret_cast<std::uintptr_t>(g_getGrabSrcA),
              reinterpret_cast<std::uintptr_t>(rawA));
    PLOG_INFO("[PULSAR] GrabSync: GetEventSrcB=0x{:X} -> singleton 0x{:X}"sv,
              reinterpret_cast<std::uintptr_t>(g_getGrabSrcB),
              reinterpret_cast<std::uintptr_t>(rawB));

    if (rawA) {
        auto* srcA = reinterpret_cast<RE::BSTEventSource<RE::TESGrabReleaseEvent>*>(rawA);
        srcA->RegisterSink(&s_sinkA);
        PLOG_INFO("[PULSAR] GrabSync: sink A registered (active path)"sv);
    } else {
        PLOG_WARN("[PULSAR] GrabSync: source A null - sink A not registered"sv);
    }
    if (rawB) {
        auto* srcB = reinterpret_cast<RE::BSTEventSource<RE::TESGrabReleaseEvent>*>(rawB);
        srcB->RegisterSink(&s_sinkB);
        PLOG_INFO("[PULSAR] GrabSync: sink B registered (logging only)"sv);
    } else {
        PLOG_WARN("[PULSAR] GrabSync: source B null - sink B not registered"sv);
    }
}

void Tick()
{
    // Read state under lock, then release before doing the network call.
    HeldState snap{};
    {
        std::lock_guard g(s_heldMutex);
        if (!s_held.refr) return;
        snap = s_held;
    }
    if (!snap.firstSent) return;  // sink will send the initial packet

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - s_lastSendTp).count();
    if (elapsed < kTickIntervalMs) return;

    auto& net = Pulsar::Network::GetSingleton();
    static int s_disconnectLogged = 0;
    if (!net.IsConnected()) {
        if (++s_disconnectLogged % 30 == 1) {
            PLOG_INFO(
                "[PULSAR] GrabSync TICK: holding ref=0x{:08X} but NOT CONNECTED - skipping TX (count {})"sv,
                snap.refID, s_disconnectLogged);
        }
        return;
    }
    s_disconnectLogged = 0;

    float rx = 0, ry = 0, rz = 0;
    ReadRefrPos(snap.refr, rx, ry, rz);

    // No sender-side jitter gate - engine updates data.location with
    // sub-unit per-tick deltas during E-Hold; gating on movement filtered
    // out actual carry motion. The 30 Hz rate-limit + receiver-side jitter
    // guard prevent network/engine spam.
    SendGrabPacket(snap.refID, snap.baseID, snap.baseType, 1, rx, ry, rz);

    s_lastSendTp = now;
    {
        std::lock_guard g(s_heldMutex);
        s_held.lastPx = rx;
        s_held.lastPy = ry;
        s_held.lastPz = rz;
    }
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    if (data[0] != PKT_GRAB_STATE) return;
    if (length < 1 + sizeof(GrabStateData)) return;

    GrabStateData d{};
    std::memcpy(&d, data + 1, sizeof(d));

    std::uint32_t resolved = 0;
    if (d.basePlugin[0] != '\0') {
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(
            d.basePlugin, d.baseLocalID);
    }
    if (!resolved) resolved = d.baseFormID;

    {
        std::lock_guard g(s_pendingMutex);
        s_pending.push_back({
            resolved, d.baseFormType, d.grabbed,
            d.posX, d.posY, d.posZ
        });
    }

    if (d.grabbed == 0) {
        PLOG_INFO(
            "[PULSAR] GrabSync RX: RELEASE base=0x{:08X} plugin='{}' resolved=0x{:08X} @({:.2f},{:.2f},{:.2f})"sv,
            d.baseFormID,
            d.basePlugin[0] ? d.basePlugin : "(none)",
            resolved, d.posX, d.posY, d.posZ);
    }
    // grabbed=1 updates intentionally silent - they fire ~30 Hz.
}

void DrainPending()
{
    std::vector<PendingGrab> work;
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

    for (auto& d : work) {
        // Position-update path: re-use last matched REFR if we have one for
        // this base. The dist-jitter guard prevents excessive SetPosition.
        RE::TESObjectREFR* refr = nullptr;
        bool fromCache = false;
        {
            std::lock_guard g(s_activeMutex);
            auto it = s_active.find(d.resolvedBase);
            if (it != s_active.end() && it->second.refr) {
                refr = it->second.refr;
                fromCache = true;
            }
        }

        // Cache miss OR initial grab - run the matcher.
        if (!refr) {
            if (player->parentCell) {
                refr = SafeFindRefByBaseInCell(
                    player->parentCell, d.resolvedBase, d.baseType,
                    d.px, d.py, d.pz, 1500.0f);
            }
            if (!refr) {
                auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
                for (auto* cell : cells) {
                    if (!cell) continue;
                    if (cell == player->parentCell) continue;
                    refr = SafeFindRefByBaseInCell(
                        cell, d.resolvedBase, d.baseType,
                        d.px, d.py, d.pz, 1500.0f);
                    if (refr) break;
                }
            }
            if (refr && d.grabbed) {
                std::lock_guard g(s_activeMutex);
                ActiveTarget at{};
                at.refr = refr;
                at.lastPx = d.px; at.lastPy = d.py; at.lastPz = d.pz;
                s_active[d.resolvedBase] = at;
            }
        }

        if (!refr) {
            PLOG_INFO(
                "[PULSAR] GrabSync: NO MATCH base=0x{:08X} type=0x{:02X} grabbed={} @({:.1f},{:.1f},{:.1f})"sv,
                d.resolvedBase, d.baseType, d.grabbed ? 1 : 0,
                d.px, d.py, d.pz);
            continue;
        }

        if (d.grabbed) {
            // Skip if movement below jitter threshold (per-target last-pos).
            bool apply = true;
            {
                std::lock_guard g(s_activeMutex);
                auto it = s_active.find(d.resolvedBase);
                if (it != s_active.end()) {
                    float dx = d.px - it->second.lastPx;
                    float dy = d.py - it->second.lastPy;
                    float dz = d.pz - it->second.lastPz;
                    if (dx*dx + dy*dy + dz*dz < kMinMoveDist2) apply = false;
                }
            }

            // Force-Dynamic once per active session so the engine's
            // niNode-from-body sync follows our writes (via vt[0x89]'s
            // Havok-teleport flag). Stationary(1) would freeze that sync
            // and kill visual movement - verified by iter6 regression.
            bool needForce = false;
            {
                std::lock_guard g(s_activeMutex);
                auto& at = s_active[d.resolvedBase];
                if (!at.motionForced) {
                    at.motionForced = true;
                    needForce = true;
                }
            }
            if (needForce) {
                int mtOk = SafeSetMotionType(refr, 2, false);
                PLOG_INFO(
                    "[PULSAR] GrabSync: SetMotionType(Dynamic=2) ref=0x{:08X} ok={}"sv,
                    RawFormID(refr), mtOk);
            }

            if (apply) {
                int ok = SafeRefSetPosition(refr, d.px, d.py, d.pz);
                if (!ok) {
                    PLOG_WARN(
                        "[PULSAR] GrabSync: SetPosition FAILED ref=0x{:08X}"sv,
                        RawFormID(refr));
                }
                std::lock_guard g(s_activeMutex);
                auto& at = s_active[d.resolvedBase];
                at.refr = refr;
                at.lastPx = d.px; at.lastPy = d.py; at.lastPz = d.pz;
            }
        } else {
            // Release - teleport the body slightly above the final pos, then
            // restore Dynamic motion with allowActivate=true. Havok wakes the
            // body and gravity pulls it the small drop distance. Visually the
            // item just falls from "where the host released" instead of
            // hovering forever.
            SafeRefSetPosition(refr, d.px, d.py, d.pz + kReleaseLiftZ);
            int mt1 = SafeSetMotionType(refr, 1, false);
            int mt2 = SafeSetMotionType(refr, 2, true);
            std::lock_guard g(s_activeMutex);
            s_active.erase(d.resolvedBase);
            PLOG_INFO(
                "[PULSAR] GrabSync: RELEASED ref=0x{:08X} base=0x{:08X} @({:.1f},{:.1f},{:.1f}+{:.1f}) wake(1->2)={}/{}{}"sv,
                RawFormID(refr), d.resolvedBase, d.px, d.py, d.pz, kReleaseLiftZ,
                mt1, mt2, fromCache ? " (cache)" : "");
        }
    }
}

}  // namespace Pulsar::WorldSync::Grab
