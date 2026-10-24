#include "PCH.h"
#include "WorldSync/DoorSync.h"
#include "WorldSync/LootSync.h"  // CallOrigActivateAsReplay

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/NPCSync.h"           // GetActiveCellsSnapshot()
#include "NPCSync/PluginLookup.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <windows.h>

#include <RE/A/Actor.h>
#include <RE/F/FormTypes.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::WorldSync::Door {
namespace {

    // ---- Local raw-offset helpers ----------------------------------------
    // Same offsets as LootSync (formID@+0x28, formType@+0x2E, REFR-base@+0x98,
    // REFR-pos@+0x8C). Duplicated locally to keep the module standalone.

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

    // ActivateArgs mirror - 32-byte struct also defined inside LootSync.
    // Defined locally because we pass it by void* across the module boundary
    // (LootSync::CallOrigActivateAsReplay takes void* args).
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

    // ---- Pending queue ---------------------------------------------------

    struct PendingDoor {
        uint32_t resolvedBase;
        float    px, py, pz;
        uint32_t senderRefID;   // host runtime REFR id - try LookupByID first
                                // (static/persistent doors share the FormID
                                // across machines, so this resolves directly
                                // even when the door's cell isn't player-local).
    };

    static std::mutex                  s_pendingMutex;
    static std::vector<PendingDoor>    s_pending;

    // V2 cross-source jitter dedup: doorRefID -> last LOCAL (non-replay)
    // activation tick. Both machines run the same deterministic NPCs, so the
    // client's own NPC opens a door AND the host mirror replays it; the toggle
    // replay then shuts it again. DrainPending skips a replay whose door was
    // locally activated inside this window.
    static std::mutex                              s_localActMutex;
    static std::unordered_map<uint32_t, uint64_t>  s_localActMs;
    static constexpr uint64_t                      kLocalActWindowMs = 2500;

    // ---- Cell-scan helpers ----------------------------------------------

