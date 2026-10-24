#include "PCH.h"
#include "TimeWeatherSync/TimeWeatherSync.h"

#include "Config.h"
#include "Cell3DAttach.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/PluginLookup.h"

#include <atomic>
#include <mutex>
#include <windows.h>

using namespace std::string_view_literals;

// one-shot co-op time + weather sync. RE in reference_time_weather_sync_re.md.
//
// Host-authoritative: the client requests once on join, the host replies with its
// current game time + weather, the client applies it once. Engine state is reached
// raw via module-base + verified RVAs (CommonLibSF's Calendar offsets are wrong for
// 1.16.236 - see RE note), all wrapped in SEH.
namespace Pulsar::TimeWeatherSync
{
    namespace
    {
        // Verified 1.16.236 offsets (Ghidra VA - image base 0x140000000 = RVA).
        constexpr std::uintptr_t kCalendarPtr  = 0x5FDCDF8;  // Calendar* lives here (DAT_145fe4b68)
        constexpr std::uintptr_t kSkyPtr       = 0x61DF6F0;  // Sky*       lives here (DAT_1461e7478)
        constexpr std::uintptr_t kSkyCtrlPtr   = 0x61DF748;  // weather-command controller ptr (ForceActive arg0)
        constexpr std::uintptr_t kForceWeather = 0x18C3BE0;  // FUN_1418c63f0(ctrl, weatherForm, bool override)

        constexpr std::uintptr_t kOffHourGlobal = 0x28;   // Calendar + 0x28 = gameHour      TESGlobal*
        constexpr std::uintptr_t kOffDaysGlobal = 0x30;   // Calendar + 0x30 = gameDaysPassed TESGlobal*
        constexpr std::uintptr_t kOffGlobalVal  = 0x48;   // TESGlobal + 0x48 = float value
        constexpr std::uintptr_t kOffCurWeather = 0x118;  // Sky + 0x118 = current weather (BGSWeatherSettingsForm*)

        std::uintptr_t Base()
        {
            static std::uintptr_t base =
                reinterpret_cast<std::uintptr_t>(GetModuleHandleA("Starfield.exe"));
            return base;
        }

