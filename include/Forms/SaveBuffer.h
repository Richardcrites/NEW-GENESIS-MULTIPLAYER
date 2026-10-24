#pragma once

#include <cstdint>
#include <vector>

namespace RE {
    class TESForm;
}

// BGSSaveFormBuffer / BGSLoadFormBuffer wrapper for runtime form serialization.
// Used for cell-transition sync: host serializes a form (TESNPC appearance,
// REFR state, etc.) into a blob, sends it to client, client reconstructs the
// form-state via vt[0x1B] LoadGame on the matching clone reference.
//
// Struct layout reverse-engineered from FUN_14181D690 + FUN_140B2EF30 stack
// init sites - see memory/reference_cell_transition_re.md for full breakdown.
namespace Pulsar::Forms
{
    // Mirrors Starfield's BGSSaveFormBuffer (size 0x80). Layout reconstructed
    // from FUN_14185FE30 stack-init (canonical Bethesda pattern) + FUN_140B2F760
    // buffer-grow logic.
    //
    // Concrete impl is BGSSaveLoadUtilities::BGSSaveFormBufferIgnoreIDs
    // (REL::ID 450091, vtable @ 0x144CB02A8). The "abstract" vtable @
    // 0x144CB08B8 (REL::ID 450162) is used by FUN_140B2EF30/14181D690 as a
    // marker for non-virtual-dispatch save paths - but for general TESForm
    // vt[0x1A] SaveGame() calls, use the IgnoreIDs concrete vtable.
    //
    // Trailing region @ +0x59..+0x7F is left as raw bytes - Ghidra showed
    // unaligned uint64 writes there which can't be expressed cleanly in
    // standard C++ struct layout.
    struct alignas(8) SaveFormBufferRaw {
        void**        vftable;             // +0x00  set to IgnoreIDs vtable
        void*         dataBuffer;          // +0x08  heap blob (engine grows via FUN_14186B2D0)
        std::uint32_t dataCapacity;        // +0x10  ← capacity (was wrong: I had size)
        std::uint32_t dataSize;            // +0x14  ← size
        std::uint32_t* formIDArrayBegin;   // +0x18  vector<uint32_t> begin (FormID xref tracking)
        std::uint32_t* formIDArrayEnd;     // +0x20
        std::uint32_t* formIDArrayCapEnd;  // +0x28
        std::uint16_t* pluginArrayBegin;   // +0x30  vector<uint16_t> begin (plugin idx)
        std::uint16_t* pluginArrayEnd;     // +0x38
        std::uint16_t* pluginArrayCapEnd;  // +0x40
        void*         _f48;                // +0x48  (paired with pluginArrayCapEnd as 16-byte block)
        std::uint32_t changeFlags;         // +0x50  init from save mgr +0x380
        std::uint32_t _f54_pad;            // +0x54  uninit/zero
        std::uint8_t  flags;               // +0x58  set to 1 (was wrong: I had +0x54)
        std::uint8_t  _trailing[0x80 - 0x59];
    };
    static_assert(sizeof(SaveFormBufferRaw) == 0x80,
                  "SaveFormBufferRaw must be 128 bytes");
    static_assert(offsetof(SaveFormBufferRaw, dataBuffer)   == 0x08, "dataBuffer @ +0x08");
    static_assert(offsetof(SaveFormBufferRaw, dataCapacity) == 0x10, "dataCapacity @ +0x10");
    static_assert(offsetof(SaveFormBufferRaw, dataSize)     == 0x14, "dataSize @ +0x14");
    static_assert(offsetof(SaveFormBufferRaw, changeFlags)  == 0x50, "changeFlags @ +0x50");
    static_assert(offsetof(SaveFormBufferRaw, flags)        == 0x58, "flags @ +0x58");

    // Public init - runs the layout probe at plugin load. The serialize
    // probe is deferred (player isn't loaded at plugin-init time) and fires
    // from TickProbe() once the player becomes available.
    void Init();

    // Game-thread tick - calls the Phase-2 serialize probe exactly once,
    // gated on: host-mode + player loaded into a cell + client connected
    // + ≥10s settle delay after first connection (so the connect-time
    // save+transfer pipeline has finished and we don't mutate save-mgr
    // state mid-flight).
    void TickProbe(bool isHost);

    // Layout-only probe (Phase 1.5). Does not call vt methods on engine forms.
    void RunProbe();

    // Phase-2: Serialize a form via vt[0x1A] SaveGame. Returns the blob bytes.
    // Empty on failure (null form, save threw, dataBuffer null, oversized,
    // etc.). Safe to call on any TESForm - wrapped in __try/__except - but
    // forms with side-effecting SaveGame impls may misbehave outside the
    // engine's actual save context. First test target: TESNPC.
    //
    // changeFlagsOverride: if non-zero, used as the buffer's changeFlags field
    // instead of reading the engine's current state. 0xFFFFFFFF = "save
    // everything"; useful when engine changeFlags=0 (no delta) but caller
    // wants the full form state regardless.
    std::vector<std::uint8_t> SerializeForm(RE::TESForm* form,
                                             std::uint32_t changeFlagsOverride = 0);

    // Receiver-side: deserialize a previously-Serialize'd blob onto a target
    // form (typically a clone Actor). Calls vt[0x1B] LoadGame on the target
    // with a BGSLoadFormBuffer pointing at our blob bytes. Returns true if
    // the call completed without exception. Does NOT verify that engine
    // state actually changed.
    bool DeserializeForm(RE::TESForm* target,
                          const std::uint8_t* blob,
                          std::size_t blobSize,
                          std::uint32_t sourceFormID = 0);

    // Host-side: serialize the player REFR and send it to the client as
    // PKT_FORM_BLOB. targetFormID=0 means "apply to the receiver's clone of
    // the sender's player". Sender-/host-only - silently no-op on client or
    // when not connected. Use after cell-transition to trigger biped re-init.
    void BroadcastPlayerForm(std::uint32_t targetFormID = 0);

    // Receiver-side packet handler - called from NetworkHost dispatcher.
    // Caches the blob keyed by sourceFormID for later DeserializeForm call
    // (clone may not be spawned yet at receive time).
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // Game-thread: drains pending blobs and applies them to matching local
    // clone actors (via RemotePlayerManager / CloneManager lookup).
    void DrainPending();
}
