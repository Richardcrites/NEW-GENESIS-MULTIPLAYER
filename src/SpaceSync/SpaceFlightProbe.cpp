// Phase L.3 V2 V0 - Space-flight runtime probe.
//
// Goal: profile-diff approach. While player pilots in space, dump candidate
// sources of flight state every 250ms:
//   1. Player REFR.data (location/angle) - REFR is known to be at (0,0,0)
//      for the SHIP REFR; we want to confirm what PLAYER REFR shows.
//   2. Ship REFR raw memory bytes at +0x80..+0xC8 (data + loadedData pointers).
//   3. Ship loadedData (+0xB0) - pointer to the runtime data including 3D + havok.
//   4. PilotCameraShaker @ 0x1461EAF80 - confirmed shake module; check if
//      shake-displacement (+0x50..+0x5C) actually accumulates during piloting.
//   5. Ship 3D NiNode local + world transform - ship's render orientation.
//
// Static RE narrowed candidates (see reference_space_flight_architecture.md)
// but couldn't pinpoint per-frame writers. This probe gives ground truth.
//
// Output: PULSAR.log + NAS log. SEH-guarded per-section so a bad pointer
// in one source doesn't kill the rest.

#include "PCH.h"
#include "SpaceSync/SpaceFlightProbe.h"

#include "NASLog.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <REL/Relocation.h>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

using namespace std::string_view_literals;

namespace Pulsar::SpaceFlightProbe {
namespace {

    static constexpr std::uint64_t kTickIntervalMs = 250;
    static constexpr std::uint64_t kProbeWindowMs  = 30000;  // 30s of probing
    static std::atomic<std::uint64_t> g_lastTickMs{ 0 };
    static std::atomic<std::uint64_t> g_firstSpaceTickMs{ 0 };
    static std::atomic<bool>          g_inSpaceLast{ false };

    // PilotCameraShaker singleton pointer - RE'd via Ghidra to live at this VA.
    // (RVA 0x61EAF80, class PilotCameraShaker, static RE 2026-04-29.)
    // The address holds a `PilotCameraShaker*`; if non-null, +0x50..+0x5C is
    // the shake-displacement vec4 and +0x78 is the shake intensity scalar.
    static constexpr std::uintptr_t kPilotCameraShakerSingletonRVA = 0x61EAF80;

