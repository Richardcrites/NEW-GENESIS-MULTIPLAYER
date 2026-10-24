#include "PCH.h"
#include "HookGuard.h"

#include <windows.h>

using namespace std::string_view_literals;

namespace Pulsar::HookGuard {
    std::atomic<bool>          g_safe{ true };
    std::atomic<std::uint64_t> g_suspendedAtTick{ 0 };

    void Suspend() noexcept {
        auto t = ::GetTickCount64();
        g_suspendedAtTick.store(t, std::memory_order_release);
        g_safe.store(false, std::memory_order_release);
        PLOG_INFO("[PULSAR] HookGuard: SUSPENDED @ tick {}"sv, t);
    }
}