    static RE::TESObjectREFR* SafeFindDoorByBaseInCell(void* cell,
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

    // Diagnostic scan - counts all DOOR REFRs in a cell, plus how many match
    // the resolved base FormID and the closest base-match distance. Helps
    // distinguish "wrong resolved base" from "position drift > radius" from
    // "door in unloaded cell" when a no-match log fires.
    struct DoorScanStats {
        uint32_t totalDoors;
        uint32_t baseMatches;
        float    closestBaseMatchDist;
        uint32_t closestBaseMatchID;
    };
    static DoorScanStats SafeScanCellForDoorStats(void* cell,
                                                    uint32_t resolvedBase,
                                                    float px, float py, float pz) noexcept
    {
        DoorScanStats s{};
        s.closestBaseMatchDist = 1.0e30f;
        __try {
            if (!cell) return s;
            auto* cb = reinterpret_cast<uint8_t*>(cell);
            uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x80);
            auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
            if (!data || size == 0 || size > 10000) return s;
            for (uint32_t i = 0; i < size; ++i) {
                auto* ref = data[i];
                if (!ref) continue;
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(ref) + 0x98);
                if (!base) continue;
                if (RawFormType(base) != static_cast<uint8_t>(RE::FormType::kDOOR))
                    continue;
                s.totalDoors++;
                if (RawFormID(base) != resolvedBase) continue;
                s.baseMatches++;
                auto* loc = reinterpret_cast<float*>(
                    reinterpret_cast<uint8_t*>(ref) + 0x8C);
                float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
                float d = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (d < s.closestBaseMatchDist) {
                    s.closestBaseMatchDist = d;
                    s.closestBaseMatchID = RawFormID(ref);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return s;
    }

    static float SafeDistanceToRef(RE::TESObjectREFR* ref,
                                    float px, float py, float pz) noexcept
    {
        float d = 0.0f;
        __try {
            if (!ref) return 0.0f;
            auto* loc = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(ref) + 0x8C);
            float dx = loc[0] - px, dy = loc[1] - py, dz = loc[2] - pz;
            d = std::sqrt(dx*dx + dy*dy + dz*dz);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            d = 0.0f;
        }
        return d;
    }

    // ---- Wire encoding ---------------------------------------------------

    static void SendDoorPacket(uint32_t refID, uint32_t baseID,
                                float px, float py, float pz)
    {
        using namespace Pulsar::Network;

        DoorStateData data{};
        data.refFormID    = refID;
        data.baseFormID   = baseID;
        data.baseLocalID  = NPCSync::PluginLookup::FormIDToLocalID(baseID);
        data.posX         = px;
        data.posY         = py;
        data.posZ         = pz;
        data.state        = 0;  // V2-reserved
        NPCSync::PluginLookup::FormIDToPluginName(
            baseID, data.basePlugin, sizeof(data.basePlugin));

        uint8_t buf[1 + sizeof(DoorStateData)]{};
        buf[0] = PKT_DOOR_STATE;
        std::memcpy(buf + 1, &data, sizeof(data));

        auto& net = Pulsar::Network::GetSingleton();
        net.Send(buf, sizeof(buf), true);  // reliable

        PLOG_INFO(
            "[PULSAR] DoorSync TX: ref=0x{:08X} base=0x{:08X} plugin='{}' @({:.0f},{:.0f},{:.0f})"sv,
            refID, baseID,
            data.basePlugin[0] ? data.basePlugin : "(none)",
            px, py, pz);
    }

}  // anonymous namespace

// =====================================================================
// Public API
// =====================================================================

void Init()
{
    // No-op for V1. The Activate hook lives in LootSync (it pre-existed and
    // is shared); DoorSync subscribes to it via OnHostActivate(). Kept as
    // an explicit hook for ordering symmetry with LootSync::Init() and for
    // future state-init / late-join logic.
}

void NoteLocalDoorActivation(std::uint32_t refID)
{
    if (!refID) return;
    std::lock_guard g(s_localActMutex);
    s_localActMs[refID] = ::GetTickCount64();
}

bool WasLocallyActivatedRecently(std::uint32_t refID) noexcept
{
    if (!refID) return false;
    std::lock_guard g(s_localActMutex);
    auto it = s_localActMs.find(refID);
    if (it == s_localActMs.end()) return false;
    return (::GetTickCount64() - it->second) <= kLocalActWindowMs;
}

void OnHostActivate(uint32_t refID, uint32_t baseID,
                     float px, float py, float pz, bool result)
{
    auto& net = Pulsar::Network::GetSingleton();
    bool connected = net.IsConnected();
    bool willSend  = result && connected;

    PLOG_INFO(
        "[PULSAR] F.3 door post-orig: ref=0x{:08X} base=0x{:08X} result={} connected={} -> {}"sv,
        refID, baseID,
        result ? "true" : "false",
        connected ? "yes" : "no",
        willSend ? "BROADCAST" : "skip");

    if (willSend) {
        SendDoorPacket(refID, baseID, px, py, pz);
    }
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    if (data[0] != PKT_DOOR_STATE) return;
    if (length < 1 + sizeof(DoorStateData)) return;

    DoorStateData d{};
    std::memcpy(&d, data + 1, sizeof(d));

    uint32_t resolved = 0;
    if (d.basePlugin[0] != '\0') {
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(
            d.basePlugin, d.baseLocalID);
    }
    if (!resolved) resolved = d.baseFormID;

    {
        std::lock_guard g(s_pendingMutex);
        s_pending.push_back({ resolved, d.posX, d.posY, d.posZ, d.refFormID });
    }

    PLOG_INFO(
        "[PULSAR] DoorSync RX: base=0x{:08X} plugin='{}' localID=0x{:X} resolved=0x{:08X} @({:.0f},{:.0f},{:.0f})"sv,
        d.baseFormID,
        d.basePlugin[0] ? d.basePlugin : "(none)",
        d.baseLocalID, resolved, d.posX, d.posY, d.posZ);
}

void DrainPending()
{
    std::vector<PendingDoor> work;
    {
        std::lock_guard g(s_pendingMutex);
        if (s_pending.empty()) return;
        work.swap(s_pending);
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        // Try again next tick when the singleton exists.
        std::lock_guard g(s_pendingMutex);
        s_pending.insert(s_pending.begin(), work.begin(), work.end());
        return;
    }

    // F.12 jitter fix: a LINKED door pair (the two halves of an airlock /
    // double door) is activated as a group by the engine, so the host
    // broadcasts BOTH halves in the same batch. Activate is a TOGGLE, so
    // replaying each half toggles the whole group - the second replay flips
    // the pair shut again, which looks like open/close jitter on the client
    // (confirmed 2026-06-30: doors 0x1ED32A + 0x1ED49C, ~9u apart). Within
    // this drain batch, skip a door whose position is within the pair radius
    // of one we already replayed: activating the first half already toggled
    // this one through the engine link. Batch-local (cleared per tick) so it
    // never suppresses a genuinely separate door opened in a later event.
    constexpr float kLinkedDoorRadius2 = 64.0f * 64.0f;
    std::vector<std::array<float, 3>> replayedPos;

    for (auto& d : work) {
        // F.12: linked-pair dedup (see note above the loop).
        bool linkedDup = false;
        for (auto& rp : replayedPos) {
            float dx = d.px - rp[0], dy = d.py - rp[1], dz = d.pz - rp[2];
            if (dx * dx + dy * dy + dz * dz <= kLinkedDoorRadius2) {
                linkedDup = true;
                break;
            }
        }
        if (linkedDup) {
            PLOG_INFO(
                "[PULSAR] DoorSync: skip linked-pair partner @({:.0f},{:.0f},{:.0f}) - already toggled via engine link"sv,
                d.px, d.py, d.pz);
            continue;
        }

        // V2 anti-jitter: if the client's own (deterministic) NPC already opened
        // this exact door locally, the engine has it animating open; replaying
        // the mirror Activate would TOGGLE it shut again ("opened then sofort
        // zu"). Skip the redundant replay.
        if (WasLocallyActivatedRecently(d.senderRefID)) {
            PLOG_INFO(
                "[PULSAR] DoorSync: skip replay ref=0x{:08X} - local NPC opened it (anti-jitter)"sv,
                d.senderRefID);
            continue;
        }

        // Door bases are statically authored - cross-machine FormID match
        // works reliably. Larger radius than loot since doors don't move
        // and minor coordinate noise across machines is harmless.
        RE::TESObjectREFR* door = nullptr;

        // 2026-06-11: try the sender's runtime REFR id directly first. Static /
        // persistent doors (FF-prefix excluded) carry the SAME FormID on both
        // machines, so LookupByID resolves the exact door regardless of which
        // cell the client player is standing in - fixes "NO MATCH" when the
        // door's cell isn't player-local but is loaded (log 23:17: client had
        // ref=0x0110EDEE but playerCell doors=0 → base+pos scan missed it).
        if (d.senderRefID && (d.senderRefID & 0xFF000000u) != 0xFF000000u) {
            auto* f = RE::TESForm::LookupByID(d.senderRefID);
            if (f && RawFormType(f) == 0x4A) {  // 0x4A = kREFR (object reference)
                auto* asRefr = reinterpret_cast<RE::TESObjectREFR*>(f);
                auto* base = *reinterpret_cast<uint8_t**>(
                    reinterpret_cast<uint8_t*>(asRefr) + 0x98);
                if (base && RawFormType(base) == static_cast<uint8_t>(RE::FormType::kDOOR)) {
                    door = asRefr;
                    PLOG_INFO("[PULSAR] DoorSync: direct FormID match ref=0x{:08X}"sv,
                              d.senderRefID);
                }
            }
        }

        if (!door && player->parentCell) {
            door = SafeFindDoorByBaseInCell(player->parentCell,
                                             d.resolvedBase,
                                             d.px, d.py, d.pz, 1200.0f);
        }
        if (!door) {
            auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
            for (auto* cell : cells) {
                if (!cell) continue;
                if (cell == player->parentCell) continue;
                door = SafeFindDoorByBaseInCell(cell, d.resolvedBase,
                                                 d.px, d.py, d.pz, 1200.0f);
                if (door) break;
            }
        }

        if (!door) {
            DoorScanStats playerCellStats{};
            uint32_t      activeCellsScanned = 0;
            DoorScanStats bestActiveStats{};
            bestActiveStats.closestBaseMatchDist = 1.0e30f;
            if (player->parentCell) {
                playerCellStats = SafeScanCellForDoorStats(
                    player->parentCell, d.resolvedBase, d.px, d.py, d.pz);
            }
            auto cells = Pulsar::NPCSync::GetActiveCellsSnapshot();
            for (auto* cell : cells) {
                if (!cell) continue;
                if (cell == player->parentCell) continue;
                activeCellsScanned++;
                DoorScanStats s = SafeScanCellForDoorStats(
                    cell, d.resolvedBase, d.px, d.py, d.pz);
                bestActiveStats.totalDoors  += s.totalDoors;
                bestActiveStats.baseMatches += s.baseMatches;
                if (s.closestBaseMatchDist < bestActiveStats.closestBaseMatchDist) {
                    bestActiveStats.closestBaseMatchDist = s.closestBaseMatchDist;
                    bestActiveStats.closestBaseMatchID   = s.closestBaseMatchID;
                }
            }
            PLOG_INFO(
                "[PULSAR] DoorSync: NO MATCH base=0x{:08X} @({:.0f},{:.0f},{:.0f}) | playerCell: doors={} baseHits={} closest={:.0f}u (ref=0x{:08X}) | otherCells({}): doors={} baseHits={} closest={:.0f}u (ref=0x{:08X})"sv,
                d.resolvedBase, d.px, d.py, d.pz,
                playerCellStats.totalDoors,
                playerCellStats.baseMatches,
                playerCellStats.closestBaseMatchDist,
                playerCellStats.closestBaseMatchID,
                activeCellsScanned,
                bestActiveStats.totalDoors,
                bestActiveStats.baseMatches,
                bestActiveStats.closestBaseMatchDist,
                bestActiveStats.closestBaseMatchID);
            continue;
        }

        uint32_t matchedRefID = RawFormID(door);

        // Activator must be non-null. Prefer the remote clone - same pattern
        // as F.1.b/c/e.
        //
        // 2026-04-29 - REMOVED player-fallback. The original comment claimed
        // "Door open doesn't transfer ownership / move the player" - but
        // that's WRONG for cell-load doors (POI entrance, ship-cockpit, etc).
        // Activating a load-door teleports the activator into the linked
        // cell. With player-fallback active, host's "outside POI" door
        // matched a peer's interior-door activate (same base, different cell
        // instance) and teleported the HOST player back inside the POI when
        // the peer's clone hadn't respawned yet.
        //
        // Skipping the replay when clone is unavailable is the safe choice:
        // a single missed door-animation is harmless; teleporting the local
        // player is catastrophic.
        auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        if (!clone) {
            PLOG_INFO(
                "[PULSAR] DoorSync: SKIP replay - clone not available (would teleport host) ref=0x{:08X}"sv,
                matchedRefID);
            continue;
        }
        RE::TESObjectREFR* activator = reinterpret_cast<RE::TESObjectREFR*>(clone);

        float matchDist = SafeDistanceToRef(door, d.px, d.py, d.pz);

        ActivateArgs args{};
        args.activator   = activator;
        args.countOrFlag = 1;
        args.flag1c      = 0;
        args.flag1d      = 1;
        Pulsar::WorldSync::Loot::CallOrigActivateAsReplay(door, &args);

        PLOG_INFO(
            "[PULSAR] DoorSync: REPLAY ok ref=0x{:08X} (base=0x{:08X}) via={} matchDist={:.0f}u"sv,
            matchedRefID, d.resolvedBase,
            clone ? "clone" : "player-fallback",
            matchDist);

        // Record for linked-pair dedup so the partner half (toggled via the
        // engine link by this replay) is skipped later in the same batch.
        replayedPos.push_back({ d.px, d.py, d.pz });
    }
}

}  // namespace Pulsar::WorldSync::Door
