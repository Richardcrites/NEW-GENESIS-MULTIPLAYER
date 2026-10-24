#include "PCH.h"
#include "Companion/InteractionManager.h"
#include "Companion/CompanionManager.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <RE/A/Actor.h>
#include <RE/B/BGSInventoryItem.h>
#include <RE/B/BGSInventoryList.h>
#include <RE/B/BSContainer.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/Script.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <cstdio>

using namespace std::string_view_literals;

namespace Pulsar::Companion
{
    static InteractionManager g_interactionManager;

    InteractionManager& GetInteractionManager()
    {
        return g_interactionManager;
    }

    void InteractionManager::RequestInteract(uint32_t a_targetFormID)
    {
        std::lock_guard lock(m_mutex);
        m_pendingInteractions.push_back({ a_targetFormID });
    }

    void InteractionManager::RequestDrop(uint32_t a_itemFormID, int32_t a_count)
    {
        std::lock_guard lock(m_mutex);
        m_pendingDrops.push_back({ a_itemFormID, a_count });
    }

    void InteractionManager::RequestSpawnDrop(uint32_t a_itemFormID, int32_t a_count)
    {
        std::lock_guard lock(m_mutex);
        m_pendingSpawnDrops.push_back({ a_itemFormID, a_count });
    }

    void InteractionManager::RequestInventorySync()
    {
        m_pendingInvSync = true;
    }

    void InteractionManager::RequestRestoreInventory(const std::vector<PendingDrop>& a_items)
    {
        std::lock_guard lock(m_mutex);
        m_pendingRestoreItems.insert(m_pendingRestoreItems.end(), a_items.begin(), a_items.end());
    }

    void InteractionManager::ProcessPending()
    {
        // Process interactions
        {
            std::vector<PendingInteraction> interactions;
            {
                std::lock_guard lock(m_mutex);
                interactions.swap(m_pendingInteractions);
            }
            for (auto& interact : interactions) {
                DoInteract(interact.targetFormID);
            }
        }

        // Process drops
        {
            std::vector<PendingDrop> drops;
            {
                std::lock_guard lock(m_mutex);
                drops.swap(m_pendingDrops);
            }
            for (auto& drop : drops) {
                DoDrop(drop.itemFormID, drop.count);
            }
        }

        // Process spawn drops (items received from other side)
        {
            std::vector<PendingDrop> spawns;
            {
                std::lock_guard lock(m_mutex);
                spawns.swap(m_pendingSpawnDrops);
            }
            for (auto& s : spawns) {
                DoSpawnDrop(s.itemFormID, s.count);
            }
        }

        // Process profile restore items (add to remote clone)
        {
            std::vector<PendingDrop> restoreItems;
            {
                std::lock_guard lock(m_mutex);
                restoreItems.swap(m_pendingRestoreItems);
            }
            if (!restoreItems.empty()) {
                DoRestoreInventory(restoreItems);
            }
        }

        // Check player inventory for drops (every 15 frames)
        if (++m_invCheckCounter >= 15) {
            m_invCheckCounter = 0;
            CheckPlayerDrops();
        }

        // Process inventory sync
        if (m_pendingInvSync.exchange(false)) {
            DoInventorySync();
        }
    }

    void InteractionManager::SendDenied(uint32_t a_formID, uint8_t a_reason)
    {
        Network::ItemDeniedData denied{ a_formID, a_reason };
        uint8_t buf[64]{};
        Network::PacketWriter writer(Network::PKT_ITEM_DENIED, buf, sizeof(buf));
        writer.Write(denied);
        Network::GetSingleton().Send(writer.Data(), writer.Size());
    }

    void InteractionManager::DoInteract(uint32_t a_targetFormID)
    {
        auto* companion = GetCompanionManager().GetCompanion();
        if (!companion) {
            PLOG_WARN("[PULSAR] Interact failed: no companion"sv);
            return;
        }

        auto* targetForm = RE::TESForm::LookupByID(a_targetFormID);
        if (!targetForm) {
            PLOG_WARN("[PULSAR] Interact failed: target 0x{:08X} not found"sv, a_targetFormID);
            SendDenied(a_targetFormID, 2);
            return;
        }

        auto* targetRef = targetForm->As<RE::TESObjectREFR>();
        if (!targetRef) {
            PLOG_WARN("[PULSAR] Interact failed: 0x{:08X} is not a reference"sv, a_targetFormID);
            return;
        }

        // Check distance (max 500 units)
        auto& companionPos = companion->data.location;
        auto& targetPos = targetRef->data.location;
        float dx = companionPos.x - targetPos.x;
        float dy = companionPos.y - targetPos.y;
        float dz = companionPos.z - targetPos.z;
        float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq > 500.0f * 500.0f) {
            PLOG_WARN("[PULSAR] Interact failed: target too far ({:.0f} units)"sv, std::sqrt(distSq));
            SendDenied(a_targetFormID, 1);
            return;
        }