        // ---- raw engine access (no destructable locals -> SEH-safe) ----
        bool ReadTime(float& a_days, float& a_hour) noexcept
        {
            __try {
                const std::uintptr_t cal = *reinterpret_cast<std::uintptr_t*>(Base() + kCalendarPtr);
                if (!cal) return false;
                const std::uintptr_t hourG = *reinterpret_cast<std::uintptr_t*>(cal + kOffHourGlobal);
                const std::uintptr_t daysG = *reinterpret_cast<std::uintptr_t*>(cal + kOffDaysGlobal);
                if (!hourG || !daysG) return false;
                a_hour = *reinterpret_cast<float*>(hourG + kOffGlobalVal);
                a_days = *reinterpret_cast<float*>(daysG + kOffGlobalVal);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void WriteTime(float a_days, float a_hour) noexcept
        {
            __try {
                const std::uintptr_t cal = *reinterpret_cast<std::uintptr_t*>(Base() + kCalendarPtr);
                if (!cal) return;
                const std::uintptr_t hourG = *reinterpret_cast<std::uintptr_t*>(cal + kOffHourGlobal);
                const std::uintptr_t daysG = *reinterpret_cast<std::uintptr_t*>(cal + kOffDaysGlobal);
                if (daysG) *reinterpret_cast<float*>(daysG + kOffGlobalVal) = a_days;
                if (hourG) *reinterpret_cast<float*>(hourG + kOffGlobalVal) = a_hour;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // Returns the FormID of the current weather (BGSWeatherSettingsForm*) or 0.
        std::uint32_t ReadCurrentWeatherFormID() noexcept
        {
            __try {
                const std::uintptr_t sky = *reinterpret_cast<std::uintptr_t*>(Base() + kSkyPtr);
                if (!sky) return 0;
                const std::uintptr_t w = *reinterpret_cast<std::uintptr_t*>(sky + kOffCurWeather);
                if (!w) return 0;
                return reinterpret_cast<RE::TESForm*>(w)->GetFormID();
            } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }

        // Force a weather form active (the exact path Weather.ForceActive uses).
        void ForceWeather(std::uintptr_t a_weatherForm) noexcept
        {
            if (!a_weatherForm) return;
            __try {
                const std::uintptr_t ctrl = *reinterpret_cast<std::uintptr_t*>(Base() + kSkyCtrlPtr);
                if (!ctrl) return;
                using Fn = void(__fastcall*)(std::uintptr_t, std::uintptr_t, bool);
                reinterpret_cast<Fn>(Base() + kForceWeather)(ctrl, a_weatherForm, true);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // ---- state ----
        std::atomic<bool>       s_respondPending{ false };  // host: a request arrived
        std::atomic<bool>       s_applied{ false };          // client: already synced this session
        std::mutex              s_mtx;
        Network::TimeWeatherData s_snap{};                   // client: last received snapshot
        bool                    s_applyPending = false;      // client: snapshot waiting to apply
        std::uint64_t           s_lastReqMs = 0;             // client: request rate-limit

        bool InWorld() noexcept
        {
            auto* plr = RE::PlayerCharacter::GetSingleton();
            return plr && plr->parentCell;
        }

        void SendRequest()
        {
            std::uint8_t b = static_cast<std::uint8_t>(Network::PKT_TIME_WEATHER_REQ);
            Network::GetSingleton().Send(&b, 1, true);
        }

        void SendSnapshot()
        {
            Network::TimeWeatherData d{};
            if (!ReadTime(d.gameDaysPassed, d.gameHour)) return;  // engine not ready yet

            const std::uint32_t fid = ReadCurrentWeatherFormID();
            if (fid) {
                d.weatherBase    = fid;
                d.weatherLocalID = NPCSync::PluginLookup::FormIDToLocalID(fid);
                NPCSync::PluginLookup::FormIDToPluginName(fid, d.weatherPlugin, sizeof(d.weatherPlugin));
                d.hasWeather = 1;
            }

            std::uint8_t buf[1 + sizeof(Network::TimeWeatherData)];
            buf[0] = static_cast<std::uint8_t>(Network::PKT_TIME_WEATHER);
            std::memcpy(buf + 1, &d, sizeof(d));
            Network::GetSingleton().Send(buf, sizeof(buf), true);
            PLOG_DIAG("[PULSAR] co-op: sent time+weather snapshot (hasWeather={})"sv, d.hasWeather);
        }

        void ApplySnapshot(const Network::TimeWeatherData& d)
        {
            WriteTime(d.gameDaysPassed, d.gameHour);

            if (d.hasWeather) {
                std::uint32_t resolved = 0;
                if (d.weatherPlugin[0] != '\0')
                    resolved = NPCSync::PluginLookup::ResolveCrossPlugin(d.weatherPlugin, d.weatherLocalID);
                if (!resolved) resolved = d.weatherBase;
                if (resolved) {
                    auto* form = RE::TESForm::LookupByID(resolved);
                    if (form) ForceWeather(reinterpret_cast<std::uintptr_t>(form));
                }
            }
            PLOG_DIAG("[PULSAR] co-op: applied host time+weather"sv);
        }
    }

    void OnRequestReceived()
    {
        s_respondPending.store(true, std::memory_order_release);
    }

    void OnSnapshotReceived(const std::uint8_t* data, std::size_t length)
    {
        if (!data || length < 1 + sizeof(Network::TimeWeatherData)) return;
        std::lock_guard<std::mutex> lk(s_mtx);
        std::memcpy(&s_snap, data + 1, sizeof(s_snap));
        s_snap.weatherPlugin[sizeof(s_snap.weatherPlugin) - 1] = '\0';
        s_applyPending = true;
    }

    // Re-arm the client sync (called after each item-restore = each bridge into the host's
    // world). Clears the one-shot flag so Tick re-requests + re-applies the host's CURRENT
    // time/weather. Covers planet landings, where weather is regenerated per instance.
    void RequestResync()
    {
        s_applied.store(false, std::memory_order_release);
        s_lastReqMs = 0;   // request on the next tick, no 3s wait
        PLOG_INFO("[PULSAR] co-op: time+weather re-sync armed (post-restore)"sv);
    }

    void Tick()
    {
        if (!Pulsar::GetConfig().coopTimeWeatherSync) return;

        auto& net = Network::GetSingleton();
        if (!net.IsConnected()) {
            // reset so a fresh connect re-syncs
            s_applied.store(false, std::memory_order_release);
            return;
        }
        // never touch time/weather mid cell-stream / load (same guard as)
        if (Pulsar::Cell3DAttach::IsQuiet() || !InWorld()) return;

        if (Pulsar::GetConfig().isHost) {
            if (s_respondPending.exchange(false, std::memory_order_acq_rel))
                SendSnapshot();
            return;
        }

        // client
        bool doApply = false;
        Network::TimeWeatherData snap{};
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            if (s_applyPending) { snap = s_snap; s_applyPending = false; doApply = true; }
        }
        if (doApply) {
            ApplySnapshot(snap);
            s_applied.store(true, std::memory_order_release);
        }

        if (!s_applied.load(std::memory_order_acquire)) {
            const std::uint64_t now = ::GetTickCount64();
            if (now - s_lastReqMs > 3000) {  // request once, retry every 3s until applied
                s_lastReqMs = now;
                SendRequest();
            }
        }
    }
}
