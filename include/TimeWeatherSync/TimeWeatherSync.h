#pragma once

#include <cstddef>
#include <cstdint>

// one-shot co-op time + weather sync (host-authoritative).
// On join the CLIENT requests the host's current game time-of-day + weather once;
// the host replies with a snapshot; the client applies it once so both machines see
// the same time and weather. After the one-shot, both clocks advance at the same
// timeScale. RE: reference_time_weather_sync_re.md.
namespace Pulsar::TimeWeatherSync
{
    // Game-thread tick (called from Main.cpp permanent task). Drives the client
    // request/apply and the host response. Cheap; gated to co-op + in-world.
    void Tick();

    // Network-thread callbacks (dispatched from NetworkHost).
    void OnRequestReceived();                                     // PKT_TIME_WEATHER_REQ (host)
    void OnSnapshotReceived(const std::uint8_t* data, std::size_t length);  // PKT_TIME_WEATHER (client)

    // Re-arm the client sync so it pulls + re-applies the host's CURRENT time/weather
    // again. Called after each item-restore (each bridge into the host's world) so the
    // weather matches after planet landings, where weather is regenerated per instance.
    void RequestResync();
}
