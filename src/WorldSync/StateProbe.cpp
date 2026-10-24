#include "PCH.h"
#include "WorldSync/StateProbe.h"
#include "NASLog.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/B/BGSSaveLoad.h>
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>

using namespace std::string_view_literals;

namespace Pulsar::WorldSync::StateProbe {

    namespace {

        using PreLoadInit_t = void(__fastcall*)(char a_param);

        // V13m - the bit-0x4 load wrapper FUN_141843BF0 (the relaunch+int3 trap).
        // Widest-scalar return so whatever the original leaves in RAX/AL (the safe
        // loader FUN_1418436E0 returns a byte) is forwarded to the engine caller.
        using LoadWrap_t = std::uintptr_t(__fastcall*)(std::uintptr_t a_p1, std::uintptr_t a_p2,
                                                       std::uint32_t a_p3, std::uint32_t a_p4);

        // FUN_14189A8E0 full VA → RVA = 0x141898210 - 0x140000000 = 0x1898210
        constexpr std::uintptr_t kPreLoadInitRVA  = 0x1898210ULL;
        constexpr std::uintptr_t kGuardByteRVA    = 0x5E71706ULL;
        constexpr std::uintptr_t kCounterRVA      = 0x5E6D6DCULL;
        // FUN_141843BF0 (RVA 0x1841460) - bit-0x4 in-process load wrapper; the
        // single chokepoint that contains the "counter>1 && in-game → relaunch+int3"
        // trap (Ghidra-verified 1.16.236, double-checked).
        constexpr std::uintptr_t kLoadWrapRVA     = 0x1841460ULL;
        constexpr std::uintptr_t kWorldDataRVA    = 0x5E716F8ULL;
        constexpr std::uintptr_t kPlayerInitRVA   = 0x5FB4B78ULL;
        constexpr std::uintptr_t kArrCountRVA     = 0x5F47C10ULL;
        constexpr std::uintptr_t kArrDataRVA      = 0x5F47C18ULL;

        static PreLoadInit_t          g_origPreLoadInit = nullptr;
        static LoadWrap_t             g_origLoadWrap    = nullptr;
        static std::atomic<bool>      g_installed{ false };
        static std::atomic<uint32_t>  g_probeCounter{ 0 };
        // V13f: separate counter - only the HOOK entries (real engine load
        // events), not manual DumpManual calls. WorldSync's load-start
        // detection compares this against a captured-at-arm value.
        static std::atomic<uint32_t>  g_entryHookCount{ 0 };

        // Resolve once + cache.
        struct Addrs {
            volatile std::uint8_t*  guardByte;
            volatile std::uint32_t* counter;
            volatile std::uintptr_t* worldDataPtr;   // *(this) yields the real base
            volatile std::uintptr_t* playerInitPtr;
            volatile std::uint32_t* arrCount;
            volatile std::uintptr_t* arrData;
        };

        static Addrs ResolveOnce()
        {
            static Addrs a = []() {
                Addrs r{};
                r.guardByte     = reinterpret_cast<volatile std::uint8_t*>( REL::Offset(kGuardByteRVA).address() );
                r.counter       = reinterpret_cast<volatile std::uint32_t*>(REL::Offset(kCounterRVA).address());
                r.worldDataPtr  = reinterpret_cast<volatile std::uintptr_t*>(REL::Offset(kWorldDataRVA).address());
                r.playerInitPtr = reinterpret_cast<volatile std::uintptr_t*>(REL::Offset(kPlayerInitRVA).address());
                r.arrCount      = reinterpret_cast<volatile std::uint32_t*>(REL::Offset(kArrCountRVA).address());
                r.arrData       = reinterpret_cast<volatile std::uintptr_t*>(REL::Offset(kArrDataRVA).address());
                return r;
            }();
            return a;
        }

