#include "PCH.h"
#include "DialogSync/DialogSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "EventTrace/EventTrace.h"
#include "NPCSync/NPCSync.h"  // V36: MarkNpcInScene - suspend NPC puppet during scenes

#include <REL/Relocation.h>
#include "WorldSync/LootSync.h"  // CallOrigActivateAsReplay

#include <RE/A/Actor.h>
#include <RE/B/BSContainer.h>
#include <RE/E/Events.h>
#include <RE/I/INIPrefSettingCollection.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/Setting.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>
#include <RE/B/BSFixedString.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <RE/T/TESForm.h>

using namespace std::string_view_literals;

namespace Pulsar::DialogSync {
namespace {

    // L.8 Phase 3 V1 - engine event sources verified via Ghidra sub-agent
    // 2026-05-09 + cross-checked against SFSE 0.2.19 GameEvents.h:
    //
    //   PlayerInDialogueChanged::Event @ RVA 0x597A298  (start AND end)
    //   TESOnPCDialogueTargetEvent     @ RVA 0x5978698  (speaker Actor*)
    //   TESTopicInfoEvent              @ RVA 0x5978468  (per-line; V2+)
    //
    // SFSE DECLARE_EVENT_SOURCE_EX pattern → RVA IS the BSTEventSource
    // singleton instance directly (RelocPtr<BSTEventSource<X>>). We cast
    // `(modBase + RVA)` to `BSTEventSource<X>*` and call RegisterSink.
    //
    // Recommendation from RE pass: prefer event sinks over MinHook on the
    // setters - the two setters live in 3.4 KB / 1.2 KB functions with
    // multiple call paths and thread-local init prologues that are
    // ABI-fragile across patches.
    constexpr std::uintptr_t kRVA_PlayerInDialogueChanged = 0x597A298;
    constexpr std::uintptr_t kRVA_TESOnPCDialogueTarget   = 0x5978698;
    constexpr std::uintptr_t kRVA_TESTopicInfoEvent       = 0x5978468;

    // L.8 Phase 3 - TESTopicInfo::Fire engine primitive (the say-mirror core).
    // Re-RE'd 2026-06-28 on 1.16.244 (verified): the 3rd arg is the SPEAKER's
    // REFR HANDLE, not a formID - Fire resolves it via the handle manager to
    // get the speaking actor, then plays voice + subtitle + lipsync with NO
    // menu and NO player hijack. Called twice per line: (info,0,handle,0)
    // starts it, (info,1,handle,0) drives the continuation. Game-thread only
    // (touches BSTEventSource::Notify + ExtraDataList). The old code passed the
    // topicInfo formID as arg3 (a non-handle) -> no speaker context -> the line
    // barely played; THAT was the "only one stray line" bug.
    using TopicInfoFireFn = void (*)(void* topicInfo, int startEnd,
                                     std::uint32_t speakerHandle, char unk4);
    constexpr std::uintptr_t kRVA_TopicInfoFire = 0xD48170;
    static TopicInfoFireFn g_topicInfoFire = nullptr;

    // Object->REFR-handle primitive (HandleManager::EmplaceHandle) - builds the
    // handle Fire's arg3 needs from an Actor*. RE'd 2026-06-28 on 1.16.244
    // (verified, high confidence): find-or-create - for a loaded actor it
    // returns the EXISTING handle (no duplicate) and is game-thread-safe
    // (internal lock). __fastcall(HandleManager* mgr, TESObjectREFR* refr) ->
    // u32 handle in EAX. Manager singleton @ *(moduleBase + 0x5E60380) - the
    // SAME manager Fire reads, so the handle round-trips. (Note: CommonLibSF
    // GetSmartPointer{72432} is a stale jmp-thunk on 244 - never use it here.)
    using EmplaceHandleFn = std::uint32_t (*)(void* mgr, void* refr);
    constexpr std::uintptr_t kRVA_EmplaceHandle = 0x28CCAC0;   // REL::ID 1186745
    constexpr std::uintptr_t kRVA_HandleMgrSlot = 0x5E60380;   // &singleton
    static EmplaceHandleFn g_emplaceHandle = nullptr;
    static void**          g_handleMgrSlot = nullptr;  // deref at call time (null pre-init)

    // V7 - Actor::SetDialogueTarget. RE'd via dialog_re.txt prior pass.
    // Signature: void(Actor* self, TESObjectREFR* target). When target!=0
    // sets self+0x238 = target and notifies TESOnPCDialogueTargetEvent;
    // when target==0 clears the field. Lighter than Activate (no menu, no
    // camera transition, no input lock) - just establishes the engine's
    // internal dialog-state pointer so subsequent line fires have
    // correct context. This is what V6's Activate did internally but
    // WITHOUT the menu/camera/freeze side-effects.
    using SetDialogueTargetFn = void (*)(void* actor, void* target);
    constexpr std::uintptr_t kRVA_SetDialogueTarget = 0x194A1C0;
    static SetDialogueTargetFn g_setDialogueTarget    = nullptr;
    static SetDialogueTargetFn g_origSetDialogTarget  = nullptr;  // V13 hook trampoline

    // V20 - scene-less dialog open primitive. RE'd via scene_storyteller_re.txt:
    // FUN_141795760(MenuTopicManager*, TESObjectREFR* speaker, int unk1=0, int unk2=1, int unk3=0)
    // is the engine's free-form (E-press) dialog opener - bypasses the
    // BGSScene path that wraps quest-driven dialogues. Hooking it lets
    // us capture E-press dialog opens (which V13 SetCurrentScene hook
    // misses entirely). Calling it directly is also a cleaner RX-replay
    // primitive than Activate (which can have many side-effects).
    using OpenDialogFn = void (*)(void* mtm, void* speaker,
                                  int u1, int u2, int u3);
    constexpr std::uintptr_t kRVA_OpenDialog = 0x1792DF0;
    static OpenDialogFn g_openDialog       = nullptr;
    static OpenDialogFn g_origOpenDialog   = nullptr;  // V20 hook trampoline

    // V25 REMOVED - see Init() comment. FUN_141793d10 has 6 params, our
    // 2-arg hook corrupted R8/R9/stack args → vanilla dialog menus
    // failed to open. Lesson learned: verify full signature first.

    // V20 - MenuTopicManager singleton (raw object at this address).
    // 0x1461E22B0 is the absolute, runtime = moduleBase + (this - 0x140000000).
    constexpr std::uintptr_t kRVA_MTM = 0x61E22B0;
    static void* g_mtm = nullptr;

    // Engine event payload layouts (forward-declared here - neither
    // CommonLibSF nor SFSE 0.2.19 expose them as full structs; both
    // declare empty `struct {};` placeholders. Layouts derived from
    // Notify-fire-site decompile in the RE pass).
    struct PlayerInDialogueChangedEvent
    {
        bool inDialogue;  // +0x00 - single byte, only field accessed by sinks
    };

    // Layout derived from runtime diagnostic 2026-05-09 18:57 (3 events
    // fire per dialogue start, one per involved actor - the source field
    // varies per event but the TARGET pointer at +0x10 is always the NPC
    // and is identical across all 3 events of a session). Cross-machine
    // FormID consistency confirmed (host+client both saw 0x0028A2B5 from
    // separate heap pointers). +0x08 reads as caller-frame stack data so
    // is treated as opaque padding.
    struct TESOnPCDialogueTargetEvent
    {
        RE::TESObjectREFR* source;       // +0x00 - actor whose dialogue-target changed
        std::uintptr_t     opaque_pad;   // +0x08 - caller-frame spill, NOT part of struct
        RE::TESObjectREFR* target;       // +0x10 - NPC being addressed (or null)
    };

    // V4 payload layout from dialog_v4_re.txt - built in FUN_140D48780.
    // First-pass best-guess; will hex-dump on first fire and verify before
    // shipping. Structure may need adjustment after diagnostic.
    struct TESTopicInfoEvent
    {
        RE::TESObjectREFR* speaker;   // +0x00
        void*              _pad08;    // +0x08 (zero/lambda capture)
        std::uint32_t      formID;    // +0x10
        std::uint32_t      isEnd;     // +0x14 (0=start, 1=end)
    };

    // Latest-target tracker. PlayerInDialogueChanged fires with a bool;
    // TESOnPCDialogueTarget fires with the actual Actor*. The two events
    // do NOT fire atomically - typically Target fires *first* (when the
    // speaker is set), then InDialogue fires (when the menu commits). We
    // store the most-recent target FormID so the InDialogue=true broadcast
    // can include the speaker. Reset on InDialogue=false.
    static std::atomic<std::uint32_t> s_lastTargetFormID{ 0 };

    // Last broadcasted state - dedupes spurious double-fires (engine
    // sometimes runs the setters twice during cinematic transitions).
    static std::atomic<bool> s_lastBroadcastInDialogue{ false };

