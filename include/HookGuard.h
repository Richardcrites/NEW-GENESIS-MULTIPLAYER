#pragma once

#include <atomic>

namespace Pulsar::HookGuard {

    // Global "hooks safe to run" flag. When false, every PULSAR hook should
    // forward to the original engine function and bail out without doing any
    // PULSAR-side work - preventing UAF on stale Actor*/REFR* pointers during
    // engine save-load and cell-streaming chaos.
    //
    // Set to false on PKT_SAVE_TRANSFER receive (client) or before any other
    // operation that invalidates engine actor pointers. Set back to true via
    // a settle-window check in Main.cpp's tick (5s after WorldSync LOADED +
    // loadMenu closed).
    //
    // Usage in a hook:
    //   void __fastcall HookedX(SomeArgs...) {
    //       if (!Pulsar::HookGuard::IsSafe()) return s_origX(SomeArgs...);
    //       // ... pulsar-specific work ...
    //       return s_origX(SomeArgs...);
    //   }
    extern std::atomic<bool>          g_safe;
    extern std::atomic<std::uint64_t> g_suspendedAtTick;  // GetTickCount64()

    inline bool IsSafe() noexcept {
        return g_safe.load(std::memory_order_acquire);
    }

    // Suspend hooks NOW. Records the tick so the auto-resume check in
    // Main.cpp can verify that a NEW LOADED transition has happened
    // AFTER this point (not just an old one from connect-time).
    void Suspend() noexcept;

    // Resume hooks. Caller must ensure all preconditions met:
    // WorldSync::GetLastLoadCompletedTick() > suspendedAtTick
    // !loadMenu open
    // settle window passed
    inline void Resume() noexcept {
        g_safe.store(true, std::memory_order_release);
    }

    // Returns the tick when Suspend was last called. 0 = never suspended.
    inline std::uint64_t GetSuspendedAtTick() noexcept {
        return g_suspendedAtTick.load(std::memory_order_acquire);
    }

}  // namespace Pulsar::HookGuard
