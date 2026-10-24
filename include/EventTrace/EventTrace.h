#pragma once

#include <cstdint>
#include <string>

// EventTrace - diagnostic, dev-only telemetry channel for co-op SYNC DESIGN.
//
// Purpose: during a live 2-player session we want a single, human-readable,
// HOST/CLIENT-tagged timeline of the engine events that drive co-op state -
// quest progress, quest/dialogue scripts, NPC spawns, and NPC dialogue lines -
// so we can diff the two machines' logs and design correct sync.
//
// Output goes to BOTH PULSAR.log (REX/spdlog, lands LOCALLY on each player's
// machine, the right place for a host-vs-client diff) and the NAS log (dev
// machine only). Everything routes through PLOG/NASLOG, so it is compiled OUT
// entirely in the hardened Release build (see include/Log.h) - zero footprint
// in the shipped binary.
//
// Grep the resulting log for "[TRACE]" to pull the whole timeline.

namespace Pulsar::EventTrace {

    // Register the diagnostic event sinks: ActorCellChangeEvent for NPC
    // spawn / cell presence (TESObjectLoadedEvent is a REL::ID(0) placeholder
    // in CommonLibSF, so we use this real-id event instead) and
    // Activation::Event for activations / script triggers. Call once at
    // kPostLoad, after the other sync modules' Init(). Safe to call multiple
    // times.
    void Init();

    void Shutdown();

    // SEH-safe form resolver. Returns "EditorID(0xFORMID)[tNN]" when the form
    // is loaded and has an EditorID, "0xFORMID[tNN]" when loaded without one,
    // or a "(notloaded)"/"(seh)" marker otherwise. Never throws. Used by the
    // sinks here AND by QuestSync/DialogSync to make their logs readable.
    //
    // Use NameOf in functions WITHOUT __try; use the buffer form NameOfBuf in
    // any function that contains a __try block (a std::string temporary in such
    // a function is a C2712 compile error - a char[] is fine).
    std::string NameOf(std::uint32_t formID);
    void        NameOfBuf(std::uint32_t formID, char* out, std::size_t cap);

    // Formatted trace line -> PULSAR.log + NAS, prefixed
    // "[PULSAR][TRACE][HOST|CLIENT][<category>] ". printf-style (%s, %08X,
    // ...), NOT fmt {} style. No-op when the traceEvents config flag is off
    // or in the hardened build.
    void Trace(const char* category, const char* fmt, ...);

}  // namespace Pulsar::EventTrace