    // V24 - track which speaker formID we last broadcasted START for.
    // Vanilla scene-driven dialogs cycle through multiple speakers within
    // a single continuous engine session. V19's `s_lastBroadcastInDialogue`
    // flag stays true across speaker changes, so the original first-speaker
    // START was the only one Client received. Client's menu becomes stale
    // when Host's actual conversation moves to a different NPC.
    // V24: when V19 sees a NEW speaker different from s_lastBroadcastNpc,
    // re-broadcast START with the new NPC even if the dialog flag is set.
    // Client's RX handler will queue another Activate → switches menu.
    static std::atomic<std::uint32_t> s_lastBroadcastNpc{ 0 };

    // V26 - post-END cooldown to break engine auto-greet echo loop.
    // When player stands next to a quest NPC, the engine auto-greets
    // continuously. Each cycle:
    //   1. local END (V18 form=0) → broadcasts END
    //   2. peer END processed
    //   3. peer engine auto-greets the same NPC (player still nearby)
    //   4. peer V20/V19 broadcasts START → echoes back
    //   5. local Activate replay → menu reopens
    //   6. user can't escape - pressing "leave" repeats the cycle
    // V26: after observing END for NPC X (local broadcast OR peer RX),
    // refuse to broadcast NEW START for X for kEndCooldownMs, AND refuse
    // to apply incoming START Activate for X. Different NPC unaffected.
    constexpr std::uint64_t kEndCooldownMs = 2000;  // V26.1 - was 5000, too long
    static std::atomic<std::uint32_t> g_lastEndedNpc{ 0 };
    static std::atomic<std::uint64_t> g_lastEndedMs{ 0 };

    static bool IsNpcOnEndCooldown(std::uint32_t npcFormID) noexcept
    {
        if (npcFormID == 0) return false;
        if (npcFormID != g_lastEndedNpc.load(std::memory_order_acquire)) return false;
        std::uint64_t lastEnd = g_lastEndedMs.load(std::memory_order_acquire);
        if (lastEnd == 0) return false;
        std::uint64_t now = ::GetTickCount64();
        return (now - lastEnd) < kEndCooldownMs;
    }
    static void MarkNpcEnded(std::uint32_t npcFormID) noexcept
    {
        if (npcFormID == 0) return;
        g_lastEndedNpc.store(npcFormID, std::memory_order_release);
        g_lastEndedMs.store(::GetTickCount64(), std::memory_order_release);
    }

    // V4 echo-suppression: when DrainPending re-fires a remote pick,
    // FUN_140D48780 internally re-Notifies TESTopicInfoEvent on the
    // local sink → without this flag we'd echo the pick back to the
    // peer in an infinite loop. Per-thread because Drain runs on game
    // thread and the engine may also fire from game thread.
    static thread_local bool tls_applyingRemoteTopic = false;

    // V4 RX queue - Network thread receives PKT_DIALOG_VOTE, pushes
    // here; game-thread DrainPending pops + replays. TESTopicInfo::Fire
    // and ExtraDataList mutex are not network-thread-safe.
    struct PendingPick
    {
        std::uint32_t topicInfoFormID;
        std::uint32_t speakerFormID;
        std::uint8_t  playerSlot;
    };
    static std::mutex                s_pendingPicksMutex;
    static std::vector<PendingPick>  s_pendingPicks;

    // V10 - TopicInfo-driven session tracker. PlayerInDialogueChanged
    // doesn't fire in scene-style dialogues where player is a spectator.
    // V11 - synthesize START on first foreign-speaker TopicInfo fire,
    // and END after a 30s idle gap (was 5s in V10 - caused mid-scene
    // tear-down + V9 Activate spam that destabilized engine).
    static std::atomic<std::uint64_t> s_lastTopicInfoMs{ 0 };
    static constexpr std::uint64_t    kSessionIdleMs = 30000;

    // V11 - dedupe V9 dialog-open per NPC. Same NPC speaking again
    // within an active session should NOT re-fire Activate - the menu
    // is already open. Reset on END.
    static std::atomic<std::uint32_t> s_lastV9OpenedNpc{ 0 };

    // V6 - pending engine-natural dialog opens (NPC formIDs) + closes
    // (player slots). Drained on game thread because Activate touches
    // cell-streamer + ExtraDataList + UI message queue.
    static std::vector<std::uint32_t> s_pendingDialogOpens;
    static std::vector<std::uint8_t>  s_pendingDialogCloses;

    // V4 first-N hex-dump diagnostic. Set kDiagDumpRemaining=N to log the
    // first N TESTopicInfoEvent fires with raw payload bytes - confirms
    // the layout in production before we trust it for broadcast.
    static std::atomic<int> s_diagDumpRemaining{ 6 };

    static constexpr std::uint8_t kSlotHost   = 0;
    static constexpr std::uint8_t kSlotClient = 1;

    static std::uint32_t RawFormID(const void* form) noexcept
    {
        if (!form) return 0;
        return *reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(form) + 0x28);
    }

