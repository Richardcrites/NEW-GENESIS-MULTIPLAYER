// M.1 V1 - NPC Ship Discovery probe.
//
// Periodically (5s) while player is in space, enumerate parentCell.references
// and report:
// total ref count
// histogram of base form types
// per-ref: those that GetSpaceship(false) returns non-null on (= ships
//     or ship-attached actors) → log FormID/base/pos/distance
//
// Reuses the verified +0x80 BSTArray<NiPointer<TESObjectREFR>> offset from
// CellContentSync. Auto-disables after a 5min probe window to avoid log spam.

#include "PCH.h"
#include "SpaceSync/NPCShipProbe.h"

#include "NASLog.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

using namespace std::string_view_literals;

namespace Pulsar::NPCShipProbe {
namespace {

    constexpr std::uint64_t kTickIntervalMs = 5000;
    constexpr std::uint64_t kProbeWindowMs  = 300000;  // 5 min in space
    constexpr std::ptrdiff_t kCellRefsOffset = 0x80;
    constexpr std::uint32_t kMaxSaneRefCount = 65536;
    constexpr std::size_t   kLogPerShipCap   = 16;

    static std::atomic<std::uint64_t> g_lastTickMs{ 0 };
    static std::atomic<std::uint64_t> g_firstSpaceTickMs{ 0 };
    static std::atomic<bool>          g_inSpaceLast{ false };
    static std::atomic<std::uint64_t> g_probeIndex{ 0 };

    static int SafeReadCellRefsHeader(const void*    cell,
                                       std::uint32_t* outSize,
                                       std::uint32_t* outCapacity,
                                       void**         outData) noexcept
    {
        __try {
            const auto base = reinterpret_cast<const std::uint8_t*>(cell)
                            + kCellRefsOffset;
            *outSize     = *reinterpret_cast<const std::uint32_t*>(base + 0x00);
            *outCapacity = *reinterpret_cast<const std::uint32_t*>(base + 0x04);
            *outData     = *reinterpret_cast<void* const*>(base + 0x08);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *outSize     = 0;
            *outCapacity = 0;
            *outData     = nullptr;
            return 0;
        }
    }

    static int SafeReadNiPointer(void* arrayBase, std::uint32_t i,
                                  RE::TESObjectREFR** outRef) noexcept
    {
        __try {
            auto* slot = reinterpret_cast<RE::TESObjectREFR**>(
                reinterpret_cast<std::uint8_t*>(arrayBase) + (i * 8));
            *outRef = *slot;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *outRef = nullptr;
            return 0;
        }
    }

    struct RefSnap {
        std::uint32_t refFormID;
        std::uint32_t baseFormID;
        std::uint8_t  refFormType;
        std::uint8_t  baseFormType;
        float         posX, posY, posZ;
        bool          ok;
    };

    static int SafeProbeRef(RE::TESObjectREFR* ref, RefSnap* out) noexcept
    {
        __try {
            out->refFormID    = ref->formID;
            out->refFormType  = static_cast<std::uint8_t>(ref->formType.get());
            const auto& pos   = ref->data.location;
            out->posX = pos.x;
            out->posY = pos.y;
            out->posZ = pos.z;

            auto* base = ref->data.objectReference.get();
            if (base) {
                out->baseFormID   = base->formID;
                out->baseFormType = static_cast<std::uint8_t>(base->formType.get());
            } else {
                out->baseFormID   = 0;
                out->baseFormType = 0;
            }
            out->ok = true;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out->ok = false;
            return 0;
        }
    }

