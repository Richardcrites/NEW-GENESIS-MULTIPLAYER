#pragma once

#include <cstdint>
#include <cstddef>

namespace Pulsar::SpaceSync::FastTravel {

// Plugin-init. Currently a no-op - the sink for PlayerFastTravel::Event was
// removed because it does not fire for Star Map ship jumps (it is wired to
// surface-map fast-travel only). The fast-travel arrival is now detected via
// the `FarTravel → RampDown` event sequence reported by SpaceSync's existing
// Spaceship sinks; see OnFarTravelFired / OnRampDownFired below.
void Init();

// Hook from SpaceSync's GravJump sink - records the timestamp of the latest
// host-side GravJump fire. Used by the delta-probe diagnostics.
void OnGravJumpFired();

// Hook from SpaceSync's FarTravel sink - engine fires this at the START of a
// Star Map fast-travel. We only record the timestamp; the actual SEND fires
// from the RampDown handler below once arrival completes.
void OnFarTravelFired();

// Hook from SpaceSync's RampDown sink - engine fires this when the post-jump
// ramp animation finishes. Combined with a recent FarTravel timestamp this
// is the cleanest "arrival happened" signal we have. Arms the deferred
// PKT_FAST_TRAVEL_SYNC send (3s settle, then read ship.parentCell + pos and
// broadcast).
void OnRampDownFired();

// Network dispatch entry - called from NetworkHost when a PKT_FAST_TRAVEL_SYNC
// packet arrives. Stages the destination data; the actual MoveTo on the local
// ship runs from Tick() on the game thread.
void OnFastTravelPacket(const std::uint8_t* data, std::size_t length);

// Network dispatch entry for PKT_CLIENT_LOAD_COMPLETE. Called on host when
// client finishes a save-bridge load (initial or post-jump). Marks
// g_clientLoadCompleted so subsequent save-bridges can fire safely.
void OnClientLoadCompletePacket(const std::uint8_t* data, std::size_t length);

// Network dispatch entry for PKT_BRIDGE_REQUEST (L.7). Called on host when
// the client's AutoFollow had to SKIP a teleport across a procedural / cross-
// worldspace boundary. Runs on the network-receive thread: only queues the
// request; TickManualBridgeHotkey (game thread) drains it and fires the bridge.
void OnBridgeRequestPacket(const std::uint8_t* data, std::size_t length);

// Called from WorldSync on the client side once a host-bridged save load
// finishes. Sends PKT_CLIENT_LOAD_COMPLETE to host. `reason` = 0 for
// initial-connect load, 1 for any subsequent fast-travel bridge load.
void NotifyClientLoadComplete(std::uint8_t reason);

// Per-tick driver. Runs the side-effect-free state-delta probe, the
// host-side PFT-deferred-send, and the client-side PFT-MoveTo applier. The
// older v6 save-bridge is compiled out under PULSAR_L7_FT_BRIDGE_ENABLED in
// FastTravelSync.cpp.
void Tick();

// L.7 manual save-bridge trigger via configurable hotkey. Last-resort
// override for cases where the engine's natural autosave-on-disembark
// doesn't fire (e.g. cached planet - engine skips autosave when it considers
// state unchanged). User presses the bound key → host triggers a save +
// sends to client. Bypasses the auto-fire gates (cooldown, first-ack
// narrowing) since the user is taking explicit responsibility.
//
// Default 0x77 = VK_F8 preserves historical behaviour; binding overridable
// via PULSAR.ini's HotkeyManualBridge.
void TickManualBridgeHotkey(int a_vkey = 0x77);

// L.7 V14 - generic save-bridge entry. Used by:
// PFT-arrival path (TickHostPFTDeferredSend): a_triggerOwnSave=false
//     (engine auto-quicksaves post-grav-jump; just poll for it)
// Deferred TakeOff/Landing fire (a_triggerOwnSave=true) - see
//     ArmTransitionFire below.
// Gates: clientReady ack + 15s cooldown. Returns true if fired.
// Reason string is logged for diagnostics. Host-only.
bool FireSaveBridge(const char* a_reason, bool a_triggerOwnSave);

// L.7 V14.1 - arm a deferred transition fire. Called by SpaceshipSink<
// TakeOff/Landing> on event. Sets a pending timer; the actual save trigger
// runs from Tick() once `kTransitionSettleMs` has elapsed. Re-arming
// (multiple sink fires per real action) is idempotent - only the last
// arm-time + reason takes effect, so quick burst from cutscene events
// collapses to a single fire 5s after the last one.
//
// V15: returns true if the event was treated as a REAL transition
// (inSpace flip detected - bridge will fire), false if SPURIOUS
// (inSpace unchanged - door/POI misclassified as ship event). Caller
// should suppress the PKT_SPACE_TRANSITION broadcast on spurious so
// the client doesn't get a flood of useless events that confuse its
// auto-follow state machine.
bool ArmTransitionFire(const char* a_reason);

}  // namespace Pulsar::SpaceSync::FastTravel
