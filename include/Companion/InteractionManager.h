#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

namespace Pulsar::Companion
{
    struct PendingInteraction
    {
        uint32_t targetFormID;
    };

    struct PendingDrop
    {
        uint32_t itemFormID;
        int32_t  count;
    };

    // Inventory snapshot entry for network serialization
    struct InventorySnapshotEntry
    {
        uint32_t formID;
        int32_t  count;
        bool     equipped;
    };

    class InteractionManager
    {
    public:
        // Called from network thread
        void RequestInteract(uint32_t a_targetFormID);
        void RequestDrop(uint32_t a_itemFormID, int32_t a_count);
        void RequestSpawnDrop(uint32_t a_itemFormID, int32_t a_count);
        void RequestInventorySync();
        void RequestRestoreInventory(const std::vector<PendingDrop>& a_items);

        // Called from game thread
        void ProcessPending();

    private:
        void DoInteract(uint32_t a_targetFormID);
        void DoDrop(uint32_t a_itemFormID, int32_t a_count);
        void DoSpawnDrop(uint32_t a_itemFormID, int32_t a_count);
        void DoRestoreInventory(const std::vector<PendingDrop>& a_items);
        void DoInventorySync();
        void SendDenied(uint32_t a_formID, uint8_t a_reason);
        void CheckPlayerDrops();

        std::mutex                      m_mutex;
        std::vector<PendingInteraction> m_pendingInteractions;
        std::vector<PendingDrop>        m_pendingDrops;
        std::vector<PendingDrop>        m_pendingSpawnDrops; // received from other side
        std::vector<PendingDrop>        m_pendingRestoreItems; // profile restore to clone
        std::atomic<bool>               m_pendingInvSync{ false };

        // Inventory tracking for drop detection
        struct InvEntry { uint32_t formID; int32_t count; };
        std::vector<InvEntry>           m_lastInventory;
        int                             m_invCheckCounter = 0;
        bool                            m_suppressDropCheck = false;

    };

    InteractionManager& GetInteractionManager();
}
