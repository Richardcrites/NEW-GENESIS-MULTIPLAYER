#include "PCH.h"
#include "Lobby/LobbyMenu.h"

#include "RE/S/ScaleformMemoryHeap.h"
#include "RE/S/ScaleformPtr.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"

#include <cstdlib>
#include <cstring>

using namespace std::string_view_literals;

namespace Pulsar::Lobby
{
    // ── menuMap (CONFIRMED in-game) ──────────────────────────────────────────
    // Base = UI+0x470 (CLibSF's 0x430 is stale). Engine hashes the BSFixedString
    // DATA POINTER folded (low32 ^ high32) & (cap-1); keys compared by pointer.
    static constexpr std::uintptr_t kMenuMapOffset = 0x470;

    struct EngineMenuHash
    {
        std::uint32_t operator()(const RE::BSFixedString& a_key) const noexcept
        {
            const auto p = reinterpret_cast<std::uintptr_t>(a_key.c_str());
            return static_cast<std::uint32_t>(p) ^ static_cast<std::uint32_t>(p >> 32);
        }
    };
    using EngineMenuMap = RE::BSTHashMap<RE::BSFixedString, RE::UI::UIMenuEntry, EngineMenuHash>;

    // ── robust vtable (copy of the engine IMenu vtable, our getters patched) ──
    // CLibSF's IMenu vtable thunks are all stale {0}. Instead we point a created
    // LobbyMenu's vtable pointers at the engine's REAL IMenu vtables, patching
    // only the 3 pure-virtual getters. Engine vtable IDs (IDs_VTABLE.h:13455):
    // primary 475515 (0x4D7E9A8), sub2/BSInputEventUser 475519, sub3/BSTEventSink 475517.
    static std::uintptr_t g_vtblPrimary[48];
    static std::uintptr_t g_sub2 = 0;
    static std::uintptr_t g_sub3 = 0;
    static bool           g_vtblReady = false;

    static const char* MenuGetName(RE::IMenu*)
    {
        static bool s_l = false; if (!s_l) { s_l = true; PLOG_INFO("[PULSAR] [LobbyMenu] open step: GetName"sv); }
        return "LobbyMenu";
    }
    static const char* MenuGetRootPath(RE::IMenu*)
    {
        static bool s_l = false; if (!s_l) { s_l = true; PLOG_INFO("[PULSAR] [LobbyMenu] open step: GetRootPath"sv); }
        return "";   // empty = use the movie root (file path here crashed the engine, used as a clip path)
    }
    static std::uint64_t MenuGetUnk05(RE::IMenu*)
    {
        static bool s_l = false; if (!s_l) { s_l = true; PLOG_INFO("[PULSAR] [LobbyMenu] open step: GetUnk05"sv); }
        return 0;
    }

    // Real Scaleform heap - CLibSF's ID::Scaleform::MemoryHeapPT::Singleton is {0}
    // (stale), so its operator new derefs garbage and freezes. REL::ID 820297 is
    // the real singleton for 1.16.236.
    static RE::Scaleform::MemoryHeapPT* RealScaleformHeap()
    {
        static REL::Relocation<RE::Scaleform::MemoryHeapPT**> singleton{ REL::ID(820297) };
        return *singleton;
    }

    static void InitVtbl()
    {
        if (g_vtblReady) return;
        auto* src = reinterpret_cast<std::uintptr_t*>(REL::ID(475515).address());  // engine IMenu primary
        for (int i = 0; i < 48; ++i) g_vtblPrimary[i] = src[i];
        g_vtblPrimary[3] = reinterpret_cast<std::uintptr_t>(&MenuGetName);     // GetName   (slot 03)
        g_vtblPrimary[4] = reinterpret_cast<std::uintptr_t>(&MenuGetRootPath); // GetRootPath(slot 04)
        g_vtblPrimary[5] = reinterpret_cast<std::uintptr_t>(&MenuGetUnk05);    // GetUnk05  (slot 05)
        g_sub2 = REL::ID(475519).address();
        g_sub3 = REL::ID(475517).address();
        g_vtblReady = true;
    }

