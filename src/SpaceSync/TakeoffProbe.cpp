// Phase L.1 V1 - Takeoff/Landing/etc state-change probe.
//
// Logs comprehensive player+ship state at the moment a Spaceship event fires
// AND at delayed checkpoints (100ms / 500ms / 2s / 5s) so we can see when
// state actually changes during the takeoff cutscene.
//
// Output: PULSAR.log (REX::INFO) + NASLog (so client logs land on <NAS>).
//
// Scope: read-only, no mutations. Pure diagnostic.
//
// Key things we want to learn:
// Does player.parentCell.formID change during takeoff?
// Does parentCell.cellWorldspace flip from planet-WS to space-WS?
// When is GetSpaceship() valid vs null relative to event firing?
// Where does the spaceship REFR sit (cell + position) before/after?
// Are pilot/dock relationships stable across the transition?

#include "PCH.h"
#include "SpaceSync/TakeoffProbe.h"

#include "Network/NetworkHost.h"
#include "NASLog.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESWorldSpace.h>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::TakeoffProbe {
namespace {

    static const char* KindStr(std::uint8_t k)
    {
        switch (k) {
            case 0: return "TakeOff";
            case 1: return "Landing";
            case 2: return "GravJump";
            case 3: return "Dock";
            case 4: return "TakeoffMenu_Launch";
            case 5: return "RampDown";
            case 6: return "FarTravel";
        }
        return "?";
    }

    // Delayed re-capture schedule (ms after event fire).
    static constexpr std::uint64_t kDelaysMs[] = { 100, 500, 2000, 5000 };
    static constexpr std::size_t   kDelayCount = sizeof(kDelaysMs) / sizeof(kDelaysMs[0]);

    // L.6 V1.2 - Dock-Probe-Window. After DockEvent fires, we sample every
    // kDockProbeIntervalMs for kDockProbeDurationMs to capture the full
    // Phase A (cutscene + visual attach) sequence. Marker logs on every
    // detectable change to pinpoint the moment the engine flips state.
    constexpr std::uint64_t kDockProbeIntervalMs = 200;   // 5 Hz
    constexpr std::uint64_t kDockProbeDurationMs = 30000; // 30s
    static std::atomic<std::uint64_t> g_dockProbeUntilMs{ 0 };
    static std::atomic<std::uint64_t> g_dockProbeNextSampleMs{ 0 };
    static std::atomic<std::uint32_t> g_dockProbeLastPlayerCell{ 0 };
    static std::atomic<std::uint32_t> g_dockProbeLastShipCell{ 0 };
    static std::atomic<std::uint32_t> g_dockProbeLastDockFormID{ 0 };
    static std::atomic<int>           g_dockProbeSampleCounter{ 0 };

