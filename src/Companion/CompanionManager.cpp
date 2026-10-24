#include "PCH.h"
#include "Companion/CompanionManager.h"

#include <RE/A/Actor.h>
#include <RE/A/AIProcess.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSScriptUtil.h>
#include <RE/G/GameVM.h>
#include <RE/I/IAnimationGraphManagerHolder.h>
#include <RE/I/IVirtualMachine.h>
#include <RE/N/NiPoint.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/V/Variable.h>
#include "NASLog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>

using namespace std::string_view_literals;

namespace Pulsar::Companion
{
    static CompanionManager g_companionManager;

    // HumanRace FormID in Starfield.esm
    static constexpr RE::TESFormID HUMAN_RACE_ID = 0x00000019;

    // Companion ref ID - 0x57BE = Vasco (robot, fallback)
    static constexpr RE::TESFormID VASCO_REF_ID = 0x000057BE;

    // ChangeAnimArchetype - confirmed via Ghidra RE of Starfield 1.16.236
    static bool ChangeAnimArchetype(RE::Actor* actor, RE::BGSKeyword* keyword)
    {
        if (!actor || !keyword) return false;
        REL::Relocation<bool(*)(RE::Actor*, RE::BGSKeyword*)> func{ REL::ID(97226) };
        return func(actor, keyword);
    }

