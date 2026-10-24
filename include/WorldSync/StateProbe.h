#pragma once

#include <cstdint>

// L.7 V13b - Engine pre-load state probe.
//
// Hooks FUN_14189A8E0 (RVA 0x141898210) - the engine's pre-load init
// handler called from BGSSaveLoadManager queue task processor when bit 0x4
// (load) is set in mgr+0x50. On entry, dumps a snapshot of all candidate
// stuck-state items so we can diff load #1/#2/#3 and identify what changes
// between a working load and the broken second-mid-game load.
//
// Probe-only: does NOT modify any state. Runs ALWAYS regardless of code
// path (mid-game load, main-menu Continue, our PULSAR_Session bridge),
// so single-player quickload→quickload→quickload reproduces the diff.
//
// State items dumped:
//   1. Outer guard byte    DAT_145e794be   (V12 clears)
//   2. Increment counter   DAT_145e794dc   (V12 clears)
//   3. Player flag         player+0x113c   (V12 clears)
//   4. Player flag         player+0xdd4    (FUN_141A8DA20 gate, NOT cleared)
//   5. Player-init guard   DAT_145fbc8f8   (FUN_141957ca0 gate)
//   6. World-data base ptr DAT_145e794b0
//   7. World init A slot   *(DAT_145e794b0 + 0x1B0)   (FUN_140582780 gate)
//   8. World init B slot   *(DAT_145e794b0 + 0x1B8)   (in-fn use)
//   9. World init C slot   *(DAT_145e794b0 + 0x1C0)   (FUN_140582A50 gate)
//  10. World init D slot   *(DAT_145e794b0 + 0x1C8)   (FUN_140582C30 gate)
//  11. Array count         DAT_145f4f9b0   (FUN_141A90820 use)
//  12. Array data ptr      DAT_145f4f9b8
//  13. SaveLoadMgr+0x50    queued-tasks bitmask (bit 0x4 = load trigger)
//  14. Player parentCell   ptr + parentCell formID
//  15. Player formID       (sanity)

namespace Pulsar::WorldSync::StateProbe {

    // Install the FUN_14189A8E0 entry hook. Idempotent.
    void Init();

    // Manually dump a snapshot tagged "[PROBE MANUAL]" - useful to
    // capture state from outside the load path (e.g. from PENDING_TRIGGER
    // right before we set bit 0x4, to compare with the snapshot the
    // hook itself writes inside FUN_14189A8E0).
    void DumpManual(const char* a_tag);

    // Total count of FUN_14189A8E0 entry hooks that have fired since plugin
    // start. Monotonic. Used by WorldSync's V13f load-start detection: it
    // captures the count at PENDING_TRIGGER, then watches for it to go up
    // (= engine's pre-load init handler ran = load has actually begun).
    std::uint32_t GetEntryHookCount();

}  // namespace Pulsar::WorldSync::StateProbe