    // initFunc: the engine calls this when opening the menu. Build a LobbyMenu and
    // point its 3 vtable pointers at the engine-real vtables (patched primary).
    // (Refcount: reset() AddRefs; a leftover ctor ref may leak on close - tolerable
    // for now, never crashes.)
    static RE::Scaleform::Ptr<RE::IMenu>* LobbyMenuCreator(RE::Scaleform::Ptr<RE::IMenu>* a_out)
    {
        PLOG_INFO("[PULSAR] [LobbyMenu] open step: creator entry (vtblReady={})"sv, g_vtblReady);
        if (!g_vtblReady) return a_out;
        // Recipe from decompiling MessageBoxMenu's creator (FUN_141edde50): allocate
        // a real-menu-sized buffer (0x3c8; use 0x400 to be safe), run the engine's
        // BASE IMenu ctor FUN_142553970 (RVA 0x25516B0) to init the common members
        // (uiMovie/flags/movie-context - what our zero-init object lacked), then
        // overwrite the primary vtable with our patched copy + set the menu flags.
        void* m = std::malloc(0x400);
        if (!m) { PLOG_WARN("[PULSAR] [LobbyMenu] open: malloc failed"sv); return a_out; }
        std::memset(m, 0, 0x400);
        PLOG_INFO("[PULSAR] [LobbyMenu] open step: allocated, calling base ctor"sv);
        {
            using ctor_t = void (*)(void*);
            static REL::Relocation<ctor_t> baseCtor{ REL::Offset(0x25516B0) };
            baseCtor(m);
        }
        PLOG_INFO("[PULSAR] [LobbyMenu] open step: base ctor done"sv);
        const auto base = reinterpret_cast<std::uintptr_t>(m);
        // Overwrite the primary vtable with our patched copy (engine IMenu vtable +
        // our GetName/GetRootPath/GetUnk05). Sub-vtables (0x10/0x50) keep what the
        // base ctor set (the real IMenu input/event-sink vtables).
        *reinterpret_cast<std::uintptr_t*>(base + 0x00) = reinterpret_cast<std::uintptr_t>(g_vtblPrimary);
        *reinterpret_cast<std::uint32_t*>(base + 0xC0) |= 0x40483u; // menu flags (from LoadingMenu -
        *reinterpret_cast<std::uint8_t*>(base + 0xD2)  = 1;         // passive full-screen, not the
                                                                    // data-driven MessageBox 0x2293)
        a_out->reset(reinterpret_cast<RE::IMenu*>(m));
        PLOG_INFO("[PULSAR] [LobbyMenu] open step: creator done"sv);
        return a_out;
    }

    // ── registration (SEH-guarded; no unwinding objects inside __try) ─────────
    static void SehRegister(RE::UI* a_ui, const RE::BSFixedString& a_hud,
                            const RE::BSFixedString& a_lobby,
                            bool& a_hud_found, bool& a_lobby_found, bool& a_faulted) noexcept
    {
        auto* mm = reinterpret_cast<EngineMenuMap*>(
            reinterpret_cast<std::uintptr_t>(a_ui) + kMenuMapOffset);
        __try {
            a_hud_found = mm->contains(a_hud);
            if (a_hud_found && !mm->contains(a_lobby)) {
                (*mm)[a_lobby].initFunc = &LobbyMenuCreator;
            }
            a_lobby_found = mm->contains(a_lobby);
        } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
            a_faulted = true;
        }
    }

    void TryRegisterMenuOnce()
    {
        static bool s_done = false;
        if (s_done) return;

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;   // UI singleton not up yet - retry next tick
        s_done = true;

        InitVtbl();

        const RE::BSFixedString hudName("HUDMenu");
        const RE::BSFixedString lobbyName("LobbyMenu");
        bool hud = false, lobby = false, faulted = false;
        SehRegister(ui, hudName, lobbyName, hud, lobby, faulted);

        if (faulted) {
            PLOG_WARN("[PULSAR] [LobbyMenu] register FAULTED (game safe)"sv);
            return;
        }
        PLOG_INFO("[PULSAR] [LobbyMenu] registered (real creator) - HUDMenu={}, LobbyMenu={}, vtbl={}"sv,
                  hud, lobby, g_vtblReady);

        // PROBE: log existing menus' creator (initFunc) + live menu ptr, so we can
        // statically decompile a concrete menu's CONSTRUCTION - the recipe we need
        // to init our object properly (the engine crashed on our zero-init object).
        // Reads the confirmed menuMap; only existing keys (no insert). Safe.
        if (hud) {
            // Runtime module base via the IMenu vtable (RVA 0x4D7E9A8) → convert
            // creator VAs to Ghidra addresses (0x140000000 + RVA) for static decompile.
            const std::uintptr_t base = REL::ID(475515).address() - 0x4D7E9A8ull;
            auto* mm = reinterpret_cast<EngineMenuMap*>(
                reinterpret_cast<std::uintptr_t>(ui) + kMenuMapOffset);
            const char* names[] = { "MessageBoxMenu", "LoadingMenu", "HUDMenu", "DataMenu" };
            for (auto* nm : names) {
                const RE::BSFixedString n(nm);
                if (mm->contains(n)) {
                    auto& e = (*mm)[n];
                    const auto rva = reinterpret_cast<std::uintptr_t>(e.initFunc) - base;
                    PLOG_INFO("[PULSAR] [LobbyMenu] probe {} -> creator ghidra=0x{:x}"sv,
                              nm, 0x140000000ull + rva);
                }
            }
        }
    }

    void ToggleLobbyMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        auto* q  = RE::UIMessageQueue::GetSingleton();
        if (!ui || !q) return;
        const RE::BSFixedString name("LobbyMenu");
        const bool open = ui->IsMenuOpen(name);
        q->AddMessage(name, open ? RE::UI_MESSAGE_TYPE::kHide : RE::UI_MESSAGE_TYPE::kShow);
        PLOG_INFO("[PULSAR] [LobbyMenu] toggle -> {}"sv, open ? "hide"sv : "show"sv);
    }
}
