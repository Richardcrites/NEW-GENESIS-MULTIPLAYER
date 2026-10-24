#include "PCH.h"
#include "Cell3DAttach.h"

#include "RemotePlayer/RemotePlayerManager.h"
#include "WorldSync/WorldSync.h"
#include "NASLog.h"

#include <windows.h>
#include <REL/Relocation.h>
#include <RE/A/Actor.h>
#include <RE/B/BSTEvent.h>

#include <atomic>
#include <cstdint>

using namespace std::string_view_literals;

namespace Pulsar::Cell3DAttach {
namespace {

    // ---------------------------------------------------------------------
    // Engine RVAs (verified via Ghidra 2026-04-27, Starfield 1.16.236)
    //
    //   Singleton: BSTEventSource<TESCellReference3DAttachEvent> @ 0x145977A40
    //              (data, in image .rdata; lazy-init guard at 0x146219FCC).
    //
    //   AttachBiped (FUN_140B24E60), reachable via Actor's primary vtable:
    //     re-attaches inv slots 0x29/0x34/0x38/0x4C/0x4E/0x5A, calls
    //     FUN_1405E2CF0 to build 3D, then post-attach via FUN_140B37390.
    //
    //   Slot in Actor::vftable @ 0x144CB9248 (clone's primary vtable):
    //     CORRECTED 2026-04-29 - original RE had slot 0x95 wrong; verified
    //     via Ghidra script RE_FindAttachBipedSlot.java (scan for ptr to
    //     FUN_140B24E60 in all Actor-derived vtables). The clone's primary
    //     vtable (0x144CB9248) has AttachBiped at slot 0x13, not 0x95.
    //     Slot 0x95 in that vtable points to FUN_141961880 (a formType-
    //     dispatched equip/something function - calling it on a clone with
    //     empty NiNode silently no-ops, which is why Phase H was inert).
    //
    //   See:
    //     local RE notes   (sink RE)
    //     local RE notes      (vt[0x95] decompile)
    //     local RE notes (slot recheck)
    //     memory/reference_cell_transition_re.md      (consolidated)
    // ---------------------------------------------------------------------
    constexpr std::uintptr_t kRVA_3DAttachEventSrc = 0x5977A40;
    constexpr std::size_t    kVTAttachBipedSlot   = 0x13;
    constexpr std::uintptr_t kExpectedAttachBipedRVA = 0xB24710;

    // Settle window for fallback: how long to wait after WorldSync LOADED
    // before manually calling vt[0x95] on a clone that still has empty
    // NiNode. Long enough for the engine's own attach pipeline to fire,
    // short enough to feel responsive.
    constexpr std::uint64_t kFallbackDelayMs = 2000;

    // Post-attach grace period - don't probe again for this long after
    // we've fired a manual attach (success OR failure).
    constexpr std::uint64_t kManualAttachCooldownMs = 5000;

    // Atomic state. The sink only writes/observes these; main work is
    // driven from Tick() on the game thread.
    std::atomic<bool>          g_sinkFired{ false };
    std::atomic<std::uint32_t> g_lastSinkRefID{ 0 };
    std::atomic<std::uint32_t> g_sinkFireCount{ 0 };
    std::atomic<bool>          g_initialised{ false };

    // Phase I - Position-sync quiet window state.
    std::atomic<bool>          g_inLoadMenu{ false };
    std::atomic<std::uint64_t> g_lastLoadMenuCloseTick{ 0 };
    // 2026-06-11: 1500 → 3500. The post-load anim-graph rebuild takes longer
    // than 1.5s; anim-var reads/sends during that window pure-virtual-crash on
    // the half-built graph (both-down reload, client crash 22:43 +0x377E90).
    constexpr std::uint64_t    kQuietPostCloseMs = 3500;

    // Per-cycle state - guarded by tick (single-thread).
    struct CycleState {
        std::uint64_t cellChangeTick = 0;       // when player cell changed
        std::uint64_t lastManualAttachTick = 0; // last fallback fire
        bool          fallbackArmed = false;    // armed at cell-change, disarmed on success/timeout
    };
    static CycleState s_state;

    // ---------------------------------------------------------------------
    // SEH-wrapped helpers (C2712 forbids __try in functions with C++
    // unwinding objects; isolate raw memory reads in noexcept helpers).

