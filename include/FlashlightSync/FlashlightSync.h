#pragma once

// Helmet headlamp (flashlight) sync. Detection: the engine stores the
// headlamp BSShadowLight* at Actor+0xEB0 (null = off). Found via dynamic
// byte-diff (see memory reference_flashlight_re). Apply: the engine headlamp
// setup FUN_141a68a10(actor, on, flag) is actor-parameterized, so we call it on
// the clone to create/destroy a REAL BSShadowLight that follows the clone.
//
// Transport: 1 spare bit (bit 12) on PKT_POSITION animFlags (continuous,
// latest-wins) - no dedicated packet.

namespace RE { class Actor; }

namespace Pulsar::FlashlightSync {

    // True if the LOCAL player's helmet flashlight is currently ON
    // (*(void**)(player + 0xEB0) != nullptr). SEH-guarded.
    bool IsLocalOn();

    // Drive the engine headlamp on a CLONE actor to match a_wantOn. No-op when
    // the clone is already in the desired state. SEH-guarded (a clone may lack
    // some player sub-components the engine toggle touches).
    void ApplyToClone(RE::Actor* a_clone, bool a_wantOn);

    // Release our owned light on a clone BEFORE the clone/its 3D node is torn
    // down (call from the controlled despawn path while the clone is still
    // valid) - prevents a dangling-ref use-after-free.
    void ReleaseForClone(RE::Actor* a_clone);

    // Drop ALL tracking WITHOUT releasing (save-load full reset already frees the
    // light objects; touching them would deref freed memory). Call on the
    // save-load clone-invalidation path.
    void ForgetAll();

    // DIAG - dump the player's working headlamp light vs our created clone
    // light (both on the host) so we can diff the config that makes one visible.
    void DumpLights();

}
