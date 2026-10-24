// Co-op ship-combat sync (shared host ship). See ShipCombatSync.h.
//
// Slice 2: hull-health state sync. The host reads its ship's hull AV and
// broadcasts it (on change + 1s heartbeat); the passive client mirrors it onto
// its local same-FormID ship so both games agree on the ship's condition.
//
// Verified live (slice 1): hull cur drops 366->0 under fire, max stays 366,
// read via ship+0x70 ActorValueOwner. CommonLibSF's RE::ActorValueOwner lives
// exactly at that sub-object, so we cast to it and use the typed vtable:
//   GetActorValue          (01) = current hull
//   GetPermanentActorValue (02) = effective max
//   ModActorValue(kDamage) (07) = nudge current toward the host's value
//
// hull-AV form = *(FUN_14049acc0() + 0x108)  (ship-AV/damage registry).
//
// The client clamps the mirror target above 0 so it never DESTROYS the local
// ship - real ship-destruction (host hull -> 0 triggers the player Downed
// system) is a separate death-edge handled later.

#include "PCH.h"
#include "SpaceSync/ShipCombatSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "Coop/CoopMortality.h"

#include <RE/A/ActorValueInfo.h>
#include <RE/A/ActorValueOwner.h>
#include <RE/A/ActorValues.h>
#include <RE/N/NiPoint.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <cmath>
#include <mutex>
#include <vector>

namespace Pulsar::ShipCombatSync {
namespace {
    using namespace std::string_view_literals;

    // FUN_14049acc0 - ship-AV / damage-type registry getter; +0x108 = hull AV.
    constexpr std::uintptr_t kRVA_ShipAvRegistry = 0x49A430;
    constexpr std::size_t    kHullAvFormOffset   = 0x108;
    // ActorValueOwner sub-object offset on the ship REFR (verified slice 1).
    constexpr std::size_t    kAvOwnerOffset      = 0x70;

    // Host read/broadcast cadence + change threshold.
    constexpr std::uint64_t  kReadIntervalMs = 150;
    constexpr std::uint64_t  kHeartbeatMs    = 1000;
    constexpr float          kHullChangeEps  = 0.5f;

    // Client apply: mirror the host hull exactly (incl. 0 = destruction); ignore
    // micro-deltas to avoid per-tick damage-effect spam.
    constexpr float          kApplyEps        = 0.5f;
    // ship destruction: hull at/below this = ship destroyed → both-down
    // game-over. Small epsilon (not exactly 0) absorbs float noise.
    constexpr float          kShipDeadHull    = 0.5f;

    std::atomic<std::uint64_t> g_lastReadMs{ 0 };
    std::atomic<std::uint64_t> g_lastLogMs{ 0 };
    std::atomic<bool>          g_readFailLogged{ false };

    // Host last-broadcast tracking (game-thread only).
    float                      g_lastSentCur = -1.0f;
    float                      g_lastSentMax = -1.0f;
    std::uint64_t              g_lastSentMs  = 0;

    // Client pending apply (network thread -> game thread).
    std::atomic<bool>          g_applyArmed{ false };
    std::atomic<std::uint32_t> g_pendShipForm{ 0 };
    std::atomic<float>         g_pendHullCur{ 0.0f };
    std::atomic<float>         g_pendHullMax{ 0.0f };

    // ---- slice 3: client-side projectile spawn (visible aimed shots) ----
    // Proven REFR-spawn primitive (used by F.1.d loot drops). REL::ID 63591.
    using SpawnRefFn = void (__fastcall*)(
        void* a_self, std::uint32_t* a_outHandle, void* a_baseForm, std::int32_t a_count,
        void* a_extra, RE::NiPoint3* a_pos, RE::NiPoint3* a_rot, std::int32_t* a_refcount);
    constexpr std::uintptr_t kRVA_SpawnRef = 0xB68F10;
    // LookupReferenceByHandle (REL::ID 72986). Writes the REFR into a NiPointer;
    // we pass a raw TESObjectREFR** (leaks one refcount per shot - negligible
    // for cosmetic bolts that despawn shortly).
    using LookupRefFn = bool (__fastcall*)(const std::uint32_t* a_handle, void* a_nipOut);
    constexpr std::uint32_t kID_LookupRefByHandle = 72986;