    // Read clone REFR formID + data3D pointer + child count without
    // tripping any C++ destructors inside the __try.
    struct CloneState {
        std::uint32_t refID;
        std::uintptr_t data3D;
        std::uint32_t  childCount;
        bool ok;
    };
    CloneState SafeProbeClone(RE::Actor* clone) noexcept
    {
        CloneState s{};
        if (!clone) return s;
        __try {
            auto* p = reinterpret_cast<std::uint8_t*>(clone);
            s.refID = *reinterpret_cast<std::uint32_t*>(p + 0x28);
            // loadedData lives at the BSGuarded slot; we read the raw
            // pointer at +0xB8 (verified via existing CellDiag probe).
            std::uintptr_t loadedData = *reinterpret_cast<std::uintptr_t*>(p + 0xB8);
            if (!loadedData) { s.ok = true; return s; }
            s.data3D = *reinterpret_cast<std::uintptr_t*>(loadedData + 0x08);
            if (!s.data3D) { s.ok = true; return s; }
            // NiNode children array @ +0x130 (NiTArray<NiPointer<NiAVObject>>):
            //   +0x00 dataPtr, +0x08 capacity, +0x0C size
            s.childCount = *reinterpret_cast<std::uint32_t*>(s.data3D + 0x130 + 0x0C);
            s.ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s.ok = false;
        }
        return s;
    }

