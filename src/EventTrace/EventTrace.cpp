#include "PCH.h"
#include "EventTrace/EventTrace.h"

#include "Config.h"
#include "Network/NetworkHost.h"
#include "NASLog.h"

#include <RE/E/Events.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

using namespace std::string_view_literals;

namespace Pulsar::EventTrace {
namespace {

    static std::atomic<bool> s_initDone{ false };
    static std::atomic<bool> s_enabled{ true };

    static const char* RoleTag() noexcept
    {
        // IsHost() reflects the config role even before a peer connects, so the
        // tag is correct from the first event. Cheap, lock-free read.
        return Pulsar::Network::GetSingleton().IsHost() ? "HOST" : "CLIENT";
    }

    // SEH-only resolver. Writes a readable identifier for a form into `out`.
    // No C++ unwinding objects here (C2712), so the caller wraps the result in
    // a std::string. Resolves: EditorID + numeric FormType. For actor refs we
    // also fall back to the BASE NPC's EditorID (placed actor refs usually have
    // an empty ref EditorID but the base TESNPC carries the name).
    static void SafeResolve(std::uint32_t formID, char* out, std::size_t cap) noexcept
    {
        if (cap == 0) return;
        out[0] = '\0';
        if (formID == 0) { _snprintf_s(out, cap, _TRUNCATE, "0x0"); return; }
        __try {
            auto* form = RE::TESForm::LookupByID(formID);
            if (!form) {
                _snprintf_s(out, cap, _TRUNCATE, "0x%08X(notloaded)", formID);
                return;
            }
            const int ft = static_cast<int>(form->GetFormType());
            const char* eid = form->GetFormEditorID();
            // Placed actor: prefer the base NPC EditorID when the ref has none.
            // OBJ_REFR.objectReference lives at ref+0x98 (data@0x80 + 0x18).
            // NiPointer is layout-compatible with a raw pointer, so read it raw
            // constructing a NiPointer temporary (refcounted) inside __try is
            // a C2712 error.
            if ((!eid || !eid[0]) && form->IsActor()) {
                auto* base = *reinterpret_cast<RE::TESForm**>(
                    reinterpret_cast<std::uint8_t*>(form) + 0x98);
                if (base) {
                    const char* beid = base->GetFormEditorID();
                    if (beid && beid[0]) {
                        _snprintf_s(out, cap, _TRUNCATE, "%s(0x%08X)[t%d]", beid, formID, ft);
                        return;
                    }
                }
            }
            if (eid && eid[0]) {
                _snprintf_s(out, cap, _TRUNCATE, "%s(0x%08X)[t%d]", eid, formID, ft);
            } else {
                _snprintf_s(out, cap, _TRUNCATE, "0x%08X[t%d]", formID, ft);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(out, cap, _TRUNCATE, "0x%08X(seh)", formID);
        }
    }

    // Raw +0x28 formID read (verified universal offset, see
    // memory/reference_formid_offset.md). Avoids vtable calls on a pointer
    // pulled from an event payload that may be mid-mutation.
    static std::uint32_t SafeRawFormID(const void* obj) noexcept
    {
        if (!obj) return 0;
        __try {
            return *reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(obj) + 0x28);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Raw parentCell formID of a TESObjectREFR (parentCell @ refr+0xB0,
    // verified offset; formID @ cell+0x28). SEH-safe. 0 if no cell.
    static std::uint32_t SafeRefCellID(const void* refr) noexcept
    {
        if (!refr) return 0;
        __try {
            auto* cell = *reinterpret_cast<void* const*>(
                static_cast<const std::uint8_t*>(refr) + 0xB0);
            if (!cell) return 0;
            return *reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(cell) + 0x28);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // The local player's current parentCell formID, or 0 when not in a cell
    // (main menu / mid load). Used to filter the spawn/activation firehose down
    // to events in the player's OWN cell (= the shared-area events that matter
    // for co-op sync; load-time world prewarm + distant ambient are dropped).
    static std::uint32_t SafePlayerCellID() noexcept
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        return p ? SafeRefCellID(p) : 0;
    }

    // --- Sinks -----------------------------------------------------------

    // NPC spawn / cell presence. ActorCellChangeEvent fires when an actor's
    // parentCell changes: null->cell on attach (spawn / load into a cell),
    // cell->cell on a MoveTo, cell->null on detach. This is the "an NPC
    // appeared near me" signal the sync work needs and it carries the actor +
    // both cells directly.
    //
    // We use this INSTEAD of TESObjectLoadedEvent because CommonLibSF ships
    // TESObjectLoadedEvent::GetEventSource (and TESCellFullyLoadedEvent) as a
    // REL::ID(0) placeholder - calling it is a fatal "Invalid ID: 0" that SEH
    // cannot catch. ActorCellChangeEvent has a REAL id (64106 -> RVA 0xB98F20,
    // verified in the 1.16.244 versionlib + Ghidra disasm: canonical singleton
    // getter), so the address library resolves it correctly and portably.
    struct ActorCellChangeSink
        : public RE::BSTEventSink<RE::ActorCellChangeEvent::Event>
    {
        RE::BSEventNotifyControl ProcessEvent(
            const RE::ActorCellChangeEvent::Event& e,
            RE::BSTEventSource<RE::ActorCellChangeEvent::Event>*) override
        {
            if (!s_enabled.load(std::memory_order_acquire))
                return RE::BSEventNotifyControl::kContinue;

            std::uint32_t actorFID = 0, cellAID = 0, cellBID = 0;
            __try {
                actorFID = SafeRawFormID(e.actor.get());
                cellAID  = SafeRawFormID(e.cellA);
                cellBID  = SafeRawFormID(e.cellB);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!actorFID) return RE::BSEventNotifyControl::kContinue;

            // Player-cell filter: only actors entering/leaving the player's own
            // cell. Cuts the load-time world-prewarm + distant-ambient firehose.
            const std::uint32_t pc = SafePlayerCellID();
            if (pc == 0 || (cellAID != pc && cellBID != pc))
                return RE::BSEventNotifyControl::kContinue;

            const char* dir = (cellAID == 0) ? "ENTER"
                            : (cellBID == 0) ? "leave" : "move";
            char an[192], ca[192], cb[192];
            NameOfBuf(actorFID, an, sizeof(an));
            NameOfBuf(cellAID,  ca, sizeof(ca));
            NameOfBuf(cellBID,  cb, sizeof(cb));
            Trace("SPAWN", "%s actor=%s cell %s -> %s", dir, an, ca, cb);
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static ActorCellChangeSink g_actorCellChangeSink;

    // Activation - a ref activated another (player presses E on a door /
    // terminal / NPC, an activator fires). Often the trigger for quest/object
    // scripts, so it is useful context for "script events".
    struct ActivationSink : public RE::BSTEventSink<RE::Activation::Event>
    {
        RE::BSEventNotifyControl ProcessEvent(
            const RE::Activation::Event& e,
            RE::BSTEventSource<RE::Activation::Event>*) override
        {
            if (!s_enabled.load(std::memory_order_acquire))
                return RE::BSEventNotifyControl::kContinue;

            const void* srcPtr = nullptr;
            const void* tgtPtr = nullptr;
            std::uint32_t srcFID = 0, tgtFID = 0;
            __try {
                srcPtr = e.source.get();
                tgtPtr = e.target.get();
                srcFID = SafeRawFormID(srcPtr);
                tgtFID = SafeRawFormID(tgtPtr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!srcFID && !tgtFID) return RE::BSEventNotifyControl::kContinue;

            // Player-cell filter: only activations involving the player's cell
            // (e.g. a quest NPC opening a door next to the players) - exactly
            // the events we need to design door / activation sync.
            const std::uint32_t pc = SafePlayerCellID();
            if (pc == 0) return RE::BSEventNotifyControl::kContinue;
            if (SafeRefCellID(srcPtr) != pc && SafeRefCellID(tgtPtr) != pc)
                return RE::BSEventNotifyControl::kContinue;

            char src[192], tgt[192];
            NameOfBuf(srcFID, src, sizeof(src));
            NameOfBuf(tgtFID, tgt, sizeof(tgt));
            Trace("ACTIVATE", "src=%s -> target=%s", src, tgt);
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static ActivationSink g_activationSink;

    // Register a sink on a CommonLibSF event source, fully guarded so a missing
    // address-library mapping degrades to "no trace for this event" instead of
    // a crash right before a live test.
    template <class EventT, class SinkT>
    static void RegisterGuarded(SinkT* sink, const char* label)
    {
        // PLOG/REX builds temporaries with destructors, so it cannot live in a
        // __try body (C2712). Capture a status, log it outside the SEH scope.
        bool ok = false, threw = false;
        __try {
            auto* src = EventT::GetEventSource();
            if (src) { src->RegisterSink(sink); ok = true; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            threw = true;
        }
        if (threw)    PLOG_WARN("[PULSAR] EventTrace: {} RegisterSink threw"sv, label);
        else if (!ok) PLOG_WARN("[PULSAR] EventTrace: {} GetEventSource null"sv, label);
        else          PLOG_INFO("[PULSAR] EventTrace: {} sink registered"sv, label);
    }

}  // anonymous namespace

void Init()
{
    if (s_initDone.exchange(true)) return;

    s_enabled.store(Pulsar::GetConfig().traceEvents, std::memory_order_release);

    PLOG_INFO("[PULSAR] EventTrace: Init (traceEvents={})"sv,
              s_enabled.load() ? 1 : 0);

    // Only event sources with a REAL (non-zero) CommonLibSF id - a REL::ID(0)
    // placeholder would be a fatal "Invalid ID: 0" at resolve time, which SEH
    // cannot catch. ActorCellChangeEvent=64106, Activation::Event=63133 (both
    // verified against the 1.16.244 versionlib).
    RegisterGuarded<RE::ActorCellChangeEvent::Event>(&g_actorCellChangeSink,
                                                     "ActorCellChangeEvent");
    RegisterGuarded<RE::Activation::Event>(&g_activationSink,
                                           "Activation::Event");
}

void Shutdown()
{
    // No-op: engine event sources outlive the plugin; sinks are static.
}

std::string NameOf(std::uint32_t formID)
{
    char buf[192];
    SafeResolve(formID, buf, sizeof(buf));
    return std::string(buf);
}

void NameOfBuf(std::uint32_t formID, char* out, std::size_t cap)
{
    SafeResolve(formID, out, cap);
}

void Trace(const char* category, const char* fmt, ...)
{
#ifndef PULSAR_HARDENED
    if (!s_enabled.load(std::memory_order_acquire)) return;

    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    const char* role = RoleTag();

    // PULSAR.log (local, per-player) + NAS (dev machine). PLOG is fmt-style;
    // the body is passed as a single argument so any literal braces in it are
    // harmless.
    PLOG_INFO("[PULSAR][TRACE][{}][{}] {}"sv, role, category ? category : "?", body);
    NASLOG("[TRACE][%s][%s] %s", role, category ? category : "?", body);
#else
    (void)category;
    (void)fmt;
#endif
}

}  // namespace Pulsar::EventTrace
