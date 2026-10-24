#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Pulsar::Companion
{
    struct CompanionPositionData
    {
        float x, y, z;
        float rotX, rotY, rotZ;

        // Animation (filled from network packet)
        float    speed = 0.0f;
        float    direction = 0.0f;
        uint16_t animFlags = 0;
    };

    class CompanionManager
    {
    public:
        // Called from network thread - sets a pending request
        void RequestDisableAI();
        void RequestEnableAI();
        void RequestSetPosition(float x, float y, float z, float rotX, float rotY, float rotZ,
                                float speed = 0.0f, float direction = 0.0f, uint16_t animFlags = 0);

        // Called from game thread (main update) - processes pending requests
        void ProcessPending();

        bool IsAIDisabled() const { return m_aiDisabled; }
        RE::Actor* GetCompanion() const { return m_companion; }
        CompanionPositionData GetCompanionPosition() const;

    private:
        void DoDisableAI();
        void DoEnableAI();
        void DoSetPosition();

        RE::Actor* m_companion = nullptr;
        bool       m_aiDisabled = false;

        std::atomic<bool> m_pendingDisable{ false };
        std::atomic<bool> m_pendingEnable{ false };
        std::atomic<bool> m_pendingPosition{ false };
        bool              m_hasPosition = false;

        std::mutex            m_posMutex;
        CompanionPositionData m_pendingPosData{};
        CompanionPositionData m_targetPosData{};
        CompanionPositionData m_currentPosData{};
        CompanionPositionData m_lastAppliedPos{};

        // SetPosition hook - blocks engine from moving remote companion
        static inline bool s_allowSetPosition = false;
        static inline void* s_originalSetPosition = nullptr;
        static void __fastcall HookedSetPosition(RE::Actor* a_this, const RE::NiPoint3& a_pos, bool a_updateCharController);

        void InstallSetPositionHook();
        void RemoveSetPositionHook();
    };

    CompanionManager& GetCompanionManager();
}