    // Resolve clone->vt[0x95] and verify it points at the expected
    // AttachBiped (sanity check vs vtable layout drift). Returns nullptr
    // if mismatch.
    using AttachBipedFn = void* (__fastcall*)(void*, void*, void*);
    AttachBipedFn SafeResolveAttachBiped(RE::Actor* clone,
                                          std::uintptr_t& outRaw) noexcept
    {
        outRaw = 0;
        if (!clone) return nullptr;
        __try {
            auto* vtable = *reinterpret_cast<void* const* const*>(clone);
            if (!vtable) return nullptr;
            outRaw = reinterpret_cast<std::uintptr_t>(vtable[kVTAttachBipedSlot]);
            return reinterpret_cast<AttachBipedFn>(vtable[kVTAttachBipedSlot]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // SEH-wrapped invocation of vt[0x95]. Engine function may dereference
    // various clone-internals; SEH catches anything that misbehaves.
    bool SafeCallAttachBiped(AttachBipedFn fn, RE::Actor* clone) noexcept
    {
        if (!fn || !clone) return false;
        __try {
            fn(clone, nullptr, nullptr);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ---------------------------------------------------------------------
    // The event payload - opaque to us. From the RE pass:
    //   +0x00  ObjectAttachmentInstance::vftable
    //   +0x08  attachKeyA  (uint)
    //   +0x0C  attachKeyB  (uint, from DAT_146202d18)
    //   +0x10  ptrA        (NiPointer-like)  ← one of these is the REFR
    //   +0x18  ptrB        (NiPointer-like)
    //   +0x20  byte flag
    //
    // We don't know if ptrA or ptrB is the REFR until we observe it live.
    // The sink logs the formID read from each candidate offset for the
    // first few events; once we've matched our clone's formID we can
    // narrow to the correct slot. For now: read both and compare.
    struct OpaqueRef3DAttachEvent {
        const void* vftable;
        std::uint32_t attachKeyA;
        std::uint32_t attachKeyB;
        const void*  ptrA;
        const void*  ptrB;
        std::uint8_t flag;
    };
    static_assert(sizeof(OpaqueRef3DAttachEvent) <= 0x28,
                  "OpaqueRef3DAttachEvent layout drifted");

    // Try to read a TESForm* candidate's formID at +0x28. Returns 0 on
    // failure / null input.
    std::uint32_t SafeReadFormID(const void* candidate) noexcept
    {
        if (!candidate) return 0;
        __try {
            return *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(candidate) + 0x28);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Sink subclass over an opaque event. Since we can't include a header
    // for TESCellReference3DAttachEvent (CommonLibSF doesn't define the
    // struct), template the sink over our opaque type - vtable layout of
    // BSTEventSink is the same regardless of T (single virtual func +
    // dtor).
    class Ref3DAttachSink : public RE::BSTEventSink<OpaqueRef3DAttachEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const OpaqueRef3DAttachEvent& e,
            RE::BSTEventSource<OpaqueRef3DAttachEvent>* /*src*/) override
        {
            std::uint32_t fired = g_sinkFireCount.fetch_add(1) + 1;

            std::uint32_t fidA = SafeReadFormID(e.ptrA);
            std::uint32_t fidB = SafeReadFormID(e.ptrB);

            // Cheap diagnostic for the first 30 events so we can confirm
            // payload layout in the live log.
            if (fired <= 30) {
                PLOG_INFO(
                    "[PULSAR] Cell3DAttach: SINK fire#{} keys=({:#x},{:#x}) "
                    "ptrA={:p} fidA=0x{:08X} ptrB={:p} fidB=0x{:08X} flag={}"sv,
                    fired, e.attachKeyA, e.attachKeyB,
                    e.ptrA, fidA, e.ptrB, fidB, e.flag);
            }

            // Match against current clone - if either candidate's formID
            // equals our clone, we know 3D became ready for it.
            std::uint32_t cloneRef = 0;
            auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
            if (clone) {
                CloneState cs = SafeProbeClone(clone);
                if (cs.ok) cloneRef = cs.refID;
            }

            if (cloneRef && (fidA == cloneRef || fidB == cloneRef)) {
                g_sinkFired.store(true, std::memory_order_release);
                g_lastSinkRefID.store(cloneRef, std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] Cell3DAttach: SINK matched clone refID=0x{:08X} "
                    "(slot={}) - 3D is now READY"sv,
                    cloneRef, (fidA == cloneRef) ? "A" : "B");
                NASLOG("Cell3DAttach: sink matched clone 0x%08X", cloneRef);
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    static Ref3DAttachSink s_sink;

}  // namespace

// =====================================================================
// Public API
// =====================================================================

void Init()
{
    if (g_initialised.exchange(true)) {
        PLOG_WARN("[PULSAR] Cell3DAttach: Init called twice - ignoring"sv);
        return;
    }

    // The singleton at 0x145977A40 is lazy-initialised inside the sender
    // (FUN_14046B770) - by the time we register a sink at kPostLoad, the
    // singleton may NOT yet have its vftable patched in. Reading from it
    // before any 3D-attach event has fired returns garbage. We treat the
    // address as a BSTEventSource regardless; RegisterSink only writes
    // to the sink-list pointers (+0x08..) which are zero-initialised in
    // the .rdata section, so it's safe even pre-init.
    void* singletonAddr = reinterpret_cast<void*>(
        REL::Offset{ kRVA_3DAttachEventSrc }.address());

    PLOG_INFO(
        "[PULSAR] Cell3DAttach: Init - sink registering on EventSource @ {:p}"sv,
        singletonAddr);

    auto* src = reinterpret_cast<RE::BSTEventSource<OpaqueRef3DAttachEvent>*>(singletonAddr);
    __try {
        src->RegisterSink(&s_sink);
        PLOG_INFO("[PULSAR] Cell3DAttach: sink registered"sv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PLOG_WARN(
            "[PULSAR] Cell3DAttach: RegisterSink threw - singleton may not be "
            "initialised yet; will retry on first Tick"sv);
        // Mark as not-yet-registered by clearing init flag so next Tick
        // can retry. But we don't have a separate retry flag; for now
        // accept this as a soft warning and rely on the fallback path.
    }
}

void OnPlayerCellChange()
{
    s_state.cellChangeTick = ::GetTickCount64();
    s_state.fallbackArmed = true;
    g_sinkFired.store(false, std::memory_order_release);
    g_lastSinkRefID.store(0, std::memory_order_release);
}

bool DidSinkFireSinceLastTransition()
{
    return g_sinkFired.load(std::memory_order_acquire);
}

void ManualAttachBiped(RE::Actor* clone)
{
    if (!clone) return;

    // Probe the clone first - we need parentCell + loadedData non-null
    // for vt[0x95] to succeed (per RE preconditions).
    CloneState cs = SafeProbeClone(clone);
    if (!cs.ok || cs.refID == 0) {
        PLOG_WARN("[PULSAR] Cell3DAttach: ManualAttach probe failed - skip"sv);
        return;
    }

    // Resolve + sanity-check the vtable entry.
    std::uintptr_t fnRaw = 0;
    AttachBipedFn fn = SafeResolveAttachBiped(clone, fnRaw);
    if (!fn) {
        PLOG_WARN("[PULSAR] Cell3DAttach: ManualAttach vt[0x95] resolve failed"sv);
        return;
    }

    // Compute expected absolute VA from RVA + image base.
    std::uintptr_t expected = REL::Offset{ kExpectedAttachBipedRVA }.address();
    if (fnRaw != expected) {
        // Vtable layout drifted vs RE pass - log loud and bail. We do
        // NOT call something other than what we verified is AttachBiped.
        PLOG_WARN(
            "[PULSAR] Cell3DAttach: ManualAttach vt[0x95]={:#x} != expected "
            "{:#x} - layout drift, bailing"sv,
            fnRaw, expected);
        return;
    }

    PLOG_INFO(
        "[PULSAR] Cell3DAttach: ManualAttach FIRING refID=0x{:08X} children={} "
        "data3D={:#x}"sv,
        cs.refID, cs.childCount, cs.data3D);
    NASLOG("Cell3DAttach: ManualAttach fire 0x%08X children=%u",
           cs.refID, cs.childCount);

    bool ok = SafeCallAttachBiped(fn, clone);

    // Re-probe to see what happened.
    CloneState after = SafeProbeClone(clone);
    PLOG_INFO(
        "[PULSAR] Cell3DAttach: ManualAttach result ok={} children {}->{} "
        "data3D {:#x}->{:#x}"sv,
        ok, cs.childCount, after.ok ? after.childCount : 0u,
        cs.data3D, after.ok ? after.data3D : 0u);
    NASLOG("Cell3DAttach: ManualAttach result ok=%d children=%u",
           ok ? 1 : 0, after.ok ? after.childCount : 0u);
}

void SetLoadMenuState(bool a_inLoadMenu)
{
    bool prev = g_inLoadMenu.exchange(a_inLoadMenu, std::memory_order_acq_rel);
    if (prev && !a_inLoadMenu) {
        g_lastLoadMenuCloseTick.store(::GetTickCount64(),
            std::memory_order_release);
    }
}

bool WasInLoadMenu()
{
    return g_inLoadMenu.load(std::memory_order_acquire);
}

bool IsQuiet()
{
    if (g_inLoadMenu.load(std::memory_order_acquire)) return true;
    auto last = g_lastLoadMenuCloseTick.load(std::memory_order_acquire);
    if (last == 0) return false;
    return (::GetTickCount64() - last) < kQuietPostCloseMs;
}

void Tick(bool /*isHost*/)
{
    // Symmetric on host + client - each side has a clone representing
    // the other player. The fallback fires only if the sink hasn't
    // matched our clone within the settle window.
    if (!s_state.fallbackArmed) return;

    // Crash guard 2026-04-27: NEVER touch the clone pointer while loadMenu
    // is open or right after - engine may be tearing down/rebuilding the
    // actor and m_remoteActor can be a dangling pointer. Probing it then
    // crashed client during fast-travel (verified 22:59:26 log + crash).
    if (IsQuiet() || WasInLoadMenu()) return;

    // Need a clone to fix.
    auto* clone = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
    if (!clone) return;

    std::uint64_t now = ::GetTickCount64();

    // Cooldown so a failed manual attach doesn't fire every tick.
    if (now - s_state.lastManualAttachTick < kManualAttachCooldownMs) return;

    // Sink already reported attach for this clone? Disarm and exit.
    if (DidSinkFireSinceLastTransition()) {
        s_state.fallbackArmed = false;
        return;
    }

    // Wait for the settle window to elapse since the cell-change.
    if (s_state.cellChangeTick == 0) return;
    if (now - s_state.cellChangeTick < kFallbackDelayMs) return;

    // Probe - only fire fallback if data3D is actually empty.
    CloneState cs = SafeProbeClone(clone);
    if (!cs.ok || cs.refID == 0) return;
    if (cs.data3D != 0 && cs.childCount > 0) {
        // 3D is healthy. Disarm fallback.
        s_state.fallbackArmed = false;
        return;
    }

    // Empty NiNode - fire the manual attach. Single shot per cycle.
    PLOG_INFO(
        "[PULSAR] Cell3DAttach: empty-NiNode detected after {}ms (children={}); "
        "firing manual attach"sv,
        now - s_state.cellChangeTick, cs.childCount);

    ManualAttachBiped(clone);
    s_state.lastManualAttachTick = now;
    s_state.fallbackArmed = false;  // single attempt - no retry loop
}

}  // namespace Pulsar::Cell3DAttach
