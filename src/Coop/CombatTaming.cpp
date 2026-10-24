#include "PCH.h"
#include "Coop/CombatTaming.h"
#include "Network/NetworkHost.h"
#include "Config.h"
#include "NASLog.h"

#include <array>
#include <cstdint>

#include <RE/G/GameSettingCollection.h>
#include <RE/S/Setting.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace std::string_view_literals;

namespace Pulsar::Coop::CombatTaming
{
    // The "erratic movement" combat GMSTs. Zeroing the CHANCE settings makes NPCs
    // stop circling / strafing / dodging / avoiding / cover-flanking → they move
    // to optimal range once, then hold + aim + fire. That is what keeps the two
    // machines' AI positions from diverging. (Names verified in the 1.16.244
    // binary string table.)
    static constexpr std::array kTameSettings = {
        // circle around the target
        "fCombatCircleChanceMax",
        "fCombatCircleChanceMin",
        "fCombatCircleDistantChanceMax",
        "fCombatCircleDistantChanceMin",
        // dodge / avoid-threat (sidestep)
        "fCombatDodgeChanceMax",
        "fCombatDodgeChanceMin",
        "fCombatAvoidThreatChanceMax",
        "fCombatAvoidThreatChanceMin",
        // periodic range re-adjust (strafe / shuffle / lateral)
        "fCombatAdjustRangeChanceMax",
        "fCombatAdjustRangeChanceMin",
        "fCombatAdjustRangeShuffleChance",
        "fCombatAdjustRangeDirectionChanceMax",
        "fCombatAdjustRangeDirectionChanceMid",
        "fCombatAdjustRangeDirectionChanceMin",
        // cover-based flanking (cross-map repositioning)
        "fCombatCoverFlankChanceMax",
        "fCombatCoverFlankChanceMid",
        "fCombatCoverFlankChanceMin",
    };

    static bool                                    g_applied = false;
    static std::array<float, kTameSettings.size()> g_saved{};   // originals
    static std::array<bool,  kTameSettings.size()> g_savedOk{}; // which resolved

    // SEH-wrapped GMST float get/set (defensive; engine pointer ops).
    static bool SafeGetFloat(const char* a_name, float& a_out)
    {
        __try {
            auto* col = RE::GameSettingCollection::GetSingleton();
            if (!col) return false;
            auto* s = col->GetSetting(a_name);
            if (!s || s->GetType() != RE::Setting::Type::kFloat) return false;
            a_out = s->GetFloat();
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SafeSetFloat(const char* a_name, float a_value)
    {
        __try {
            auto* col = RE::GameSettingCollection::GetSingleton();
            if (!col) return false;
            auto* s = col->GetSetting(a_name);
            if (!s || s->GetType() != RE::Setting::Type::kFloat) return false;
            s->SetFloat(a_value);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void Apply()
    {
        int done = 0;
        for (std::size_t i = 0; i < kTameSettings.size(); ++i) {
            float orig = 0.0f;
            g_savedOk[i] = SafeGetFloat(kTameSettings[i], orig);
            if (g_savedOk[i]) {
                g_saved[i] = orig;
                if (SafeSetFloat(kTameSettings[i], 0.0f)) ++done;
            }
        }
        g_applied = true;
        PLOG_INFO("[COMBATTAME] co-op combat AI tamed: {}/{} GMSTs zeroed (circle/dodge/avoid/adjust/flank off)"sv,
                  done, static_cast<int>(kTameSettings.size()));
    }

    static void Restore()
    {
        int done = 0;
        for (std::size_t i = 0; i < kTameSettings.size(); ++i) {
            if (g_savedOk[i] && SafeSetFloat(kTameSettings[i], g_saved[i])) ++done;
            g_savedOk[i] = false;
        }
        g_applied = false;
        PLOG_INFO("[COMBATTAME] co-op combat AI restored: {}/{} GMSTs reverted"sv,
                  done, static_cast<int>(kTameSettings.size()));
    }

    void Tick()
    {
        const bool want = Pulsar::Network::GetSingleton().IsConnected()
                          && Pulsar::GetConfig().coopTameCombatAI;
        if (want && !g_applied)       Apply();
        else if (!want && g_applied)  Restore();
    }
}