        // Try to activate the target as the companion
        RE::OBJ_ACTIVATE_DATA activateData{};
        activateData.activatedRef = targetRef;
        activateData.actionRef = companion;
        activateData.objectToGet = nullptr;
        activateData.count = 1;

        auto baseObj = targetRef->GetBaseObject();
        if (baseObj) {
            bool result = baseObj->Activate(activateData);
            PLOG_INFO("[PULSAR] Activate 0x{:08X} by companion: {}"sv, a_targetFormID, result);

            if (result) {
                // Send item taken confirmation
                Network::ItemTakenData taken{};
                taken.targetFormID = a_targetFormID;
                taken.itemFormID = baseObj->GetFormID();
                taken.count = 1;

                uint8_t buf[64]{};
                Network::PacketWriter writer(Network::PKT_ITEM_TAKEN, buf, sizeof(buf));
                writer.Write(taken);
                Network::GetSingleton().Send(writer.Data(), writer.Size());

                // Trigger inventory sync after interaction
                m_pendingInvSync = true;
            }
        }
    }

    void InteractionManager::DoDrop(uint32_t a_itemFormID, int32_t a_count)
    {
        auto* companion = GetCompanionManager().GetCompanion();
        if (!companion) {
            PLOG_WARN("[PULSAR] Drop failed: no companion"sv);
            SendDenied(a_itemFormID, 2);
            return;
        }

        // Find item in companion's inventory to get the correct TESBoundObject*
        RE::TESBoundObject* boundObj = nullptr;
        companion->ForEachInventoryItem([&](const RE::BGSInventoryItem& a_item) -> RE::BSContainer::ForEachResult {
            if (a_item.object && a_item.object->GetFormID() == a_itemFormID) {
                boundObj = a_item.object;
                return RE::BSContainer::ForEachResult::kStop;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        if (!boundObj) {
            PLOG_WARN("[PULSAR] Drop failed: 0x{:08X} not in companion inventory"sv, a_itemFormID);
            SendDenied(a_itemFormID, 0); // already taken / not found
            return;
        }

        PLOG_INFO("[PULSAR] Drop: 0x{:08X} found in inventory, removing {}x"sv, a_itemFormID, a_count);

        companion->AddObjectToContainer(boundObj, nullptr, -a_count, nullptr,
            RE::ITEM_TRANSFER_REASON::kDropping);

        PLOG_INFO("[PULSAR] Dropped {}x 0x{:08X} from companion"sv, a_count, a_itemFormID);

        // Trigger inventory sync after drop
        m_pendingInvSync = true;
    }

    void InteractionManager::DoSpawnDrop(uint32_t a_itemFormID, int32_t a_count)
    {
        // Other player dropped an item - add it to our player's inventory
        // 3D world drops will come automatically with World Sync (shared entities)
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto* form = RE::TESForm::LookupByID(a_itemFormID);
        if (!form) {
            PLOG_WARN("[PULSAR] SpawnDrop: form 0x{:08X} not found"sv, a_itemFormID);
            return;
        }

        auto* boundObj = reinterpret_cast<RE::TESBoundObject*>(form);
        m_suppressDropCheck = true;  // don't detect this as OUR drop
        player->AddObjectToContainer(boundObj, nullptr, a_count, nullptr,
            RE::ITEM_TRANSFER_REASON::kNone);
        PLOG_INFO("[PULSAR] SpawnDrop: added {}x 0x{:08X} to player inventory"sv, a_count, a_itemFormID);
    }

    void InteractionManager::CheckPlayerDrops()
    {
        // DISABLED 2026-04-19: Any inventory decrease (ammo consumption when
        // firing, food/stim consumption, equip-from-inventory, etc.) is
        // detected as a "drop" and sent to the remote side, which then
        // AddObjectToContainer's it onto the remote player. Firing a full-auto
        // weapon triggered dozens of PKT_DROP_ITEM per second → rapid container
        // mutations on the peer → crash. Whole drop-sync needs a real design
        // (proper ItemDropEvent hook, filter by drop-vs-use, etc.). Gate off
        // until rewritten.
        return;
        // Skip check if we just transferred items ourselves
        if (m_suppressDropCheck) {
            m_suppressDropCheck = false;
            m_lastInventory.clear(); // reset snapshot to avoid false diff
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Build current inventory snapshot
        std::vector<InvEntry> current;
        player->ForEachInventoryItem([&](const RE::BGSInventoryItem& a_item) -> RE::BSContainer::ForEachResult {
            if (!a_item.object) return RE::BSContainer::ForEachResult::kContinue;
            int32_t total = 0;
            for (auto& stack : a_item.stacks) {
                total += static_cast<int32_t>(stack.count);
            }
            if (total > 0) {
                current.push_back({ a_item.object->GetFormID(), total });
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // Compare with last snapshot - find items that decreased
        // Cooldown: skip if we just sent a drop recently (prevents spam)
        static uint64_t s_lastDropSend = 0;
        uint64_t dropNow = GetTickCount64();
        if (!m_lastInventory.empty() && (dropNow - s_lastDropSend > 500)) {
            for (auto& prev : m_lastInventory) {
                int32_t newCount = 0;
                for (auto& cur : current) {
                    if (cur.formID == prev.formID) {
                        newCount = cur.count;
                        break;
                    }
                }
                int32_t diff = prev.count - newCount;
                if (diff > 0) {
                    PLOG_INFO("[PULSAR] Player dropped {}x 0x{:08X}"sv, diff, prev.formID);

                    Network::DropItemData drop{ prev.formID, diff };
                    uint8_t buf[64]{};
                    Network::PacketWriter writer(Network::PKT_DROP_ITEM, buf, sizeof(buf));
                    writer.Write(drop);
                    Network::GetSingleton().Send(writer.Data(), writer.Size());
                    s_lastDropSend = dropNow;
                }
            }
        }

        m_lastInventory = std::move(current);
    }

    void InteractionManager::DoInventorySync()
    {
        auto* companion = GetCompanionManager().GetCompanion();
        if (!companion) {
            return;
        }

        // Build inventory snapshot
        std::vector<Network::InventoryEntry> entries;

        companion->ForEachInventoryItem([&](const RE::BGSInventoryItem& a_item) -> RE::BSContainer::ForEachResult {
            if (!a_item.object) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            int32_t totalCount = 0;
            for (auto& stack : a_item.stacks) {
                totalCount += static_cast<int32_t>(stack.count);
            }

            if (totalCount > 0) {
                Network::InventoryEntry entry{};
                entry.formID = a_item.object->GetFormID();
                entry.count = totalCount;
                entry.equipped = a_item.IsEquipped() ? 1 : 0;
                entries.push_back(entry);
            }

            return RE::BSContainer::ForEachResult::kContinue;
        });

        // Send inventory packet: header + count(uint16) + entries
        // Use a dynamic buffer since inventory can be large
        size_t payloadSize = sizeof(uint16_t) + entries.size() * sizeof(Network::InventoryEntry);
        size_t totalSize = Network::HEADER_SIZE + payloadSize;
        std::vector<uint8_t> buf(totalSize);

        Network::PacketWriter writer(Network::PKT_INVENTORY, buf.data(), buf.size());
        uint16_t entryCount = static_cast<uint16_t>(entries.size());
        writer.Write(entryCount);
        for (auto& entry : entries) {
            writer.Write(entry);
        }

        Network::GetSingleton().Send(writer.Data(), writer.Size());
        PLOG_INFO("[PULSAR] Sent inventory sync: {} items"sv, entryCount);
    }

    void InteractionManager::DoRestoreInventory(const std::vector<PendingDrop>& a_items)
    {
        auto& remoteMgr = RemotePlayer::GetRemotePlayerManager();
        auto* clone = remoteMgr.GetRemoteActor();
        if (!clone) {
            PLOG_WARN("[PULSAR] RestoreInventory: clone not spawned yet, requeueing {} items"sv, a_items.size());
            // Requeue - will try again next ProcessPending cycle
            std::lock_guard lock(m_mutex);
            m_pendingRestoreItems.insert(m_pendingRestoreItems.end(), a_items.begin(), a_items.end());
            return;
        }

        int added = 0;
        for (auto& item : a_items) {
            auto* form = RE::TESForm::LookupByID(item.itemFormID);
            if (!form) {
                PLOG_WARN("[PULSAR] RestoreInventory: form 0x{:08X} not found"sv, item.itemFormID);
                continue;
            }
            auto* boundObj = reinterpret_cast<RE::TESBoundObject*>(form);
            clone->AddObjectToContainer(boundObj, nullptr, item.count, nullptr,
                RE::ITEM_TRANSFER_REASON::kNone);
            ++added;
        }
        PLOG_INFO("[PULSAR] RestoreInventory: added {}/{} items to clone"sv, added, a_items.size());
    }
}