    static std::uint32_t SafeRawFormID(const void* form) noexcept
    {
        __try { return RawFormID(form); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // V16 - extended diagnostic readers for trace mode.
    static std::uint8_t SafeRawFormType(const void* form) noexcept
    {
        if (!form) return 0xFF;
        __try {
            return *reinterpret_cast<const std::uint8_t*>(
                static_cast<const std::uint8_t*>(form) + 0x2E);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0xFF;
        }
    }

    // Read first 32 bytes of a form for hex inspection.
    static void SafeDumpForm32(const void* form, std::uint64_t out[4]) noexcept
    {
        out[0] = out[1] = out[2] = out[3] = 0;
        if (!form) return;
        __try {
            auto* p = reinterpret_cast<const std::uint64_t*>(form);
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Read player.dialogTarget pointer (raw +0x238).
    static std::uintptr_t SafeReadPlayerTargetPtr() noexcept
    {
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return 0;
            return *reinterpret_cast<std::uintptr_t*>(
                reinterpret_cast<std::uint8_t*>(player) + 0x238);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // V23 - live UI menu-state read. V22 used MTM+0xA8 (assumed to be
    // the current speaker REFR*) but logs showed it returning non-null
    // even when no DialogueMenu was open, blocking every RX Activate.
    // Root cause: CommonLibSF MenuTopicManager declares speaker as a
    // BSPointerHandle at +0x14, NOT a pointer at +0xA8. The +0xA8 read
    // landed on some pre-populated engine state field.
    //
    // V23 switches to RE::UI::IsMenuOpen("DialogueMenu") - the actual
    // authoritative on-screen menu state. Wrapped in SEH for safety
    // (mirrors the IsLoadingMenuOpen helper in Main.cpp).
    static bool IsDialogueMenuOpenRaw(RE::UI* ui,
                                      const RE::BSFixedString& name) noexcept
    {
        if (!ui) return false;
        __try { return ui->IsMenuOpen(name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool IsDialogueMenuOpenSafe() noexcept
    {
        static const RE::BSFixedString kDialogueMenu{ "DialogueMenu" };
        return IsDialogueMenuOpenRaw(RE::UI::GetSingleton(), kDialogueMenu);
    }

    // Read player position from raw +0x8C.
    static void SafeReadPlayerPos(float out[3]) noexcept
    {
        out[0] = out[1] = out[2] = 0.0f;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            auto* pos = reinterpret_cast<float*>(
                reinterpret_cast<std::uint8_t*>(player) + 0x8C);
            out[0] = pos[0]; out[1] = pos[1]; out[2] = pos[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void SafeReadFormPos(const void* form, float out[3]) noexcept
    {
        out[0] = out[1] = out[2] = 0.0f;
        if (!form) return;
        __try {
            auto* pos = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(form) + 0x8C);
            out[0] = pos[0]; out[1] = pos[1]; out[2] = pos[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Squared distance from a ref to the local player (raw +0x8C positions).
    // Used by the say-mirror to scope which NPC lines the host broadcasts to
    // "near the player" (shared scene) instead of the whole loaded world.
    static float SafeDist2ToPlayer(const void* refr) noexcept
    {
        if (!refr) return 1e30f;
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return 1e30f;
            const float* pa = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(refr) + 0x8C);
            const float* pb = reinterpret_cast<const float*>(
                reinterpret_cast<std::uint8_t*>(player) + 0x8C);
            float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
            return dx * dx + dy * dy + dz * dz;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 1e30f;
        }
    }

    // Read parentCell pointer + formID (REFR+0xC0 typical).
    static std::uint32_t SafeReadFormCell(const void* form) noexcept
    {
        if (!form) return 0;
        __try {
            auto cellPtr = *reinterpret_cast<void**>(
                const_cast<std::uint8_t*>(
                    static_cast<const std::uint8_t*>(form) + 0xC0));
            return SafeRawFormID(cellPtr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // V12 - read PlayerCharacter.dialogTarget from raw +0x238. Set by
    // FUN_14194CA60 (Actor::SetDialogueTarget) when player engages an
    // NPC via E-press. 0 if no active dialog. Used to filter ambient
    // NPC TopicInfo events from the actual conversation partner -
    // V11 had "first speaker wins" which incorrectly locked onto
    // passerby NPCs that fired ambient lines before the real partner.
    static std::uint32_t SafeReadPlayerDialogTarget() noexcept
    {
        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return 0;
            auto* tgt = *reinterpret_cast<RE::TESObjectREFR**>(
                reinterpret_cast<std::uint8_t*>(player) + 0x238);
            if (!tgt) return 0;
            return RawFormID(tgt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // V6 - engine-natural dialog open via Activate replay. Mirrors
    // LootSync's ActivateArgs layout (32 bytes). Calling Activate with
    // (NPC, args.activator=player) routes through the engine's normal
    // E-press path, which decides "target is NPC with dialog → open
    // DialogueMenu + camera transition" - no need for a private engine
    // primitive we don't have access to.
    struct ActivateArgs {
        RE::TESObjectREFR* activator;
        std::uint64_t      unused1;
        std::uint64_t      unused2;
        std::uint32_t      countOrFlag;
        std::uint8_t       flag1c;
        std::uint8_t       flag1d;
        std::uint8_t       flag1e;
        std::uint8_t       flag1f;
    };
    static_assert(sizeof(ActivateArgs) == 0x20, "ActivateArgs size");

    // V6 echo-suppression: when client opens dialog via replay, host's
    // own dialog session is unrelated - but the ENGINE on client will
    // fire PlayerInDialogueChanged + TESOnPCDialogueTarget locally as a
    // result of the replayed activate. Without this flag, those events
    // would broadcast back to host as a "client started dialog" event.
    static thread_local bool tls_applyingRemoteDialogOpen = false;

    // V9 - DialogueCamera setting. INI key `bDialogueEnable:Interface`
    // (Bethesda legacy name from FO4/76; user-facing label is "Starfield
    // Dialogue Camera" in Accessibility menu). When 0, the engine's PC
    // dialog state machine (FUN_141794720) skips the camera-transition
    // path that caused V6's freeze when client wasn't near the NPC.
    // We force-flip OFF only while applying a remote dialog open, then
    // restore the user's value on END - accessibility pref isn't
    // silently overridden outside our co-op flow. In-memory only;
    // never WriteSetting to disk so nothing persists.
    static RE::Setting* g_dialogueCameraSetting  = nullptr;
    static bool         g_dialogueCameraOriginal = true;
    static bool         g_dialogueCameraSaved    = false;

    static std::uint8_t LocalSlot()
    {
        // We don't have a player-id concept yet - use isHost for V1. When
        // multi-client lands this becomes the seat number from handshake.
        return Pulsar::Network::GetSingleton().IsHost() ? kSlotHost : kSlotClient;
    }

    // V13 forward decl - body lives in second anon-ns block past Reset().
    static void __fastcall HookedSetDialogTarget(void* actor, void* target);

    // V20 forward decl - body in same downstream block.
    static void __fastcall HookedOpenDialog(void* mtm, void* speaker,
                                            int u1, int u2, int u3);

    // V25 forward decls - REMOVED, see Init() comment.

    static void BroadcastDialogStart(std::uint32_t npcFormID)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        // V28 - host-only architecture. Client never broadcasts dialog
        // events. Eliminates echo loops entirely (client→host→client).
        // Client's V19/V20 sinks still fire when client's engine auto-
        // greets locally (or replays our Activate), but those events stay
        // local. Host is master: only host's dialog drives the broadcast.
        if (!net.IsHost()) {
            PLOG_INFO(
                "[PULSAR] DialogSync V28: TX START suppressed on client "
                "(npc=0x{:08X}) - host-only sync"sv, npcFormID);
            return;
        }

        // V26 - post-END cooldown. If we just ended a dialog with this NPC,
        // suppress the broadcast for kEndCooldownMs to break engine auto-greet
        // echo loop. Engine repeats the dialog automatically when player
        // stands near; without this lockout the dialog never closes.
        if (IsNpcOnEndCooldown(npcFormID)) {
            PLOG_INFO(
                "[PULSAR] DialogSync V26: BroadcastStart blocked - NPC 0x{:08X} "
                "on post-END cooldown ({}ms remaining)"sv,
                npcFormID,
                static_cast<int>(kEndCooldownMs -
                    (::GetTickCount64() - g_lastEndedMs.load())));
            return;
        }

        // V24 - speaker-change dedupe. Skip only if we already broadcasted
        // START for THIS exact npcFormID. Different NPC = scene-cycled
        // through speakers, push a new START so Client switches menu.
        std::uint32_t prevNpc = s_lastBroadcastNpc.exchange(
            npcFormID, std::memory_order_acq_rel);
        if (prevNpc == npcFormID) return;

        // Mark "in dialogue" - kept for legacy V18 BroadcastDialogEnd
        // logic + DRAIN dedupe paths that still consult this flag.
        s_lastBroadcastInDialogue.store(true, std::memory_order_release);

        std::uint8_t buf[1 + sizeof(Pulsar::Network::DialogStartData)]{};
        buf[0] = Pulsar::Network::PKT_DIALOG_START;
        Pulsar::Network::DialogStartData payload{ LocalSlot(), npcFormID };
        std::memcpy(buf + 1, &payload, sizeof(payload));
        net.Send(buf, sizeof(buf), /*reliable=*/true);

        PLOG_INFO(
            "[PULSAR] DialogSync TX: START slot={} npc=0x{:08X} (V24 prevNpc=0x{:08X})"sv,
            payload.playerSlot, npcFormID, prevNpc);
    }

    static void BroadcastDialogEnd()
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        // V28 - host-only sync. Client never broadcasts END.
        if (!net.IsHost()) return;

        bool wasIn = s_lastBroadcastInDialogue.exchange(false,
                                                       std::memory_order_acq_rel);
        if (!wasIn) return;

        // V26 - capture which NPC we just ended dialog with, BEFORE clearing
        // s_lastBroadcastNpc, so the cooldown check can identify echo loops.
        std::uint32_t endedNpc = s_lastBroadcastNpc.load(std::memory_order_acquire);
        MarkNpcEnded(endedNpc);

        // V24 - clear speaker tracker so the next dialog (even if it's
        // with the same NPC we just left) gets a fresh START broadcast.
        s_lastBroadcastNpc.store(0, std::memory_order_release);

        std::uint8_t buf[1 + sizeof(Pulsar::Network::DialogEndData)]{};
        buf[0] = Pulsar::Network::PKT_DIALOG_END;
        Pulsar::Network::DialogEndData payload{ LocalSlot() };
        std::memcpy(buf + 1, &payload, sizeof(payload));
        net.Send(buf, sizeof(buf), /*reliable=*/true);

        PLOG_INFO(
            "[PULSAR] DialogSync TX: END slot={}"sv,
            payload.playerSlot);
    }

    static void BroadcastVote(std::uint32_t topicInfoFormID,
                              std::uint32_t speakerFormID)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;
        if (!topicInfoFormID) return;

        // V28 - host-only sync. Client doesn't broadcast TopicInfo lines.
        if (!net.IsHost()) return;

        std::uint8_t buf[1 + sizeof(Pulsar::Network::DialogVoteData)]{};
        buf[0] = Pulsar::Network::PKT_DIALOG_VOTE;
        Pulsar::Network::DialogVoteData payload{
            LocalSlot(),
            topicInfoFormID,
            speakerFormID  // say-mirror: receiver needs the speaker to build the Fire handle
        };
        std::memcpy(buf + 1, &payload, sizeof(payload));
        net.Send(buf, sizeof(buf), /*reliable=*/true);

        PLOG_INFO(
            "[PULSAR] DialogSync TX: VOTE slot={} topicInfo=0x{:08X} speaker=0x{:08X}"sv,
            payload.playerSlot, topicInfoFormID, speakerFormID);
    }

    // V35 (2026-06-30): say-mirror master switch. The opening tutorial dialogue
    // is a quest SCENE that runs on BOTH machines (the user observed the NPCs
    // gesture/animate locally), so the client already plays each line with voice
    // AND gestures. Mirroring the host's voice on top fought the local scene and
    // (with the V34 dedup) stalled dialogue after the first exchange. Default
    // OFF: let the local scene own scene dialogue. Flip on only if testing shows
    // host-only lines the client never plays.
    static bool s_sayMirrorEnabled = false;

    struct TopicInfoSink : public RE::BSTEventSink<TESTopicInfoEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(
            const TESTopicInfoEvent& e,
            RE::BSTEventSource<TESTopicInfoEvent>*) override
        {
            // Diagnostic dump - first N events get logged with raw payload
            // so we can verify the +0x10 / +0x14 layout matches reality.
            int rem = s_diagDumpRemaining.load(std::memory_order_acquire);
            if (rem > 0 &&
                s_diagDumpRemaining.compare_exchange_strong(
                    rem, rem - 1, std::memory_order_acq_rel))
            {
                std::uint64_t b0 = 0, b1 = 0, b2 = 0;
                __try {
                    auto* p = reinterpret_cast<const std::uint64_t*>(&e);
                    b0 = p[0]; b1 = p[1]; b2 = p[2];
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                PLOG_INFO(
                    "[PULSAR] DialogSync DIAG TopicInfo: bytes=[{:016x} {:016x} {:016x}]"sv,
                    b0, b1, b2);
            }

            // Echo suppression - DrainPending fires this same path on RX
            // replay, we MUST NOT rebroadcast.
            if (tls_applyingRemoteTopic) {
                return RE::BSEventNotifyControl::kContinue;
            }

            std::uint32_t isEnd = 0;
            std::uint32_t formID = 0;
            RE::TESObjectREFR* speaker = nullptr;
            __try {
                isEnd   = e.isEnd;
                formID  = e.formID;
                speaker = e.speaker;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Capture only line-START fires - agent confirmed (param_2==0
            // = engine's start, param_2==1 = end). Broadcasting on end
            // would double-fire on the receiver.
            if (isEnd != 0) return RE::BSEventNotifyControl::kContinue;
            if (!formID)    return RE::BSEventNotifyControl::kContinue;

            std::uint32_t speakerFormID = SafeRawFormID(speaker);
            std::uint8_t  speakerType   = SafeRawFormType(speaker);

            // Skip the player's own lines and procedural/runtime refs. FF-prefix
            // formIDs are dynamically created and NOT stable across machines, so
            // the peer can't resolve them - only persistent NPCs (Lin/Heller/
            // Barrett/...) are mirrorable.
            const bool isProcRef = (speakerFormID & 0xFF000000) == 0xFF000000;
            if (speakerFormID == 0 || speakerFormID == 0x14 || isProcRef)
                return RE::BSEventNotifyControl::kContinue;

            // V33 - say-mirror scope. The old gate required the DialogueMenu to
            // be open, which dropped EVERY event-driven scene/forced/ambient
            // line (no menu) - exactly the dialogue we now want to mirror.
            // Replace it with a proximity gate: mirror lines from NPCs near the
            // host player (the shared scene), which keeps city-wide ambient
            // banter from far NPCs out.
            constexpr float kSayDist2 = 6000.0f * 6000.0f;   // ~60m, "same scene"
            if (SafeDist2ToPlayer(speaker) > kSayDist2)
                return RE::BSEventNotifyControl::kContinue;

            // V36: this persistent NPC is speaking a scene line near the player,
            // so it's in a scripted scene. Tell NPCSync to suspend its host->
            // client DirectControl puppet so its local scene (path + animations +
            // dialogue) plays out instead of being frozen in place. Refreshes the
            // per-NPC scene window on every line; expires shortly after the scene.
            Pulsar::NPCSync::MarkNpcInScene(speakerFormID);

            s_lastTopicInfoMs.store(::GetTickCount64(),
                                    std::memory_order_release);
            s_lastTargetFormID.store(speakerFormID, std::memory_order_release);

            // EventTrace timeline.
            char spkName[192], lineName[192];
            Pulsar::EventTrace::NameOfBuf(speakerFormID, spkName, sizeof(spkName));
            Pulsar::EventTrace::NameOfBuf(formID, lineName, sizeof(lineName));
            Pulsar::EventTrace::Trace("DIALOG", "line speaker=%s topicInfo=%s",
                spkName, lineName);

            // Say-mirror: broadcast the line so the peer's copy of this NPC
            // speaks it (voice + subtitle + lipsync, no menu). V35: gated OFF -
            // scene dialogue runs locally on both machines, so the mirror just
            // fights the client's own scene. See s_sayMirrorEnabled note.
            if (s_sayMirrorEnabled) {
                BroadcastVote(formID, speakerFormID);
            }

            // Menu-mirror (BG3 beta path) - ONLY when the host's dialogue menu
            // is actually open. Never fire START for scene/ambient lines, so the
            // client is not dragged into a menu / Activate-replay for NPC banter.
            if (IsDialogueMenuOpenSafe()) {
                std::uint32_t prevSeen = s_lastBroadcastNpc.load(
                    std::memory_order_acquire);
                BroadcastDialogStart(speakerFormID);
                if (prevSeen != speakerFormID) {
                    PLOG_INFO(
                        "[PULSAR] DialogSync V24: speaker=0x{:08X} type=0x{:02X} "
                        "(prev=0x{:08X}) - broadcasting START (menu open)"sv,
                        speakerFormID, speakerType, prevSeen);
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static TopicInfoSink g_topicSink;

    struct PlayerInDialogueChangedSink
        : public RE::BSTEventSink<PlayerInDialogueChangedEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(
            const PlayerInDialogueChangedEvent& e,
            RE::BSTEventSource<PlayerInDialogueChangedEvent>*) override
        {
            // V10 unconditional diagnostic so we know if this event is
            // even firing for the user's dialog flow (current logs show
            // it doesn't fire for scene-style multi-NPC dialogue).
            bool inDialogue = false;
            __try { inDialogue = e.inDialogue; }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                PLOG_INFO("[PULSAR] DialogSync DIAG: InDialogue SEH"sv);
                return RE::BSEventNotifyControl::kContinue;
            }
            PLOG_INFO(
                "[PULSAR] DialogSync DIAG: PlayerInDialogueChanged fired "
                "inDialogue={} (suppressed={})"sv,
                inDialogue ? 1 : 0,
                tls_applyingRemoteDialogOpen ? 1 : 0);

            Pulsar::EventTrace::Trace("DIALOG", "player inDialogue=%d",
                inDialogue ? 1 : 0);

            if (tls_applyingRemoteDialogOpen) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (inDialogue) {
                BroadcastDialogStart(s_lastTargetFormID.load(
                    std::memory_order_acquire));
            } else {
                s_lastTargetFormID.store(0, std::memory_order_release);
                BroadcastDialogEnd();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static PlayerInDialogueChangedSink g_inDialogueSink;

    struct DialogTargetSink
        : public RE::BSTEventSink<TESOnPCDialogueTargetEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(
            const TESOnPCDialogueTargetEvent& e,
            RE::BSTEventSource<TESOnPCDialogueTargetEvent>*) override
        {
            RE::TESObjectREFR* target = nullptr;
            __try { target = e.target; }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return RE::BSEventNotifyControl::kContinue;
            }

            std::uint32_t formID = SafeRawFormID(target);
            std::uint8_t  fType  = SafeRawFormType(target);

            // V17 - diagnostic so we can see if this event fires for
            // E-press dialog. If yes, we have the real NPC FormID.
            PLOG_INFO(
                "[PULSAR] DialogSync V17 DIAG TgtEvt: target=0x{:x} "
                "formID=0x{:08X} formType=0x{:02X}"sv,
                reinterpret_cast<std::uintptr_t>(target),
                formID, fType);

            if (!target) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (!formID || formID == 0x14) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // V17 - only treat ACHR (0x40 = Actor reference) as real NPC.
            // Anything else (BGSScene 0x88, BGSDialogueBranch 0x81, etc.)
            // is NOT a dialog partner - skip.
            if (fType != 0x40) {
                PLOG_INFO(
                    "[PULSAR] DialogSync V17: target type=0x{:02X} not ACHR - "
                    "ignoring (only Actor refs are real dialog partners)"sv,
                    fType);
                return RE::BSEventNotifyControl::kContinue;
            }

            s_lastTargetFormID.store(formID, std::memory_order_release);

            char tgtName[192];
            Pulsar::EventTrace::NameOfBuf(formID, tgtName, sizeof(tgtName));
            Pulsar::EventTrace::Trace("DIALOG", "target NPC=%s", tgtName);

            // V17 - broadcast START from THIS sink (the proper one).
            if (!s_lastBroadcastInDialogue.load(std::memory_order_acquire) &&
                !tls_applyingRemoteDialogOpen)
            {
                BroadcastDialogStart(formID);
                s_lastTopicInfoMs.store(::GetTickCount64(),
                                        std::memory_order_release);
                PLOG_INFO(
                    "[PULSAR] DialogSync V17: broadcast START via DialogTargetSink "
                    "(real ACHR NPC=0x{:08X})"sv, formID);
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static DialogTargetSink g_targetSink;

    static std::atomic<bool> s_initDone{ false };

    static void* ResolveSingletonRaw(std::uintptr_t rva) noexcept
    {
        std::uintptr_t moduleBase = 0;
        __try {
            moduleBase = REX::FModule::GetExecutingModule().GetBaseAddress();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        return reinterpret_cast<void*>(moduleBase + rva);
    }

    // ===== V34 say-mirror dedup (2026-06-30) =================================
    // The opening tutorial scene (and any quest scene) runs on BOTH machines,
    // so a shared NPC line is said TWICE on the client: once by the client's
    // own local scene, once by the mirror replay from the host (host leads by
    // ~15s). Confirmed in the 20:40 trace: line 0x179C0 played at 20:38:15
    // (mirror RX-SAY) and again 20:38:30 (local scene).
    //
    // Fix: hook TESTopicInfo::Fire and suppress the SECOND, cross-source play
    // of the same line within a window. "First source wins", so the client
    // follows whoever spoke first - the host mirror, since it leads. Same-
    // source repeats (a scene legitimately re-saying a line) pass through.
    // Host-only lines (the local scene never produces them) are never matched,
    // so they still mirror normally. The logic is self-gating: the mirror only
    // replays on the client (BroadcastVote is host-only), so isMirror is never
    // true on the host and nothing is ever suppressed there.
    static TopicInfoFireFn g_origTopicFire = nullptr;
    static std::mutex      s_dedupMutex;
    struct DedupEntry { std::uint64_t ms; bool mirror; };
    static std::unordered_map<std::uint32_t, DedupEntry>     s_recentPlay;    // topicInfo -> last allowed start
    static std::unordered_map<std::uint32_t, std::uint64_t>  s_blockedStart;  // topicInfo -> last blocked start
    constexpr std::uint64_t kDedupWindowMs   = 45000;  // cross-source duplicate window (host lead ~15s)
    constexpr std::uint64_t kDedupEndMatchMs = 4000;   // suppress the end paired with a blocked start

    // Raw read of TESTopicInfo formID (+0x28). noexcept/SEH so a torn pointer
    // just yields 0 (no dedup) instead of crashing the dialogue path.
    static std::uint32_t SafeReadTopicFormID(void* info) noexcept
    {
        __try {
            return info ? *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(info) + 0x28) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // True if this Fire is a cross-source duplicate to suppress. No __try here
    // (mutex/map are unwindable objects; mixing with __try is C2712).
    static bool ShouldSuppressTopicPlay(std::uint32_t fid, int startEnd, bool isMirror)
    {
        if (!fid) return false;
        const std::uint64_t now = ::GetTickCount64();
        std::lock_guard g(s_dedupMutex);
        if (startEnd == 0) {
            auto it = s_recentPlay.find(fid);
            if (it != s_recentPlay.end()
                && now - it->second.ms <= kDedupWindowMs
                && it->second.mirror != isMirror) {
                // The OTHER source already started this line - duplicate.
                s_blockedStart[fid] = now;
                return true;
            }
            s_recentPlay[fid] = DedupEntry{ now, isMirror };
            return false;
        }
        // line-end / continuation: suppress only if we blocked its start, so a
        // suppressed line never leaves a dangling end fire.
        auto bit = s_blockedStart.find(fid);
        return (bit != s_blockedStart.end() && now - bit->second <= kDedupEndMatchMs);
    }

    static void HookedTopicFire(void* info, int startEnd,
                                std::uint32_t speakerHandle, char unk4)
    {
        const bool isMirror = tls_applyingRemoteTopic;
        const std::uint32_t fid = SafeReadTopicFormID(info);
        if (ShouldSuppressTopicPlay(fid, startEnd, isMirror)) {
            if (startEnd == 0) {
                PLOG_INFO(
                    "[PULSAR] DialogSync DEDUP: suppressed duplicate topicInfo=0x{:08X} "
                    "(this={}, other source already said it)"sv,
                    fid, isMirror ? "mirror" : "local");
            }
            return;
        }
        if (g_origTopicFire) g_origTopicFire(info, startEnd, speakerHandle, unk4);
    }

}  // anonymous namespace

void Init()
{
    if (s_initDone.exchange(true)) return;

    // Subscribe to PlayerInDialogueChanged
    void* inDialogueRaw = ResolveSingletonRaw(kRVA_PlayerInDialogueChanged);
    if (inDialogueRaw) {
        auto* src = reinterpret_cast<
            RE::BSTEventSource<PlayerInDialogueChangedEvent>*>(inDialogueRaw);
        __try { src->RegisterSink(&g_inDialogueSink); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN(
                "[PULSAR] DialogSync: PlayerInDialogueChanged RegisterSink threw"sv);
        }
        PLOG_INFO(
            "[PULSAR] DialogSync: PlayerInDialogueChanged sink registered "
            "(src=0x{:x}, RVA 0x{:X})"sv,
            reinterpret_cast<std::uintptr_t>(inDialogueRaw),
            kRVA_PlayerInDialogueChanged);
    } else {
        PLOG_WARN(
            "[PULSAR] DialogSync: PlayerInDialogueChanged singleton resolve failed"sv);
    }

    // V4 - resolve TopicInfo::Fire worker (RVA 0xD48170)
    // V7 - also resolve SetDialogueTarget worker (RVA 0x194A1C0)
    {
        std::uintptr_t moduleBase = 0;
        __try {
            moduleBase = REX::FModule::GetExecutingModule().GetBaseAddress();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (moduleBase) {
            g_topicInfoFire = reinterpret_cast<TopicInfoFireFn>(
                moduleBase + kRVA_TopicInfoFire);
            PLOG_INFO(
                "[PULSAR] DialogSync: TopicInfo::Fire resolved at 0x{:x} (RVA 0x{:X})"sv,
                reinterpret_cast<std::uintptr_t>(g_topicInfoFire),
                kRVA_TopicInfoFire);

            // V34 - hook Fire for client-side say-mirror dedup. Both the local
            // scene and the mirror replay funnel through here, so the hook can
            // suppress whichever says a shared line SECOND (see the dedup block
            // in the anonymous namespace). g_topicInfoFire keeps pointing at the
            // (now hooked) entry, so the mirror's own replay rides the dedup too
            // if the local scene already said the line, the mirror is the one
            // suppressed.
            void* fireFn = reinterpret_cast<void*>(moduleBase + kRVA_TopicInfoFire);
            MH_STATUS fst = MH_CreateHook(
                fireFn,
                reinterpret_cast<LPVOID>(&HookedTopicFire),
                reinterpret_cast<LPVOID*>(&g_origTopicFire));
            if (fst != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] DialogSync V34: MH_CreateHook Fire failed: {}"sv,
                    static_cast<int>(fst));
            } else {
                fst = MH_EnableHook(fireFn);
                PLOG_INFO(
                    "[PULSAR] DialogSync V34: TopicInfo::Fire dedup hook {} (RVA 0x{:X})"sv,
                    fst == MH_OK ? "installed" : "ENABLE-FAILED",
                    kRVA_TopicInfoFire);
            }

            // Say-mirror - resolve EmplaceHandle (object->handle) + the handle
            // manager singleton slot. Used on RX to build the speaker handle
            // Fire needs.
            g_emplaceHandle = reinterpret_cast<EmplaceHandleFn>(
                moduleBase + kRVA_EmplaceHandle);
            g_handleMgrSlot = reinterpret_cast<void**>(
                moduleBase + kRVA_HandleMgrSlot);
            PLOG_INFO(
                "[PULSAR] DialogSync: EmplaceHandle @ 0x{:x} (RVA 0x{:X}), "
                "handleMgr slot @ 0x{:x}"sv,
                reinterpret_cast<std::uintptr_t>(g_emplaceHandle),
                kRVA_EmplaceHandle,
                reinterpret_cast<std::uintptr_t>(g_handleMgrSlot));
            g_setDialogueTarget = reinterpret_cast<SetDialogueTargetFn>(
                moduleBase + kRVA_SetDialogueTarget);
            PLOG_INFO(
                "[PULSAR] DialogSync: SetDialogueTarget resolved at 0x{:x} (RVA 0x{:X})"sv,
                reinterpret_cast<std::uintptr_t>(g_setDialogueTarget),
                kRVA_SetDialogueTarget);

            // V13 - install MinHook on FUN_14194CA60 to capture all
            // engine SetDialogueTarget calls.
            void* setTgtFn = reinterpret_cast<void*>(
                moduleBase + kRVA_SetDialogueTarget);
            MH_STATUS sst = MH_CreateHook(
                setTgtFn,
                reinterpret_cast<LPVOID>(&HookedSetDialogTarget),
                reinterpret_cast<LPVOID*>(&g_origSetDialogTarget));
            if (sst != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] DialogSync V13: MH_CreateHook failed: {}"sv,
                    static_cast<int>(sst));
            } else {
                sst = MH_EnableHook(setTgtFn);
                if (sst != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] DialogSync V13: MH_EnableHook failed: {}"sv,
                        static_cast<int>(sst));
                } else {
                    PLOG_INFO(
                        "[PULSAR] DialogSync V13: SetDialogueTarget hook installed"sv);
                }
            }

            // V20 - install MinHook on FUN_141795760 (scene-less E-press
            // dialog opener). Captures the path V13 SetCurrentScene
            // misses (free-form NPC chat with no quest scene wrapper).
            g_openDialog = reinterpret_cast<OpenDialogFn>(
                moduleBase + kRVA_OpenDialog);
            void* openFn = reinterpret_cast<void*>(
                moduleBase + kRVA_OpenDialog);
            MH_STATUS ost = MH_CreateHook(
                openFn,
                reinterpret_cast<LPVOID>(&HookedOpenDialog),
                reinterpret_cast<LPVOID*>(&g_origOpenDialog));
            if (ost != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] DialogSync V20: MH_CreateHook OpenDialog failed: {}"sv,
                    static_cast<int>(ost));
            } else {
                ost = MH_EnableHook(openFn);
                if (ost != MH_OK) {
                    PLOG_WARN(
                        "[PULSAR] DialogSync V20: MH_EnableHook OpenDialog failed: {}"sv,
                        static_cast<int>(ost));
                } else {
                    PLOG_INFO(
                        "[PULSAR] DialogSync V20: OpenDialog hook installed at "
                        "0x{:x} (RVA 0x{:X})"sv,
                        reinterpret_cast<std::uintptr_t>(openFn),
                        kRVA_OpenDialog);
                }
            }

            // V20 - resolve MenuTopicManager singleton address. Object
            // lives at this address directly (not a pointer-to-pointer).
            g_mtm = reinterpret_cast<void*>(moduleBase + kRVA_MTM);
            PLOG_INFO(
                "[PULSAR] DialogSync V20: MenuTopicManager resolved at 0x{:x}"sv,
                reinterpret_cast<std::uintptr_t>(g_mtm));

            // V25 - REMOVED. Diagnostic hooks on FUN_141793640 (2 args, fine) +
            // FUN_141793d10 (6 args, our 2-arg hook truncated R8/R9/stack →
            // corrupted engine state for vanilla dialogs → menu never opened
            // for Sarah/Vendor). Lesson: never hook without verified signature.
        } else {
            PLOG_WARN(
                "[PULSAR] DialogSync: module base resolve failed - V4/V7 RX disabled"sv);
        }
    }

    // V9 - cache DialogueCamera setting pointer. Setting* is global +
    // persistent so safe to cache once.
    {
        auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
        if (prefs) {
            __try {
                g_dialogueCameraSetting = prefs->GetSetting(
                    "bDialogueEnable:Interface");
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_dialogueCameraSetting = nullptr;
            }
            if (g_dialogueCameraSetting) {
                PLOG_INFO(
                    "[PULSAR] DialogSync: bDialogueEnable:Interface cached "
                    "(currentValue={})"sv,
                    g_dialogueCameraSetting->GetBool() ? 1 : 0);
            } else {
                PLOG_WARN(
                    "[PULSAR] DialogSync: bDialogueEnable:Interface lookup failed"sv);
            }
        } else {
            PLOG_WARN(
                "[PULSAR] DialogSync: INIPrefSettingCollection null"sv);
        }
    }

    // V4 - subscribe to TESTopicInfoEvent for line-by-line capture
    void* topicRaw = ResolveSingletonRaw(kRVA_TESTopicInfoEvent);
    if (topicRaw) {
        auto* src = reinterpret_cast<
            RE::BSTEventSource<TESTopicInfoEvent>*>(topicRaw);
        __try { src->RegisterSink(&g_topicSink); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN(
                "[PULSAR] DialogSync: TESTopicInfoEvent RegisterSink threw"sv);
        }
        PLOG_INFO(
            "[PULSAR] DialogSync: TESTopicInfoEvent sink registered "
            "(src=0x{:x}, RVA 0x{:X})"sv,
            reinterpret_cast<std::uintptr_t>(topicRaw),
            kRVA_TESTopicInfoEvent);
    } else {
        PLOG_WARN(
            "[PULSAR] DialogSync: TESTopicInfoEvent singleton resolve failed"sv);
    }

    // Subscribe to TESOnPCDialogueTargetEvent
    void* targetRaw = ResolveSingletonRaw(kRVA_TESOnPCDialogueTarget);
    if (targetRaw) {
        auto* src = reinterpret_cast<
            RE::BSTEventSource<TESOnPCDialogueTargetEvent>*>(targetRaw);
        __try { src->RegisterSink(&g_targetSink); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_WARN(
                "[PULSAR] DialogSync: TESOnPCDialogueTarget RegisterSink threw"sv);
        }
        PLOG_INFO(
            "[PULSAR] DialogSync: TESOnPCDialogueTarget sink registered "
            "(src=0x{:x}, RVA 0x{:X})"sv,
            reinterpret_cast<std::uintptr_t>(targetRaw),
            kRVA_TESOnPCDialogueTarget);
    } else {
        PLOG_WARN(
            "[PULSAR] DialogSync: TESOnPCDialogueTarget singleton resolve failed"sv);
    }
}

void Shutdown()
{
    // No-op: engine event sources outlive the plugin. Sinks are static
    // members; they remain registered until process exit.
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    const std::uint8_t pktType = data[0];

    if (pktType == PKT_DIALOG_START) {
        if (length < 1 + sizeof(DialogStartData)) return;
        DialogStartData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        PLOG_INFO(
            "[PULSAR] DialogSync RX: START slot={} npc=0x{:08X} "
            "(queueing engine-natural dialog open)"sv,
            d.playerSlot, d.npcFormID);

        if (!d.npcFormID || d.npcFormID == 0x14) return;

        // V26 - drop incoming START if this NPC is on post-END cooldown.
        // Breaks the engine auto-greet echo loop: peer's auto-greet right
        // after END would otherwise re-open the menu we just closed.
        if (IsNpcOnEndCooldown(d.npcFormID)) {
            PLOG_INFO(
                "[PULSAR] DialogSync V26: RX START dropped - NPC 0x{:08X} "
                "on post-END cooldown ({}ms remaining)"sv,
                d.npcFormID,
                static_cast<int>(kEndCooldownMs -
                    (::GetTickCount64() - g_lastEndedMs.load())));
            return;
        }

        // Queue for game-thread DrainPending - Activate is game-thread-only
        // (touches cell-streamer + animation graph + UI message queue).
        std::lock_guard g(s_pendingPicksMutex);
        s_pendingDialogOpens.push_back(d.npcFormID);
        return;
    }

    if (pktType == PKT_DIALOG_END) {
        if (length < 1 + sizeof(DialogEndData)) return;
        DialogEndData d{};
        std::memcpy(&d, data + 1, sizeof(d));
        PLOG_INFO(
            "[PULSAR] DialogSync RX: END slot={} (queueing dialog close)"sv,
            d.playerSlot);

        // V22 - clear local broadcast flag. V23 made UI::IsMenuOpen the
        // canonical "in dialog" guard, but this flag is still consulted
        // on a few legacy paths.
        s_lastBroadcastInDialogue.store(false, std::memory_order_release);

        // V26 - also start the post-END cooldown for the NPC we were
        // talking to (peer-broadcasted END means peer just left a dialog
        // with someone; we need cooldown on our own auto-greet for that
        // NPC too). Use s_lastTargetFormID before we clear it.
        std::uint32_t endedNpc = s_lastTargetFormID.load(std::memory_order_acquire);
        if (endedNpc == 0) endedNpc = s_lastBroadcastNpc.load(std::memory_order_acquire);
        MarkNpcEnded(endedNpc);

        s_lastTargetFormID.store(0, std::memory_order_release);
        s_lastV9OpenedNpc.store(0, std::memory_order_release);
        // V24 - reset speaker tracker so next session can broadcast START
        // even for the same NPC.
        s_lastBroadcastNpc.store(0, std::memory_order_release);

        std::lock_guard g(s_pendingPicksMutex);
        s_pendingDialogCloses.push_back(d.playerSlot);
        return;
    }

    // PKT_DIALOG_VOTE - Phase 3 V4. Peer picked a TopicInfo; queue for
    // game-thread replay via DrainPending. We can't replay here on the
    // network thread - TESTopicInfo::Fire is not thread-safe.
    if (pktType == PKT_DIALOG_VOTE) {
        if (length < 1 + sizeof(DialogVoteData)) return;
        DialogVoteData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        PLOG_INFO(
            "[PULSAR] DialogSync RX: VOTE slot={} topicInfo=0x{:08X} speaker=0x{:08X} "
            "(queued for game-thread say-replay)"sv,
            d.playerSlot, d.topicInfoFormID, d.speakerFormID);

        std::lock_guard g(s_pendingPicksMutex);
        s_pendingPicks.push_back({
            d.topicInfoFormID,
            d.speakerFormID,  // say-mirror: who speaks the line
            d.playerSlot
        });
        return;
    }
}

void Reset()
{
    s_lastTargetFormID.store(0, std::memory_order_release);
    s_lastBroadcastInDialogue.store(false, std::memory_order_release);
    s_lastBroadcastNpc.store(0, std::memory_order_release);  // V24
    {
        std::lock_guard g(s_pendingPicksMutex);
        s_pendingPicks.clear();
        s_pendingDialogOpens.clear();
        s_pendingDialogCloses.clear();
    }
    tls_applyingRemoteTopic     = false;
    tls_applyingRemoteDialogOpen = false;
    s_lastTopicInfoMs.store(0, std::memory_order_release);
    s_lastV9OpenedNpc.store(0, std::memory_order_release);
    PLOG_INFO("[PULSAR] DialogSync: Reset"sv);
}

// SEH-only helper for the worker call. C2712 forbids __try in functions
// containing object-unwinding scopes (locks, std::vector ops).
namespace {
    // Say-mirror - build the speaker REFR handle (Fire's arg3) from an actor
    // pointer via HandleManager::EmplaceHandle. Returns 0 on failure. SEH-safe.
    static std::uint32_t SafeMakeRefHandle(void* refr) noexcept
    {
        if (!refr || !g_emplaceHandle || !g_handleMgrSlot) return 0;
        __try {
            void* mgr = *g_handleMgrSlot;   // singleton; null until engine init
            if (!mgr) return 0;
            return g_emplaceHandle(mgr, refr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static void SafeFireTopic(void* info, int startEnd, std::uint32_t speakerHandle) noexcept
    {
        if (!info || !g_topicInfoFire) return;
        __try { g_topicInfoFire(info, startEnd, speakerHandle, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // V14 proximity check helper - wraps the raw +0x8C reads in SEH so
    // DrainPending (which has lock_guard scopes) can call this without
    // C2712 conflict.
    static float SafeDistanceSquared(const void* a, const void* b) noexcept
    {
        if (!a || !b) return 1e30f;
        __try {
            const float* pa = reinterpret_cast<const float*>(
                reinterpret_cast<const std::uint8_t*>(a) + 0x8C);
            const float* pb = reinterpret_cast<const float*>(
                reinterpret_cast<const std::uint8_t*>(b) + 0x8C);
            float dx = pa[0] - pb[0];
            float dy = pa[1] - pb[1];
            float dz = pa[2] - pb[2];
            return dx*dx + dy*dy + dz*dz;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 1e30f;
        }
    }

    static void SafeSetDialogueTarget(void* actor, void* target) noexcept
    {
        // V13 - call the trampoline (orig) instead of the patched fn so
        // our own RX-replay path doesn't recurse through the hook.
        auto* fn = g_origSetDialogTarget ? g_origSetDialogTarget : g_setDialogueTarget;
        if (!actor || !fn) return;
        __try { fn(actor, target); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // V13 - hook FUN_14194CA60 to catch the engine's E-press dialog
    // engagement directly. V12's player+0x238 polling never observed
    // a transition, so either the offset is wrong or this function is
    // bypassed by the E-press path. Hooking captures every call regardless
    // of code path so we can see who/what triggers it.
    // V20 - hook FUN_141795760 to capture E-press / scene-less dialog
    // open. This is the engine path that V13 SetCurrentScene hook
    // missed entirely. Args:
    //   mtm = MenuTopicManager*
    //   speaker = TESObjectREFR* NPC actor (the one player engaged)
    //   u1=0, u2=1, u3=0 - open params
    // After this returns, MTM+0xA8 = speaker, dialog menu is open.
    static void __fastcall HookedOpenDialog(void* mtm, void* speaker,
                                            int u1, int u2, int u3)
    {
        __try {
            if (g_origOpenDialog) g_origOpenDialog(mtm, speaker, u1, u2, u3);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        if (tls_applyingRemoteDialogOpen) return;
        if (!speaker) return;

        std::uint32_t fid = SafeRawFormID(speaker);
        std::uint8_t  ft  = SafeRawFormType(speaker);

        PLOG_INFO(
            "[PULSAR] DialogSync V20 HOOK: OpenDialog(MTM, speaker=0x{:08X} "
            "type=0x{:02X})"sv, fid, ft);

        // V21 - args u2/u3 turned out to be stack garbage (function takes
        // 2 args, not 5 as RE-agent guessed). Drop the open/close
        // discrimination - V13 SetCurrentScene hook handles END on
        // form=0. V20 hook only broadcasts START for E-press paths
        // (scene-less) that V13 misses entirely.
        const bool isProcRef = (fid & 0xFF000000) == 0xFF000000;
        if (fid != 0 && fid != 0x14 && !isProcRef &&
            !s_lastBroadcastInDialogue.load(std::memory_order_acquire))
        {
            s_lastTargetFormID.store(fid, std::memory_order_release);
            BroadcastDialogStart(fid);
            s_lastTopicInfoMs.store(::GetTickCount64(),
                                    std::memory_order_release);
            PLOG_INFO(
                "[PULSAR] DialogSync V20: dialog open detected - "
                "broadcasting START with NPC=0x{:08X}"sv, fid);
        }
    }

    static void __fastcall HookedSetDialogTarget(void* actor, void* target)
    {
        __try {
            if (g_origSetDialogTarget) g_origSetDialogTarget(actor, target);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        if (tls_applyingRemoteDialogOpen) return;

        std::uint32_t actorFid = SafeRawFormID(actor);
        std::uint32_t tgtFid   = SafeRawFormID(target);
        if (actorFid != 0x14) return;

        // V17 - also read formType to identify what we're really dealing
        // with. FUN_14194CA60 turned out to set Actor.currentScene
        // (BGSScene form, formType 0x88), NOT dialog-target. Only
        // ACHR (0x40) targets are real NPC refs we should broadcast.
        const std::uint8_t tgtType = SafeRawFormType(target);

        PLOG_INFO(
            "[PULSAR] DialogSync V13 HOOK: SetCurrentScene(player, "
            "form=0x{:08X} type=0x{:02X})"sv,
            tgtFid, tgtType);

        // V18 - use scene-exit (form=0) as canonical session-end. Engine
        // wraps every dialog in a BGSScene; when scene clears, dialog is
        // over. This replaces the unreliable idle-timeout for END.
        if (tgtFid == 0 &&
            s_lastBroadcastInDialogue.load(std::memory_order_acquire))
        {
            PLOG_INFO(
                "[PULSAR] DialogSync V18: scene exited - broadcasting END"sv);
            BroadcastDialogEnd();
            s_lastTargetFormID.store(0, std::memory_order_release);
            s_lastV9OpenedNpc.store(0, std::memory_order_release);
        }
    }

    // V25 hook bodies REMOVED. FUN_141793d10 has 6-param signature; our
    // 2-arg hook truncated R8/R9/stack args, breaking vanilla dialog flow.
}

void DrainPending()
{
    // V10 - idle-END detector: if we're tracking an active session and
    // no TopicInfo event fired for 5+ seconds, synthesize END so client
    // restores camera setting + clears dialog-target.
    if (s_lastBroadcastInDialogue.load(std::memory_order_acquire)) {
        std::uint64_t lastMs = s_lastTopicInfoMs.load(std::memory_order_acquire);
        std::uint64_t nowMs  = ::GetTickCount64();
        if (lastMs > 0 && (nowMs - lastMs) > kSessionIdleMs) {
            PLOG_INFO(
                "[PULSAR] DialogSync V11: idle {}s - synthesizing END"sv,
                static_cast<int>(kSessionIdleMs / 1000));
            BroadcastDialogEnd();
            s_lastTargetFormID.store(0, std::memory_order_release);
            s_lastV9OpenedNpc.store(0, std::memory_order_release);
        }
    }

    // V12 - poll player.dialogTarget transitions. If host pressed E on
    // an NPC the engine sets player+0x238 = NPC. Detecting this transition
    // gives us the clean "player engaged conversation X" signal that
    // PlayerInDialogueChanged doesn't reliably provide. Game-thread only
    // (DrainPending runs on game thread), no SEH issue.
    {
        static std::uint32_t s_polledTarget = 0;
        std::uint32_t curTarget = SafeReadPlayerDialogTarget();
        if (curTarget != s_polledTarget) {
            PLOG_INFO(
                "[PULSAR] DialogSync V12: player.dialogTarget transition "
                "0x{:08X} → 0x{:08X}"sv,
                s_polledTarget, curTarget);
            s_polledTarget = curTarget;

            const bool isProcRef = (curTarget & 0xFF000000) == 0xFF000000;
            if (curTarget != 0 && curTarget != 0x14 && !isProcRef &&
                !s_lastBroadcastInDialogue.load(std::memory_order_acquire))
            {
                s_lastTargetFormID.store(curTarget,
                                         std::memory_order_release);
                BroadcastDialogStart(curTarget);
                s_lastTopicInfoMs.store(::GetTickCount64(),
                                        std::memory_order_release);
            } else if (curTarget == 0 &&
                       s_lastBroadcastInDialogue.load(std::memory_order_acquire))
            {
                BroadcastDialogEnd();
                s_lastTargetFormID.store(0, std::memory_order_release);
                s_lastV9OpenedNpc.store(0, std::memory_order_release);
            }
        }
    }

    // Snapshot all pending queues under one lock.
    std::vector<PendingPick>   localPicks;
    std::vector<std::uint32_t> localOpens;
    std::vector<std::uint8_t>  localCloses;
    {
        std::lock_guard g(s_pendingPicksMutex);
        localPicks.swap(s_pendingPicks);
        localOpens.swap(s_pendingDialogOpens);
        localCloses.swap(s_pendingDialogCloses);
    }

    // V14 - re-enable V9 with proximity gate. The freeze in V6/V9 was
    // caused by the engine's dialog system trying to focus a player
    // who is far from the target NPC (input-gates lock with no menu
    // visible). With a distance check we only fire Activate when the
    // local player is within talking range - same constraint Bethesda's
    // own E-press has. Both players physically together = both see the
    // dialog (BG3 party-gather pattern). Far apart = outcome-only sync.
    // V15 - bumped from 500u (E-press range) to 5000u (~50m). Logs
    // showed user testing at 3327u away, which is "same area" coop
    // distance. Engine freeze in V6 was at much-larger distances (or
    // different cell entirely). 50m gives BG3 "same room" flexibility
    // without inviting freezes from cross-cell Activate attempts.
    constexpr float kDialogDist     = 5000.0f;
    constexpr float kDialogDist2    = kDialogDist * kDialogDist;

    // V27 - only force bDialogueEnable=0 for procedural NPCs (FD/FF prefix).
    // V14 was added to skip camera-transition freeze on far NPCs, but at
    // the 5000u proximity gate vanilla camera works fine. For VANILLA NPCs
    // (non-procedural formIDs), bDialogueEnable=0 breaks menu opening -
    // logs 12:44 showed Activate POST-ACT dialogTarget=0 for Sarah/vendor,
    // meaning the menu state didn't transition because the engine relies
    // on camera-transition completion as the trigger for vanilla menu UI.
    bool anyProcedural = false;
    for (std::uint32_t fid : localOpens) {
        if ((fid & 0xFF000000) >= 0xFD000000) { anyProcedural = true; break; }
    }
    if (anyProcedural && g_dialogueCameraSetting && !g_dialogueCameraSaved) {
        g_dialogueCameraOriginal = g_dialogueCameraSetting->GetBool();
        g_dialogueCameraSaved = true;
        g_dialogueCameraSetting->SetBool(false);
        PLOG_INFO(
            "[PULSAR] DialogSync V14/V27: bDialogueEnable forced OFF "
            "(procedural NPC in batch - restored to {} on END)"sv,
            g_dialogueCameraOriginal ? 1 : 0);
    } else if (!localOpens.empty()) {
        PLOG_INFO(
            "[PULSAR] DialogSync V27: keeping bDialogueEnable as-is "
            "(vanilla NPCs need camera-transition for menu to open)"sv);
    }

    for (std::uint32_t npcFormID : localOpens) {
        // V23 - UI-menu-state guard (replaces V22 broken MTM+0xA8 read).
        // RE::UI::IsMenuOpen("DialogueMenu") is the authoritative signal:
        // true iff the dialogue menu is actually on screen. V22 misread
        // a non-pointer field at MTM+0xA8 and returned non-null even
        // when no menu was open, blocking every Activate replay.
        if (IsDialogueMenuOpenSafe()) {
            PLOG_INFO(
                "[PULSAR] DialogSync DRAIN: NPC 0x{:08X} skip - DialogueMenu "
                "already open (V23 UI check)"sv,
                npcFormID);
            continue;
        }

        // V11 dedupe.
        if (s_lastV9OpenedNpc.load(std::memory_order_acquire) == npcFormID) {
            PLOG_INFO(
                "[PULSAR] DialogSync DRAIN: NPC 0x{:08X} already opened - skip"sv,
                npcFormID);
            continue;
        }

        auto* form = RE::TESForm::LookupByID(npcFormID);
        if (!form) {
            PLOG_INFO(
                "[PULSAR] DialogSync DRAIN: NPC 0x{:08X} not loaded - drop"sv,
                npcFormID);
            continue;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) continue;

        // V16 - verbose pre-Activate state dump.
        const std::uint8_t formType    = SafeRawFormType(form);
        const std::uint32_t formCellID = SafeReadFormCell(form);
        std::uint64_t formBytes[4];
        SafeDumpForm32(form, formBytes);

        float playerPos[3], npcPos[3];
        SafeReadPlayerPos(playerPos);
        SafeReadFormPos(form, npcPos);

        const std::uintptr_t tgtPtrBefore = SafeReadPlayerTargetPtr();

        const float dist2  = SafeDistanceSquared(form, player);
        const bool  tooFar = dist2 > kDialogDist2;

        PLOG_INFO(
            "[PULSAR] DialogSync V16 PRE: npc=0x{:08X} formPtr=0x{:x} "
            "type=0x{:02X} cellID=0x{:08X}"sv,
            npcFormID,
            reinterpret_cast<std::uintptr_t>(form),
            formType, formCellID);
        PLOG_INFO(
            "[PULSAR] DialogSync V16 PRE: form bytes [{:016x} {:016x} {:016x} {:016x}]"sv,
            formBytes[0], formBytes[1], formBytes[2], formBytes[3]);
        PLOG_INFO(
            "[PULSAR] DialogSync V16 PRE: playerPos=({:.0f},{:.0f},{:.0f}) "
            "npcPos=({:.0f},{:.0f},{:.0f}) dist²={:.0f} tooFar={}"sv,
            playerPos[0], playerPos[1], playerPos[2],
            npcPos[0], npcPos[1], npcPos[2],
            dist2, tooFar ? 1 : 0);
        PLOG_INFO(
            "[PULSAR] DialogSync V16 PRE: player.dialogTarget=0x{:x}"sv,
            tgtPtrBefore);

        if (tooFar) {
            PLOG_INFO(
                "[PULSAR] DialogSync DRAIN: NPC 0x{:08X} too far - skip Activate"sv,
                npcFormID);
            continue;
        }

        s_lastV9OpenedNpc.store(npcFormID, std::memory_order_release);
        tls_applyingRemoteDialogOpen = true;

        PLOG_INFO("[PULSAR] DialogSync V16: calling SetDialogueTarget..."sv);
        if (g_setDialogueTarget) {
            SafeSetDialogueTarget(player, form);
        }
        const std::uintptr_t tgtPtrAfterSet = SafeReadPlayerTargetPtr();
        PLOG_INFO(
            "[PULSAR] DialogSync V16 POST-SET: player.dialogTarget=0x{:x}"sv,
            tgtPtrAfterSet);

        PLOG_INFO("[PULSAR] DialogSync V16: calling Activate..."sv);
        ActivateArgs args{};
        args.activator = reinterpret_cast<RE::TESObjectREFR*>(player);
        Pulsar::WorldSync::Loot::CallOrigActivateAsReplay(
            static_cast<RE::TESObjectREFR*>(form), &args);

        tls_applyingRemoteDialogOpen = false;

        const std::uintptr_t tgtPtrAfterAct = SafeReadPlayerTargetPtr();
        PLOG_INFO(
            "[PULSAR] DialogSync V16 POST-ACT: player.dialogTarget=0x{:x} "
            "(non-zero = engine accepted)"sv,
            tgtPtrAfterAct);

        PLOG_INFO(
            "[PULSAR] DialogSync DRAIN: V16 dialog-open NPC=0x{:08X} "
            "(dist²={:.0f}, in range - Activate fired)"sv,
            npcFormID, dist2);
    }

    // V7 - clear dialogue-target on END so engine state mirrors peer.
    for (std::uint8_t slot : localCloses) {
        if (!g_setDialogueTarget) continue;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) continue;

        // V11 - reset dedupe so next session can re-open.
        s_lastV9OpenedNpc.store(0, std::memory_order_release);

        tls_applyingRemoteDialogOpen = true;
        SafeSetDialogueTarget(player, nullptr);
        tls_applyingRemoteDialogOpen = false;

        PLOG_INFO(
            "[PULSAR] DialogSync DRAIN: SetDialogueTarget(player, null) "
            "for slot={} END"sv, slot);
    }

    // V9 - restore user's DialogCamera setting on END.
    if (!localCloses.empty() && g_dialogueCameraSetting && g_dialogueCameraSaved) {
        g_dialogueCameraSetting->SetBool(g_dialogueCameraOriginal);
        g_dialogueCameraSaved = false;
        PLOG_INFO(
            "[PULSAR] DialogSync V9: bDialogueEnable restored to {}"sv,
            g_dialogueCameraOriginal ? 1 : 0);
    }

    if (!g_topicInfoFire || localPicks.empty()) return;

    for (const auto& pick : localPicks) {
        auto* form = RE::TESForm::LookupByID(pick.topicInfoFormID);
        if (!form) {
            PLOG_INFO(
                "[PULSAR] DialogSync DRAIN: topicInfo 0x{:08X} not loaded - drop"sv,
                pick.topicInfoFormID);
            continue;
        }

        // TESTopicInfo's FormType is INFO (0x59). We don't reject other
        // types - defensive logging only - because LookupByID may
        // resolve to a child class we don't have a struct for.
        void* topicInfo = static_cast<void*>(form);

        // Say-mirror: resolve the speaker actor and build its REFR handle -
        // Fire's arg3. Without a valid speaker handle the line plays without
        // voice context (the old "pass topicInfo formID as handle" bug), so
        // skip if we can't build one.
        std::uint32_t speakerHandle = 0;
        if (pick.speakerFormID) {
            auto* spk = RE::TESForm::LookupByID(pick.speakerFormID);
            if (spk) speakerHandle = SafeMakeRefHandle(spk);
        }
        if (speakerHandle == 0) {
            PLOG_INFO(
                "[PULSAR] DialogSync DRAIN: no speaker handle (topicInfo=0x{:08X} "
                "speaker=0x{:08X}) - skip say"sv,
                pick.topicInfoFormID, pick.speakerFormID);
            continue;
        }

        tls_applyingRemoteTopic = true;
        SafeFireTopic(topicInfo, 0, speakerHandle);  // line-start: voice+subtitle+lipsync
        SafeFireTopic(topicInfo, 1, speakerHandle);  // line-end / continuation
        tls_applyingRemoteTopic = false;

        Pulsar::EventTrace::Trace("DIALOG", "RX-SAY applied speaker=0x%08X line=0x%08X",
            pick.speakerFormID, pick.topicInfoFormID);
        PLOG_INFO(
            "[PULSAR] DialogSync DRAIN say applied: topicInfo=0x{:08X} "
            "speaker=0x{:08X} handle=0x{:08X}"sv,
            pick.topicInfoFormID, pick.speakerFormID, speakerHandle);
    }
}

}  // namespace Pulsar::DialogSync