    // Increment counter - used for sample numbering in logs.
    static int NextDockSampleNum() {
        return g_dockProbeSampleCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    struct Pending {
        std::uint64_t fireTick;
        std::uint8_t  kind;
        std::uint8_t  delayIdx;   // next index into kDelaysMs to fire
    };
    static std::mutex          g_mutex;
    static std::vector<Pending> g_queue;

    // Captured snapshot for one moment in time. All raw reads SEH-guarded.
    //
    // NOTE: CommonLibSF has GetSpaceshipPilot, GetSpaceshipParentDock,
    // IsSpaceshipDocked, IsSpaceshipLanded as REL::ID(0) PLACEHOLDERS but the
    // REAL IDs are in comments. Versionlib resolves them, but actual function
    // signatures don't always match CommonLibSF declarations. We use raw
    // RVA-based wrappers for those with clean signatures and skip the BSComp.DB2
    // queries that need component-key context.
    //
    // RVA verification 2026-04-29 (Ghidra the local Ghidra project):
    //   GetSpaceship              119881 → 0x2116840  ✅ usable via CLib
    //   IsInSpace                 63482  → 0xB58D50   ✅ usable via CLib
    //   GetSpaceshipParentDock    174134 → 0x3428FB0  ✅ raw wrapper (this, out**)
    //   GetSpaceshipPilot         173834 → 0x341FF20  ❌ 4-arg BSComponentDB2 query
    //   IsSpaceshipLanded         173880 → 0x3421410  ❌ 4-arg BSComponentDB2 query
    //   IsSpaceshipDocked         174146 → 0x3429390  ❌ thunks float arg + vt[33]
    struct Snapshot {
        bool          valid                = false;
        std::uint32_t playerCellFormID     = 0;
        std::uint32_t playerCellWSFormID   = 0;
        std::uint32_t cellFlagsRaw         = 0;     // raw cellFlags+state dword
        std::uint8_t  cellState            = 0xFF;
        bool          cellIsInterior       = false;
        float         playerX = 0, playerY = 0, playerZ = 0;
        float         playerAngZ           = 0;
        std::uint32_t shipFormID           = 0;     // GetSpaceship(true) result
        std::uint32_t shipParentCellID     = 0;
        std::uint32_t shipCellWSFormID     = 0;
        std::uint32_t shipCellFlagsRaw     = 0;     // ship's cell flags
        bool          shipCellIsInterior   = false;
        bool          shipCellIsProc       = false; // FF-prefix instance?
        float         shipX = 0, shipY = 0, shipZ = 0;
        std::uint32_t shipDockFormID       = 0;     // GetSpaceshipParentDock raw
        bool          playerIsInSpace      = false; // IsInSpace() helper
    };

    // Raw-RVA wrapper for GetSpaceshipParentDock (CommonLibSF placeholder
    // REL::ID(0), real ID 174134, RVA 0x3428FB0 verified in Ghidra).
    // Disasm: push rbx; sub rsp,0x20; mov rcx,[rcx+0x88]; mov rbx,rdx;
    //         mov rax,[rcx]; call [rax+0x190] (vt[50]); movsd [rbx],xmm0;
    //         add rsp,0x20; pop rbx; ret
    // Signature: void(this, void** outDock).
    using GetParentDockFn = void(__fastcall*)(RE::TESObjectREFR* this_, void** outDock);
    static GetParentDockFn ResolveGetParentDock()
    {
        static GetParentDockFn fn = nullptr;
        if (!fn) {
            fn = reinterpret_cast<GetParentDockFn>(
                REL::Offset{ 0x3428FB0 }.address());
        }
        return fn;
    }

    // Capture all the state we can. Each chunk SEH-guarded so failure in one
    // section doesn't kill the others.
    static Snapshot CaptureNow()
    {
        Snapshot s;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return s;
        s.valid = true;

        // --- Player position ---
        __try {
            s.playerX = player->data.location.x;
            s.playerY = player->data.location.y;
            s.playerZ = player->data.location.z;
            s.playerAngZ = player->data.angle.z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // --- Player parentCell ---
        __try {
            auto* cell = player->parentCell;
            if (cell) {
                s.playerCellFormID = cell->formID;
                // cellFlags raw - read 4 bytes at cell+0x48 (raw layout, +8
                // shifted from CommonLibSF compiled-struct).
                s.cellFlagsRaw = *reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(cell) + 0x48);
                s.cellState = *reinterpret_cast<std::uint8_t*>(
                    reinterpret_cast<std::uint8_t*>(cell) + 0x4F);
                s.cellIsInterior = (s.cellFlagsRaw & 0x01) != 0;
                if (cell->cellWorldspace) {
                    s.playerCellWSFormID = cell->cellWorldspace->formID;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // --- Player ship (GetSpaceship) - VALID REL::ID 119881 ---
        __try {
            auto* ship = player->GetSpaceship(true);
            if (ship) {
                s.shipFormID = ship->formID;
                s.shipX = ship->data.location.x;
                s.shipY = ship->data.location.y;
                s.shipZ = ship->data.location.z;
                if (ship->parentCell) {
                    auto* sc = ship->parentCell;
                    s.shipParentCellID = sc->formID;
                    s.shipCellIsProc   = (sc->formID & 0xFF000000u) == 0xFF000000u;
                    s.shipCellFlagsRaw = *reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<std::uint8_t*>(sc) + 0x48);
                    s.shipCellIsInterior = (s.shipCellFlagsRaw & 0x01) != 0;
                    if (sc->cellWorldspace) {
                        s.shipCellWSFormID = sc->cellWorldspace->formID;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // --- IsInSpace - VALID REL::ID 63482 ---
        __try {
            s.playerIsInSpace = player->IsInSpace(true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // --- GetSpaceshipParentDock - raw RVA 0x3428FB0 ---
        // Reads this->member@+0x88, calls vt[50], writes result via xmm0/movsd.
        // L.6 V1.1 FIX (2026-05-02): CommonLibSF declares this on
        // TESObjectREFR - must be called on the SHIP REFR, not the player.
        // Pre-fix logs showed dock=0 always; calling on ship resolves it.
        __try {
            void* dockOut = nullptr;
            auto fn = ResolveGetParentDock();
            auto* ship = player->GetSpaceship(true);
            if (fn && ship) {
                fn(ship, &dockOut);
                if (dockOut) {
                    auto* dockRef = reinterpret_cast<RE::TESObjectREFR*>(dockOut);
                    s.shipDockFormID = dockRef->formID;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // GetSpaceshipPilot (173834) and IsSpaceshipLanded (173880) are
        // BSComponentDB2 queries with 4-arg signatures requiring a component-
        // key argument that callers must construct. Skipped here - would need
        // deeper RE to integrate; see feedback_commonlibsf_zero_id_lookup.md.
        // IsSpaceshipDocked (174146) thunks a float arg + tail-calls vt[33] -
        // signature is unclear, also skipped.
        return s;
    }

    static void LogSnapshot(std::uint8_t kind, const char* a_when, const Snapshot& s)
    {
        if (!s.valid) {
            PLOG_INFO("[PULSAR] TakeoffProbe {} {}: <player null>"sv,
                      KindStr(kind), a_when);
            NASLOG("TakeoffProbe %s %s: <player null>", KindStr(kind), a_when);
            return;
        }
        PLOG_INFO(
            "[PULSAR] TakeoffProbe {} {}: "
            "pcell=0x{:08X} pcellWS=0x{:08X} pflags=0x{:08X} pintr={} "
            "ppos=({:.0f},{:.0f},{:.0f}) angZ={:.2f} inSpace={} | "
            "ship=0x{:08X} shipCell=0x{:08X} shipCellWS=0x{:08X} shipFlags=0x{:08X} "
            "shipIntr={} shipProc={} shipPos=({:.0f},{:.0f},{:.0f}) dock=0x{:08X}"sv,
            KindStr(kind), a_when,
            s.playerCellFormID, s.playerCellWSFormID, s.cellFlagsRaw, s.cellIsInterior ? 1 : 0,
            s.playerX, s.playerY, s.playerZ, s.playerAngZ, s.playerIsInSpace ? 1 : 0,
            s.shipFormID, s.shipParentCellID, s.shipCellWSFormID, s.shipCellFlagsRaw,
            s.shipCellIsInterior ? 1 : 0, s.shipCellIsProc ? 1 : 0,
            s.shipX, s.shipY, s.shipZ, s.shipDockFormID);

        NASLOG(
            "TakeoffProbe %s %s: pcell=0x%08X pWS=0x%08X pintr=%d ppos=(%.0f,%.0f,%.0f) "
            "ship=0x%08X shipCell=0x%08X shipWS=0x%08X shipIntr=%d shipProc=%d "
            "shipPos=(%.0f,%.0f,%.0f) dock=0x%08X inSpace=%d",
            KindStr(kind), a_when,
            s.playerCellFormID, s.playerCellWSFormID, s.cellIsInterior ? 1 : 0,
            s.playerX, s.playerY, s.playerZ,
            s.shipFormID, s.shipParentCellID, s.shipCellWSFormID,
            s.shipCellIsInterior ? 1 : 0, s.shipCellIsProc ? 1 : 0,
            s.shipX, s.shipY, s.shipZ,
            s.shipDockFormID, s.playerIsInSpace ? 1 : 0);
    }

    // L.6 V1.2 - Arm/extend the probe window. Called from Dock + RampDown.
    static void ArmDockProbeWindow(const char* a_reason)
    {
        std::uint64_t now = ::GetTickCount64();
        g_dockProbeUntilMs.store(now + kDockProbeDurationMs, std::memory_order_release);
        g_dockProbeNextSampleMs.store(now, std::memory_order_release);
        g_dockProbeSampleCounter.store(0, std::memory_order_release);

        PLOG_INFO("[PULSAR] L.6 V1.2 DockProbe ARMED reason={} duration={}ms interval={}ms"sv,
                  a_reason, kDockProbeDurationMs, kDockProbeIntervalMs);
        NASLOG("L.6 V1.2 DockProbe ARMED reason=%s duration=%llums",
               a_reason, (unsigned long long)kDockProbeDurationMs);
    }

    static void DockProbeSample()
    {
        int n = NextDockSampleNum();
        auto s = CaptureNow();

        // Detect changes since last sample.
        std::uint32_t prevPCell = g_dockProbeLastPlayerCell.exchange(s.playerCellFormID,
                                       std::memory_order_acq_rel);
        std::uint32_t prevSCell = g_dockProbeLastShipCell.exchange(s.shipParentCellID,
                                       std::memory_order_acq_rel);
        std::uint32_t prevDock  = g_dockProbeLastDockFormID.exchange(s.shipDockFormID,
                                       std::memory_order_acq_rel);

        // Always log the periodic sample.
        PLOG_INFO(
            "[PULSAR] L.6 V1.2 DockProbe #{}: pcell=0x{:08X} ppos=({:.0f},{:.0f},{:.0f}) "
            "ship=0x{:08X} shipCell=0x{:08X} shipPos=({:.0f},{:.0f},{:.0f}) "
            "dock=0x{:08X} inSpace={}"sv,
            n, s.playerCellFormID, s.playerX, s.playerY, s.playerZ,
            s.shipFormID, s.shipParentCellID, s.shipX, s.shipY, s.shipZ,
            s.shipDockFormID, s.playerIsInSpace ? 1 : 0);

        // Specially highlight changes - these are the points where engine
        // state flipped (pre vs post Phase A, or hatch transit, etc.).
        if (n > 1 && prevPCell != s.playerCellFormID) {
            PLOG_INFO("[PULSAR] L.6 V1.2 DockProbe ⚡ PLAYER-CELL CHANGE 0x{:08X} → 0x{:08X}"sv,
                      prevPCell, s.playerCellFormID);
            NASLOG("L.6 V1.2 DockProbe PLAYER-CELL 0x%08X -> 0x%08X",
                   prevPCell, s.playerCellFormID);
        }
        if (n > 1 && prevSCell != s.shipParentCellID) {
            PLOG_INFO("[PULSAR] L.6 V1.2 DockProbe ⚡ SHIP-CELL CHANGE 0x{:08X} → 0x{:08X}"sv,
                      prevSCell, s.shipParentCellID);
            NASLOG("L.6 V1.2 DockProbe SHIP-CELL 0x%08X -> 0x%08X",
                   prevSCell, s.shipParentCellID);
        }
        if (n > 1 && prevDock != s.shipDockFormID) {
            PLOG_INFO("[PULSAR] L.6 V1.2 DockProbe ⚡ DOCK-FORMID CHANGE 0x{:08X} → 0x{:08X}"sv,
                      prevDock, s.shipDockFormID);
            NASLOG("L.6 V1.2 DockProbe DOCK-FORMID 0x%08X -> 0x%08X",
                   prevDock, s.shipDockFormID);
        }
    }

    // Tick - drain probe window samples (called from main Tick()).
    static void TickDockProbe()
    {
        std::uint64_t until = g_dockProbeUntilMs.load(std::memory_order_acquire);
        if (until == 0) return;
        std::uint64_t now = ::GetTickCount64();
        if (now >= until) {
            g_dockProbeUntilMs.store(0, std::memory_order_release);
            int n = g_dockProbeSampleCounter.load(std::memory_order_acquire);
            PLOG_INFO("[PULSAR] L.6 V1.2 DockProbe WINDOW CLOSED - {} samples taken"sv, n);
            NASLOG("L.6 V1.2 DockProbe WINDOW CLOSED (%d samples)", n);
            return;
        }
        std::uint64_t nextSample = g_dockProbeNextSampleMs.load(std::memory_order_acquire);
        if (now >= nextSample) {
            g_dockProbeNextSampleMs.store(now + kDockProbeIntervalMs,
                                          std::memory_order_release);
            DockProbeSample();
        }
    }

}  // namespace

bool IsDockProbeActive()
{
    std::uint64_t until = g_dockProbeUntilMs.load(std::memory_order_acquire);
    return until != 0 && ::GetTickCount64() < until;
}

void LogActivateDuringDockProbe(std::uint32_t a_targetFormID,
                                 std::uint32_t a_baseFormID,
                                 std::uint8_t a_baseFormType)
{
    if (!IsDockProbeActive()) return;
    PLOG_INFO("[PULSAR] L.6 V1.2 DockProbe ⚡ ACTIVATE target=0x{:08X} base=0x{:08X} ft=0x{:02X}"sv,
              a_targetFormID, a_baseFormID, a_baseFormType);
    NASLOG("L.6 V1.2 DockProbe ACTIVATE target=0x%08X base=0x%08X ft=0x%02X",
           a_targetFormID, a_baseFormID, a_baseFormType);
}

void Init()
{
    PLOG_INFO("[PULSAR] TakeoffProbe L.1 V1 / L.6 V1.2 init"sv);
    NASLOG("TakeoffProbe: init");
}

void OnEventFire(std::uint8_t a_kind)
{
    // Capture snapshot immediately (event-thread, may not be game thread).
    auto snap = CaptureNow();
    LogSnapshot(a_kind, "T+0", snap);

    // L.6 V1.2 - Dock-Probe-Window arming on Dock and RampDown.
    if (a_kind == 3 /*Dock*/) {
        ArmDockProbeWindow("DockEvent");
    } else if (a_kind == 5 /*RampDown*/ && IsDockProbeActive()) {
        // Extend window so we keep probing past RampDown into hatch usage.
        ArmDockProbeWindow("RampDown(extended)");
    }

    // Schedule delayed re-captures.
    Pending p{};
    p.fireTick = ::GetTickCount64();
    p.kind     = a_kind;
    p.delayIdx = 0;
    {
        std::lock_guard lock(g_mutex);
        g_queue.push_back(p);
    }
}

void Tick()
{
    // L.6 V1.2 - Dock-Probe-Window sampling (always runs, gates inside).
    TickDockProbe();

    std::vector<Pending> toFire;
    std::vector<Pending> retain;
    {
        std::lock_guard lock(g_mutex);
        if (g_queue.empty()) return;
        auto now = ::GetTickCount64();
        for (auto& p : g_queue) {
            // How many delays in the schedule have expired since fireTick?
            // Capture each one we haven't logged yet, then advance delayIdx.
            while (p.delayIdx < kDelayCount &&
                   (now - p.fireTick) >= kDelaysMs[p.delayIdx])
            {
                toFire.push_back(p);
                p.delayIdx++;
            }
            if (p.delayIdx < kDelayCount) {
                retain.push_back(p);
            }
        }
        g_queue = std::move(retain);
    }

    for (const auto& p : toFire) {
        char label[24]{};
        // p.delayIdx in toFire holds the index that just fired (push happens
        // before the increment in the producer loop).
        if (p.delayIdx < kDelayCount) {
            std::uint64_t ms = kDelaysMs[p.delayIdx];
            snprintf(label, sizeof(label), "T+%llums", static_cast<unsigned long long>(ms));
        } else {
            snprintf(label, sizeof(label), "T+?");
        }
        auto snap = CaptureNow();
        LogSnapshot(p.kind, label, snap);
    }
}

}  // namespace Pulsar::TakeoffProbe
