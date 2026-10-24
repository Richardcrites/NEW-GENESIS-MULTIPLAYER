#include "PCH.h"
#include "Coop/KillXpSync.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/NPCSync.h"
#include "Config.h"
#include "NASLog.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <MinHook.h>

using namespace std::string_view_literals;

namespace Pulsar::Coop::KillXp
{
    // ----------------------------------------------------------------
    // Engine XP-award primitive - FUN_141962310 (1.16.236) = REL::ID(101322).
    // float(Actor* target /*RCX*/, float xp /*XMM1*/, int source /*R8*/,
    //       char applyMult /*R9*/, void* p5, void* victim /*p6*/, void* p7);
    // RETURNS the final (post-multiplier) awarded XP as a FLOAT in XMM0 - verified
    // by 244 disasm (`vmovss xmm0,[rbp+0x28]; ret`). Declaring it uint64_t (RAX)
    // read garbage → the v1/v2 capture saw finalXp=0 and bailed. It is a float.
    // Entry guard proceeds iff xp>0 OR source∈{10,11} (244 confirmed
    // `lea eax,[r8-0xa]; cmp eax,1`). applyMult==1 → scaled award; applyMult==0 →
    // adds raw xp. Always then ModActorValue(Experience) + level-up/HUD check.
    //   source enums: 7 = KILL (death handler @0x1907BEE etc.), 0xc = generic
    // RewardPlayerXP. We capture source==7 and re-award shares with source==0xc
    // so our own hook never re-captures them (no echo / recursion).
    // ----------------------------------------------------------------
    using AwardXpFn = float(__fastcall*)(RE::Actor*, float, int, char,
                                         void*, void*, void*);
    static AwardXpFn s_origAwardXp = nullptr;
    // FUN_141962310 has a stable address-library ID. The raw RVA 0x1962310 from
    // the 1.16.236 RE shifted on 1.16.244 (it landed mid-function on a vtable
    // `call [rax+0x560]` → hooking it crashed the engine on load). Mapped via
    // port_map.py: 236 RVA 0x1962310 → REL::ID(101322) → 244 RVA 0x195FA70
    // (verified: prologue saves XMM1 = the float `xp` arg). Use the ID so
    // CommonLibSF resolves the right offset on any version.
    static constexpr std::uint64_t kAwardXpID = 101322;

    static constexpr int kSourceKill    = 7;     // death-handler kill award
    static constexpr int kSourceGeneric = 0xc;   // generic RewardPlayerXP (our shares)

    // Host-canonical FormIDs we earned natural kill-XP for this session. Keyed so
    // both machines agree (host = victim FormID, client = remapped). On receiving
    // a share for a key in this set we KNOW we already got natural XP for that
    // kill (the both-shoot case) → skip to avoid double-counting. Game-thread
    // mostly, but guarded for safety. Bounded: cleared past a high watermark.
    static std::mutex                     g_killSetMutex;
    static std::unordered_set<std::uint32_t> g_myNaturalKills;
    static constexpr std::size_t          kKillSetCap = 1024;

    // Shares received from the peer, queued on the net thread, awarded on the
    // game thread (engine XP-award must run on the game thread).
    static std::mutex                       g_applyMutex;
    static std::vector<Pulsar::Network::KillXpData> g_applyQueue;

    static void RecordNaturalKill(std::uint32_t a_hostRef)
    {
        std::lock_guard lk(g_killSetMutex);
        if (g_myNaturalKills.size() >= kKillSetCap) g_myNaturalKills.clear();
        g_myNaturalKills.insert(a_hostRef);
    }

    // Returns true (and erases) if we already earned natural XP for this host-ref.
    static bool ConsumeNaturalKill(std::uint32_t a_hostRef)
    {
        std::lock_guard lk(g_killSetMutex);
        auto it = g_myNaturalKills.find(a_hostRef);
        if (it == g_myNaturalKills.end()) return false;
        g_myNaturalKills.erase(it);
        return true;
    }

