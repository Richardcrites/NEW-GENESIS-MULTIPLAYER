#pragma once

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Pulsar::Engine::ProcGenGate
{
    // Starfield.exe 1.16.236 - BGSPlanet::ProcGenRuntimeService streaming state.
    // Reversed via Ghidra (T3). `DAT_14590b180` block holds the proc-gen tile/
    // chunk streaming phase. Non-blocking reads are safe from SFSE threads;
    // the engine writes this with LOCK/WakeByAddress, plain volatile read is
    // consistent enough for a skip/retry decision.
    constexpr std::uintptr_t kProcGenChunkKeyRVA  = 0x5903184;  // u32 current chunk
    constexpr std::uintptr_t kProcGenStateLockRVA = 0x5903198;  // u32 owner TID
    constexpr std::uintptr_t kProcGenStateFlagRVA = 0x590319C;  // u32 packed phase
    constexpr std::uint32_t  kProcGenPhaseMask    = 0xFFC00000u;
    constexpr std::uint32_t  kProcGenPhaseActive  = 0x00400000u;

    inline std::uintptr_t ModuleBase()
    {
        static const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        return base;
    }

    inline bool IsProcGenBusy()
    {
        const auto v = *reinterpret_cast<volatile std::uint32_t*>(
            ModuleBase() + kProcGenStateFlagRVA);
        return (v & kProcGenPhaseMask) == kProcGenPhaseActive;
    }

    inline bool IsProcGenLockHeldByOther()
    {
        const auto owner = *reinterpret_cast<volatile std::uint32_t*>(
            ModuleBase() + kProcGenStateLockRVA);
        return owner != 0 && owner != GetCurrentThreadId();
    }

    inline std::uint32_t CurrentChunkKey()
    {
        return *reinterpret_cast<volatile std::uint32_t*>(
            ModuleBase() + kProcGenChunkKeyRVA);
    }

    // Cheap pre-write gate. True = safe frame, false = defer.
    inline bool SafeToMutateActor()
    {
        return !IsProcGenBusy() && !IsProcGenLockHeldByOther();
    }
}