    struct PendingFire {
        std::uint32_t baseProjForm;
        std::uint32_t weaponForm;
        std::uint32_t shooterShipForm;
        float         origin[3];
        float         dir[3];
        float         vel[3];
    };
    std::vector<PendingFire>   g_fireQueue;
    std::mutex                 g_fireMtx;
    constexpr std::size_t      kMaxQueuedFires = 32;
    std::atomic<std::uint64_t> g_fireSpawnCount{ 0 };
    // projectile-replay spawn is NOT working yet (handle-lookup fails,
    // dir/vel offsets misaligned) and the generic spawn leaves stray invisible
    // projectile REFRs that accumulate. OFF for the Alpha; flip on for
    // dedicated session. Host still SENDS PKT_SHIP_FIRE (harmless) for testing.
    constexpr bool             kFireSpawnEnabled = false;

    // SEH-guarded: spawn the base projectile at the local ship's muzzle (pos=NULL
    // -> engine uses ship.pos + forward, robust to space-frame divergence) and
    // stamp the host's real movementDirection (+0x158) + velocity (+0x164) so it
    // flies aimed. Status: 0 fault, 1 spawned+flying, 2 no ship, 3 no base form,
    // 4 spawn handle 0, 5 spawned but handle lookup failed (static bolt).
    int SpawnShot(const PendingFire& f, std::uint32_t& a_baseType)
    {
        int status = 0;
        a_baseType = 0;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ship   = player ? player->GetSpaceship(true) : nullptr;
            auto* base   = RE::TESForm::LookupByID(f.baseProjForm);
            if (base) a_baseType = static_cast<std::uint32_t>(base->GetFormType());
            if (!ship) {
                status = 2;
            } else if (!base) {
                status = 3;
            } else {
                auto spawnFn = reinterpret_cast<SpawnRefFn>(
                    REL::Offset{ kRVA_SpawnRef }.address());
                std::uint32_t handle = 0;
                spawnFn(ship, &handle, base, 1, nullptr, nullptr, nullptr, nullptr);
                if (handle == 0) {
                    status = 4;
                } else {
                    auto lookupFn = reinterpret_cast<LookupRefFn>(
                        REL::ID(kID_LookupRefByHandle).address());
                    RE::TESObjectREFR* proj = nullptr;
                    lookupFn(&handle, &proj);
                    if (proj) {
                        auto* p = reinterpret_cast<std::uint8_t*>(proj);
                        // Robust trajectory: normalize host dir; speed from host
                        // |vel| if sane else default (guards garbage vel reads).
                        float vmag = sqrtf(f.vel[0] * f.vel[0] + f.vel[1] * f.vel[1] + f.vel[2] * f.vel[2]);
                        if (!(vmag > 1.0f && vmag < 1.0e6f)) vmag = 14000.0f;
                        float dmag = sqrtf(f.dir[0] * f.dir[0] + f.dir[1] * f.dir[1] + f.dir[2] * f.dir[2]);
                        float nx = 0.0f, ny = 1.0f, nz = 0.0f;  // fallback dir
                        if (dmag > 0.01f && dmag < 1.0e6f) { nx = f.dir[0] / dmag; ny = f.dir[1] / dmag; nz = f.dir[2] / dmag; }
                        // Projectile.movementDirection @ +0x158, velocity @ +0x164
                        *reinterpret_cast<float*>(p + 0x158) = nx;
                        *reinterpret_cast<float*>(p + 0x15C) = ny;
                        *reinterpret_cast<float*>(p + 0x160) = nz;
                        *reinterpret_cast<float*>(p + 0x164) = nx * vmag;
                        *reinterpret_cast<float*>(p + 0x168) = ny * vmag;
                        *reinterpret_cast<float*>(p + 0x16C) = nz * vmag;
                        status = 1;
                    } else {
                        status = 5;  // spawned (visible static bolt) but couldn't stamp velocity
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = 0;
        }
        return status;
    }

    // Game-thread: drain queued remote shots and spawn them. Plain C++ (no __try
    // here - SpawnShot owns the SEH frame).
    void DrainFires()
    {
        std::vector<PendingFire> local;
        {
            std::lock_guard<std::mutex> lk(g_fireMtx);
            if (g_fireQueue.empty()) return;
            local.swap(g_fireQueue);
        }
        for (const auto& f : local) {
            std::uint32_t baseType = 0;
            const int st = SpawnShot(f, baseType);
            const std::uint64_t c = g_fireSpawnCount.fetch_add(1) + 1;
            if ((c % 6) == 1) {
                // st: 1 flying, 2 no-ship, 3 no-base-form, 4 spawn-handle-0,
                // 5 spawned-static (lookup failed), 0 fault.
                PLOG_INFO(
                    "[PULSAR] ship-fire spawn st={} base=0x{:08X} baseType={} "
                    "dir=({:.2f},{:.2f},{:.2f}) vel=({:.0f},{:.0f},{:.0f})"sv,
                    st, f.baseProjForm, baseType,
                    f.dir[0], f.dir[1], f.dir[2], f.vel[0], f.vel[1], f.vel[2]);
            }
        }
    }

    // SEH-guarded read of the local ship's hull AV. False on fault / no ship.
    bool ReadLocalHull(float& a_cur, float& a_max, std::uint32_t& a_shipForm)
    {
        bool ok = false;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ship   = player ? player->GetSpaceship(true) : nullptr;
            if (ship) {
                a_shipForm = ship->formID;
                auto regGetter = reinterpret_cast<void* (*)()>(
                    REL::Offset{ kRVA_ShipAvRegistry }.address());
                void* reg = regGetter();
                if (reg) {
                    auto* hullInfo = *reinterpret_cast<RE::ActorValueInfo**>(
                        reinterpret_cast<std::uint8_t*>(reg) + kHullAvFormOffset);
                    if (hullInfo) {
                        auto* avOwner = reinterpret_cast<RE::ActorValueOwner*>(
                            reinterpret_cast<std::uint8_t*>(ship) + kAvOwnerOffset);
                        a_cur = avOwner->GetActorValue(*hullInfo);
                        a_max = avOwner->GetPermanentActorValue(*hullInfo);
                        ok = true;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    // SEH-guarded mirror: nudge local current hull toward a_target (pre-clamped)
    // via the damage modifier. Outputs the pre-apply local cur + applied delta.
    // Returns: 0 = fault, 1 = applied (ship present), 2 = no local ship.
    int ApplyLocalHull(float a_target, float& a_localCurOut, float& a_deltaOut)
    {
        int status = 0;
        a_localCurOut = -1.0f;
        a_deltaOut    = 0.0f;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ship   = player ? player->GetSpaceship(true) : nullptr;
            if (!ship) {
                status = 2;  // client not in a ship - nothing to mirror
            } else {
                auto regGetter = reinterpret_cast<void* (*)()>(
                    REL::Offset{ kRVA_ShipAvRegistry }.address());
                void* reg = regGetter();
                if (reg) {
                    auto* hullInfo = *reinterpret_cast<RE::ActorValueInfo**>(
                        reinterpret_cast<std::uint8_t*>(reg) + kHullAvFormOffset);
                    if (hullInfo) {
                        auto* avOwner = reinterpret_cast<RE::ActorValueOwner*>(
                            reinterpret_cast<std::uint8_t*>(ship) + kAvOwnerOffset);
                        const float localCur = avOwner->GetActorValue(*hullInfo);
                        a_localCurOut = localCur;
                        const float delta = a_target - localCur;
                        if (delta > kApplyEps || delta < -kApplyEps) {
                            avOwner->ModActorValue(
                                RE::ACTOR_VALUE_MODIFIER::kDamage, *hullInfo, delta);
                            a_deltaOut = delta;
                        }
                        status = 1;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = 0;
        }
        return status;
    }
}  // namespace

void Init()
{
    PLOG_INFO("[PULSAR] ShipCombatSync ready (slice2 hull + slice3 fire-spawn)"sv);
}

void Tick()
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected()) return;

    const std::uint64_t now = ::GetTickCount64();

    if (net.IsHost()) {
        // ---- Host: read hull; broadcast on change + heartbeat. ----
        // falling-edge guard (game-thread only): only treat hull=0 as a real
        // destruction once we have seen THIS ship healthy - rejects a cold/transient
        // 0 (max>0) read on ship spawn/load. Disarmed on trigger so a post-reload
        // transient 0 cannot loop the game-over.
        static bool s_hostHullSeenHealthy = false;

        if ((now - g_lastReadMs.load(std::memory_order_relaxed)) < kReadIntervalMs) return;
        g_lastReadMs.store(now, std::memory_order_relaxed);

        float         cur = 0.0f, max = 0.0f;
        std::uint32_t shipForm = 0;
        if (!ReadLocalHull(cur, max, shipForm)) {
            s_hostHullSeenHealthy = false;  // ship gone → re-arm needs a fresh healthy read
            if (shipForm != 0 && !g_readFailLogged.exchange(true)) {
                PLOG_INFO("[PULSAR] ShipAV read FAILED (ship=0x{:08X})"sv, shipForm);
            }
            return;
        }
        g_readFailLogged.store(false, std::memory_order_relaxed);

        // ship destruction: our own (shared) ship hull fell to 0 AFTER being
        // healthy → enter the downed state. The peer mirrors this hull via
        // PKT_SHIP_AV and triggers the same on its end, so both sides flag down →
        // OnGameThreadTick's both-downed game-over reloads BOTH. Idempotent.
        if (max > 0.0f && cur > kShipDeadHull) s_hostHullSeenHealthy = true;
        const bool shipDestroyed = s_hostHullSeenHealthy && (max > 0.0f) && (cur <= kShipDeadHull);
        if (shipDestroyed) {
            s_hostHullSeenHealthy = false;  // disarm → no post-reload re-trigger loop
            Pulsar::Coop::Mortality::TriggerShipDowned();
        }

        const bool changed =
            (cur < g_lastSentCur - kHullChangeEps) || (cur > g_lastSentCur + kHullChangeEps) ||
            (max < g_lastSentMax - kHullChangeEps) || (max > g_lastSentMax + kHullChangeEps);
        const bool heartbeat = (now - g_lastSentMs) >= kHeartbeatMs;
        if (!changed && !heartbeat) return;

        g_lastSentCur = cur;
        g_lastSentMax = max;
        g_lastSentMs  = now;

        Pulsar::Network::ShipAvData d{};
        d.shipFormId = shipForm;
        d.hullCur    = cur;
        d.hullMax    = max;
        std::uint8_t buf[Pulsar::Network::HEADER_SIZE + sizeof(Pulsar::Network::ShipAvData)]{};
        Pulsar::Network::PacketWriter w(Pulsar::Network::PKT_SHIP_AV, buf, sizeof(buf));
        if (w.Write(d)) {
            // Destruction (hull=0) MUST reach the client so it also goes downed -
            // send it reliable; the wreck may despawn before the next 1 Hz
            // heartbeat could retry. Normal hull updates stay unreliable.
            net.Send(w.Data(), w.Size(), /*reliable=*/shipDestroyed);
        }
        if ((now - g_lastLogMs.load(std::memory_order_relaxed)) >= 1000) {
            g_lastLogMs.store(now, std::memory_order_relaxed);
            PLOG_INFO(
                "[PULSAR] ShipAV TX hull cur={:.1f} max={:.1f} ship=0x{:08X}"sv,
                cur, max, shipForm);
        }
        return;
    }

    // ---- Client ----
    // slice 3 - spawn any queued remote ship shots (client only; the host
    // fires its own real weapons and must never replay-spawn duplicates).
    DrainFires();

    // Apply the latest received hull.
    if (!g_applyArmed.exchange(false, std::memory_order_acq_rel)) return;

    const float         hostCur  = g_pendHullCur.load(std::memory_order_relaxed);
    const float         hostMax  = g_pendHullMax.load(std::memory_order_relaxed);
    const std::uint32_t hostShip = g_pendShipForm.load(std::memory_order_relaxed);

    // ship destruction: the host's (shared) ship hull fell to 0 AFTER being
    // healthy → the client must ALSO go downed so the both-down game-over reloads
    // both sides. Authoritative signal = the host's hull (independent of the local
    // mirror apply below). Falling-edge guarded like the host; disarmed on trigger.
    static bool s_clientHullSeenHealthy = false;
    if (hostMax > 0.0f && hostCur > kShipDeadHull) s_clientHullSeenHealthy = true;
    if (s_clientHullSeenHealthy && (hostMax > 0.0f) && (hostCur <= kShipDeadHull)) {
        s_clientHullSeenHealthy = false;
        Pulsar::Coop::Mortality::TriggerShipDowned();
    }

    // death-edge - mirror the host hull EXACTLY, incl. 0. When the host
    // ship is destroyed (hull→0) the client ship must ALSO be destroyed so BOTH
    // players die in space → coordinated both-down game-over (the CoopMortality
    // ship-death gate then runs vanilla explosion + reload on each machine). The
    // old floor (never reach 0) left the client ship alive → only the host died.
    float target = hostCur;
    if (target < 0.0f) target = 0.0f;
    if (hostMax > 0.0f && target > hostMax) target = hostMax;

    float     localCur = -1.0f, applied = 0.0f;
    const int st = ApplyLocalHull(target, localCur, applied);
    if (st == 2) return;  // client not in a ship - nothing to mirror, no log

    if ((now - g_lastLogMs.load(std::memory_order_relaxed)) >= 1000) {
        g_lastLogMs.store(now, std::memory_order_relaxed);
        if (st == 1) {
            PLOG_INFO(
                "[PULSAR] ShipAV RX hostHull={:.1f}/{:.1f} localWas={:.1f} "
                "applied={:.1f} target={:.1f} ship=0x{:08X}{}"sv,
                hostCur, hostMax, localCur, applied, target, hostShip,
                hostCur <= 0.0f ? " (host ship DESTROYED - mirroring 0 → client ship dies too)" : "");
        } else {
            PLOG_INFO(
                "[PULSAR] ShipAV apply FAULT (hostHull={:.1f} ship=0x{:08X})"sv,
                hostCur, hostShip);
        }
    }
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    Pulsar::Network::PacketReader r(data, length);
    if (!r.IsValid()) return;
    if (r.Type() != Pulsar::Network::PKT_SHIP_AV) return;

    Pulsar::Network::ShipAvData d{};
    if (!r.Read(d)) return;

    g_pendShipForm.store(d.shipFormId, std::memory_order_relaxed);
    g_pendHullCur.store(d.hullCur, std::memory_order_relaxed);
    g_pendHullMax.store(d.hullMax, std::memory_order_relaxed);
    g_applyArmed.store(true, std::memory_order_release);
}

void OnRemoteFire(const Pulsar::Network::ShipFireData& f)
{
    if (!kFireSpawnEnabled) return;  // replay off for Alpha (no stray REFRs)
    PendingFire pf{};
    pf.baseProjForm    = f.baseProjFormId;
    pf.weaponForm      = f.weaponFormId;
    pf.shooterShipForm = f.shooterShipFormId;
    pf.origin[0] = f.origin[0]; pf.origin[1] = f.origin[1]; pf.origin[2] = f.origin[2];
    pf.dir[0]    = f.dir[0];    pf.dir[1]    = f.dir[1];    pf.dir[2]    = f.dir[2];
    pf.vel[0]    = f.vel[0];    pf.vel[1]    = f.vel[1];    pf.vel[2]    = f.vel[2];
    std::lock_guard<std::mutex> lk(g_fireMtx);
    if (g_fireQueue.size() < kMaxQueuedFires) g_fireQueue.push_back(pf);
}

}  // namespace Pulsar::ShipCombatSync
