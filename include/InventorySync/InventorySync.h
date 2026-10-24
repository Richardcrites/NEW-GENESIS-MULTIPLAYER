#pragma once

// M.3 Player Inventory Separation
//
// Goal: when client connects to host, both load the same save → identical
// PlayerCharacter state. Co-op needs each player to keep their own
// equipment/inventory/skills/credits independently.
//
// Architecture: client-side profile swap built on Forms::SaveBuffer.
//
//   On client disconnect:
// Pulsar::Forms::SerializeForm(player, 0xFFFFFFFF) → engine-native blob
// Write blob to PULSAR_Profiles/client.psprof
//
//   On client save-load completed (post-host-save-bridge):
// Read blob from PULSAR_Profiles/client.psprof (if exists)
// Pulsar::Forms::DeserializeForm(player, blob, ...) → engine overwrites
//       host-loaded player state with client's profile state
// Existing L.7 fast-travel-TP corrects position to host's cell
//
// Host side: no-op. Host plays naturally; their save is canonical.
//
// First-MP-connect: no profile file → skip apply, log warning. On next
// disconnect, current state becomes baseline profile.

#include <cstdint>
#include <string>

namespace Pulsar::InventorySync
{
    void Init();
    void Shutdown();

    // Thread-safe queue requests. Actual engine calls happen in DrainPending
    // on the game thread (vt[0x1A]/[0x1B] are not thread-safe).
    void RequestSnapshot();
    void RequestApply();

    // Game-thread: process queued requests.
    void DrainPending();

    // Trigger hooks called from other modules. All gate on "we are client";
    // host calls are no-ops.
    //
    // Order in a typical session:
    //   1. TickPreBridgeSnapshot() - fired every game tick. When state =
    //      WAITING_FOR_SAVE AND player has parentCell, fires snapshot once
    //      per save-bridge cycle. This handles all order permutations:
    // User loads save then connects → fires immediately
    // User connects then loads save → fires when load completes
    // Save bridge starts before player loads → fires never (no
    //        solo state to capture; existing profile or no profile applies)
    //   2. OnLoadCompleted() - fired after host save-bridge loads, settled.
    //      Applies the snapshot → client retains their solo items.
    //   3. OnNetworkDisconnect() - fired when peer disconnects. Snapshots
    //      current state so any MP-session pickups carry over.
    void TickPreBridgeSnapshot();
    void OnNetworkDisconnect();
    void OnLoadCompleted();

    // Hotkey handlers (manual snapshot/apply for solo testing).
    void OnHotkeySnapshot();
    void OnHotkeyApply();

    // Diagnostic: returns the absolute path to the current profile file.
    std::string GetProfilePath();

    // True between the moment TickPreBridgeSnapshot fires and OnLoadCompleted
    // applies the blob. Signals to WorldSync that the L.7 pre-load snapshot
    // is misleading: it was captured from the client's freshly-loaded solo
    // save, not from a continuation of an MP session. Without this signal,
    // L.7 same-cell-match logic skip-teleports when client's solo save
    // happens to share a cell template with host's save (e.g. both on
    // default Frontier ship interior 0x05100136 → Same-cell match → skip
    // teleport → cross-cell desync).
    bool IsPreBridgeSnapshotPending();

    // L.7 V3 - arm the pre-bridge-snapshot flag for an incoming bridge
    // load. Called from WorldSync::OnSaveReceived so the flag is TRUE during
    // the chunk-read window for EVERY bridge cycle, not just the first one.
    // (TickPreBridgeSnapshot only fires on the WAITING_FOR_SAVE transition,
    // which doesn't occur for F8-from-host or other subsequent bridges.)
    // Idempotent - safe to call when flag is already set.
    void ArmForBridge();
}