    // Wrap the GetSpaceship virtual call. Returns the ship REFR pointer or
    // nullptr. SEH-guarded because the ref may be in a transient state.
    static RE::TESObjectREFR* SafeGetSpaceship(RE::TESObjectREFR* ref) noexcept
    {
        if (!ref) return nullptr;
        __try {
            return ref->GetSpaceship(false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static const char* FormTypeName(std::uint8_t ft) noexcept
    {
        switch (static_cast<RE::FormType>(ft)) {
            case RE::FormType::kREFR:  return "REFR";
            case RE::FormType::kACHR:  return "ACHR";
            case RE::FormType::kACTI:  return "ACTI";
            case RE::FormType::kCONT:  return "CONT";
            case RE::FormType::kDOOR:  return "DOOR";
            case RE::FormType::kFURN:  return "FURN";
            case RE::FormType::kSTAT:  return "STAT";
            case RE::FormType::kMSTT:  return "MSTT";
            case RE::FormType::kMOVT:  return "MOVT";
            case RE::FormType::kNPC_:  return "NPC_";
            case RE::FormType::kGBFM:  return "GBFM";
            case RE::FormType::kGBFT:  return "GBFT";
            case RE::FormType::kPKIN:  return "PKIN";
            case RE::FormType::kSCOL:  return "SCOL";
            default:                   return "OTHER";
        }
    }

}  // namespace

void Init()
{
    PLOG_INFO("[PULSAR] NPCShipProbe M.1 V1 init"sv);
    NASLOG("NPCShipProbe: init");
    g_lastTickMs.store(0);
    g_firstSpaceTickMs.store(0);
    g_inSpaceLast.store(false);
    g_probeIndex.store(0);
}

void Tick()
{
    const std::uint64_t nowMs  = ::GetTickCount64();
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

    if (nowInSpace && !wasInSpace) {
        g_firstSpaceTickMs.store(nowMs, std::memory_order_relaxed);
        g_probeIndex.store(0);
        PLOG_INFO("[PULSAR] NPCShipProbe: entering space, starting discovery window"sv);
        NASLOG("NPCShipProbe: entering space");
    }
    if (!nowInSpace) return;

    const std::uint64_t startMs = g_firstSpaceTickMs.load(std::memory_order_relaxed);
    if (startMs && (nowMs - startMs) > kProbeWindowMs) return;

    // V1.1 - playerCell is the ship-INTERIOR-cell when piloting. NPC ships
    // live in the SPACE-cell, which is the parentCell of the player's ship
    // REFR (not the player itself).
    auto* playerCellInterior = player->parentCell;

    // Player ship handle - also used as the gateway to the space cell.
    auto* playerShip = SafeGetSpaceship(player);
    RE::TESObjectCELL* spaceCell = nullptr;
    __try {
        if (playerShip) spaceCell = playerShip->parentCell;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    auto* cell = spaceCell ? spaceCell : playerCellInterior;
    if (!cell) return;

    const std::uint32_t cellFormID = cell->formID;
    const std::uint32_t interiorFormID = playerCellInterior ? playerCellInterior->formID : 0u;

    // Player ship handle - exclude from "NPC ship" tally.
    std::uint32_t playerShipFormID = 0;
    if (playerShip) {
        __try { playerShipFormID = playerShip->formID; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Player position for distance calc.
    float plX = 0, plY = 0, plZ = 0;
    __try {
        const auto& pl = player->data.location;
        plX = pl.x; plY = pl.y; plZ = pl.z;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    std::uint32_t size = 0;
    std::uint32_t capacity = 0;
    void*         data = nullptr;
    if (!SafeReadCellRefsHeader(cell, &size, &capacity, &data)) {
        PLOG_WARN("[PULSAR] NPCShipProbe: refs header read SEH-faulted cell=0x{:08X}"sv, cellFormID);
        return;
    }
    if (!data || size == 0 || size > kMaxSaneRefCount) {
        PLOG_INFO("[PULSAR] NPCShipProbe: cell empty / insane (size={} cap={} data={})"sv,
                  size, capacity, data ? "ok" : "NULL");
        return;
    }

    const std::uint64_t probeIdx = g_probeIndex.fetch_add(1) + 1;

    PLOG_INFO(
        "[PULSAR] NPCShipProbe ENUM #{} spaceCell=0x{:08X} interior=0x{:08X} size={} cap={} playerShip=0x{:08X}"sv,
        probeIdx, cellFormID, interiorFormID, size, capacity, playerShipFormID);

    std::uint32_t perTypeCount[256]   = { 0 };
    std::uint32_t perBaseTypeCount[256] = { 0 };
    std::size_t   total            = 0;
    std::size_t   probeFailed      = 0;
    std::size_t   nullRef          = 0;
    std::size_t   shipsTotal       = 0;
    std::size_t   shipsNPC         = 0;
    std::size_t   logged           = 0;

    for (std::uint32_t i = 0; i < size; ++i) {
        ++total;
        RE::TESObjectREFR* ref = nullptr;
        if (!SafeReadNiPointer(data, i, &ref)) {
            ++probeFailed;
            continue;
        }
        if (!ref) {
            ++nullRef;
            continue;
        }
        const std::uintptr_t refAddr = reinterpret_cast<std::uintptr_t>(ref);
        if (refAddr < 0x10000ULL || refAddr >= 0x7FFFFFFFFFFFULL) {
            ++nullRef;
            continue;
        }

        RefSnap p{};
        if (!SafeProbeRef(ref, &p) || !p.ok) {
            ++probeFailed;
            continue;
        }

        ++perTypeCount[p.refFormType];
        ++perBaseTypeCount[p.baseFormType];

        if (p.refFormID == playerShipFormID) continue;

        // Probe GetSpaceship - for diagnosis we want to see what it returns
        // on space-cell refs.
        auto* shipRef = SafeGetSpaceship(ref);
        std::uint32_t shipFID = 0;
        if (shipRef) {
            __try { shipFID = shipRef->formID; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            ++shipsTotal;
        }

        // Skip refs that GetSpaceship resolves to the player-ship - those are
        // crew/items inside our own ship, not interesting for NPC-ship sync.
        if (shipFID == playerShipFormID && playerShipFormID != 0) continue;

        // Anything left is a candidate NPC-ship-related ref.
        ++shipsNPC;

        if (logged < kLogPerShipCap) {
            const float dx = p.posX - plX;
            const float dy = p.posY - plY;
            const float dz = p.posZ - plZ;
            const float d2 = dx * dx + dy * dy + dz * dz;
            PLOG_INFO(
                "[PULSAR]   CAND #{} ref=0x{:08X} (rt=0x{:02X}) base=0x{:08X} bt=0x{:02X} {} "
                "pos=({:.0f},{:.0f},{:.0f}) d2={:.0f} ship=0x{:08X}"sv,
                logged + 1, p.refFormID, p.refFormType,
                p.baseFormID, p.baseFormType, FormTypeName(p.baseFormType),
                p.posX, p.posY, p.posZ, d2, shipFID);
            ++logged;
        }
    }

    PLOG_INFO(
        "[PULSAR] NPCShipProbe DONE #{} total={} hasShipRef={} candNonPlayer={} nullRef={} probeFail={}"sv,
        probeIdx, total, shipsTotal, shipsNPC, nullRef, probeFailed);

    // Top-3 base-form-type histogram so we know what dominates the cell.
    int topIdx[3] = { -1, -1, -1 };
    for (int i = 0; i < 256; ++i) {
        if (perBaseTypeCount[i] == 0) continue;
        for (int t = 0; t < 3; ++t) {
            if (topIdx[t] < 0 || perBaseTypeCount[i] > perBaseTypeCount[topIdx[t]]) {
                for (int s = 2; s > t; --s) topIdx[s] = topIdx[s - 1];
                topIdx[t] = i;
                break;
            }
        }
    }
    for (int t = 0; t < 3; ++t) {
        if (topIdx[t] < 0) break;
        PLOG_INFO("[PULSAR]   topBase{} type=0x{:02X} {} count={}"sv,
                  t + 1, topIdx[t],
                  FormTypeName(static_cast<std::uint8_t>(topIdx[t])),
                  perBaseTypeCount[topIdx[t]]);
    }

    NASLOG("NPCShipProbe #%llu cell=0x%08X total=%zu ships=%zu npcShips=%zu",
           probeIdx, cellFormID, total, shipsTotal, shipsNPC);
}

}  // namespace Pulsar::NPCShipProbe
