#pragma once

namespace Pulsar::CrashGuard {

    // Install MinHook guards for known engine race crashes.
    // Currently covers: 0x30B350E bone-array NULL-deref ("TtgetGround"
    // foot-IK / ground-snap loop - T114 RE).
    //
    // Returns true on success. Failures are logged but non-fatal - plugin
    // continues without protection.
    bool Init();

}