    // SEH read of an Actor's FormID (+0x28, TESForm.formID). POD-only.
    static bool SafeReadFormID(void* a_actor, std::uint32_t& a_out)
    {
        __try {
            a_out = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(a_actor) + 0x28);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // SEH award via the trampoline (bypasses our hook → no echo). source=0xc,
    // applyMult=0 → raw add of the exact XP the killer's engine awarded.
    static int SafeAwardShare(RE::Actor* a_player, float a_xp)
    {
        __try {
            if (s_origAwardXp) {
                s_origAwardXp(a_player, a_xp, kSourceGeneric, /*applyMult=*/0,
                              nullptr, nullptr, nullptr);
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static void SendKillXp(std::uint32_t a_hostRef, float a_xp)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        using namespace Pulsar::Network;
        std::uint8_t buf[1 + sizeof(KillXpData)];
        buf[0] = PKT_KILL_XP;
        KillXpData d{};
        d.hostRefFormID = a_hostRef;
        d.xp            = a_xp;
        std::memcpy(&buf[1], &d, sizeof(d));
        net.Send(buf, sizeof(buf), /*reliable=*/true);
    }

    // Capture: a KILL award (source==7) just fired for the local player. victim
    // is param_6 (== param_5 per decompile; p5 is the fallback). Runs on the game
    // thread inside the hook. No C++ destructibles past the SEH reads.
    static void TryCaptureKill(RE::Actor* a_target, void* a_p5, void* a_victim,
                               float a_finalXp)
    {
        if (a_finalXp <= 0.0f) return;  // nothing to share

        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        if (!Pulsar::GetConfig().coopShareKillXp) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || a_target != static_cast<RE::Actor*>(player)) return;

        void* victimPtr = a_victim ? a_victim : a_p5;
        if (!victimPtr) return;

        std::uint32_t victimFid = 0;
        if (!SafeReadFormID(victimPtr, victimFid)) return;
        if (victimFid == 0 || victimFid == 0x14) return;  // 0x14 = the player

        const bool isHost = net.IsHost();
        std::uint32_t key = isHost ? victimFid
                                   : Pulsar::NPCSync::LocalRefToHostRef(victimFid);
        // The combat NPCs are mostly tier=local (each machine runs its OWN copy,
        // not host-driven) → no remap entry → LocalRefToHostRef returns 0. Use the
        // local FormID as the dedup token so client→host sharing still works. For
        // a host-driven (tracked) NPC the key IS the host-canonical FormID and
        // matches both sides; for a local NPC the keys differ across machines so a
        // simultaneous both-shoot double-counts (accepted for Alpha - reads as a
        // teamwork bonus). Solo kills always share correctly one-for-one.
        if (key == 0) key = victimFid;

        static std::atomic<bool> s_diagged{ false };
        if (!s_diagged.exchange(true)) {
            PLOG_INFO("[KILLXP] first capture: isHost={} victimFid=0x{:08X} finalXp={:.1f} -> key=0x{:08X}"sv,
                      isHost ? 1 : 0, victimFid, a_finalXp, key);
        }

        RecordNaturalKill(key);
        SendKillXp(key, a_finalXp);
    }

    static float __fastcall Hooked_AwardXp(RE::Actor* a_target, float a_xp,
                                           int a_source, char a_applyMult,
                                           void* a_p5, void* a_victim, void* a_p7)
    {
        const float ret = s_origAwardXp
            ? s_origAwardXp(a_target, a_xp, a_source, a_applyMult, a_p5, a_victim, a_p7)
            : 0.0f;

        if (a_source == kSourceKill) {
            // One-shot raw diag - confirms target/victim/xp/ret for the first kill.
            static std::atomic<bool> s_killDiag{ false };
            if (!s_killDiag.exchange(true)) {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                std::uint32_t vfid = 0;
                if (a_victim) SafeReadFormID(a_victim, vfid);
                PLOG_INFO("[KILLXP-DIAG] KILL src7: targetIsPlayer={} victimFid=0x{:08X} xpIn={:.1f} ret={:.1f} conn={}"sv,
                          (a_target == static_cast<RE::Actor*>(pc)) ? 1 : 0, vfid, a_xp, ret,
                          Pulsar::Network::GetSingleton().IsConnected() ? 1 : 0);
            }
            TryCaptureKill(a_target, a_p5, a_victim, ret);
        }
        return ret;
    }

    void Init()
    {
        MH_STATUS mhInit = MH_Initialize();
        if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
            PLOG_WARN("[KILLXP] MH_Initialize failed: {}"sv, static_cast<int>(mhInit));
            return;
        }

        const std::uintptr_t addr = REL::ID(kAwardXpID).address();
        if (!addr) {
            PLOG_WARN("[KILLXP] award-XP address resolved to 0 (REL::ID {})"sv, kAwardXpID);
            return;
        }

        // Verify the prologue so a wrong/shifted address fails SAFE instead of
        // crashing the engine (the 0x1962310 raw-RVA bug: it landed mid-function
        // on a `call [rax+0x560]` = 0xFF byte, and hooking it crashed on load).
        std::uint8_t pro[16] = {};
        __try { std::memcpy(pro, reinterpret_cast<void*>(addr), sizeof(pro)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        PLOG_INFO("[KILLXP] award-XP @ 0x{:X} (REL::ID {}) prologue {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}"sv,
                  addr, kAwardXpID, pro[0], pro[1], pro[2], pro[3], pro[4], pro[5],
                  pro[6], pro[7], pro[8], pro[9], pro[10], pro[11]);
        // 0xFF = indirect call/jmp (mid-function), 0xE9/0xE8 = rel jmp/call
        // (mid-function or already hooked), 0x00 = unmapped. None is a valid
        // function entry → refuse the hook rather than corrupt the engine.
        if (pro[0] == 0xFF || pro[0] == 0xE9 || pro[0] == 0xE8 || pro[0] == 0x00) {
            PLOG_WARN("[KILLXP] prologue 0x{:02X} is not a function entry - REFUSING hook (address wrong for this version)"sv, pro[0]);
            return;
        }

        auto target = reinterpret_cast<LPVOID>(addr);
        if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hooked_AwardXp),
                          reinterpret_cast<LPVOID*>(&s_origAwardXp)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            PLOG_INFO("[KILLXP] XP-award hook installed @ 0x{:X} (REL::ID {} FUN_141962310)"sv, addr, kAwardXpID);
        } else {
            PLOG_WARN("[KILLXP] XP-award hook install FAILED"sv);
            s_origAwardXp = nullptr;
        }
    }

    void OnKillXpReceived(const std::uint8_t* a_data, std::size_t a_length)
    {
        using namespace Pulsar::Network;
        if (a_length < 1 + sizeof(KillXpData)) return;
        KillXpData d{};
        std::memcpy(&d, a_data + 1, sizeof(d));
        if (d.hostRefFormID == 0) return;
        std::lock_guard lk(g_applyMutex);
        g_applyQueue.push_back(d);
    }

    void OnGameThreadTick()
    {
        std::vector<Pulsar::Network::KillXpData> work;
        {
            std::lock_guard lk(g_applyMutex);
            if (g_applyQueue.empty()) return;
            work.swap(g_applyQueue);
        }

        if (!Pulsar::GetConfig().coopShareKillXp) return;  // drop (toggled off)
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        for (const auto& d : work) {
            // We already earned natural XP for this NPC (both-shoot) → skip.
            if (ConsumeNaturalKill(d.hostRefFormID)) {
                static std::atomic<bool> s_dedupDiag{ false };
                if (!s_dedupDiag.exchange(true)) {
                    PLOG_INFO("[KILLXP] share dedup: already earned natural XP for hostRef=0x{:08X} (skip)"sv,
                              d.hostRefFormID);
                }
                continue;
            }
            if (d.xp <= 0.0f) continue;
            const int ok = SafeAwardShare(player, d.xp);
            static std::atomic<bool> s_applyDiag{ false };
            if (!s_applyDiag.exchange(true)) {
                PLOG_INFO("[KILLXP] first apply: awarded shared XP={} for hostRef=0x{:08X} ok={}"sv,
                          d.xp, d.hostRefFormID, ok);
            }
        }
    }
}
