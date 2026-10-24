#include "PCH.h"
#include "WorldSync/ActiSync.h"
#include "WorldSync/LootSync.h"  // CallOrigActivateAsReplay

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/NPCSync.h"           // GetActiveCellsSnapshot()
#include "NPCSync/PluginLookup.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <windows.h>

#include <RE/A/Actor.h>
#include <RE/F/FormTypes.h>
#include <RE/G/GameVM.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <REL/Relocation.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::WorldSync::Acti {
namespace {

    static uint32_t RawFormID(const void* form) noexcept
    {
        if (!form) return 0;
        return *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(form) + 0x28);
    }

    static uint8_t RawFormType(const void* form) noexcept
    {
        if (!form) return 0;
        return *reinterpret_cast<const uint8_t*>(
            static_cast<const uint8_t*>(form) + 0x2E);
    }

    // Mirror of LootSync::ActivateArgs (passed by void* across modules).
    struct ActivateArgs {
        RE::TESObjectREFR* activator;
        uint64_t           unused1;
        uint64_t           unused2;
        uint32_t           countOrFlag;
        uint8_t            flag1c;
        uint8_t            flag1d;
        uint8_t            flag1e;
        uint8_t            flag1f;
    };
    static_assert(sizeof(ActivateArgs) == 0x20, "ActivateArgs must be 32 bytes");

    struct PendingActi {
        uint32_t resolvedBase;
        float    px, py, pz;
        // F.7 V2: ghost-cell info
        uint32_t consumedResolved;  // resolved local FormID, 0 = none
        int32_t  consumedCount;
    };
    static std::mutex                  s_pendingMutex;
    static std::vector<PendingActi>    s_pending;

    // F.7 V2: ObjectReference.AddItem Papyrus native - RVA 0x2067EA0.
    // Identified via Ghidra (string xref "AddItem" → registration block at
    // FUN_1420C2250 → param_1[10] = FUN_14206B040). Template:
    // NativeFunction3<TESObjectREFR, bool, FormOrInventoryObj, uint, bool>.
    // **NOT** NativeLatentFunction → safe to direct-call from C++.
    // Used on receiver to ghost-add the consumed cell to the local player
    // before activator replay so the activator script finds it in inventory
    // and consumes it naturally during the replay.
    constexpr std::uintptr_t kRVA_AddItem = 0x2067EA0;

    using AddItem_t = bool (__fastcall*)(void*, std::uint32_t,
                                          RE::TESObjectREFR*,
                                          void*, std::uint32_t, bool);
    static AddItem_t g_addItem = nullptr;

    static int SafeCallAddItem(RE::TESObjectREFR* recipient,
                                RE::TESForm* form,
                                std::uint32_t count,
                                bool silent) noexcept
    {
        if (!recipient || !form || !g_addItem) return 0;
        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return 0;
        auto* vm = gameVM->GetVM();
        if (!vm) return 0;
        __try {
            return g_addItem(vm, 0, recipient, form, count, silent) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Cell scan: find a kACTI REFR with matching base near (px,py,pz).
    static RE::TESObjectREFR* SafeFindActiByBaseInCell(void* cell,
                                                        uint32_t resolvedBase,
                                                        float px, float py, float pz,
                                                        float radius) noexcept
    {
        __try {
            if (!cell) return nullptr;
            auto* cb = reinterpret_cast<uint8_t*>(cell);
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return nullptr;

            RE::TESObjectREFR* best = nullptr;
            float bestDist2 = radius * radius;
            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(ref) + 0x98);
                if (!base) continue;
                if (RawFormType(base) != static_cast<uint8_t>(RE::FormType::kACTI)) continue;
                if (RawFormID(base) != resolvedBase) continue;
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);
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

    static void SendActiPacket(uint32_t refID, uint32_t baseID,
                                float px, float py, float pz,
                                uint32_t consumedFormID, int32_t consumedCount)
    {
        using namespace Pulsar::Network;

        ActiStateData data{};
        data.refFormID    = refID;
        data.baseFormID   = baseID;
        data.baseLocalID  = NPCSync::PluginLookup::FormIDToLocalID(baseID);
        data.posX         = px;
        data.posY         = py;
        data.posZ         = pz;
        data.consumedFormID = consumedFormID;
        data.consumedLocalID = consumedFormID
            ? NPCSync::PluginLookup::FormIDToLocalID(consumedFormID) : 0;
        data.consumedCount = consumedCount;
        NPCSync::PluginLookup::FormIDToPluginName(
            baseID, data.basePlugin, sizeof(data.basePlugin));
        if (consumedFormID) {
            NPCSync::PluginLookup::FormIDToPluginName(
                consumedFormID, data.consumedPlugin, sizeof(data.consumedPlugin));
        }

        uint8_t buf[1 + sizeof(ActiStateData)]{};
        buf[0] = PKT_ACTI_STATE;
        std::memcpy(buf + 1, &data, sizeof(data));

        auto& net = Pulsar::Network::GetSingleton();
        net.Send(buf, sizeof(buf), true);  // reliable

        if (consumedFormID) {
            PLOG_INFO(
                "[PULSAR] ActiSync TX: ref=0x{:08X} base=0x{:08X} plugin='{}' @({:.0f},{:.0f},{:.0f}) consumed=0x{:08X} ({}x) plugin='{}'"sv,
                refID, baseID,
                data.basePlugin[0] ? data.basePlugin : "(none)",
                px, py, pz, consumedFormID, consumedCount,
                data.consumedPlugin[0] ? data.consumedPlugin : "(none)");
        } else {
            PLOG_INFO(
                "[PULSAR] ActiSync TX: ref=0x{:08X} base=0x{:08X} plugin='{}' @({:.0f},{:.0f},{:.0f}) (no consumed item)"sv,
                refID, baseID,
                data.basePlugin[0] ? data.basePlugin : "(none)",
                px, py, pz);
        }
    }

}  // anonymous namespace

void Init()
{
    g_addItem = reinterpret_cast<AddItem_t>(
        REL::Offset{ kRVA_AddItem }.address());
    PLOG_INFO(
        "[PULSAR] ActiSync: AddItem resolved at 0x{:X}"sv,
        reinterpret_cast<std::uintptr_t>(g_addItem));
}

void OnHostActivate(uint32_t refID, uint32_t baseID,
                     float px, float py, float pz, bool result,
                     uint32_t consumedFormID, int32_t consumedCount)
{
    auto& net = Pulsar::Network::GetSingleton();
    bool connected = net.IsConnected();
    // Activators routinely return false from engine Activate when their
    // Papyrus script handles inline (verified for Outpost power-conduit
    // base 0x0000EDC1). We ignore result and always broadcast.
    PLOG_INFO(
        "[PULSAR] F.7 acti post-orig: ref=0x{:08X} base=0x{:08X} result={} connected={} consumed=0x{:08X}({}x) -> {}"sv,
        refID, baseID,
        result ? "true" : "false",
        connected ? "yes" : "no",
        consumedFormID, consumedCount,
        connected ? "BROADCAST" : "skip");
    if (connected) {
        SendActiPacket(refID, baseID, px, py, pz,
                       consumedFormID, consumedCount);
    }
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    if (data[0] != PKT_ACTI_STATE) return;
    if (length < 1 + sizeof(ActiStateData)) return;

    ActiStateData d{};
    std::memcpy(&d, data + 1, sizeof(d));

    uint32_t resolved = 0;
    if (d.basePlugin[0] != '\0') {
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(
            d.basePlugin, d.baseLocalID);
    }
    if (!resolved) resolved = d.baseFormID;

    uint32_t consumedResolved = 0;
    if (d.consumedFormID && d.consumedPlugin[0] != '\0') {
        consumedResolved = NPCSync::PluginLookup::ResolveCrossPlugin(
            d.consumedPlugin, d.consumedLocalID);
    }
    if (d.consumedFormID && !consumedResolved) consumedResolved = d.consumedFormID;

    {
        std::lock_guard g(s_pendingMutex);
        s_pending.push_back({
            resolved, d.posX, d.posY, d.posZ,
            consumedResolved, d.consumedCount
        });
    }

    PLOG_INFO(
        "[PULSAR] ActiSync RX: base=0x{:08X} plugin='{}' resolved=0x{:08X} @({:.0f},{:.0f},{:.0f}) consumed=0x{:08X}({}x)"sv,
        d.baseFormID,
        d.basePlugin[0] ? d.basePlugin : "(none)",
        resolved, d.posX, d.posY, d.posZ,
        consumedResolved, d.consumedCount);
}

void DrainPending()
{
    std::vector<PendingActi> work;
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
        RE::TESObjectREFR* acti = nullptr;
        if (player->parentCell) {
            acti = SafeFindActiByBaseInCell(player->parentCell,
                                              d.resolvedBase,
                                              d.px, d.py, d.pz, 1200.0f);
        }
        if (!acti) {
            auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
            for (auto* cell : cells) {
                if (!cell) continue;
                if (cell == player->parentCell) continue;
                acti = SafeFindActiByBaseInCell(cell, d.resolvedBase,
                                                  d.px, d.py, d.pz, 1200.0f);
                if (acti) break;
            }
        }

        if (!acti) {
            PLOG_INFO(
                "[PULSAR] ActiSync: NO MATCH base=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
                d.resolvedBase, d.px, d.py, d.pz);
            continue;
        }

        // Prefer remote clone as activator (same as DoorSync); fall back to
        // local player when clone hasn't spawned yet. Activator must be
        // non-null - Activate is gated on *param_2 != 0.
        auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        RE::TESObjectREFR* activator = clone
            ? reinterpret_cast<RE::TESObjectREFR*>(clone)
            : reinterpret_cast<RE::TESObjectREFR*>(player);
        if (!activator) {
            PLOG_WARN(
                "[PULSAR] ActiSync: no activator (clone+player both null), skipping ref=0x{:08X}"sv,
                RawFormID(acti));
            continue;
        }

        // F.7 V2: ghost-add consumed item to LOCAL player BEFORE replay so
        // the activator's Papyrus script finds it in inventory and consumes
        // it naturally. Adding to local player (not clone) because the
        // script does Game.GetPlayer().GetItemCount(theCell).
        int addOk = 0;
        if (d.consumedResolved && d.consumedCount > 0) {
            auto* form = RE::TESForm::LookupByID(d.consumedResolved);
            if (form) {
                addOk = SafeCallAddItem(reinterpret_cast<RE::TESObjectREFR*>(player),
                                        form,
                                        static_cast<uint32_t>(d.consumedCount),
                                        true);  // silent
                PLOG_INFO(
                    "[PULSAR] ActiSync: ghost-add 0x{:08X}({}x) to player ok={}"sv,
                    d.consumedResolved, d.consumedCount, addOk);
            } else {
                PLOG_WARN(
                    "[PULSAR] ActiSync: ghost-add failed - TESForm::LookupByID(0x{:08X}) null"sv,
                    d.consumedResolved);
            }
        }

        ActivateArgs args{};
        args.activator   = activator;
        args.countOrFlag = 1;
        args.flag1c      = 0;
        args.flag1d      = 1;
        Pulsar::WorldSync::Loot::CallOrigActivateAsReplay(acti, &args);

        PLOG_INFO(
            "[PULSAR] ActiSync: REPLAY ok ref=0x{:08X} (base=0x{:08X}) via={} ghost-add={}"sv,
            RawFormID(acti), d.resolvedBase,
            clone ? "clone" : "player-fallback",
            d.consumedResolved ? (addOk ? "y" : "FAIL") : "no");
    }
}

}  // namespace Pulsar::WorldSync::Acti