    // Read a vec3 from raw offset on REFR. Returns zero on access fault.
    static void ReadFloat3Raw(const void* base, std::uintptr_t off,
                              float& x, float& y, float& z)
    {
        x = y = z = 0.0f;
        if (!base) return;
        __try {
            const auto* p = reinterpret_cast<const std::uint8_t*>(base) + off;
            x = *reinterpret_cast<const float*>(p + 0);
            y = *reinterpret_cast<const float*>(p + 4);
            z = *reinterpret_cast<const float*>(p + 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            x = y = z = 0.0f;
        }
    }

    // Read a qword pointer from raw offset. Returns 0 on access fault.
    static std::uintptr_t ReadPtrRaw(const void* base, std::uintptr_t off)
    {
        if (!base) return 0;
        __try {
            const auto* p = reinterpret_cast<const std::uint8_t*>(base) + off;
            return *reinterpret_cast<const std::uintptr_t*>(p);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static float ReadFloatRaw(const void* base, std::uintptr_t off)
    {
        if (!base) return 0.0f;
        __try {
            const auto* p = reinterpret_cast<const std::uint8_t*>(base) + off;
            return *reinterpret_cast<const float*>(p);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0.0f;
        }
    }

    // Resolve PilotCameraShaker singleton pointer. The DAT itself stores a
    // `PilotCameraShaker*` - could be null until first flight init.
    static void* ResolvePilotCameraShaker()
    {
        __try {
            const auto* slot = reinterpret_cast<const std::uintptr_t*>(
                REL::Offset{ kPilotCameraShakerSingletonRVA }.address());
            return slot ? reinterpret_cast<void*>(*slot) : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Snapshot - all fields zero by default. Booleans gate "did we read".
    struct Snapshot {
        // Player REFR
        bool   havePlayer = false;
        float  pX = 0, pY = 0, pZ = 0;
        float  pAngX = 0, pAngY = 0, pAngZ = 0;

        // Ship REFR
        bool          haveShip = false;
        std::uint32_t shipFormID = 0;
        float         sX = 0, sY = 0, sZ = 0;
        float         sAngX = 0, sAngY = 0, sAngZ = 0;
        std::uintptr_t shipLoadedDataPtr = 0;     // ship+0xB0
        std::uintptr_t shipData3DPtr     = 0;     // loadedData->data3D (NiNode*)
        // Ship 3D NiNode world transform (rotation row 1 + translation if reachable).
        float niWorldTrans[3] = { 0, 0, 0 };
        float niLocalTrans[3] = { 0, 0, 0 };
        float niWorldRot00 = 0, niWorldRot01 = 0, niWorldRot02 = 0;
        float niWorldRot10 = 0, niWorldRot11 = 0, niWorldRot12 = 0;
        float niWorldRot20 = 0, niWorldRot21 = 0, niWorldRot22 = 0;

        // PilotCameraShaker singleton state.
        std::uintptr_t shakerInst = 0;
        float          shakeDispX = 0, shakeDispY = 0, shakeDispZ = 0, shakeDispW = 0;
        float          shakeIntensity = 0;
        float          shakeBound = 0;

        // Player REFR NiNode (separate object - player is child of ship).
        std::uintptr_t playerLoadedDataPtr = 0;
        std::uintptr_t playerData3DPtr     = 0;
        float          plNiWorldT[3] = { 0, 0, 0 };
        float          plNiWorldRot00 = 0, plNiWorldRot01 = 0, plNiWorldRot02 = 0;
        float          plNiWorldRot10 = 0, plNiWorldRot11 = 0, plNiWorldRot12 = 0;
        float          plNiWorldRot20 = 0, plNiWorldRot21 = 0, plNiWorldRot22 = 0;

        // Raw dump of first 8 qwords of ship's LOADED_REF_DATA (verify layout).
        std::uint64_t ldRaw[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

        // IsInSpace flag.
        bool inSpace = false;

        // Ship cell formID + flags (for context).
        std::uint32_t shipCellFormID = 0;
        bool          shipCellIsProc = false;
    };

    static Snapshot CaptureNow()
    {
        Snapshot s;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return s;
        s.havePlayer = true;

        __try {
            s.pX = player->data.location.x;
            s.pY = player->data.location.y;
            s.pZ = player->data.location.z;
            s.pAngX = player->data.angle.x;
            s.pAngY = player->data.angle.y;
            s.pAngZ = player->data.angle.z;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        __try {
            s.inSpace = player->IsInSpace(true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        __try {
            auto* ship = player->GetSpaceship(true);
            if (ship) {
                s.haveShip   = true;
                s.shipFormID = ship->formID;
                s.sX = ship->data.location.x;
                s.sY = ship->data.location.y;
                s.sZ = ship->data.location.z;
                s.sAngX = ship->data.angle.x;
                s.sAngY = ship->data.angle.y;
                s.sAngZ = ship->data.angle.z;

                // Cell context.
                auto* cell = ship->parentCell;
                if (cell) {
                    s.shipCellFormID = cell->formID;
                    s.shipCellIsProc = (cell->formID & 0xFF000000u) == 0xFF000000u;
                }

                // Ship loadedData @ TESObjectREFR + 0xB8 (CommonLibSF - verified
                // by CellDiag/Cell3DAttach which read live heap pointers here).
                // Earlier V0.2 used +0xB0 (sfse-0.2.19 value) and got vtable-shaped
                // garbage - wrong offset.
                s.shipLoadedDataPtr = ReadPtrRaw(ship, 0xB8);

                if (s.shipLoadedDataPtr) {
                    // Verify layout: dump first 8 qwords of LOADED_REF_DATA.
                    auto* ldBase = reinterpret_cast<void*>(s.shipLoadedDataPtr);
                    for (int i = 0; i < 8; ++i) {
                        s.ldRaw[i] = static_cast<std::uint64_t>(
                            ReadPtrRaw(ldBase, i * 8));
                    }
                }
                if (s.shipLoadedDataPtr) {
                    // Verified layout: REFR.loadedData (+0xB0) is BSGuarded
                    // wrapping LOADED_REF_DATA*. The qword AT +0xB0 is the
                    // LOADED_REF_DATA*. data3D is NiPointer<NiAVObject> at
                    // LOADED_REF_DATA+0x08 (CommonLibSF + sfse confirmed).
                    // NiPointer's raw ptr is at offset 0, so:
                    //   ship->loadedData (REFR+0xB0) -> LOADED_REF_DATA*
                    //   *(LOADED_REF_DATA + 0x08)    -> NiAVObject*
                    auto base = reinterpret_cast<void*>(s.shipLoadedDataPtr);
                    s.shipData3DPtr = ReadPtrRaw(base, 0x08);

                    if (s.shipData3DPtr) {
                        // NiAVObject:
                        //   +0x40: local NiTransform { rot 0x30, trans 0x30..0x3C, scale 0x3C }
                        //   +0x80: world NiTransform { rot 0x80, trans 0xB0..0xBC, scale 0xBC }
                        //   sizeof(NiTransform) = 0x40
                        // → world.rotate = +0x80 (9 floats, 3x3)
                        // → world.translate = +0xB0 (3 floats)
                        // → local.translate = +0x70 (3 floats)
                        auto* nb = reinterpret_cast<void*>(s.shipData3DPtr);
                        ReadFloat3Raw(nb, 0x70, s.niLocalTrans[0],
                                                  s.niLocalTrans[1],
                                                  s.niLocalTrans[2]);
                        ReadFloat3Raw(nb, 0xB0, s.niWorldTrans[0],
                                                  s.niWorldTrans[1],
                                                  s.niWorldTrans[2]);
                        s.niWorldRot00 = ReadFloatRaw(nb, 0x80);
                        s.niWorldRot01 = ReadFloatRaw(nb, 0x84);
                        s.niWorldRot02 = ReadFloatRaw(nb, 0x88);
                        s.niWorldRot10 = ReadFloatRaw(nb, 0x8C);
                        s.niWorldRot11 = ReadFloatRaw(nb, 0x90);
                        s.niWorldRot12 = ReadFloatRaw(nb, 0x94);
                        s.niWorldRot20 = ReadFloatRaw(nb, 0x98);
                        s.niWorldRot21 = ReadFloatRaw(nb, 0x9C);
                        s.niWorldRot22 = ReadFloatRaw(nb, 0xA0);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // PlayerCharacter NiNode (player is parented to ship cockpit while
        // piloting - its NiNode may rotate even if ship REFR doesn't).
        __try {
            s.playerLoadedDataPtr = ReadPtrRaw(player, 0xB8);
            if (s.playerLoadedDataPtr) {
                auto* base = reinterpret_cast<void*>(s.playerLoadedDataPtr);
                s.playerData3DPtr = ReadPtrRaw(base, 0x08);
                if (s.playerData3DPtr) {
                    auto* nb = reinterpret_cast<void*>(s.playerData3DPtr);
                    ReadFloat3Raw(nb, 0xB0, s.plNiWorldT[0],
                                              s.plNiWorldT[1],
                                              s.plNiWorldT[2]);
                    s.plNiWorldRot00 = ReadFloatRaw(nb, 0x80);
                    s.plNiWorldRot01 = ReadFloatRaw(nb, 0x84);
                    s.plNiWorldRot02 = ReadFloatRaw(nb, 0x88);
                    s.plNiWorldRot10 = ReadFloatRaw(nb, 0x8C);
                    s.plNiWorldRot11 = ReadFloatRaw(nb, 0x90);
                    s.plNiWorldRot12 = ReadFloatRaw(nb, 0x94);
                    s.plNiWorldRot20 = ReadFloatRaw(nb, 0x98);
                    s.plNiWorldRot21 = ReadFloatRaw(nb, 0x9C);
                    s.plNiWorldRot22 = ReadFloatRaw(nb, 0xA0);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // PilotCameraShaker singleton snapshot.
        __try {
            void* shaker = ResolvePilotCameraShaker();
            if (shaker) {
                s.shakerInst = reinterpret_cast<std::uintptr_t>(shaker);
                s.shakeDispX = ReadFloatRaw(shaker, 0x50);
                s.shakeDispY = ReadFloatRaw(shaker, 0x54);
                s.shakeDispZ = ReadFloatRaw(shaker, 0x58);
                s.shakeDispW = ReadFloatRaw(shaker, 0x5C);
                s.shakeIntensity = ReadFloatRaw(shaker, 0x78);
                s.shakeBound     = ReadFloatRaw(shaker, 0x7C);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return s;
    }

    static void LogSnapshot(const Snapshot& s)
    {
        if (!s.havePlayer) {
            PLOG_INFO("[PULSAR] FlightProbe: <player null>"sv);
            NASLOG("FlightProbe: <player null>");
            return;
        }
        // Two log lines so each line stays under any reasonable cap.
        PLOG_INFO(
            "[PULSAR] FlightProbe inSpace={} P=({:.2f},{:.2f},{:.2f}) "
            "Pang=({:.3f},{:.3f},{:.3f}) S=0x{:08X} cell=0x{:08X} proc={} "
            "Spos=({:.2f},{:.2f},{:.2f}) Sang=({:.3f},{:.3f},{:.3f})"sv,
            s.inSpace ? 1 : 0,
            s.pX, s.pY, s.pZ, s.pAngX, s.pAngY, s.pAngZ,
            s.shipFormID, s.shipCellFormID, s.shipCellIsProc ? 1 : 0,
            s.sX, s.sY, s.sZ, s.sAngX, s.sAngY, s.sAngZ);

        PLOG_INFO(
            "[PULSAR] FlightProbe ldData=0x{:X} 3D=0x{:X} "
            "niWorldT=({:.2f},{:.2f},{:.2f}) "
            "niRow0=({:.3f},{:.3f},{:.3f}) niRow1=({:.3f},{:.3f},{:.3f}) "
            "niRow2=({:.3f},{:.3f},{:.3f})"sv,
            s.shipLoadedDataPtr, s.shipData3DPtr,
            s.niWorldTrans[0], s.niWorldTrans[1], s.niWorldTrans[2],
            s.niWorldRot00, s.niWorldRot01, s.niWorldRot02,
            s.niWorldRot10, s.niWorldRot11, s.niWorldRot12,
            s.niWorldRot20, s.niWorldRot21, s.niWorldRot22);

        PLOG_INFO(
            "[PULSAR] FlightProbe pl3D=0x{:X} "
            "plWorldT=({:.2f},{:.2f},{:.2f}) "
            "plRow0=({:.3f},{:.3f},{:.3f}) plRow1=({:.3f},{:.3f},{:.3f}) "
            "plRow2=({:.3f},{:.3f},{:.3f}) | shaker=0x{:X} "
            "shakeDisp=({:.3f},{:.3f},{:.3f},{:.3f}) shakeI={:.3f} shakeB={:.3f}"sv,
            s.playerData3DPtr,
            s.plNiWorldT[0], s.plNiWorldT[1], s.plNiWorldT[2],
            s.plNiWorldRot00, s.plNiWorldRot01, s.plNiWorldRot02,
            s.plNiWorldRot10, s.plNiWorldRot11, s.plNiWorldRot12,
            s.plNiWorldRot20, s.plNiWorldRot21, s.plNiWorldRot22,
            s.shakerInst,
            s.shakeDispX, s.shakeDispY, s.shakeDispZ, s.shakeDispW,
            s.shakeIntensity, s.shakeBound);

        PLOG_INFO(
            "[PULSAR] FlightProbe ldRaw=[{:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}]"sv,
            s.ldRaw[0], s.ldRaw[1], s.ldRaw[2], s.ldRaw[3],
            s.ldRaw[4], s.ldRaw[5], s.ldRaw[6], s.ldRaw[7]);

        NASLOG(
            "FlightProbe inSpace=%d P=(%.2f,%.2f,%.2f) Pang=(%.3f,%.3f,%.3f) "
            "S=0x%08X Spos=(%.2f,%.2f,%.2f) Sang=(%.3f,%.3f,%.3f) cell=0x%08X proc=%d "
            "ldData=0x%llX 3D=0x%llX niWorldT=(%.2f,%.2f,%.2f) "
            "niRow0=(%.3f,%.3f,%.3f) niRow1=(%.3f,%.3f,%.3f) niRow2=(%.3f,%.3f,%.3f) "
            "pl3D=0x%llX plWorldT=(%.2f,%.2f,%.2f) "
            "plRow0=(%.3f,%.3f,%.3f) plRow1=(%.3f,%.3f,%.3f) plRow2=(%.3f,%.3f,%.3f) "
            "shaker=0x%llX shakeDisp=(%.3f,%.3f,%.3f,%.3f) shakeI=%.3f shakeB=%.3f "
            "ldRaw=[%llX %llX %llX %llX %llX %llX %llX %llX]",
            s.inSpace ? 1 : 0,
            s.pX, s.pY, s.pZ, s.pAngX, s.pAngY, s.pAngZ,
            s.shipFormID, s.sX, s.sY, s.sZ, s.sAngX, s.sAngY, s.sAngZ,
            s.shipCellFormID, s.shipCellIsProc ? 1 : 0,
            (unsigned long long)s.shipLoadedDataPtr, (unsigned long long)s.shipData3DPtr,
            s.niWorldTrans[0], s.niWorldTrans[1], s.niWorldTrans[2],
            s.niWorldRot00, s.niWorldRot01, s.niWorldRot02,
            s.niWorldRot10, s.niWorldRot11, s.niWorldRot12,
            s.niWorldRot20, s.niWorldRot21, s.niWorldRot22,
            (unsigned long long)s.playerData3DPtr,
            s.plNiWorldT[0], s.plNiWorldT[1], s.plNiWorldT[2],
            s.plNiWorldRot00, s.plNiWorldRot01, s.plNiWorldRot02,
            s.plNiWorldRot10, s.plNiWorldRot11, s.plNiWorldRot12,
            s.plNiWorldRot20, s.plNiWorldRot21, s.plNiWorldRot22,
            (unsigned long long)s.shakerInst,
            s.shakeDispX, s.shakeDispY, s.shakeDispZ, s.shakeDispW,
            s.shakeIntensity, s.shakeBound,
            (unsigned long long)s.ldRaw[0], (unsigned long long)s.ldRaw[1],
            (unsigned long long)s.ldRaw[2], (unsigned long long)s.ldRaw[3],
            (unsigned long long)s.ldRaw[4], (unsigned long long)s.ldRaw[5],
            (unsigned long long)s.ldRaw[6], (unsigned long long)s.ldRaw[7]);
    }

}  // namespace

void Init()
{
    PLOG_INFO("[PULSAR] SpaceFlightProbe L.3 V2 V0 init"sv);
    NASLOG("SpaceFlightProbe: init");
    g_lastTickMs.store(0);
    g_firstSpaceTickMs.store(0);
    g_inSpaceLast.store(false);
}

void Tick()
{
    const std::uint64_t nowMs = ::GetTickCount64();
    const std::uint64_t lastMs = g_lastTickMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < kTickIntervalMs) return;
    g_lastTickMs.store(nowMs, std::memory_order_relaxed);

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    bool nowInSpace = false;
    __try {
        nowInSpace = player->IsInSpace(true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    const bool wasInSpace = g_inSpaceLast.exchange(nowInSpace, std::memory_order_relaxed);

    // Mark first-space-tick on transition into space.
    if (nowInSpace && !wasInSpace) {
        g_firstSpaceTickMs.store(nowMs, std::memory_order_relaxed);
        PLOG_INFO("[PULSAR] FlightProbe: entering space, starting probe window"sv);
        NASLOG("FlightProbe: entering space - probe window starts");
    }
    if (!nowInSpace) return;

    // Auto-stop probing after kProbeWindowMs to avoid log spam in long flights.
    const std::uint64_t startMs = g_firstSpaceTickMs.load(std::memory_order_relaxed);
    if (startMs && (nowMs - startMs) > kProbeWindowMs) return;

    auto snap = CaptureNow();
    LogSnapshot(snap);
}

}  // namespace Pulsar::SpaceFlightProbe