    // SEH wrapper for direct Papyrus native handler call
    static int TryPapyrusNative(void* funcAddr, void* vm, uint32_t stackId, void* refr,
                                 float x, float y, float z, float angX, float angY, float angZ,
                                 float speed, float maxRotSpeed)
    {
        __try {
            auto func = reinterpret_cast<void(*)(void*, uint32_t, void*,
                float, float, float, float, float, float, float, float)>(funcAddr);
            func(vm, stackId, refr, x, y, z, angX, angY, angZ, speed, maxRotSpeed);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    // Call TranslateTo via direct native handler
    static bool CallPapyrusTranslateTo(RE::Actor* actor, float x, float y, float z, float rotZ, float speed)
    {
        if (!actor) return false;

        REL::Relocation<uintptr_t> handlerFunc{ REL::ID(118487) };

        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        // Diagnostic: check REFR+0xC8 (extraDataList / translation controller)
        static bool s_diagDone = false;
        if (!s_diagDone) {
            s_diagDone = true;
            auto* refMem = reinterpret_cast<uint8_t*>(static_cast<RE::TESObjectREFR*>(actor));
            auto* ptrC8 = *reinterpret_cast<void**>(refMem + 0xC8);
            auto* ptrD0 = *reinterpret_cast<void**>(refMem + 0xD0);
            auto* ptrD8 = *reinterpret_cast<void**>(refMem + 0xD8);
            PLOG_INFO("[PULSAR] REFR+0xC8={}, +0xD0={}, +0xD8={}"sv,
                      ptrC8 ? "OK" : "NULL", ptrD0 ? "OK" : "NULL", ptrD8 ? "OK" : "NULL");

            PLOG_INFO("[PULSAR] Actor currentProcess={}, high={}"sv,
                      actor->currentProcess ? "OK" : "NULL",
                      (actor->currentProcess && actor->currentProcess->high) ? "OK" : "NULL");
        }

        float rotZDeg = rotZ * 57.2957795f;

        int ok = TryPapyrusNative(
            reinterpret_cast<void*>(handlerFunc.address()),
            vm, 1,
            static_cast<RE::TESObjectREFR*>(actor),
            x, y, z,
            0.0f, 0.0f, rotZDeg,
            speed, 0.0f);

        static int s_logCount = 0;
        if (++s_logCount <= 20) {
            PLOG_INFO("[PULSAR] NativeTTo: ok={}, target=({:.0f},{:.0f},{:.0f}), speed={:.0f}"sv,
                      ok, x, y, z, speed);
        }

        return ok == 1;
    }

    // Call PulsarBridge.DoStopTranslation via DispatchStaticCall
    static bool CallPapyrusStopTranslation(RE::Actor* actor)
    {
        if (!actor) return false;

        auto* gameVM = RE::GameVM::GetSingleton();
        if (!gameVM) return false;
        auto* vm = gameVM->GetVM();
        if (!vm) return false;

        RE::BSScript::Variable actorVar;
        RE::BSScript::PackVariable(actorVar, static_cast<RE::TESObjectREFR*>(actor));
        if (actorVar.is<std::nullptr_t>()) return false;

        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(1);
            args[0] = actorVar;
            return true;
        };

        RE::BSFixedString objName("PulsarBridge");
        RE::BSFixedString funcName("DoStopTranslation");
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;

        return vm->DispatchStaticCall(objName, funcName, argFn, callback, 0);
    }

    CompanionManager& GetCompanionManager()
    {
        return g_companionManager;
    }

    void CompanionManager::RequestDisableAI()
    {
        m_pendingDisable = true;
    }

    void CompanionManager::RequestEnableAI()
    {
        m_pendingEnable = true;
    }

    void CompanionManager::RequestSetPosition(float x, float y, float z, float rotX, float rotY, float rotZ,
                                              float speed, float direction, uint16_t animFlags)
    {
        std::lock_guard lock(m_posMutex);
        m_pendingPosData = { x, y, z, rotX, rotY, rotZ, speed, direction, animFlags };
        m_pendingPosition = true;
    }

    void CompanionManager::ProcessPending()
    {
        if (m_pendingDisable.exchange(false)) {
            DoDisableAI();
        }
        if (m_pendingEnable.exchange(false)) {
            DoEnableAI();
        }
        if (m_pendingPosition.exchange(false)) {
            std::lock_guard lock(m_posMutex);
            m_targetPosData = m_pendingPosData;
            if (!m_hasPosition) {
                m_currentPosData = m_pendingPosData;
                m_lastAppliedPos = m_pendingPosData;
                m_hasPosition = true;
            }
        }
        if (m_hasPosition && m_aiDisabled && m_companion) {
            DoSetPosition();
        }
    }

    void CompanionManager::DoDisableAI()
    {
        if (m_aiDisabled) {
            return;
        }

        // Find companion: try Sarah (0x5182, human), then Vasco (0x57BE, robot)
        if (!m_companion) {
            // Test NPCs - use civilian to avoid companion follow-AI
            static constexpr RE::TESFormID SARAH_REF_ID = 0x00044C4E;
            auto* form = RE::TESForm::LookupByID(SARAH_REF_ID);
            if (form && form->IsActor()) {
                m_companion = static_cast<RE::Actor*>(form);
            } else {
                form = RE::TESForm::LookupByID(VASCO_REF_ID);
                if (!form || !form->IsActor()) {
                    static int skipLog = 0;
                    if (++skipLog >= 60) { skipLog = 0; PLOG_WARN("[PULSAR] Neither Sarah nor Vasco found"sv); }
                    return;
                }
                m_companion = static_cast<RE::Actor*>(form);
            }
            auto* race = m_companion->race;
            PLOG_INFO("[PULSAR] Companion: 0x{:08X}, race=0x{:08X} (HumanRace=0x19)"sv,
                      m_companion->GetFormID(), race ? race->GetFormID() : 0);
        }

        auto* process = m_companion->currentProcess;
        if (!process || !process->high) {
            static int skipLog2 = 0;
            if (++skipLog2 >= 60) { skipLog2 = 0; PLOG_WARN("[PULSAR] Companion waiting for high process data"sv); }
            return;
        }

        PLOG_INFO("[PULSAR] Companion found and ready: 0x{:08X}"sv, m_companion->GetFormID());

        // Full AI disable + SetPosition hook
        process->high->commandType = RE::COMMAND_TYPE::kStay;
        process->high->inCommandState = true;
        process->high->stop = true;
        process->ignoringCombat = true;

        InstallSetPositionHook();

        m_aiDisabled = true;
        PLOG_INFO("[PULSAR] AI disabled + hook, SetPosition+Animation mode"sv);
    }

    void CompanionManager::DoEnableAI()
    {
        if (!m_aiDisabled) {
            return;
        }

        if (!m_companion) {
            m_aiDisabled = false;
            return;
        }

        auto* process = m_companion->currentProcess;
        if (process && process->high) {
            process->high->commandType = RE::COMMAND_TYPE::kFollow;
            process->high->inCommandState = false;
            process->high->stop = false;
            process->ignoringCombat = false;

            m_companion->boolBits.set(RE::Actor::BOOL_BITS::kResetAI);
        }

        PLOG_INFO("[PULSAR] Companion AI re-enabled"sv);
        RemoveSetPositionHook();
        m_aiDisabled = false;
        m_hasPosition = false;
        m_companion = nullptr;
    }

    CompanionPositionData CompanionManager::GetCompanionPosition() const
    {
        if (!m_companion) {
            return {};
        }
        auto& pos = m_companion->data.location;
        auto& ang = m_companion->data.angle;
        return { pos.x, pos.y, pos.z, ang.x, ang.y, ang.z };
    }

    // SEH wrappers for vtable probing (MSVC C2712: __try can't coexist with C++ destructors)
    static int TryCallVtable(void* graphPtr, void* fnPtr, const void* namePtr, float val)
    {
        __try {
            using SetVarFn = bool(*)(void*, const void*, float);
            auto fn = reinterpret_cast<SetVarFn>(fnPtr);
            fn(graphPtr, namePtr, val);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    static int TryCallVtableInt(void* graphPtr, void* fnPtr, const void* namePtr, int32_t val)
    {
        __try {
            using SetVarFn = bool(*)(void*, const void*, int32_t);
            reinterpret_cast<SetVarFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    static int TryCallVtableBool(void* graphPtr, void* fnPtr, const void* namePtr, bool val)
    {
        __try {
            using SetVarFn = bool(*)(void*, const void*, bool);
            reinterpret_cast<SetVarFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    // SEH-safe wrapper for bhkCharacterController vtable[7] call
    static int TryCallCCVtable7(void* ccPtr, float velX, float velY)
    {
        __try {
            auto** vtable = *reinterpret_cast<void***>(ccPtr);
            __declspec(align(16)) float vel[4] = { velX, velY, 0.0f, 0.0f };
            using SetVelFn = void(*)(void*, const float*);
            reinterpret_cast<SetVelFn>(vtable[7])(ccPtr, vel);
            return 1;
        } __except(1) {
            return 0; // crashed
        }
    }

    // Force-set a float variable on the behavior graph bypassing the Get check.
    // Uses vtable[0x21] which was confirmed as SetVariableFloat.
    // SEH-safe plain C function (no C++ objects on stack).
    static int ForceSetGraphFloatImpl(void* graphPtr, void* fnPtr, const void* namePtr, float val)
    {
        __try {
            using SetVarFn = bool(*)(void*, const void*, float);
            reinterpret_cast<SetVarFn>(fnPtr)(graphPtr, namePtr, val);
            return 1;
        } __except(1) {
            return 0;
        }
    }

    static void ForceSetGraphFloat(RE::Actor* a_actor, const char* a_name, float a_val)
    {
        if (!a_actor) return;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        if (!a_actor->GetAnimationGraphManagerImpl(graphMgr) || !graphMgr) return;
        if (graphMgr->graphs.size() == 0) return;

        auto& graph = graphMgr->graphs[graphMgr->activeGraph];
        if (!graph) return;

        auto** vtable = *reinterpret_cast<void***>(graph.get());
        RE::BSFixedString varName(a_name);
        ForceSetGraphFloatImpl(graph.get(), vtable[0x21], &varName, a_val);
    }

    static bool SetGraphVarFloat(RE::Actor* a_actor, const char* a_name, float a_val)
    {
        if (!a_actor) return false;

        RE::BSFixedString varName(a_name);
        float testVal = 0;
        if (!a_actor->GetGraphVariableImplFloat(varName, testVal)) return false;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        if (!a_actor->GetAnimationGraphManagerImpl(graphMgr) || !graphMgr) return false;
        if (graphMgr->graphs.size() == 0) return false;

        auto& graph = graphMgr->graphs[graphMgr->activeGraph];
        if (!graph) return false;

        auto** vtable = *reinterpret_cast<void***>(graph.get());

        static int s_foundIndex = -1;
        if (s_foundIndex >= 0) {
            TryCallVtable(graph.get(), vtable[s_foundIndex], &varName, a_val);
            return true;
        }

        // Probe vtable indices to find SetVariableFloat
        for (int idx = 0x1F; idx <= 0x25; idx++) {
            if (!TryCallVtable(graph.get(), vtable[idx], &varName, 12345.0f))
                continue;

            float check = 0;
            a_actor->GetGraphVariableImplFloat(varName, check);
            if (check > 12344.0f && check < 12346.0f) {
                s_foundIndex = idx;
                PLOG_INFO("[PULSAR] SetGraphVar FOUND at vtable[0x{:02X}]!"sv, idx);
                TryCallVtable(graph.get(), vtable[idx], &varName, a_val);
                return true;
            }
            // Reset
            TryCallVtable(graph.get(), vtable[idx], &varName, 0.0f);
        }

        return false;
    }

    static bool SetGraphVarInt(RE::Actor* a_actor, const char* a_name, int32_t a_val)
    {
        if (!a_actor) return false;

        RE::BSFixedString varName(a_name);
        int32_t testVal = 0;
        if (!a_actor->GetGraphVariableImplInt(varName, testVal)) return false;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        if (!a_actor->GetAnimationGraphManagerImpl(graphMgr) || !graphMgr) return false;
        if (graphMgr->graphs.size() == 0) return false;

        auto& graph = graphMgr->graphs[graphMgr->activeGraph];
        if (!graph) return false;

        auto** vtable = *reinterpret_cast<void***>(graph.get());

        static int s_foundIndex = -1;
        if (s_foundIndex >= 0) {
            TryCallVtableInt(graph.get(), vtable[s_foundIndex], &varName, a_val);
            return true;
        }

        for (int idx = 0x1F; idx <= 0x25; idx++) {
            if (!TryCallVtableInt(graph.get(), vtable[idx], &varName, 99999))
                continue;

            int32_t check = 0;
            a_actor->GetGraphVariableImplInt(varName, check);
            if (check == 99999) {
                s_foundIndex = idx;
                PLOG_INFO("[PULSAR] SetGraphVarInt FOUND at vtable[0x{:02X}]!"sv, idx);
                TryCallVtableInt(graph.get(), vtable[idx], &varName, a_val);
                return true;
            }
        }

        return false;
    }

    static bool SetGraphVarBool(RE::Actor* a_actor, const char* a_name, bool a_val)
    {
        if (!a_actor) return false;

        RE::BSFixedString varName(a_name);
        bool testVal = false;
        if (!a_actor->GetGraphVariableImplBool(varName, testVal)) return false;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        if (!a_actor->GetAnimationGraphManagerImpl(graphMgr) || !graphMgr) return false;
        if (graphMgr->graphs.size() == 0) return false;

        auto& graph = graphMgr->graphs[graphMgr->activeGraph];
        if (!graph) return false;

        auto** vtable = *reinterpret_cast<void***>(graph.get());

        static int s_foundIndex = -1;
        if (s_foundIndex >= 0) {
            TryCallVtableBool(graph.get(), vtable[s_foundIndex], &varName, a_val);
            return true;
        }

        for (int idx = 0x1F; idx <= 0x25; idx++) {
            if (!TryCallVtableBool(graph.get(), vtable[idx], &varName, true))
                continue;

            bool check = false;
            a_actor->GetGraphVariableImplBool(varName, check);
            if (check == true) {
                s_foundIndex = idx;
                PLOG_INFO("[PULSAR] SetGraphVarBool FOUND at vtable[0x{:02X}]!"sv, idx);
                TryCallVtableBool(graph.get(), vtable[idx], &varName, a_val);
                return true;
            }
        }

        return false;
    }

    void __fastcall CompanionManager::HookedSetPosition(RE::Actor* a_this, const RE::NiPoint3& a_pos, bool a_updateCharController)
    {
        auto& mgr = GetCompanionManager();
        if (a_this != mgr.m_companion || s_allowSetPosition) {
            using OrigFn = void(__fastcall*)(RE::Actor*, const RE::NiPoint3&, bool);
            reinterpret_cast<OrigFn>(s_originalSetPosition)(a_this, a_pos, a_updateCharController);
        }
        // else: blocked - log periodically
        static int s_blockCount = 0;
        if (++s_blockCount >= 300) {
            PLOG_INFO("[PULSAR] SetPosition hook blocked {} engine calls"sv, s_blockCount);
            s_blockCount = 0;
        }
    }

    void CompanionManager::InstallSetPositionHook()
    {
        if (!m_companion || s_originalSetPosition) return;

        auto** vtable = *reinterpret_cast<void***>(m_companion);
        s_originalSetPosition = vtable[0x137];

        DWORD oldProtect;
        VirtualProtect(&vtable[0x137], sizeof(void*), PAGE_READWRITE, &oldProtect);
        vtable[0x137] = reinterpret_cast<void*>(&HookedSetPosition);
        VirtualProtect(&vtable[0x137], sizeof(void*), oldProtect, &oldProtect);

        PLOG_INFO("[PULSAR] SetPosition hook installed on companion vtable[0x137]"sv);
    }

    void CompanionManager::RemoveSetPositionHook()
    {
        if (!m_companion || !s_originalSetPosition) return;

        auto** vtable = *reinterpret_cast<void***>(m_companion);

        DWORD oldProtect;
        VirtualProtect(&vtable[0x137], sizeof(void*), PAGE_READWRITE, &oldProtect);
        vtable[0x137] = s_originalSetPosition;
        VirtualProtect(&vtable[0x137], sizeof(void*), oldProtect, &oldProtect);

        s_originalSetPosition = nullptr;
        PLOG_INFO("[PULSAR] SetPosition hook removed"sv);
    }

    void CompanionManager::DoSetPosition()
    {
        static int s_frameCount = 0;
        s_frameCount++;

        // Skip invalid positions (zero or suspiciously small coords)
        if (std::abs(m_targetPosData.x) < 100.0f && std::abs(m_targetPosData.y) < 100.0f) return;

        auto& npcPos = m_companion->data.location;
        float dx = m_targetPosData.x - npcPos.x;
        float dy = m_targetPosData.y - npcPos.y;
        float dist = std::sqrt(dx*dx + dy*dy);

        // Initial teleport: first time we get valid position, teleport NPC there
        static bool s_initialDone = false;
        if (!s_initialDone) {
            s_initialDone = true;
            s_allowSetPosition = true;
            RE::NiPoint3A pos(m_targetPosData.x, m_targetPosData.y, m_targetPosData.z);
            m_companion->SetPosition(pos, true);
            s_allowSetPosition = false;
            m_companion->data.angle.z = m_targetPosData.rotZ;
            PLOG_INFO("[PULSAR] Initial teleport to ({:.0f},{:.0f},{:.0f})"sv,
                      m_targetPosData.x, m_targetPosData.y, m_targetPosData.z);
            return;
        }

        // === Move 3D node directly - bypasses CharacterController completely ===
        // SetPosition kills animation pipeline. Direct NiNode transform update doesn't.
        // Update data.location (game state) + world.translate (render position)

        // Update game-state position (without calling SetPosition)
        m_companion->data.location.x = m_targetPosData.x;
        m_companion->data.location.y = m_targetPosData.y;
        m_companion->data.location.z = m_targetPosData.z;

        // Update 3D render position via loadedData->data3D->world.translate
        auto* loadedGuard = reinterpret_cast<uint8_t*>(m_companion) + 0xB8;
        // BSGuarded<LOADED_REF_DATA*, BSReadWriteLock>: ptr at offset 0, lock at offset 8
        auto* loadedData = *reinterpret_cast<RE::LOADED_REF_DATA**>(loadedGuard);
        if (loadedData && loadedData->data3D) {
            auto* niObj = loadedData->data3D.get();
            niObj->local.translate.x = m_targetPosData.x;
            niObj->local.translate.y = m_targetPosData.y;
            niObj->local.translate.z = m_targetPosData.z;
            niObj->world.translate.x = m_targetPosData.x;
            niObj->world.translate.y = m_targetPosData.y;
            niObj->world.translate.z = m_targetPosData.z;
        }

        // Rotation toward movement direction
        m_companion->data.angle.z = m_targetPosData.rotZ;

        // Animation graph variables from network data - EVERY call
        float netSpeed = m_targetPosData.speed;
        float netDirection = m_targetPosData.direction;

        ForceSetGraphFloat(m_companion, "Speed", netSpeed);
        ForceSetGraphFloat(m_companion, "Direction", netDirection);

        if (netSpeed > 0.5f) {
            SetGraphVarInt(m_companion, "iSyncIdleLocomotion", 1);
        } else {
            SetGraphVarInt(m_companion, "iSyncIdleLocomotion", 0);
        }

        if (s_frameCount % 1000 == 0) {
            float readSpeed = 0;
            RE::BSFixedString speedVar("Speed");
            m_companion->GetGraphVariableImplFloat(speedVar, readSpeed);
            PLOG_INFO("[PULSAR] Anim: dist={:.0f}, Speed={:.1f}(net={:.1f}), dir={:.1f}, npc=({:.0f},{:.0f}), tgt=({:.0f},{:.0f})"sv,
                      dist, readSpeed, netSpeed, netDirection, npcPos.x, npcPos.y,
                      m_targetPosData.x, m_targetPosData.y);
        }
    }
}