        // SEH-safe single-byte read.
        static bool SafeReadU8(volatile std::uint8_t* p, std::uint8_t& out)
        {
            if (!p) return false;
            __try { out = *p; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        static bool SafeReadU32(volatile std::uint32_t* p, std::uint32_t& out)
        {
            if (!p) return false;
            __try { out = *p; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        static bool SafeReadPtr(volatile std::uintptr_t* p, std::uintptr_t& out)
        {
            if (!p) return false;
            __try { out = *p; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // Read *(base + offset) safely. Returns 0 on failure (also writes ok=false).
        static std::uintptr_t SafeDerefAt(std::uintptr_t a_base, std::uintptr_t a_offset, bool& a_ok)
        {
            a_ok = false;
            if (!a_base) return 0;
            __try {
                auto* p = reinterpret_cast<volatile std::uintptr_t*>(a_base + a_offset);
                std::uintptr_t v = *p;
                a_ok = true;
                return v;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        struct Snapshot {
            std::uint32_t  probeNum;
            std::uint8_t   guardByte;
            std::uint32_t  counter;
            std::uintptr_t worldBase;
            std::uintptr_t slot1B0;
            std::uintptr_t slot1B8;
            std::uintptr_t slot1C0;
            std::uintptr_t slot1C8;
            std::uintptr_t playerInitGuard;
            std::uint32_t  arrCount;
            std::uintptr_t arrData;
            std::uintptr_t playerPtr;
            std::uint8_t   player_113c;
            std::uint32_t  player_dd4;
            std::uint32_t  playerFormID;
            std::uintptr_t playerCellPtr;
            std::uint32_t  playerCellFormID;
            std::uintptr_t mgrPtr;
            std::uint32_t  mgrTaskBits;
            // Validity flags
            bool guardOk      = false;
            bool counterOk    = false;
            bool worldBaseOk  = false;
            bool slot1B0Ok    = false;
            bool slot1B8Ok    = false;
            bool slot1C0Ok    = false;
            bool slot1C8Ok    = false;
            bool playerInitOk = false;
            bool arrCountOk   = false;
            bool arrDataOk    = false;
            bool player113cOk = false;
            bool playerDD4Ok  = false;
            bool playerCellOk = false;
            bool mgrTasksOk   = false;
        };

        static Snapshot Capture(std::uint32_t a_probeNum)
        {
            Snapshot s{};
            s.probeNum = a_probeNum;

            const auto a = ResolveOnce();

            s.guardOk     = SafeReadU8 (a.guardByte, s.guardByte);
            s.counterOk   = SafeReadU32(a.counter,   s.counter);
            s.worldBaseOk = SafeReadPtr(a.worldDataPtr, s.worldBase);

            if (s.worldBaseOk && s.worldBase) {
                s.slot1B0 = SafeDerefAt(s.worldBase, 0x1B0, s.slot1B0Ok);
                s.slot1B8 = SafeDerefAt(s.worldBase, 0x1B8, s.slot1B8Ok);
                s.slot1C0 = SafeDerefAt(s.worldBase, 0x1C0, s.slot1C0Ok);
                s.slot1C8 = SafeDerefAt(s.worldBase, 0x1C8, s.slot1C8Ok);
            }

            s.playerInitOk = SafeReadPtr(a.playerInitPtr, s.playerInitGuard);
            s.arrCountOk   = SafeReadU32(a.arrCount,      s.arrCount);
            s.arrDataOk    = SafeReadPtr(a.arrData,       s.arrData);

            // Player.
            __try {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    s.playerPtr = reinterpret_cast<std::uintptr_t>(player);

                    auto* p113c = reinterpret_cast<volatile std::uint8_t*>(s.playerPtr + 0x113C);
                    auto* pdd4  = reinterpret_cast<volatile std::uint32_t*>(s.playerPtr + 0xDD4);
                    s.player113cOk = SafeReadU8 (p113c, s.player_113c);
                    s.playerDD4Ok  = SafeReadU32(pdd4,  s.player_dd4);

                    // FormID is at +0x28 (verified per memory).
                    auto* pformid = reinterpret_cast<volatile std::uint32_t*>(s.playerPtr + 0x28);
                    SafeReadU32(pformid, s.playerFormID);

                    // parentCell pointer
                    s.playerCellPtr = reinterpret_cast<std::uintptr_t>(player->parentCell);
                    if (s.playerCellPtr) {
                        auto* pcellid = reinterpret_cast<volatile std::uint32_t*>(s.playerCellPtr + 0x28);
                        s.playerCellOk = SafeReadU32(pcellid, s.playerCellFormID);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // leave all player flags as not-ok
            }

            // SaveLoadMgr+0x50
            __try {
                auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
                if (mgr) {
                    s.mgrPtr = reinterpret_cast<std::uintptr_t>(mgr);
                    auto* tasks = reinterpret_cast<volatile std::uint32_t*>(s.mgrPtr + 0x50);
                    s.mgrTasksOk = SafeReadU32(tasks, s.mgrTaskBits);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { /* mgrTasksOk stays false */ }

            return s;
        }

        // Single-line dense format so it's easy to diff with `diff` or `meld`.
        // Each line has a fixed key=val schema.
        static void Emit(const Snapshot& s, const char* a_tag)
        {
            PLOG_INFO(
                "[PULSAR] [L.7 PROBE #{} {}] guard=0x{:02X}{} cnt={}{} "
                "wbase=0x{:X}{} slot1B0=0x{:X}{} slot1B8=0x{:X}{} "
                "slot1C0=0x{:X}{} slot1C8=0x{:X}{} pInitG=0x{:X}{} "
                "arrCnt={}{} arrData=0x{:X}{} "
                "player=0x{:X} pFID=0x{:08X} p113c=0x{:02X}{} pDD4={}{} "
                "pCell=0x{:X} pCellFID=0x{:08X}{} "
                "mgr=0x{:X} mgrTasks=0x{:08X}{}"sv,
                s.probeNum, a_tag,
                s.guardByte,        s.guardOk     ? ""sv : "?"sv,
                s.counter,          s.counterOk   ? ""sv : "?"sv,
                s.worldBase,        s.worldBaseOk ? ""sv : "?"sv,
                s.slot1B0,          s.slot1B0Ok   ? ""sv : "?"sv,
                s.slot1B8,          s.slot1B8Ok   ? ""sv : "?"sv,
                s.slot1C0,          s.slot1C0Ok   ? ""sv : "?"sv,
                s.slot1C8,          s.slot1C8Ok   ? ""sv : "?"sv,
                s.playerInitGuard,  s.playerInitOk? ""sv : "?"sv,
                s.arrCount,         s.arrCountOk  ? ""sv : "?"sv,
                s.arrData,          s.arrDataOk   ? ""sv : "?"sv,
                s.playerPtr,        s.playerFormID,
                s.player_113c,      s.player113cOk? ""sv : "?"sv,
                s.player_dd4,       s.playerDD4Ok ? ""sv : "?"sv,
                s.playerCellPtr,    s.playerCellFormID,
                                    s.playerCellOk? ""sv : "?"sv,
                s.mgrPtr,           s.mgrTaskBits,
                s.mgrTasksOk        ? ""sv : "?"sv);

            char nas[512];
            ::sprintf_s(nas, sizeof(nas),
                "L7 PROBE #%u %s g=%02X c=%u wb=%llX 1B0=%llX 1B8=%llX 1C0=%llX 1C8=%llX "
                "pIG=%llX ac=%u ad=%llX p113c=%02X pDD4=%u pCellFID=%08X tasks=%08X",
                s.probeNum, a_tag,
                s.guardByte, s.counter, (unsigned long long)s.worldBase,
                (unsigned long long)s.slot1B0, (unsigned long long)s.slot1B8,
                (unsigned long long)s.slot1C0, (unsigned long long)s.slot1C8,
                (unsigned long long)s.playerInitGuard, s.arrCount,
                (unsigned long long)s.arrData,
                s.player_113c, s.player_dd4, s.playerCellFormID, s.mgrTaskBits);
            NASLOG(nas);
        }

        static void __fastcall HookedPreLoadInit(char a_param)
        {
            const auto n = ++g_probeCounter;
            ++g_entryHookCount;
            const Snapshot snap = Capture(n);
            Emit(snap, "ENTRY");

            // Forward to original. SEH-safe in case the hook installation
            // landed on a non-prologue.
            __try {
                if (g_origPreLoadInit) g_origPreLoadInit(a_param);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN(
                    "[PULSAR] [L.7 PROBE] orig FUN_14189A8E0 threw"sv);
            }

            // Post-init snapshot - see what the engine actually mutated.
            const Snapshot post = Capture(n);
            Emit(post, "EXIT ");
        }

        // ----------------------------------------------------------------
        // V13m - relaunch-trap neutralizer (the real root-cause fix).
        //
        // FUN_141843BF0 (kLoadWrapRVA) is the engine's bit-0x4 in-process load
        // wrapper.  Ghidra-verified body (1.16.236):
        //     iVar = DAT_145e794dc;                       // load counter (READ only here)
        //     if (1 < iVar && DAT_145f4a4a0 == 1) {       // >1 loads this session && in-game
        //         relaunch .exe with "-relaunch"; swi(3); // int3 fail-stop  ← THE CRASH
        //     }
        //     FUN_1418436E0(p1,p2,p3,p4);                 // safe in-process load
        //
        // Bethesda intentionally refuses the Nth in-process load and relaunches.
        // Co-op legitimately loads many times per session (the V13 chain proved the
        // safe loader works unbounded once the counter stays ≤1).  Per-caller counter
        // clears (WorldSync V13i, CoopMortality) are RACY: they only cover their own
        // load path and stop running once the triggering tick ends.  The both-downed
        // game-over crash was exactly this - the game-over reload succeeded (counter
        // cleared in the death-tick window) but left the counter at 2, and the next
        // FUN_141843BF0 call (a co-op resync load) saw 2>1 → int3.
        //
        // FIX: this function is the SINGLE chokepoint for the trap decision (verified:
        // the only site that compares the counter and the only caller of the relaunch
        // helper).  Hook it and zero the counter on EVERY call, just before the engine
        // re-reads it → the guard always takes the safe-load branch, for ALL load paths
        // (game-over reload, co-op resync, fast-travel, dock).  Same-thread clear right
        // before the original runs → no race window.  No behaviour change beyond the
        // relaunch trap never firing.
        // ----------------------------------------------------------------
        static std::uintptr_t __fastcall HookedLoadWrap(std::uintptr_t a_p1, std::uintptr_t a_p2,
                                                        std::uint32_t a_p3, std::uint32_t a_p4)
        {
            const auto a = ResolveOnce();
            if (a.counter) {
                __try { *a.counter = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            std::uintptr_t ret = 0;
            __try {
                if (g_origLoadWrap) ret = g_origLoadWrap(a_p1, a_p2, a_p3, a_p4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_WARN("[PULSAR] [L.7 V13m] orig FUN_141843BF0 threw"sv);
            }
            return ret;
        }

    } // anonymous

    void DumpManual(const char* a_tag)
    {
        const auto n = ++g_probeCounter;
        const Snapshot snap = Capture(n);
        Emit(snap, a_tag ? a_tag : "MANUAL");
    }

    std::uint32_t GetEntryHookCount()
    {
        return g_entryHookCount.load();
    }

    void Init()
    {
        if (g_installed.exchange(true)) {
            PLOG_INFO("[PULSAR] [L.7 PROBE] StateProbe already installed"sv);
            return;
        }

        const auto fnAddr = REL::Offset(kPreLoadInitRVA).address();
        if (!fnAddr) {
            PLOG_WARN(
                "[PULSAR] [L.7 PROBE] FUN_14189A8E0 addr unresolved - probe disabled"sv);
            g_installed = false;
            return;
        }

        void* target = reinterpret_cast<void*>(fnAddr);
        MH_STATUS st = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&HookedPreLoadInit),
            reinterpret_cast<LPVOID*>(&g_origPreLoadInit));
        if (st != MH_OK) {
            PLOG_WARN(
                "[PULSAR] [L.7 PROBE] MH_CreateHook failed: {}"sv,
                static_cast<int>(st));
            g_installed = false;
            return;
        }
        st = MH_EnableHook(target);
        if (st != MH_OK) {
            PLOG_WARN(
                "[PULSAR] [L.7 PROBE] MH_EnableHook failed: {}"sv,
                static_cast<int>(st));
            g_installed = false;
            return;
        }

        PLOG_INFO(
            "[PULSAR] [L.7 PROBE] FUN_14189A8E0 hook installed @ 0x{:X} - "
            "every load fires a snapshot (entry+exit) tagged "
            "[L.7 PROBE #N ENTRY] / [L.7 PROBE #N EXIT]"sv,
            fnAddr);
        NASLOG("L.7 PROBE FUN_14189A8E0 hook installed");

        // V13m - install the relaunch-trap neutralizer on FUN_141843BF0.
        // This is the authoritative fix for the "Nth in-process load → relaunch+int3"
        // crash class (the both-downed game-over host crash); covers every load path.
        const auto wrapAddr = REL::Offset(kLoadWrapRVA).address();
        if (!wrapAddr) {
            PLOG_WARN("[PULSAR] [L.7 V13m] FUN_141843BF0 addr unresolved - relaunch trap NOT neutralized"sv);
            return;
        }
        void* wrapTarget = reinterpret_cast<void*>(wrapAddr);
        MH_STATUS wst = MH_CreateHook(
            wrapTarget,
            reinterpret_cast<LPVOID>(&HookedLoadWrap),
            reinterpret_cast<LPVOID*>(&g_origLoadWrap));
        if (wst != MH_OK) {
            PLOG_WARN("[PULSAR] [L.7 V13m] MH_CreateHook(FUN_141843BF0) failed: {}"sv,
                      static_cast<int>(wst));
            return;
        }
        wst = MH_EnableHook(wrapTarget);
        if (wst != MH_OK) {
            PLOG_WARN("[PULSAR] [L.7 V13m] MH_EnableHook(FUN_141843BF0) failed: {}"sv,
                      static_cast<int>(wst));
            return;
        }
        PLOG_INFO(
            "[PULSAR] [L.7 V13m] FUN_141843BF0 relaunch-trap neutralizer installed @ 0x{:X} "
            "(counter 0x5E6D6DC zeroed on every in-process load)"sv,
            wrapAddr);
        NASLOG("L.7 V13m relaunch-trap neutralizer installed (FUN_141843BF0)");
    }

}  // namespace Pulsar::WorldSync::StateProbe
