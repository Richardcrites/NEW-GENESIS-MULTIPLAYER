#include "PCH.h"
#include "InventorySync/DirectRestore.h"
#include "WorldSync/LootSync.h"
#include "TimeWeatherSync/TimeWeatherSync.h"   // re-sync time/weather after each restore

#include <Windows.h>
#include <RE/A/Actor.h>
#include <RE/A/ActorValueInfo.h>
#include <RE/B/BGSInventoryItem.h>
#include <RE/B/BGSInventoryList.h>
#include <RE/B/BGSObjectInstance.h>
#include <RE/T/TBO_InstanceData.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/B/BGSPerk.h>
#include <RE/S/SpellItem.h>                 // Powers (gap #2): GetSpellType filter
#include <RE/M/MagicSystem.h>               // MagicSystem::SpellType
#include <RE/F/FormTypes.h>

#include <RE/E/ExtraDataList.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::InventorySync::DirectRestore
{
    namespace
    {
        // V4 - rich cache holding a refcounted copy of each stack's
        // ExtraDataList. Refcounting via BSTSmartPointer keeps the list
        // objects alive across the bridge, even when the engine drops
        // its own references while wiping/replacing the player's inventory.
        // Restoring with the held smartptrs makes Items come back with
        // their original ammo, mods, durability, equip flags, ownership,
        // etc. - fixes the "items unusable / can't drop" bug that the
        // V3.3 raw-count approach couldn't address.
        struct CachedStack {
            RE::BSTSmartPointer<RE::ExtraDataList> extra;
            std::uint32_t                          count = 0;
        };
        struct CachedItem {
            std::vector<CachedStack> stacks;
            bool                     equipped = false;
            // Phase B  - M.2 mod array (raw same-machine mod-form IDs) +
            // sentinel, carried ONLY for seed-sourced (DB-profile) items, which
            // are base forms with no ExtraData. After RestoreClientState re-adds
            // such an item, the mods are re-applied via LootSync::ApplyItemMods
            // (the working drop-path primitive). Empty for live-captured items -
            // those keep their mods through cs.extra (ExtraData), so applying
            // would double up; emptiness is the "live vs seed" discriminator.
            std::vector<std::uint32_t> mods;
            std::uint16_t              modSentinel = 0xFFFF;
        };
        std::unordered_map<RE::TESBoundObject*, CachedItem> g_cache;
        std::atomic<bool> g_cacheValid{ false };
        std::mutex        g_cacheMu;

        // ghostfix-v3 - host-exclusive worn base forms captured pre-wipe.
        // UnequipObject FAILS mid-bridge (engine still loading), so the raw wipe
        // removes the inventory entry while the worn-3D/equip binding dangles = a
        // worn item not in inventory (can't select/drop). We retry the unequip at
        // +5s in TickPostRestoreWatch, where the engine has settled. Storing the
        // base-form pointer (not the formID) sidesteps the abstract-base
        // LookupByID<TESBoundObject> null pitfall; base forms persist the session.
        std::vector<RE::TESBoundObject*> g_pendingGhostUnequip;

        // Phase C3 (co-op gains) - live inventory snapshot taken at +10s (restore
        // settled, dbAuth only). Delta reference for ClientProfile's gains-capture:
        // it carries the same restore artifacts + host credits as a later capture,
        // so (capture − snapshot) = pure session gains. formID → total count.
        std::unordered_map<std::uint32_t, std::uint32_t> g_gainsSnap;
        std::uint32_t     g_gainsSnapCredits = 0;
        std::uint32_t     g_gainsSnapLevel   = 0;   // V3 - body level at +10s (delta ref)
        float             g_gainsSnapXP      = 0.0f; // V3 - body "Experience" AV at +10s (delta ref)
        std::vector<std::uint32_t> g_gainsSnapPowers;   // Powers - live power SET at +10s
        std::vector<std::uint32_t> g_gainsSnapResearch; // Research - completed SET at +10s
        std::atomic<bool> g_gainsSnapValid{ false };
        // B1 fix (review 2026-06-19) - per-field snapshot validity. The powers/research
        // walks can SEH-fault to an EMPTY set; the overall snapshot is still valid for
        // inventory/credits/level/xp. Without these flags an empty (faulted) power/
        // research snapshot makes the set-delta treat the FOREIGN/host powers+research
        // as "gained" and absorb them into the DB permanently (monotonic poison). The
        // getters AND the merge are gated on these so a faulted walk keeps baseline.
        std::atomic<bool> g_gainsSnapPowersValid{ false };
        std::atomic<bool> g_gainsSnapResearchValid{ false };
        std::mutex        g_gainsSnapMu;
        constexpr std::uint32_t kCreditsFormID = 0x0000000Fu;

        // #poison-guard (overnight 2026-06-24) - host-EXCLUSIVE item forms the
        // bridge briefly placed in the client's live inventory (live forms NOT in
        // the client's restore cache), accumulated across every bridge this
        // session. ClientProfile's DB gains delta-merge excludes these so a host
        // item that leaks back into live AFTER the +10s gains snapshot can never be
        // uploaded into the client's DB as a fake "session gain". This is the
        // INVENTORY analogue of the per-field foreign-poison guard that already
        // protects powers/research (g_gainsSnapPowersValid / ...ResearchValid).
        std::unordered_set<std::uint32_t> g_foreignItemSet;
        // Same idea for SKILLS/PERKS (user-reported 2026-06-24: "the skills were
        // also like the host's"). The perk delta in ClientProfile takes the LIVE
        // perks wholesale; a host perk lingering after ApplyPerksReplace would be
        // uploaded as the client's. Record host-EXCLUSIVE perks (live perks at the
        // bridge NOT in the client's seeded DB perk set) so the upload excludes them.
        std::unordered_set<std::uint32_t> g_foreignPerkSet;
        std::mutex                        g_foreignMu;

        // V3 - SEH-safe player character-level read (TESActorBaseData::GetLevel has
        // REL::ID{0} → unusable; read the raw field instead: TESNPC inherits
        // TESActorBaseData, actorData @ +0x08, .level (uint16) @ +0x06).
        static std::uint32_t SafeReadActorLevel(RE::PlayerCharacter* a_p) noexcept
        {
            std::uint32_t lvl = 0;
            __try {
                if (auto* npc = a_p->GetNPC()) lvl = npc->actorData.level;
            } __except (EXCEPTION_EXECUTE_HANDLER) { lvl = 0; }
            return lvl;
        }

        // Phase C3 - DB credits to re-apply at +10s (the raw wipe strips the credits
        // item and the seed doesn't carry it). 0 = nothing to apply.
        std::atomic<std::uint32_t> g_savedCredits{ 0 };

        // V3.3 - cache lockout flag. While true, all CaptureClientState
        // calls (periodic ticks + ad-hoc) are no-ops. Set by LockCache()
        // post-pre-bridge-capture, cleared by UnlockCache() at end of
        // RestoreClientState. See DirectRestore.h for rationale.
        std::atomic<bool> g_cacheLocked{ false };

        // A unification Phase 3 - "bridged this session" marker. Set true in
        // LockCache() (THE pre-bridge marker - WorldSync::OnSaveReceived calls
        // it right before the engine applies host's save over the client). Once
        // a bridge has occurred the live inventory is a host-merge and is NO
        // LONGER a safe source for the authoritative DB profile (it may carry
        // host-leaked items - the exact Bug-A poison). Sticky for the rest of
        // the game launch (a static, so it resets on process restart); the
        // clean DB-profile auto-capture only fires while this is false, i.e.
        // from the client's OWN loaded save BEFORE any co-op bridge. Not cleared
        // on disconnect/Reset on purpose: after a co-op session the player is
        // still standing in the host-merged world, so re-opening the capture
        // window there would re-poison the DB.
        std::atomic<bool> g_bridgedThisSession{ false };

        // 2026-06-18 - "loaded save is the client's OWN character" flag.
        // Set at join by ClientProfile::ApplyProfileData (compares the loaded
        // character's Health base to the DB profile's healthBase: match → own,
        // mismatch → a foreign/HOST save, e.g. the co-op auto-save the engine
        // makes on leaving, which IS the host's bridged character). Default true
        // (no DB profile / first-time → assume own → save-authoritative).
        //
        // When FALSE (foreign/host save): CaptureClientState is suppressed so the
        // DB-profile seed (the client's own loadout) STAYS in the cache and the
        // bridge restore puts THAT back - not the host items the loaded save has.
        // Also stops the clean-capture from uploading the host save to the DB
        // (no poisoning). Fixes "loaded the co-op auto-save → 1:1 host items".
        // When TRUE (own save): save-authoritative - the live capture wins (the
        // behaviour confirmed working for the client's own save).
        std::atomic<bool> g_loadedSaveIsOwn{ true };

        // Phase C (2026-06-19) - DB-AUTHORITATIVE. When true (config
        // `dbAuthoritative` ON AND a non-empty DB seed), the DB profile is the
        // restore source for EVERY join: CaptureClientState is suppressed so the
        // seeded items/AVs/perks survive the bridge, and the upload runs only
        // post-restore (gains) so a stale loaded save can never overwrite the DB.
        // Independent of g_loadedSaveIsOwn (the hp heuristic) - when this is true
        // it dominates. Set per-join by ClientProfile::ApplyProfileData; cleared
        // in Reset().
        std::atomic<bool> g_dbAuthoritative{ false };

        // Phase A (co-op gain persistence) - true once RestoreClientState has
        // finished a bridge restore: the host items were wiped and the live
        // inventory is now the CLIENT's items (+ loot gained this session), so it
        // is SAFE to capture to the DB. Cleared in LockCache (the next bridge
        // re-merges host state) and Reset. This is what lets the DB accumulate
        // co-op gains: the engine's leave-co-op auto-save is the HOST's save and
        // the client's own save is stale, so only the DB can hold the gains.
        std::atomic<bool> g_restoreComplete{ false };

        // V4.2 - post-restore verification snapshot. After a restore we
        // record what we EXPECTED to put back (form → total count + equip
        // state). A diff against the live inventory then tells us exactly
        // what landed, what is missing, and what ALIEN items survived (host
        // leftovers - the visual-mismatch culprit). Kept separate from
        // g_cache so it survives the cache move/clear in RestoreClientState
        // and can be re-checked on a timer afterwards.
        struct ExpectedEntry {
            std::uint32_t formID   = 0;
            std::uint64_t count    = 0;
            bool          equipped = false;
        };
        std::unordered_map<RE::TESBoundObject*, ExpectedEntry> g_expected;
        std::mutex                  g_expectedMu;
        std::atomic<std::uint64_t>  g_restoreTickMs{ 0 };
        // Deferred re-check stage: 0=disarmed, 1=awaiting +5s, 2=awaiting +10s.
        std::atomic<int>            g_postRestoreStage{ 0 };

        // V4.7 - client's Oxygen capacity, captured pre-bridge. The bridge
        // load gives the client the host's / a base O2 max (observed 125 → 5),
        // because skill/perk-derived AVs aren't restored - so one jump empties
        // the bar and the next costs health. We snapshot the GOOD permanent O2
        // during normal play (captures are frozen during the bridge, so this
        // keeps the last pre-bridge value) and re-apply it after the restore.
        std::atomic<float>          g_savedO2Max{ 0.0f };
        // (2026-06-18) - O2 BASE (GetBaseActorValue, excludes the suit's
        // equipment O2 mod). Restore THIS, not the permanent total above: now
        // that re-equips the suit WITH its mods, the suit's O2 applies on
        // top of the base by itself. Setting the base to the permanent total
        // (which already included the suit) double-counted the suit → O2 came
        // out higher than the host with identical gear (observed 320 vs ~315).
        // Mirrors g_savedHealthBase: Base, not Permanent, so mods don't double.
        std::atomic<float>          g_savedO2Base{ 0.0f };

        // Stage-1 (level proxy) - level-derived survivability AV BASE values
        // (Health, CarryWeight), captured pre-bridge + restored post-apply via
        // SetBaseActorValue (gear/perk mods re-apply on top). Makes the client's
        // real power travel without touching XP/level-up. Base (NOT Permanent) so
        // re-applied mods don't double-count.
        std::atomic<float>          g_savedHealthBase{ 0.0f };
        std::atomic<float>          g_savedCarryBase{ 0.0f };

        // Stage-1 progression - perks captured PRE-bridge (the client's own),
        // re-applied additively post-bridge. Mirrors g_savedO2Max: only written
        // while captures are unlocked (normal play), so it survives the bridge.
        struct PerkSnap { std::uint32_t formID = 0; std::int8_t rank = 0; };
        std::vector<PerkSnap> g_perkSnap;
        std::mutex            g_perkMu;

        // Engine perk-add, Ghidra-verified for 1.16.236 (see reference_addperk_re):
        // FUN_1419570b0(actor, perk, rank, 5, 1). rank = target currentRank
        // (0 => engine adds the next rank; >max => no-op). The Papyrus
        // Actor.AddPerk native calls it with (self, perk, 0, 5, 1); we pass the
        // captured rank to restore faithfully. REL::Offset = version-pinned.
        constexpr bool kApplyPerksOnRestore = true;

        // Phase B  - re-apply seed-sourced items' M.2 mods on restore.
        // ENABLED 2026-06-18: uses LootSync::AttachItemModsNative (the engine's
        // own per-entry attach core, FUN_14063de80 @ RVA 0x63D270) - it
        // allocates the kObjectInstance, GROWS the array, and routes each mod
        // to its CORRECT slot. The old gate routed through ApplyItemMods →
        // SafeRestoreModArrayOnInventoryItem, which can only OVERWRITE existing
        // slots (→ naked base forms / wrong mods) - that path is untouched and
        // still serves clone-equip / corpse-loot. If this regresses (crash in
        // the post-bridge window), flip back to false: items come as base forms
        // but the gains still persist.
        constexpr bool kApplyModsOnRestore = true;
        using AddPerkFn = void (*)(RE::Actor*, RE::BGSPerk*, std::uint8_t, std::uint8_t, std::uint8_t);
        static REL::Relocation<AddPerkFn> s_addPerk{ REL::Offset(0x1954810) };

        // Engine perk-REMOVE, Ghidra-verified for 1.16.236 (FindRemovePerk RE,
        // 2026-06-18): FUN_1419573a0(actor, perk, reason). Sibling of AddPerk
        // (0x1954810 / 0x1954B00). Locks perkArrayLock @+0x36C, erases the perk
        // from Actor::perks @+0x360, fires the perk-removed event (FUN_1418c7970)
        // so the perk's effects are properly undone. Removes ALL ranks of the
        // perk. The SpaceshipRef.RemovePerk native calls it with reason=5 (the
        // same constant AddPerk's a4 uses) - replicate. Used for clean perk
        // REPLACEMENT: strip the host's perks the bridge applied, then re-add the
        // client's captured set so the client ends up with EXACTLY their own.
        using RemovePerkFn = void (*)(RE::Actor*, RE::BGSPerk*, std::uint8_t);
        static REL::Relocation<RemovePerkFn> s_removePerk{ REL::Offset(0x1954B00) };

        // Perk-derived-AV REFRESH, Ghidra-verified 2026-06-18 (DecompO2Recompute):
        // FUN_1419577c0(actor) @0x1954F20 - the engine calls it inside
        // FUN_140ca46a0 right after a perk's entry-points are (un)applied. It
        // INVALIDATES the actor's cached derived ActorValues (writes -1.0f to
        // actor+0x4d4 and the AV-cache slot +0x3c) so the next read recomputes
        // them WITH the current perk set, then refreshes. Sibling of AddPerk/
        // RemovePerk. Our perk-replace applies perks via the ASYNC service path,
        // so the derived Oxygen AV recompute lands stale (base→0, Fitness O2
        // bonus not counted → "out of stamina"). Calling this SYNCHRONOUSLY after
        // the perk-replace + AV restore forces a fresh recompute with perks in
        // place. One arg (the actor); SEH-wrapped (engine-internal state).
        using RefreshDerivedAVFn = void (*)(RE::Actor*);
        static REL::Relocation<RefreshDerivedAVFn> s_refreshDerivedAV{ REL::Offset(0x1954F20) };

        // Available (unspent) skill points - engine stores a uint32 at
        // PlayerCharacter + 0xF60 (Ghidra-verified via AddPerkPoints → FUN_141a3f4b0;
        // see reference_addperk_re). Captured pre-bridge + written back post-apply so
        // the additively re-granted perks don't refund points (AddPerk is free).
        constexpr std::uintptr_t kSkillPointsOff = 0xF60;
        std::atomic<bool>        g_skillPtsValid{ false };
        std::atomic<std::uint32_t> g_savedSkillPts{ 0 };

        // Powers (2026-06-19, gap #2) - Starborn/lesser/voice powers, Ghidra-verified
        // for 1.16.236 (reference_progression_gaps_re): the actor's added-spell array
        // is begin=*(actor+0x2c8), end=*(actor+0x2d0), entries=SpellItem* (8B stride).
        // Apply native Actor::AddSpell = FUN_14199c120(actor, SpellItem*, 0) @0x1999830
        // it is HasSpell-guarded (HasSpellVisitor over the +0x2c8 list) so re-adding
        // an existing spell is a no-op (returns 0 = already had it, 1 = newly added).
        // We restore powers ADDITIVELY (never strip): foreign/quest spells are left
        // alone, and the capture-side set-delta cancels any foreign powers so the DB
        // only accumulates the player's own. Powers filtered to power-type spells so
        // gear abilities (kAbility/kEnchantment) never enter the set.
        constexpr bool kApplyPowersOnRestore = true;

        // Phase-C diag verbosity gate (2026-06-19 cleanup). false = only the concise
        // status one-liners (restore SUMMARY, perk/power/skill/credit/O2 apply,
        // gains snapshot). true = the per-item / per-AV / per-DIFF / ghost-teardown
        // detail used while debugging. Flip to true to re-enable deep diagnostics.
        constexpr bool kDiagVerbose = false;
        constexpr std::uint32_t kDiagDetailMax = kDiagVerbose ? 24u : 0u;

        constexpr std::uintptr_t kSpellListBeginOff = 0x2c8;
        constexpr std::uintptr_t kSpellListEndOff   = 0x2d0;
        using AddSpellFn = bool (*)(RE::Actor*, RE::SpellItem*, std::uint64_t);
        static REL::Relocation<AddSpellFn> s_addSpell{ REL::Offset(0x1999830) };
        std::vector<std::uint32_t> g_powerSnap;   // DB powers seeded for additive apply
        std::mutex                 g_powerMu;

        // Research apply (gap #1) - replay the engine's per-project recipe unlock.
        // FUN_1418046b0(manager, value) is the processor Complete() calls: when the
        // project's state byte (value+0x08) != 2 it grants the project's recipes
        // (FUN_1418036f0 per entry) + recurses required sub-projects. We call it for
        // each DB-completed project found in the live map (crash-safe self-lookup,
        // skip if absent), then set state=2 so the research menu reads complete and
        // future captures see it. ALL raw engine calls are SEH-wrapped.
        constexpr bool kApplyResearchOnRestore = true;
        using ResearchUnlockFn = void (*)(std::uintptr_t /*mgr*/, std::uintptr_t /*value*/);
        static REL::Relocation<ResearchUnlockFn> s_researchUnlock{ REL::Offset(0x1801EE0) };
        std::vector<std::uint32_t> g_researchSnap;   // DB completed-research formIDs to apply
        std::mutex                 g_researchMu;

        // ActorEquipManager direct-call REL::IDs - verified for 1.16.236
        // in RemotePlayerManager.cpp (used for clone weapon/armor equip).
        // CommonLibSF IDs.h has these as REL::ID(0) placeholders; we resolve
        // manually here. Centralised so future modules can lift the pattern.
        bool DirectEquipForm(RE::Actor* a_actor, RE::TESBoundObject* a_form)
        {
            if (!a_actor || !a_form) return false;
            static REL::Relocation<RE::ActorEquipManager**> singleton{
                REL::ID(938503) };
            auto* mgr = *singleton;
            if (!mgr) return false;
            using EquipFn = bool(*)(RE::ActorEquipManager*, RE::Actor*,
                const RE::BGSObjectInstance&, const RE::BGSEquipSlot*,
                bool, bool, bool, bool, bool);
            static REL::Relocation<EquipFn> equipFn{ REL::ID(101949) };

            // equip WITH the item's mod instance data. Passing null
            // equips the BASE form: a modded spacesuit loses its O2-storage mods
            // (Oxygen max collapses to the suit-less base ~5) and modded weapons
            // come back wrong/grey. The inventory item carries its
            // TBO_InstanceData at BGSInventoryItem+0x08 - pull it so the engine
            // equips the actual modded instance and applies its effects.
            RE::TBO_InstanceData* instData = nullptr;
            a_actor->ForEachInventoryItem(
                [&](const RE::BGSInventoryItem& a_item)
                    -> RE::BSContainer::ForEachResult {
                    if (a_item.object == a_form) {
                        instData = a_item.instanceData.get();
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            RE::BGSObjectInstance inst(a_form, instData);
            // (queue=false, force=true, sounds=false, applyNow=true, locked=false)
            return equipFn(mgr, a_actor, inst, nullptr,
                false, true, false, true, false);
        }

        // V4.2 - companion to DirectEquipForm. Same singleton, UnequipObject
        // is REL::ID(101951) (verified 1.16.236 in RemotePlayerManager.cpp).
        // We unequip BEFORE re-equipping so the engine tears down the stale
        // worn-state the item carried in across the bridge, then a fresh
        // EquipObject genuinely rebuilds biped 3D + ActorValue armor mods.
        bool DirectUnequipForm(RE::Actor* a_actor, RE::TESBoundObject* a_form)
        {
            if (!a_actor || !a_form) return false;
            static REL::Relocation<RE::ActorEquipManager**> singleton{
                REL::ID(938503) };
            auto* mgr = *singleton;
            if (!mgr) return false;
            using UnequipFn = bool(*)(RE::ActorEquipManager*, RE::Actor*,
                const RE::BGSObjectInstance&, const RE::BGSEquipSlot*,
                bool, bool, bool, bool, const RE::BGSEquipSlot*);
            static REL::Relocation<UnequipFn> unequipFn{ REL::ID(101951) };
            // ghost-gear fix - unequip the ACTUAL worn instance, not the base
            // form. A modded worn item (host's Grendel + weapon mods, a modded
            // spacesuit) is equipped as (form, modInstance); calling with
            // (form, nullptr) does NOT match the equipped slot → UnequipObject
            // returns false → the worn-state binding survives. The pre-wipe
            // teardown then raw-removes the inventory entry while that binding
            // dangles = an equipped item with no inventory entry that can't be
            // selected or dropped (an undroppable "ghost"). Pull the item's
            // TBO_InstanceData (BGSInventoryItem+0x08) - same path DirectEquipForm
            // uses for so the engine matches the exact worn instance and
            // tears the worn state down before the wipe.
            RE::TBO_InstanceData* instData = nullptr;
            a_actor->ForEachInventoryItem(
                [&](const RE::BGSInventoryItem& a_item)
                    -> RE::BSContainer::ForEachResult {
                    if (a_item.object == a_form) {
                        instData = a_item.instanceData.get();
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            RE::BGSObjectInstance inst(a_form, instData);
            return unequipFn(mgr, a_actor, inst, nullptr,
                false, true, false, true, nullptr);
        }

        // engine RemoveItem (TESObjectREFR vtable 0x8B). Removes `count`
        // of `a_form` from the actor. Unlike UnequipObject (which returns false
        // for bridged worn gear → the worn-binding dangles → the raw-wipe leaves
        // a re-streamed "ghost"), RemoveItem unequips the worn slot, detaches the
        // biped 3D AND deletes the inventory entry in one coherent engine step.
        // moveTo + dropLocation null = delete (no drop). Must run in the SETTLED
        // post-restore window - mid-bridge mass mutation deadlocked (V4.4); here
        // it's a handful of targeted forms on the game thread.
        bool DirectRemoveForm(RE::Actor* a_actor, RE::TESBoundObject* a_form,
                              std::int32_t a_count)
        {
            if (!a_actor || !a_form || a_count <= 0) return false;
            RE::RemoveItemRequest req{};
            req.object = a_form;
            req.count  = a_count;
            req.reason = RE::ITEM_TRANSFER_REASON::kScriptRemoveItem;
            std::uint32_t handle = 0;
            a_actor->RemoveItem(handle, req);
            return true;
        }

        // physically swap-remove inventory entries matching a_target,
        // collapsing the array slot (zero stacks → swap last entry in → shrink
        // count). Engine RemoveItem only zeroes the stack count; the dead entry
        // lingers in the entries array and the inventory MENU still renders it
        // (the raw diff walk skips count=0, so DIFF reads clean while the menu
        // shows a ghost). Mirrors the shipped LootSync::RawRemoveFromContainer
        // collapse ("swap-remove the entry so UI doesn't show a count=0 ghost").
        // SAFE ONLY for non-equipped items (DiagGhostUnequip proved equipped=0):
        // swap-removing an equipped entry would dangle the worn slot. Returns
        // number of entries collapsed.
        std::uint32_t RawSwapRemoveForm(void* a_player,
                                        RE::TESBoundObject* a_target) noexcept
        {
            if (!a_player || !a_target) return 0;
            std::uint32_t removed = 0;
            __try {
                auto* pb = reinterpret_cast<std::uint8_t*>(a_player);
                auto* invList = *reinterpret_cast<std::uint8_t**>(pb + 0xA0);
                if (!invList) return 0;
                auto* dataSizePtr =
                    reinterpret_cast<std::uint32_t*>(invList + 0x28);
                auto* entries =
                    *reinterpret_cast<std::uint8_t**>(invList + 0x30);
                if (!entries) return 0;
                std::uint32_t entryCount = *dataSizePtr;
                if (entryCount == 0 || entryCount > 100000) return 0;

                for (std::uint32_t i = 0; i < entryCount; ) {
                    auto* entry = entries + i * 0x28;
                    auto* obj = *reinterpret_cast<RE::TESBoundObject**>(
                        entry + 0x00);
                    if (obj != a_target) { ++i; continue; }

                    // zero every stack, then collapse the entry out of the array
                    const auto stackCount =
                        *reinterpret_cast<std::uint32_t*>(entry + 0x10);
                    auto* stacks =
                        *reinterpret_cast<std::uint8_t**>(entry + 0x18);
                    if (stacks && stackCount > 0 && stackCount <= 1000) {
                        for (std::uint32_t si = 0; si < stackCount; ++si)
                            *reinterpret_cast<std::uint32_t*>(
                                stacks + si * 0x10 + 0x08) = 0;
                    }
                    *reinterpret_cast<std::uint32_t*>(entry + 0x10) = 0;

                    const std::uint32_t dataCount = *dataSizePtr;
                    if (dataCount > 0 && i < dataCount - 1) {
                        std::uint8_t* last = entries + (dataCount - 1) * 0x28;
                        std::memcpy(entry, last, 0x28);
                        std::memset(last, 0, 0x28);
                    } else if (dataCount > 0) {
                        std::memset(entry, 0, 0x28);
                    }
                    *dataSizePtr = dataCount > 0 ? dataCount - 1 : 0;
                    entryCount = *dataSizePtr;
                    ++removed;
                    // don't ++i - re-check the entry swapped into slot i
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return removed;
        }

        // Raw memory layout (verified by LootSync.cpp:2362+):
        //   refr +0xA0  → BGSInventoryList*  (BSGuarded<BGSInventoryList*, ...>)
        //   list +0x28  → uint32 entryCount
        //   list +0x30  → entry array ptr
        //   entry stride = 0x28 (= sizeof(BGSInventoryItem))
        //   entry +0x00 → TESBoundObject* object
        //   entry +0x10 → uint32 stackCount
        //   entry +0x18 → stacks array ptr
        //   stack stride = 0x10
        //   stack +0x08 → uint32 count
        //
        // Raw access avoids C++ destructibles in __try blocks (MSVC C2712).

        constexpr std::size_t kMaxItems = 4096;
        struct PodPair {
            RE::TESBoundObject* obj;
            std::int32_t        count;
        };

        // Diagnostic out-params (POD pointers - no destructibles).
        struct WalkDiag {
            std::uintptr_t invListPtr;
            std::uint32_t  entryCount;
            std::uintptr_t entriesPtr;
            std::uint32_t  faulted;  // 1 if SEH fired
        };

        // SEH-protected walker: walks the inventory list and writes to a
        // POD buffer. No C++ destructibles in this function so __try is
        // allowed. Returns number of populated entries; caller transcribes
        // to map. Also fills a_diag for debugging.
        std::size_t WalkInventoryToBuffer(
            void* a_player, PodPair* a_buf, std::size_t a_bufCapacity,
            WalkDiag* a_diag) noexcept
        {
            std::size_t n = 0;
            if (a_diag) { a_diag->invListPtr = 0; a_diag->entryCount = 0;
                          a_diag->entriesPtr = 0; a_diag->faulted = 0; }
            __try {
                auto* pb = reinterpret_cast<std::uint8_t*>(a_player);
                auto* invList = *reinterpret_cast<std::uint8_t**>(pb + 0xA0);
                if (a_diag) a_diag->invListPtr =
                    reinterpret_cast<std::uintptr_t>(invList);
                if (!invList) return 0;
                const auto entryCount =
                    *reinterpret_cast<std::uint32_t*>(invList + 0x28);
                auto* entries =
                    *reinterpret_cast<std::uint8_t**>(invList + 0x30);
                if (a_diag) {
                    a_diag->entryCount = entryCount;
                    a_diag->entriesPtr =
                        reinterpret_cast<std::uintptr_t>(entries);
                }
                if (!entries || entryCount == 0 || entryCount > 100000) return 0;

                for (std::uint32_t i = 0;
                     i < entryCount && n < a_bufCapacity; ++i)
                {
                    auto* entry = entries + i * 0x28;
                    auto* obj = *reinterpret_cast<RE::TESBoundObject**>(
                        entry + 0x00);
                    if (!obj) continue;
                    const auto stackCount =
                        *reinterpret_cast<std::uint32_t*>(entry + 0x10);
                    auto* stacks =
                        *reinterpret_cast<std::uint8_t**>(entry + 0x18);
                    if (!stacks || stackCount == 0 || stackCount > 1000) continue;
                    std::int32_t total = 0;
                    for (std::uint32_t si = 0; si < stackCount; ++si) {
                        const auto cnt = *reinterpret_cast<std::uint32_t*>(
                            stacks + si * 0x10 + 0x08);
                        if (cnt > 100000) continue;
                        total += static_cast<std::int32_t>(cnt);
                    }
                    if (total > 0) {
                        a_buf[n].obj = obj;
                        a_buf[n].count = total;
                        ++n;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (a_diag) a_diag->faulted = 1;
                return 0;
            }
            return n;
        }

        // raw walk of EVERY entry obj pointer, INCLUDING count=0 entries
        // (which WalkInventoryToBuffer skips via the total>0 gate). The inventory
        // MENU renders count=0 host leftovers the diff walk treats as gone, so the
        // ghost sweep needs to see them. POD out (TESBoundObject*[]), SEH-safe.
        std::size_t WalkAllEntryObjs(void* a_player, RE::TESBoundObject** a_buf,
                                     std::size_t a_cap) noexcept
        {
            std::size_t n = 0;
            __try {
                auto* pb = reinterpret_cast<std::uint8_t*>(a_player);
                auto* invList = *reinterpret_cast<std::uint8_t**>(pb + 0xA0);
                if (!invList) return 0;
                const auto entryCount =
                    *reinterpret_cast<std::uint32_t*>(invList + 0x28);
                auto* entries =
                    *reinterpret_cast<std::uint8_t**>(invList + 0x30);
                if (!entries || entryCount == 0 || entryCount > 100000) return 0;
                for (std::uint32_t i = 0; i < entryCount && n < a_cap; ++i) {
                    auto* obj = *reinterpret_cast<RE::TESBoundObject**>(
                        entries + i * 0x28 + 0x00);
                    if (obj) a_buf[n++] = obj;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
            return n;
        }

        // V4 - primary inventory walker. Builds the rich cache (per-stack
        // BSTSmartPointer<ExtraDataList> copies + counts) via the engine's
        // ForEachInventoryItem API. Each captured smart pointer increments
        // the refcount on the ExtraDataList; that ref is what keeps the
        // list alive across the bridge load.
        //
        // No per-item cap any more - we carry stack counts through 1:1
        // because the count value lives in OUR vector and is not bounded
        // by the temporary cache map's int32 (credits stay at their real
        // value, 20M+ included). The Pass 1 add path uses the original
        // count per stack, not a clamped total.
        bool WalkInventory(
            RE::PlayerCharacter*                                  a_player,
            std::unordered_map<RE::TESBoundObject*, CachedItem>&  a_out)
        {
            if (!a_player) return false;
            std::uint32_t itemCount     = 0;
            std::uint32_t stackTotal    = 0;
            std::uint32_t equippedCount = 0;
            a_player->ForEachInventoryItem(
                [&](const RE::BGSInventoryItem& item)
                    -> RE::BSContainer::ForEachResult {
                if (!item.object) return RE::BSContainer::ForEachResult::kContinue;
                CachedItem ci;
                for (auto& stack : item.stacks) {
                    if (stack.count == 0) continue;  // dead stack
                    CachedStack cs;
                    cs.extra = stack.extra;   // BSTSmartPointer copy → IncRef
                    cs.count = stack.count;
                    ci.stacks.push_back(std::move(cs));
                    ++stackTotal;
                }
                if (ci.stacks.empty()) return RE::BSContainer::ForEachResult::kContinue;
                ci.equipped = item.IsEquipped();
                if (ci.equipped) ++equippedCount;
                a_out.emplace(item.object, std::move(ci));
                ++itemCount;
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if constexpr (kDiagVerbose) PLOG_INFO(
                "[PULSAR] [DirectRestore] WalkInventory V4: player=0x{:016X} "
                "items={} stacks={} equipped={}"sv,
                reinterpret_cast<std::uintptr_t>(a_player),
                itemCount, stackTotal, equippedCount);
            return true;
        }

        // Thin wrapper around AddObjectToContainer. The temporary
        // BSTSmartPointer<ExtraDataList> constructed at the call site has
        // a destructor → MSVC rejects __try here (C2712). The engine
        // function is a stable vt[0x9A] dispatch; rely on its own internal
        // null guards. Other PULSAR modules (LootSync, Companion, etc.)
        // call this same path without SEH wrapping.
        void SafeAddObjectToContainer(RE::TESObjectREFR*       a_refr,
                                       RE::TESBoundObject*      a_obj,
                                       std::int32_t             a_delta,
                                       RE::ITEM_TRANSFER_REASON a_reason)
        {
            a_refr->AddObjectToContainer(a_obj, nullptr, a_delta, nullptr,
                                          a_reason);
        }

        // L.7 Plan B V1.4 - RAW memory remove of items NOT in the
        // keep-set. Bypasses vt[0x9A] AddObjectToContainer entirely so no
        // engine BGSInventoryListEvent / container event cascades fire.
        // Pattern adapted from LootSync.cpp:RawRemoveFromContainer which
        // has shipped in production for 6+ months.
        //
        // For each entry: if the object pointer is NOT in keep[], zero out
        // every stack.count (offset +0x08 of each 0x10-byte stack). The
        // engine treats count=0 stacks as removed on next inventory walk.
        // We do NOT swap-remove the entry from the array - that's UI
        // cleanup the engine will do on its own at next save/load.
        //
        // Returns count of entries zeroed.
        std::uint32_t RawRemoveExceptKeepSet(
            void* a_player,
            RE::TESBoundObject* const* a_keep,
            std::size_t a_keepCount) noexcept
        {
            std::uint32_t zeroedEntries = 0;
            __try {
                auto* pb = reinterpret_cast<std::uint8_t*>(a_player);
                auto* invList = *reinterpret_cast<std::uint8_t**>(pb + 0xA0);
                if (!invList) return 0;
                auto* dataSizePtr =
                    reinterpret_cast<std::uint32_t*>(invList + 0x28);
                auto* entries =
                    *reinterpret_cast<std::uint8_t**>(invList + 0x30);
                if (!entries) return 0;
                if (*dataSizePtr == 0 || *dataSizePtr > 100000) return 0;

                // Swap-remove iteration: don't increment i when we swap so
                // we re-check the entry that was swapped into slot i.
                // Pattern matches LootSync::RawRemoveFromContainer.
                for (std::uint32_t i = 0; i < *dataSizePtr; ) {
                    auto* entry = entries + i * 0x28;
                    auto* obj = *reinterpret_cast<RE::TESBoundObject**>(
                        entry + 0x00);

                    bool inKeep = false;
                    if (obj) {
                        for (std::size_t k = 0; k < a_keepCount; ++k) {
                            if (a_keep[k] == obj) { inKeep = true; break; }
                        }
                    }
                    if (inKeep) { ++i; continue; }

                    // Zero all stacks of this entry.
                    const auto stackCount =
                        *reinterpret_cast<std::uint32_t*>(entry + 0x10);
                    auto* stacks =
                        *reinterpret_cast<std::uint8_t**>(entry + 0x18);
                    if (stacks && stackCount > 0 && stackCount <= 1000) {
                        for (std::uint32_t si = 0; si < stackCount; ++si) {
                            auto* cnt = reinterpret_cast<std::uint32_t*>(
                                stacks + si * 0x10 + 0x08);
                            *cnt = 0;
                        }
                    }
                    *reinterpret_cast<std::uint32_t*>(entry + 0x10) = 0;
                    ++zeroedEntries;

                    // Swap-remove from data array: copy last entry into
                    // this slot, decrement size. UI won't see a ghost
                    // because the entry is gone from the BSTArray.
                    const std::uint32_t dataCount = *dataSizePtr;
                    if (dataCount > 0 && i < dataCount - 1) {
                        auto* last = entries + (dataCount - 1) * 0x28;
                        std::memcpy(entry, last, 0x28);
                        std::memset(last, 0, 0x28);
                    } else if (dataCount > 0 && i == dataCount - 1) {
                        std::memset(entry, 0, 0x28);
                    }
                    *dataSizePtr = dataCount > 0 ? (dataCount - 1) : 0;
                    // Don't ++i - re-check the swapped-in entry.
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return zeroedEntries;
            }
            return zeroedEntries;
        }

        // V4.2 - walk the LIVE player inventory and diff it against the
        // expected snapshot (g_expected) recorded at restore time. Logs a
        // per-form breakdown so a test run answers, unambiguously, whether
        // the player is holding their OWN items or the HOST's:
        //
        //   matched   - form present with exactly the expected count (good)
        //   COUNT     - form present but quantity differs (partial restore)
        //   MISSING   - form expected but absent (our add failed / got wiped)
        //   ALIEN     - form present that we never cached (= host leftover!)
        //
        // The SEH-protected raw walk runs in WalkInventoryToBuffer; all the
        // std-container diff work happens out here where destructibles are
        // legal. Caller tags each invocation ("immediate", "+5s", "+10s").
        void LogLiveDiff(const char* a_tag)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                PLOG_WARN(
                    "[PULSAR] [DirectRestore] DIFF[{}]: no player, skip"sv, a_tag);
                return;
            }

            std::vector<PodPair> buf(kMaxItems);
            WalkDiag diag{};
            const std::size_t n = WalkInventoryToBuffer(
                static_cast<void*>(player), buf.data(), buf.size(), &diag);

            // Aggregate live inventory: form → total units.
            std::unordered_map<RE::TESBoundObject*, std::int64_t> live;
            for (std::size_t i = 0; i < n; ++i) {
                if (buf[i].obj) live[buf[i].obj] += buf[i].count;
            }

            std::unordered_map<RE::TESBoundObject*, ExpectedEntry> expected;
            {
                std::lock_guard lk(g_expectedMu);
                expected = g_expected;
            }

            std::uint32_t matched = 0, mismatch = 0, missing = 0, alien = 0;
            std::uint32_t mmLogged = 0, missLogged = 0, alienLogged = 0;
            std::int64_t  alienUnits = 0;

            for (const auto& [form, ee] : expected) {
                auto it = live.find(form);
                if (it == live.end()) {
                    ++missing;
                    if (missLogged < kDiagDetailMax) {
                        PLOG_WARN(
                            "[PULSAR] [DirectRestore] DIFF[{}] MISSING "
                            "form=0x{:08X} expected={}{}"sv,
                            a_tag, ee.formID, ee.count,
                            ee.equipped ? " (was equipped)" : "");
                        ++missLogged;
                    }
                } else if (static_cast<std::uint64_t>(it->second) != ee.count) {
                    ++mismatch;
                    if (mmLogged < kDiagDetailMax) {
                        PLOG_WARN(
                            "[PULSAR] [DirectRestore] DIFF[{}] COUNT "
                            "form=0x{:08X} expected={} live={}"sv,
                            a_tag, ee.formID, ee.count, it->second);
                        ++mmLogged;
                    }
                } else {
                    ++matched;
                }
            }

            for (const auto& [form, cnt] : live) {
                if (expected.find(form) != expected.end()) continue;
                ++alien;
                alienUnits += cnt;
                if (alienLogged < kDiagDetailMax) {
                    PLOG_WARN(
                        "[PULSAR] [DirectRestore] DIFF[{}] ALIEN "
                        "form=0x{:08X} live={} (NOT in client cache - host item?)"sv,
                        a_tag, form ? form->formID : 0u, cnt);
                    ++alienLogged;
                }
            }

            PLOG_INFO(
                "[PULSAR] [DirectRestore] DIFF[{}] SUMMARY: liveForms={} "
                "expectedForms={} matched={} countMismatch={} missing={} "
                "alien={} alienUnits={} faulted={}"sv,
                a_tag, live.size(), expected.size(), matched, mismatch,
                missing, alien, alienUnits, diag.faulted);
        }
    }

    // SEH-isolated perk walk: __try cannot live in a function with C++ objects
    // requiring unwinding (C2712), so the guarded read is its own helper. The
    // output vector is a reference (no destructible local in this frame).
    static bool WalkPerks(RE::PlayerCharacter* a_player, std::vector<PerkSnap>& a_out)
    {
        __try {
            auto* pk = a_player->perks;
            if (pk && pk->perkRanks) {
                for (const auto& prd : *pk->perkRanks) {
                    if (prd.perk) a_out.push_back({ prd.perk->formID, prd.currentRank });
                }
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Powers (gap #2) - is this a power-type spell we persist? In Starfield the
    // Starborn powers are SpellType **kSpell (0)** (verified in-game: a 24-power
    // char's +0x2c8 list held exactly 24 type-0 spells = the powers; the 45 type-4
    // kAbility entries were gear/trait abilities). Keep kPower/kLesserPower/
    // kVoicePower too (defensive). EXCLUDE kAbility(4)/kDisease(1)/kEnchantment(6)
    // etc. so equipment & quest effects never leak into the saved power set.
    static bool IsPowerType(RE::MagicSystem::SpellType a_t)
    {
        return a_t == RE::MagicSystem::SpellType::kSpell ||
               a_t == RE::MagicSystem::SpellType::kPower ||
               a_t == RE::MagicSystem::SpellType::kLesserPower ||
               a_t == RE::MagicSystem::SpellType::kVoicePower;
    }

    // SEH-isolated walk of the actor's added-spell array (begin=*(actor+0x2c8),
    // end=*(actor+0x2d0), SpellItem* entries). Fills a_out with the formIDs of
    // power-type spells only. POD-only frame (a_out is a reference, like WalkPerks).
    static bool WalkPowers(RE::Actor* a_actor, std::vector<std::uint32_t>& a_out)
    {
        __try {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(a_actor);
            auto* begin = *reinterpret_cast<RE::SpellItem***>(base + kSpellListBeginOff);
            auto* end   = *reinterpret_cast<RE::SpellItem***>(base + kSpellListEndOff);
            if (begin && end > begin) {
                std::size_t n = static_cast<std::size_t>(end - begin);
                if (n > 4096) n = 4096;   // sanity cap against a garbage pointer pair
                for (std::size_t i = 0; i < n; ++i) {
                    RE::SpellItem* s = begin[i];
                    if (!s) continue;
                    if (IsPowerType(s->GetSpellType())) a_out.push_back(s->formID);
                }
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Engine Actor::AddSpell (FUN_14199c120, HasSpell-guarded). Returns true if the
    // spell was NEWLY added, false if already present OR on fault. SEH-wrapped.
    static bool SafeAddSpell(RE::Actor* a_actor, RE::SpellItem* a_spell)
    {
        __try { return (*s_addSpell)(a_actor, a_spell, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Research (gap #1) - manager singleton global (RVA 0x61E26A0; file scope to keep
    // the __try frame POD, C2712). Mirrors ClientProfile::SafeWalkResearch: walk the
    // std::map @ mgr+0x18 and collect formIDs of COMPLETED projects (state==2). Used
    // for the +10s snapshot reference. Returns nodes visited (-1 on fault).
    static REL::Relocation<std::uintptr_t*> s_researchMgrGlobal{ REL::Offset(0x61E26A0) };
    static int WalkResearch(std::vector<std::uint32_t>& a_done)
    {
        int total = 0;
        std::uint32_t buf[1024];
        int n = 0;
        __try {
            const std::uintptr_t mgr = *s_researchMgrGlobal;
            if (mgr) {
                const std::uintptr_t head = *reinterpret_cast<std::uintptr_t*>(mgr + 0x18);
                if (head) {
                    std::uintptr_t stack[512];
                    int sp = 0;
                    const std::uintptr_t root = *reinterpret_cast<std::uintptr_t*>(head + 0x08);
                    if (root) stack[sp++] = root;
                    while (sp > 0 && total < 4096) {
                        std::uintptr_t node = stack[--sp];
                        if (!node) continue;
                        if (*reinterpret_cast<std::uint8_t*>(node + 0x19) != 0) continue; // _Isnil
                        ++total;
                        std::uintptr_t val = *reinterpret_cast<std::uintptr_t*>(node + 0x28);
                        if (val && *reinterpret_cast<std::uint8_t*>(val + 0x08) == 2) {
                            if (n < 1024) buf[n++] = *reinterpret_cast<std::uint32_t*>(node + 0x20);
                        }
                        if (sp < 510) {
                            std::uintptr_t l = *reinterpret_cast<std::uintptr_t*>(node + 0x00);
                            std::uintptr_t r = *reinterpret_cast<std::uintptr_t*>(node + 0x10);
                            if (l) stack[sp++] = l;
                            if (r) stack[sp++] = r;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        a_done.assign(buf, buf + n);
        return total;
    }

    // Research apply - walk ALL map nodes into POD arrays (formID/value-ptr/state)
    // and return the manager pointer via a_mgr (0 on fault/empty). POD __try frame.
    static int WalkResearchNodes(std::uint32_t* a_ids, std::uintptr_t* a_vals,
                                 std::uint8_t* a_states, int a_cap,
                                 std::uintptr_t& a_mgr)
    {
        int n = 0;
        a_mgr = 0;
        __try {
            const std::uintptr_t mgr = *s_researchMgrGlobal;
            if (mgr) {
                a_mgr = mgr;
                const std::uintptr_t head = *reinterpret_cast<std::uintptr_t*>(mgr + 0x18);
                if (head) {
                    std::uintptr_t stack[512];
                    int sp = 0, visited = 0;
                    const std::uintptr_t root = *reinterpret_cast<std::uintptr_t*>(head + 0x08);
                    if (root) stack[sp++] = root;
                    while (sp > 0 && visited < 4096 && n < a_cap) {
                        std::uintptr_t node = stack[--sp];
                        if (!node) continue;
                        if (*reinterpret_cast<std::uint8_t*>(node + 0x19) != 0) continue; // _Isnil
                        ++visited;
                        std::uintptr_t val = *reinterpret_cast<std::uintptr_t*>(node + 0x28);
                        if (val) {
                            a_ids[n]    = *reinterpret_cast<std::uint32_t*>(node + 0x20);
                            a_vals[n]   = val;
                            a_states[n] = *reinterpret_cast<std::uint8_t*>(val + 0x08);
                            ++n;
                        }
                        if (sp < 510) {
                            std::uintptr_t l = *reinterpret_cast<std::uintptr_t*>(node + 0x00);
                            std::uintptr_t r = *reinterpret_cast<std::uintptr_t*>(node + 0x10);
                            if (l) stack[sp++] = l;
                            if (r) stack[sp++] = r;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        return n;
    }

    static bool SafeUnlockResearch(std::uintptr_t a_mgr, std::uintptr_t a_value)
    {
        __try { (*s_researchUnlock)(a_mgr, a_value); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool SafeSetResearchState2(std::uintptr_t a_value)
    {
        __try { *reinterpret_cast<std::uint8_t*>(a_value + 0x08) = 2; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Research apply (gap #1) - for each DB-completed project present in the live map
    // with state != 2: replay the engine recipe-unlock then mark state=2. Crash-safe
    // (self-lookup, per-call SEH). No-op when the DB has no research or apply is off.
    static void ApplyResearch()
    {
        if (!kApplyResearchOnRestore || s_researchUnlock.address() == 0) return;
        std::vector<std::uint32_t> want;
        { std::lock_guard lk(g_researchMu); want = g_researchSnap; }
        if (want.empty()) return;

        // Game-thread only (RestoreClientState) → static POD scratch is safe (not
        // re-entrant). vals[] are state-struct heap pointers; FUN_1418046b0 grants
        // recipes + recurses sub-projects but does NOT erase research-map nodes, so
        // the cached pointers stay valid across the apply loop (no use-after-free).
        static std::uint32_t  ids[1536];
        static std::uintptr_t vals[1536];
        static std::uint8_t   states[1536];
        std::uintptr_t mgr = 0;
        const int nodeCount = WalkResearchNodes(ids, vals, states, 1536, mgr);
        if (nodeCount <= 0 || !mgr) {
            PLOG_WARN("[PULSAR] [DirectRestore] research apply: map empty/unavailable "
                      "(nodes={}, mgr={})"sv, nodeCount, mgr != 0);
            return;
        }

        int applied = 0, already = 0, missing = 0, fault = 0;
        for (auto fid : want) {
            int idx = -1;
            for (int i = 0; i < nodeCount; ++i) if (ids[i] == fid) { idx = i; break; }
            if (idx < 0) { ++missing; continue; }
            if (states[idx] == 2) { ++already; continue; }
            if (SafeUnlockResearch(mgr, vals[idx])) { SafeSetResearchState2(vals[idx]); ++applied; }
            else ++fault;
        }
        PLOG_INFO("[PULSAR] [DirectRestore] research apply: want={} nodes={} +{} unlocked, "
                  "{} already, {} not-in-map, {} fault"sv,
                  want.size(), nodeCount, applied, already, missing, fault);
    }

    // Raw read/write of the skill-points uint at player+0xF60. SEH-isolated
    // (POD-only frame) so a bad pointer can't crash.
    static bool ReadSkillPoints(RE::PlayerCharacter* a_player, std::uint32_t& a_out)
    {
        __try {
            a_out = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(a_player) + kSkillPointsOff);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool WriteSkillPoints(RE::PlayerCharacter* a_player, std::uint32_t a_val)
    {
        __try {
            *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(a_player) + kSkillPointsOff) = a_val;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // A unification Phase 3 - seed the cache from the DB-backed client profile at
    // join (clean, host-free), breaking the poisoning bootstrap. Overwrites the
    // cache wholesale; the periodic capture then maintains it from the (now clean)
    // live inventory. Base forms only (no ExtraData - mods deferred).
    void SeedCacheFromProfile(const std::vector<SeedItem>& items)
    {
        if (items.empty()) {
            PLOG_INFO("[PULSAR] [DirectRestore] SeedCacheFromProfile: empty, skip"sv);
            return;
        }
        // Never overwrite the cache mid-bridge - a profile apply that races a bridge
        // (e.g. a manual F-key restore while a save-load is in flight) would corrupt
        // the in-progress restore source. The join apply runs pre-bridge (unlocked).
        if (g_cacheLocked.load(std::memory_order_acquire)) {
            PLOG_INFO("[PULSAR] [DirectRestore] SeedCacheFromProfile: cache locked "
                      "(bridge in progress), skip seed"sv);
            return;
        }
        std::unordered_map<RE::TESBoundObject*, CachedItem> seeded;
        std::uint32_t n = 0, eq = 0;
        for (const auto& s : items) {
            if (!s.form || s.count <= 0) continue;
            CachedItem ci;
            CachedStack cs;
            cs.count = static_cast<std::uint32_t>(s.count);  // extra left null = base form
            ci.stacks.push_back(std::move(cs));
            ci.equipped = s.equipped;
            ci.mods = s.mods;                 // Phase B - carry the M.2 mods for re-apply
            ci.modSentinel = s.modSentinel;
            seeded[s.form] = std::move(ci);
            ++n; if (s.equipped) ++eq;
        }
        {
            std::lock_guard lk(g_cacheMu);
            g_cache = std::move(seeded);
        }
        g_cacheValid.store(true, std::memory_order_release);
        // Save-authoritative (user decision 2026-06-18): this seed is only a
        // FALLBACK. The live pre-bridge CaptureClientState runs after this and
        // overwrites the cache with the client's actual loaded save, which is
        // authoritative. The seed matters only if no live capture happens before
        // the bridge (e.g. cross-machine first join with no local loadout).
        PLOG_INFO("[PULSAR] [DirectRestore] SeedCacheFromProfile: seeded {} items "
                  "({} equipped) from DB profile (fallback - live save overrides)"sv, n, eq);
    }

    // Phase C - DB-authoritative flag.
    void SetDbAuthoritative(bool a_on)
    {
        const bool prev = g_dbAuthoritative.exchange(a_on, std::memory_order_acq_rel);
        if (prev != a_on) {
            PLOG_INFO("[PULSAR] [DirectRestore] DbAuthoritative {} -> {}"sv, prev, a_on);
        }
    }
    bool IsDbAuthoritative()
    {
        return g_dbAuthoritative.load(std::memory_order_acquire);
    }

    // Phase C3 - co-op gains baseline snapshot (taken at +10s post-restore).
    bool IsGainsSnapshotValid()
    {
        return g_gainsSnapValid.load(std::memory_order_acquire);
    }
    bool GetGainsSnapshot(std::unordered_map<std::uint32_t, std::uint32_t>& a_outCounts,
                          std::uint32_t& a_outCredits, std::uint32_t& a_outLevel,
                          float& a_outXP)
    {
        if (!g_gainsSnapValid.load(std::memory_order_acquire)) return false;
        std::lock_guard lk(g_gainsSnapMu);
        a_outCounts  = g_gainsSnap;
        a_outCredits = g_gainsSnapCredits;
        a_outLevel   = g_gainsSnapLevel;
        a_outXP      = g_gainsSnapXP;
        return true;
    }
    bool GetGainsSnapshotPowers(std::vector<std::uint32_t>& a_outPowers)
    {
        // B1: require BOTH the overall snapshot AND the powers walk to have succeeded -
        // else the empty set would poison the DB with foreign powers via the set-delta.
        if (!g_gainsSnapValid.load(std::memory_order_acquire) ||
            !g_gainsSnapPowersValid.load(std::memory_order_acquire)) return false;
        std::lock_guard lk(g_gainsSnapMu);
        a_outPowers = g_gainsSnapPowers;
        return true;
    }
    bool GetGainsSnapshotResearch(std::vector<std::uint32_t>& a_outResearch)
    {
        if (!g_gainsSnapValid.load(std::memory_order_acquire) ||
            !g_gainsSnapResearchValid.load(std::memory_order_acquire)) return false;
        std::lock_guard lk(g_gainsSnapMu);
        a_outResearch = g_gainsSnapResearch;
        return true;
    }

    // #poison-guard - host-exclusive item forms recorded across bridges this
    // session. ClientProfile excludes these from the DB gains upload.
    void GetForeignItemSet(std::unordered_set<std::uint32_t>& a_out)
    {
        std::lock_guard lk(g_foreignMu);
        a_out = g_foreignItemSet;
    }
    void GetForeignPerkSet(std::unordered_set<std::uint32_t>& a_out)
    {
        std::lock_guard lk(g_foreignMu);
        a_out = g_foreignPerkSet;
    }

    // Phase C - seed the AV baselines from the DB profile so RestoreClientState's
    // existing AV apply restores the DB's progression (not a live capture). Same
    // thresholds the live capture uses (DirectRestore.cpp ~779-795).
    void SeedAVsFromProfile(float a_o2Base, float a_o2Max,
                            float a_healthBase, float a_carryBase)
    {
        if (a_o2Base    > 1.0f)  g_savedO2Base.store(a_o2Base,    std::memory_order_release);
        if (a_o2Max     > 10.0f) g_savedO2Max.store(a_o2Max,      std::memory_order_release);
        if (a_healthBase > 1.0f) g_savedHealthBase.store(a_healthBase, std::memory_order_release);
        if (a_carryBase  > 1.0f) g_savedCarryBase.store(a_carryBase,   std::memory_order_release);
        PLOG_INFO("[PULSAR] [DirectRestore] SeedAVsFromProfile: o2Base={:.1f} o2Max={:.1f} hp={:.1f} cw={:.1f}"sv,
                  a_o2Base, a_o2Max, a_healthBase, a_carryBase);
    }

    // Phase C3 - seed the DB credits for the +10s re-apply.
    void SeedCreditsFromProfile(std::uint32_t a_credits)
    {
        g_savedCredits.store(a_credits, std::memory_order_release);
        PLOG_INFO("[PULSAR] [DirectRestore] SeedCreditsFromProfile: {}"sv, a_credits);
    }

    // Phase C - seed the perk snapshot + skill points from the DB profile. Empty
    // perk list is a NO-OP (never wipe g_perkSnap - an empty snapshot makes
    // ApplyPerksReplace skip the strip, so wiping would leak the loaded save's perks).
    void SeedPerksFromProfile(
        const std::vector<std::pair<std::uint32_t, std::int8_t>>& a_perks,
        std::uint32_t a_skillPoints)
    {
        if (!a_perks.empty()) {
            std::lock_guard lk(g_perkMu);
            g_perkSnap.clear();
            g_perkSnap.reserve(a_perks.size());
            for (const auto& p : a_perks) {
                g_perkSnap.push_back(PerkSnap{ p.first, p.second });
            }
        }
        g_savedSkillPts.store(a_skillPoints, std::memory_order_release);
        g_skillPtsValid.store(true, std::memory_order_release);
        PLOG_INFO("[PULSAR] [DirectRestore] SeedPerksFromProfile: {} perks, skillPts={}"sv,
                  a_perks.size(), a_skillPoints);
    }

    // Powers (gap #2) - seed the DB powers for additive re-apply. Empty list is a
    // no-op (leaves g_powerSnap as-is); additive apply never strips, so an empty
    // seed simply restores nothing (the common case: most players have no powers).
    void SeedPowersFromProfile(const std::vector<std::uint32_t>& a_powers)
    {
        std::lock_guard lk(g_powerMu);
        g_powerSnap = a_powers;
        PLOG_INFO("[PULSAR] [DirectRestore] SeedPowersFromProfile: {} powers"sv,
                  a_powers.size());
    }

    // Research (gap #1) - seed the DB's completed-research formIDs for the post-bridge
    // replay (ApplyResearch). Empty = no-op.
    void SeedResearchFromProfile(const std::vector<std::uint32_t>& a_research)
    {
        std::lock_guard lk(g_researchMu);
        g_researchSnap = a_research;
        PLOG_INFO("[PULSAR] [DirectRestore] SeedResearchFromProfile: {} projects"sv,
                  a_research.size());
    }

    void CaptureClientState()
    {
        // V3.3 - bridge transition lockout. While a save bridge is being
        // applied by the engine the live inventory transitions through
        // (client items → empty → host items). Letting any capture fire
        // during this window poisons the cache with host's items, which
        // makes RestoreClientState a no-op (cached==current). The lock
        // is set in WorldSync::OnSaveReceived right after the final
        // pre-bridge capture and cleared in RestoreClientState.
        if (g_cacheLocked.load(std::memory_order_acquire)) {
            PLOG_INFO(
                "[PULSAR] [DirectRestore] CaptureClientState: cache locked "
                "(bridge in progress), skip"sv);
            return;
        }
        // Phase C - DB-authoritative: the DB seed (items+AVs+perks) is the restore
        // source for every join. Suppress ALL live capture so the seed survives the
        // bridge regardless of which save is loaded. This is the same suppression as
        // the foreign path below, generalised from "hp says foreign" to "DB is the
        // source of truth".
        if (g_dbAuthoritative.load(std::memory_order_acquire)) {
            PLOG_INFO(
                "[PULSAR] [DirectRestore] CaptureClientState: DB-authoritative - "
                "skip (DB seed is the restore source)"sv);
            return;
        }
        // The loaded save is NOT the client's own character (a foreign/host save,
        // e.g. the co-op auto-save). Do NOT capture its inventory - that would
        // overwrite the DB-profile seed (the client's own loadout) with host
        // items, and the bridge restore would then put host items back. Keeping
        // the seed makes the restore source the client's DB loadout instead.
        if (!g_loadedSaveIsOwn.load(std::memory_order_acquire)) {
            PLOG_INFO(
                "[PULSAR] [DirectRestore] CaptureClientState: foreign/host save "
                "loaded - skip (keeping DB seed as restore source)"sv);
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_INFO(
                "[PULSAR] [DirectRestore] CaptureClientState: no player singleton, skip"sv);
            return;
        }

        // A unification (save-authoritative, user decision 2026-06-18): the
        // client's LIVE pre-bridge save is the authoritative item source. The
        // live capture always runs and OVERWRITES any earlier DB-profile seed -
        // the seed is only a fallback (cross-machine / no local inventory). The
        // g_cacheLocked check above still blocks EVERYTHING mid-bridge (the live
        // state is transitioning to host's there), so this can't capture host's
        // items; it captures the client's own save. Perks/skill-points/AVs are
        // snapshotted further down (always - the client's own pre-bridge state).
        {
            std::unordered_map<RE::TESBoundObject*, CachedItem> snap;
            const bool ok = WalkInventory(player, snap);
            if (!ok) {
                PLOG_WARN(
                    "[PULSAR] [DirectRestore] CaptureClientState: inventory walk faulted"sv);
                return;
            }

            // Diagnostic - log first 16 items with stack count + total units.
            std::uint32_t logged = 0;
            std::uint64_t totalUnits = 0;
            std::uint32_t equippedSize = 0;
            for (const auto& [form, ci] : snap) {
                for (const auto& cs : ci.stacks) totalUnits += cs.count;
                if (ci.equipped) ++equippedSize;
                if (kDiagVerbose && logged < 16) {
                    std::uint64_t formTotal = 0;
                    for (const auto& cs : ci.stacks) formTotal += cs.count;
                    PLOG_INFO(
                        "[PULSAR] [DirectRestore] cached: form=0x{:08X} stacks={} total={}{}"sv,
                        form ? form->formID : 0u, ci.stacks.size(), formTotal,
                        ci.equipped ? " EQUIPPED" : "");
                    ++logged;
                }
            }

            {
                std::lock_guard lk(g_cacheMu);
                g_cache = std::move(snap);
                g_cacheValid.store(true, std::memory_order_release);
            }

            PLOG_INFO(
                "[PULSAR] [DirectRestore] CaptureClientState: cached {} distinct items "
                "equipped={} totalUnits={}"sv,
                CachedItemCount(), equippedSize, totalUnits);
        }

        // V4.6 - log vital AVs on every capture (~5s) so we get the PRE-bridge
        // baseline AND an O2-regen-at-rest timeline to compare against the
        // post-restore AV[auto+10s] values. Tells us if the bridge breaks the
        // O2 max or if O2 simply isn't regenerating.
        {
            auto logAV = [&](const char* name) {
                auto* av = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                    RE::BSFixedString(name));
                if (!av) return;
                if constexpr (kDiagVerbose) PLOG_INFO(
                    "[PULSAR] [DirectRestore] AV[capture] {} cur={:.1f} max={:.1f} base={:.1f}"sv,
                    name, player->GetActorValue(*av),
                    player->GetPermanentActorValue(*av),
                    player->GetBaseActorValue(*av));
            };
            logAV("Oxygen");
            logAV("CarryWeight");

            // V4.7 - snapshot the client's good O2 max. This runs only when
            // captures are unlocked (i.e. normal play, NOT during the bridge),
            // so it holds the last pre-bridge value through the bridge window.
            if (auto* o2 = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                    RE::BSFixedString("Oxygen"))) {
                const float perm = player->GetPermanentActorValue(*o2);
                if (perm > 10.0f) g_savedO2Max.store(perm);
                // also snapshot the O2 BASE (suit excluded). This is what
                // we restore; the suit's O2 re-applies on top via equip.
                const float base = player->GetBaseActorValue(*o2);
                if (base > 1.0f) g_savedO2Base.store(base);
            }
            // Health + CarryWeight BASE (level-derived, mods excluded) - same
            // pre-bridge snapshot so the client's real power survives the bridge.
            if (auto* hp = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                    RE::BSFixedString("Health"))) {
                const float b = player->GetBaseActorValue(*hp);
                if (b > 1.0f) g_savedHealthBase.store(b);
            }
            if (auto* cw = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                    RE::BSFixedString("CarryWeight"))) {
                const float b = player->GetBaseActorValue(*cw);
                if (b > 1.0f) g_savedCarryBase.store(b);
            }
        }

        // Stage-1 - capture the client's own perks (pre-bridge). The SEH walk
        // lives in WalkPerks (a function with no unwinding objects - required
        // because __try cannot share a frame with C++ destructible locals,
        // C2712). Only runs while captures are unlocked, so it holds the
        // client's perks across the bridge window.
        {
            std::vector<PerkSnap> perks;
            WalkPerks(player, perks);
            size_t n;
            {
                std::lock_guard lk(g_perkMu);
                if (!perks.empty()) g_perkSnap = std::move(perks);
                n = g_perkSnap.size();
            }
            PLOG_INFO("[PULSAR] [DirectRestore] perk capture: {} perks"sv, n);
        }

        // Stage-1 - capture the client's own available skill points (pre-bridge).
        // Held across the bridge like g_savedO2Max (capture only runs when unlocked).
        {
            std::uint32_t sp = 0;
            if (ReadSkillPoints(player, sp)) {
                g_savedSkillPts.store(sp, std::memory_order_release);
                g_skillPtsValid.store(true, std::memory_order_release);
            }
        }
    }

    // SEH-isolated engine call (C2712: __try cannot share a frame with C++
    // unwinding objects - the same trap WalkPerks avoids; ApplyPerksAdditive
    // holds a vector).
    static bool SafeAddPerk(RE::Actor* a_actor, RE::BGSPerk* a_perk, std::uint8_t a_rank)
    {
        __try { (*s_addPerk)(a_actor, a_perk, a_rank, 5, 1); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SafeRemovePerk(RE::Actor* a_actor, RE::BGSPerk* a_perk)
    {
        __try { (*s_removePerk)(a_actor, a_perk, 5); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Force a perk-derived-AV recompute (Fitness→Oxygen etc.) after the
    // perk-replace, so the derived O2 stops landing on the un-perked value.
    static void SafeRefreshDerivedAVs(RE::Actor* a_actor)
    {
        if (!a_actor || s_refreshDerivedAV.address() == 0) return;
        __try { (*s_refreshDerivedAV)(a_actor); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Live player's current rank for a perk form, or -1 if absent. SEH-guarded
    // (only POD locals → safe to host __try directly).
    static std::int32_t LivePerkRank(RE::PlayerCharacter* a_player, std::uint32_t a_formID)
    {
        std::int32_t rank = -1;
        __try {
            auto* pk = a_player->perks;
            if (pk && pk->perkRanks) {
                for (const auto& prd : *pk->perkRanks) {
                    if (prd.perk && prd.perk->formID == a_formID) { rank = prd.currentRank; break; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { rank = -1; }
        return rank;
    }

    // Live perk count (diagnostic), or -1 on fault. POD-only → __try-safe.
    static int LivePerkCount()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return -1;
        int n = -1;
        __try {
            auto* pk = pc->perks;
            n = (pk && pk->perkRanks) ? static_cast<int>(pk->perkRanks->size()) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { n = -1; }
        return n;
    }

    // Additive: add only the client's perks the live player lacks (or has at a
    // lower rank), at the captured rank. Never removes host perks (Stage 1).
    // Same-machine raw FormID; a form that doesn't resolve here is skipped.
    static void ApplyPerksAdditive(RE::PlayerCharacter* a_player)
    {
        if (!kApplyPerksOnRestore || s_addPerk.address() == 0) return;
        std::vector<PerkSnap> snap;
        { std::lock_guard lk(g_perkMu); snap = g_perkSnap; }
        if (snap.empty()) return;

        int applied = 0, already = 0, missing = 0;
        for (const auto& ps : snap) {
            auto* form = RE::TESForm::LookupByID(ps.formID);
            if (!form || form->GetFormType() != RE::FormType::kPERK) { ++missing; continue; }
            auto* perk = static_cast<RE::BGSPerk*>(form);
            if (LivePerkRank(a_player, ps.formID) >= ps.rank) { ++already; continue; }
            if (SafeAddPerk(a_player, perk, static_cast<std::uint8_t>(ps.rank))) ++applied;
            else PLOG_WARN("[PULSAR] [DirectRestore] AddPerk faulted form=0x{:08X}"sv, ps.formID);
        }
        PLOG_INFO("[PULSAR] [DirectRestore] perk apply: +{} applied, {} already-had, {} unresolved"sv,
                  applied, already, missing);
    }

    // Clean perk REPLACEMENT - converge the player's perk set to EXACTLY the
    // client's captured set. Post-bridge the player carries the HOST's perks; we
    // (1) strip every LIVE perk that the client doesn't have (or has at a
    // different rank), then (2) ApplyPerksAdditive to (re-)add the client's set
    // at the captured ranks. Net result: the client ends up with only their own
    // perks/skills - fixes "Client hat die Skills von Host".
    //
    // SAFETY: if the client snapshot is EMPTY (pre-bridge capture failed), do
    // NOT strip anything - removing all perks with nothing to re-add is worse
    // than the leak. In that case keep the live perks untouched.
    static void ApplyPerksReplace(RE::PlayerCharacter* a_player)
    {
        if (!kApplyPerksOnRestore || s_addPerk.address() == 0 ||
            s_removePerk.address() == 0) {
            ApplyPerksAdditive(a_player);
            return;
        }

        std::vector<PerkSnap> want;
        { std::lock_guard lk(g_perkMu); want = g_perkSnap; }
        if (want.empty()) {
            PLOG_WARN("[PULSAR] [DirectRestore] perk replace: client snapshot EMPTY - "
                      "NOT stripping host perks (would leave the player with none)"sv);
            return;
        }

        // formID -> captured rank, for the keep/strip decision.
        std::unordered_map<std::uint32_t, std::int8_t> wantRank;
        for (const auto& p : want) wantRank[p.formID] = p.rank;

        // Snapshot the LIVE (host) perks BEFORE mutating - RemovePerk edits the
        // same array WalkPerks reads.
        std::vector<PerkSnap> live;
        WalkPerks(a_player, live);

        int removed = 0, removeFault = 0;
        for (const auto& lp : live) {
            auto it = wantRank.find(lp.formID);
            if (it != wantRank.end() && it->second == lp.rank) continue;  // client has it, same rank → keep
            auto* form = RE::TESForm::LookupByID(lp.formID);
            if (!form || form->GetFormType() != RE::FormType::kPERK) continue;
            auto* perk = static_cast<RE::BGSPerk*>(form);
            if (SafeRemovePerk(a_player, perk)) ++removed;
            else { ++removeFault; PLOG_WARN("[PULSAR] [DirectRestore] RemovePerk faulted form=0x{:08X}"sv, lp.formID); }
        }
        PLOG_INFO("[PULSAR] [DirectRestore] perk replace: live(host)={} want(client)={} stripped={} (fault={})"sv,
                  live.size(), want.size(), removed, removeFault);

        // (Re-)add the client's perks the player now lacks, at the captured rank.
        ApplyPerksAdditive(a_player);
    }

    // Powers (gap #2) - ADDITIVELY re-add the DB's powers. Never strips (foreign/
    // quest spells are left untouched; the capture-side set-delta cancels foreign
    // powers so they don't enter the DB). AddSpell is HasSpell-guarded → re-adding
    // an existing power is a no-op. Empty seed → nothing to do.
    static void ApplyPowersAdditive(RE::PlayerCharacter* a_player)
    {
        if (!kApplyPowersOnRestore || s_addSpell.address() == 0) return;
        std::vector<std::uint32_t> snap;
        { std::lock_guard lk(g_powerMu); snap = g_powerSnap; }
        if (snap.empty()) return;

        int applied = 0, already = 0, missing = 0;
        for (auto fid : snap) {
            auto* form = RE::TESForm::LookupByID(fid);
            if (!form || form->GetFormType() != RE::FormType::kSPEL) { ++missing; continue; }
            auto* spell = static_cast<RE::SpellItem*>(form);
            if (SafeAddSpell(a_player, spell)) ++applied; else ++already;
        }
        PLOG_INFO("[PULSAR] [DirectRestore] power apply: +{} added, {} already-had/fault, {} unresolved"sv,
                  applied, already, missing);
    }

    void RestoreClientState()
    {
        // V3.3 - guarantee unlock on every return path. The lock is set
        // in WorldSync::OnSaveReceived; leaving it set after any restore
        // early-return would freeze the cache for the rest of the session.
        struct UnlockOnExit {
            ~UnlockOnExit() { UnlockCache(); }
        } _unlockOnExit;

        if (!g_cacheValid.load(std::memory_order_acquire)) {
            PLOG_INFO(
                "[PULSAR] [DirectRestore] RestoreClientState: cache invalid, skip"sv);
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_WARN(
                "[PULSAR] [DirectRestore] RestoreClientState: no player singleton, skip"sv);
            return;
        }

        // V4 - move the rich cache out under the lock. We MOVE rather than
        // copy because the cache holds BSTSmartPointers; copying would
        // double the refcount briefly (harmless but wasteful). After move,
        // g_cache is empty; Reset() at session end clears it formally.
        std::unordered_map<RE::TESBoundObject*, CachedItem> cached;
        {
            std::lock_guard lk(g_cacheMu);
            cached = std::move(g_cache);
            g_cache.clear();
        }

        if (cached.empty()) {
            PLOG_WARN(
                "[PULSAR] [DirectRestore] RestoreClientState: cache is EMPTY - "
                "skipping restore."sv);
            return;
        }

        // V4.8 - capture O2 max from the freshly-loaded HOST save, BEFORE the
        // wipe below. Evidence (2026-05-25): host=125, client=5; the client's
        // pre-bridge capture ALSO reads 5, so the save is already corrupted and
        // g_savedO2Max (fed by the periodic capture) never sees a good value.
        // The bridge just loaded the host character (O2 ~125); the wipe strips
        // the equipped suit → O2 collapses to the suit-less base (~5) and the
        // re-equip does not retrigger the engine's derived-AV recompute, so O2
        // stays broken and gets saved. Snapshot the good value here (logged so
        // a test confirms the host AVs are present at this point), re-apply it
        // after the re-equip. If the host AVs are NOT loaded yet this reads ~5
        // and the >10 gate at apply-time safely skips (no regression).
        float entryO2Max = 0.0f;
        if (auto* o2 = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString("Oxygen"))) {
            entryO2Max = player->GetPermanentActorValue(*o2);
            PLOG_INFO(
                "[PULSAR] [DirectRestore] O2 entry-capture (pre-wipe): {:.1f}"sv,
                entryO2Max);
        }

        // Snapshot the post-bridge live inventory for diag only.
        std::uint32_t curItemCount = 0;
        {
            auto* pb = reinterpret_cast<std::uint8_t*>(player);
            auto* invList = *reinterpret_cast<std::uint8_t**>(pb + 0xA0);
            if (invList) {
                curItemCount = *reinterpret_cast<std::uint32_t*>(invList + 0x28);
            }
        }

        // M.2 / NPCSync hook suppression while we touch the inventory.
        const bool prevReplay =
            Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);

        // V4.3 - clear the actor's CURRENT worn-state BEFORE wiping. The
        // bridge equipped HOST's gear; the actor's worn-list + biped 3D now
        // reference host items. Our raw-wipe removes those host items from
        // the container, but the worn-list still points at them - so the
        // client visually wears host's suit/weapon ("client gets host items"),
        // the equip manager is jammed (can't equip anything new), and the
        // player has no functional armor (dies "no suit"). Pass 3's unequip
        // only touched our CLIENT forms, which aren't the ones actually worn,
        // so it no-op'd. Here we enumerate what is ACTUALLY equipped right now
        // (host's forms) and unequip each, leaving a clean slate for the
        // fresh client re-equip after the add.
        {
            // ghostfix-v2 - capture object + actual worn instance so we can both
            // unequip with it AND diagnose why a worn item won't unequip.
            struct Worn { RE::TESBoundObject* obj; RE::TBO_InstanceData* inst; };
            std::vector<Worn> nowWorn;
            player->ForEachInventoryItem(
                [&](const RE::BGSInventoryItem& item)
                    -> RE::BSContainer::ForEachResult {
                if (item.object && item.IsEquipped())
                    nowWorn.push_back({ item.object, item.instanceData.get() });
                return RE::BSContainer::ForEachResult::kContinue;
            });
            g_pendingGhostUnequip.clear();
            std::uint32_t hostUnequipped = 0;
            for (auto& w : nowWorn) {
                const bool ok = DirectUnequipForm(player, w.obj);
                if (ok) ++hostUnequipped;
                // host-EXCLUSIVE worn form = not a client item we're about to
                // re-add. If mid-bridge unequip failed, the raw wipe will strip
                // its inventory entry but leave the worn-3D dangling → ghost.
                // Queue it for the +5s post-bridge teardown retry.
                const bool hostExclusive = cached.find(w.obj) == cached.end();
                if (hostExclusive && !ok)
                    g_pendingGhostUnequip.push_back(w.obj);
            }
            PLOG_INFO(
                "[PULSAR] [DirectRestore] pre-wipe unequip of current worn "
                "(host gear) [ghostfix-v3]: wornForms={} unequipped={} pendingGhost={}"sv,
                nowWorn.size(), hostUnequipped, g_pendingGhostUnequip.size());
        }

        // #poison-guard (overnight 2026-06-24) - before the wipe, the live
        // inventory is the HOST's bridged items (the client's own are held in the
        // smart-ptr `cached`). Record every live form NOT in `cached` as a
        // host-exclusive form. ClientProfile's DB gains delta-merge excludes these
        // so a host item that lingers / re-appears after the +10s snapshot can
        // never poison the client's DB as a fake "gain". Accumulates across bridges.
        //
        // ONLY when a FOREIGN/host save was actually loaded (LoadedSaveIsOwn==false).
        // In the manual-TP path (own save loaded, no host items applied) the live
        // inventory is the CLIENT's own - capturing "live not in cache" there would
        // wrongly flag a recently-looted-but-not-yet-in-DB client item as foreign
        // and permanently drop it from the gains upload. Gating on a foreign load
        // keeps the capture to the only window where host items are actually present.
        if (!LoadedSaveIsOwn()) {
            std::unordered_set<std::uint32_t> foreign;
            player->ForEachInventoryItem(
                [&](const RE::BGSInventoryItem& item)
                    -> RE::BSContainer::ForEachResult {
                if (item.object && cached.find(item.object) == cached.end())
                    foreign.insert(item.object->GetFormID());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (!foreign.empty()) {
                std::lock_guard lk(g_foreignMu);
                for (auto fid : foreign) g_foreignItemSet.insert(fid);
                PLOG_INFO(
                    "[PULSAR] [DirectRestore] foreign-item guard: +{} host-exclusive "
                    "forms (session total {}) excluded from DB gains upload [#poison]"sv,
                    foreign.size(), g_foreignItemSet.size());
            }

            // SKILLS/PERKS - same guard. Live perks here are the HOST's (before
            // ApplyPerksReplace strips them); any not in the client's seeded DB
            // perk set (g_perkSnap) is a host-exclusive perk. Record it so the perk
            // delta-upload in ClientProfile never persists a host perk as ours.
            std::vector<PerkSnap> livePerks;
            WalkPerks(player, livePerks);
            if (!livePerks.empty()) {
                std::unordered_set<std::uint32_t> clientPerks;
                { std::lock_guard lk(g_perkMu);
                  for (const auto& p : g_perkSnap) clientPerks.insert(p.formID); }
                std::lock_guard lk(g_foreignMu);
                std::size_t addedP = 0;
                for (const auto& lp : livePerks)
                    if (!clientPerks.count(lp.formID) &&
                        g_foreignPerkSet.insert(lp.formID).second)
                        ++addedP;
                if (addedP)
                    PLOG_INFO(
                        "[PULSAR] [DirectRestore] foreign-perk guard: +{} host-exclusive "
                        "perks (session total {}) excluded from DB upload [#poison]"sv,
                        addedP, g_foreignPerkSet.size());
            }
        }

        // NOTE - V4.4 engine-path pre-remove (AddObjectToContainer(-count) over
        // all current items to fire UI removal events) was REVERTED: it froze
        // the game thread mid-bridge (2026-05-24, hang right after the pre-wipe
        // unequip line, no "engine-path pre-remove" log → deadlock inside the
        // negative-count remove loop, likely a synchronous container/UI event
        // re-entering during the bridge window). UI ghosts are a lesser evil
        // than a freeze. A safe UI refresh must avoid mass engine-path mutation
        // during the bridge - revisit with a single post-restore menu refresh
        // outside the locked window instead.

        // V4.1 - Pass 2 wipes ALL items unconditionally (empty keep-set).
        // V4 used the cache forms as keep-set to preserve overlapping
        // entries, but Pass 1's per-stack add was unconditional → if the
        // bridge didn't apply (manual-TP path: non-proc cells) the player
        // still had client items, Pass 2 kept them all, then Pass 1 ADDED
        // cache stacks ON TOP → doubled inventory.
        //
        // Because the rich cache holds BSTSmartPointer<ExtraDataList>
        // copies independently of the player's BSTArray, we don't need to
        // preserve BSTArray entries. Wiping everything always works:
        // True bridge applied (proc cell): host items present → wiped → cache restored
        // Manual-TP (non-proc cell): client items present → wiped → identical cache restored
        // Result is the cache contents exactly, regardless of bridge timing.
        // KEEP host-exclusive WORN forms through the wipe so they retain a
        // backing inventory entry. The ghost bug: UnequipObject can't clear a
        // bridged worn item mid-bridge (unequipped=0), and raw-wiping an item
        // that is still equipped leaves a dangling worn-3D binding with NO
        // inventory entry - an unkillable cosmetic ghost (no entry → nothing for
        // unequip OR RemoveItem to grab; observed DIFF alien=0 yet 2 host weapons
        // rendered). Keeping the entry means the worn slot stays backed by a real
        // entry, so the settled-window engine RemoveItem (RemoveForeignGhosts at
        // +5s/+10s, gated on g_foreignItemSet) can unequip + detach 3D + delete it
        // cleanly. g_pendingGhostUnequip holds exactly those host-exclusive worn
        // forms (captured in the pre-wipe block above).
        std::vector<RE::TESBoundObject*> keepGhosts = g_pendingGhostUnequip;
        const auto zeroedEntries = RawRemoveExceptKeepSet(
            static_cast<void*>(player),
            keepGhosts.empty() ? nullptr : keepGhosts.data(),
            keepGhosts.size());
        if (!keepGhosts.empty())
            PLOG_INFO(
                "[PULSAR] [DirectRestore] wipe kept {} host worn form(s) for "
                "settled-window RemoveItem []"sv, keepGhosts.size());

        // Pass 1 V4 - for each cached item, for each cached stack, call
        // AddObjectToContainer with the HELD ExtraDataList smart pointer.
        // Engine wraps the item-add with proper ExtraData attachment,
        // equip-slot bookkeeping, UI event firing. The smart pointer's
        // intrusive refcount kept the ExtraDataList alive across the
        // bridge load even though the engine dropped its references.
        //
        // Note: We add ONE STACK AT A TIME so each gets its own ExtraData.
        // Stackable items (credits, ammo) typically have one stack with
        // total count; non-stackable items (weapons, armor) often have N
        // stacks of count=1 each with per-instance extras (durability etc.).
        std::uint32_t addedItems  = 0;
        std::uint32_t addedStacks = 0;
        std::uint64_t addedUnits  = 0;
        std::uint32_t skipped     = 0;
        std::uint32_t modsApplied = 0;  // Phase B 
        // NOTE on credits (form 0x0000000F): null-extra add made it WORSE
        // (2026-05-24: with cached extra → 4,000 survived; with null extra →
        // 0, MISSING). Money is not a plain inventory stack - its count lives
        // elsewhere and AddObjectToContainer doesn't drive it. Tracked as a
        // separate sub-fix; here we just keep the cached extra (least-broken).
        for (auto& [form, ci] : cached) {
            if (!form) { ++skipped; continue; }
            bool itemAdded = false;
            for (auto& cs : ci.stacks) {
                if (cs.count == 0) continue;
                // Engine takes the smart pointer by value → copy-ctor →
                // IncRef. Our cached `cs.extra` keeps its own ref too,
                // so the ExtraDataList object survives until Reset().
                player->AddObjectToContainer(
                    form,
                    cs.extra,
                    static_cast<std::int32_t>(cs.count),
                    nullptr,
                    // kLoading (not kNone): bulk save-load context → the engine
                    // does NOT fire the per-item "X added" HUD pickup notification
                    // (which spammed one message per restored item on join). The
                    // item still lands with its ExtraData/mods + is equippable.
                    RE::ITEM_TRANSFER_REASON::kLoading);
                addedUnits += cs.count;
                ++addedStacks;
                itemAdded = true;
            }
            if (itemAdded) ++addedItems;
            // Phase B  - seed-sourced items are base forms; the stored
            // M.2 mods are carried in ci.mods. Re-apply them via the engine's
            // OWN per-entry attach core (AttachItemModsNative → FUN_14063de80):
            // allocates the kObjectInstance, grows the array, correct slots.
            // This is the path that lets a DB-restored suit regain its O2 mod
            // and weapons their real mods (no more naked base forms / greyed
            // epics). Returns the count actually attached; 0 just means the item
            // had no resolvable mods (still added, just base).
            if (kApplyModsOnRestore && itemAdded && !ci.mods.empty()) {
                std::uint32_t n = Pulsar::WorldSync::Loot::AttachItemModsNative(
                    player, form->GetFormID(), ci.mods, ci.modSentinel);
                if (n > 0) ++modsApplied;
                // modfix safety net - read the mods back off the just-restored item;
                // WARN only if the count differs from the captured set (a faithful
                // restore is silent). The full exp/act dump was the dev diagnostic;
                // the alarm stays so a future regression (base-default overstack) shows.
                std::vector<std::uint32_t> actual;
                std::uint16_t actSent = 0xFFFF;
                Pulsar::WorldSync::Loot::ReadItemMods(
                    player, form->GetFormID(), actual, actSent);
                if (actual.size() != ci.mods.size())
                    PLOG_WARN(
                        "[PULSAR] [DirectRestore] modfix MISMATCH form=0x{:08X} "
                        "expN={} actN={} (faithful expects equal)"sv,
                        form->GetFormID(), ci.mods.size(), actual.size());
            }
        }

        // Pass 3 - re-equip via ActorEquipManager. Many items will be
        // auto-equipped by virtue of their ExtraDataList carrying equip
        // flags, but the explicit EquipObject call is a belt-and-suspenders
        // safeguard for primary-slot items.
        std::uint32_t equippedOk   = 0;
        std::uint32_t equippedFail = 0;
        for (auto& [form, ci] : cached) {
            if (!form || !ci.equipped) continue;
            // V4.2 - force a genuine equip cycle. The re-added item carries
            // a stale 'worn' flag in its ExtraData, so the inventory shows it
            // equipped (equipped=4 in capture logs) but the bridge load tore
            // down the actor's biped 3D + ActorValue armor modifiers and
            // never rebuilt them. EquipObject(force) alone no-ops because the
            // flag already says equipped → suit shows worn but gives no
            // protection (player dies "no suit"). Unequip first to clear the
            // stale state, then equip to genuinely re-apply biped + AVs.
            DirectUnequipForm(player, form);
            if (DirectEquipForm(player, form)) {
                ++equippedOk;
            } else {
                ++equippedFail;
                PLOG_WARN(
                    "[PULSAR] [DirectRestore] re-equip FAILED: form=0x{:08X}"sv,
                    form->formID);
            }
        }

        // Bug-B (adjacent, 2026-06-18) - VERIFY the re-equip actually took.
        // Symptom: client joins with no visible weapon in hand. The unequip→equip
        // cycle above can silently no-op post-bridge (engine equip-manager jammed
        // mid-load), leaving the weapon in inventory but not worn. Re-check each
        // cached-equipped form; if it's NOT actually equipped now, retry one clean
        // cycle. Cheap (small N), and logs so a test shows ok vs retried vs still-failed.
        {
            std::uint32_t eqVerified = 0, eqRetried = 0, eqStillFail = 0;
            for (auto& [form, ci] : cached) {
                if (!form || !ci.equipped) continue;
                bool isEq = false;
                player->ForEachInventoryItem(
                    [&](const RE::BGSInventoryItem& item)
                        -> RE::BSContainer::ForEachResult {
                    if (item.object == form && item.IsEquipped()) {
                        isEq = true;
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                if (isEq) { ++eqVerified; continue; }
                DirectUnequipForm(player, form);
                if (DirectEquipForm(player, form)) ++eqRetried;
                else { ++eqStillFail;
                    PLOG_WARN("[PULSAR] [DirectRestore] equip STILL failed after retry: form=0x{:08X}"sv,
                              form->formID); }
            }
            PLOG_INFO("[PULSAR] [DirectRestore] equip-verify: ok={} retried={} stillFail={}"sv,
                      eqVerified, eqRetried, eqStillFail);
        }

        // V4.2 - rebuild the player's 3D so the freshly re-equipped armor
        // biped renders and its modifiers apply. Same flag the clone equip
        // path uses (RemotePlayerManager: Set3DUpdateFlag(0x400)). Fire AFTER the
        // verify-retry so a retried weapon also gets its 3D rebuild (Bug-B render).
        player->Set3DUpdateFlag(0x400);

        // restore the client's O2 BASE (suit EXCLUDED). The suit's O2
        // capacity re-applies on top by itself now that the equip passes the
        // item's mod instance (DirectEquipForm). Restoring the permanent TOTAL
        // here (old V4.8) double-counted the suit → O2 came out higher than the
        // host with identical gear (observed 320 vs ~315). Mirrors the
        // Health/CarryWeight base restore below: Base, not Permanent.
        {
            const float o2Base = g_savedO2Base.load();
            if (o2Base > 1.0f) {
                if (auto* o2 = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                        RE::BSFixedString("Oxygen"))) {
                    const float before = player->GetBaseActorValue(*o2);
                    player->SetBaseActorValue(*o2, o2Base);  // base only - suit re-applies on top
                    player->SetActorValue(*o2, o2Base);      // partial refill; suit capacity regens
                    PLOG_INFO(
                        "[PULSAR] [DirectRestore] O2 base restore: {:.1f} -> {:.1f} "
                        "(suit O2 re-applies via equip)"sv, before, o2Base);
                }
            }
        }

        // Progression - converge the player's perks to EXACTLY the client's
        // captured set now that inventory + O2 are back. Strips the host's perks
        // the bridge applied, then re-adds the client's (uses the pre-bridge
        // snapshot). Falls back to keeping live perks if the snapshot is empty.
        ApplyPerksReplace(player);

        // Powers (gap #2) - additively re-add the DB's Starborn/lesser/voice powers
        // (HasSpell-guarded; never strips foreign/quest spells). No-op when the DB
        // holds no powers (the common case).
        ApplyPowersAdditive(player);

        // Research (gap #1) - replay the engine's recipe-unlock for the DB's completed
        // projects (crash-safe self-lookup + per-call SEH; no-op when DB has none).
        ApplyResearch();

        // Restore the client's own available skill points. AddPerk re-grants the
        // perks for FREE (doesn't spend points), so without this the bridge would
        // leave the client with the host's pool → a refund/dupe of spent points.
        if (g_skillPtsValid.load(std::memory_order_acquire)) {
            const std::uint32_t want = g_savedSkillPts.load(std::memory_order_acquire);
            std::uint32_t before = 0;
            ReadSkillPoints(player, before);
            if (WriteSkillPoints(player, want))
                PLOG_INFO("[PULSAR] [DirectRestore] skill points restore: {} -> {}"sv, before, want);
        }

        // Restore level-derived survivability (Health, CarryWeight) BASE - the
        // practical "your level travels" without touching XP/level-up. SetBase
        // only; gear/perk mods re-apply on top (captured value is base, no mods).
        {
            const float hpBase = g_savedHealthBase.load();
            if (hpBase > 1.0f) {
                if (auto* hp = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("Health"))) {
                    const float before = player->GetBaseActorValue(*hp);
                    player->SetBaseActorValue(*hp, hpBase);
                    PLOG_INFO("[PULSAR] [DirectRestore] Health base restore: {:.1f} -> {:.1f}"sv, before, hpBase);
                }
            }
            const float cwBase = g_savedCarryBase.load();
            if (cwBase > 1.0f) {
                if (auto* cw = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("CarryWeight"))) {
                    const float before = player->GetBaseActorValue(*cw);
                    player->SetBaseActorValue(*cw, cwBase);
                    PLOG_INFO("[PULSAR] [DirectRestore] CarryWeight base restore: {:.1f} -> {:.1f}"sv, before, cwBase);
                }
            }
        }

        // O2 (RE) - now that perks are replaced and AV bases restored, force
        // the engine to recompute the perk-derived AVs (Fitness→Oxygen) so the
        // O2 bonus from the restored perks actually registers instead of the
        // async path landing on the un-perked value (base→0 → "out of stamina").
        SafeRefreshDerivedAVs(player);
        PLOG_INFO("[PULSAR] [DirectRestore] perk-derived AV refresh (FUN_1419577c0) fired"sv);

        Pulsar::WorldSync::Loot::SetReplayingFromPacket(prevReplay);

        PLOG_INFO(
            "[PULSAR] [DirectRestore] RestoreClientState V4.1 (wipe-all + ExtraData): "
            "wipedEntries={} addedItems={} addedStacks={} addedUnits={} modsApplied={} "
            "equippedOk={} equippedFail={} skipped={} cached={} curBefore={}"sv,
            zeroedEntries, addedItems, addedStacks, addedUnits, modsApplied,
            equippedOk, equippedFail, skipped, cached.size(), curItemCount);

        // Phase A - host items are now wiped and the live inventory is the
        // client's own (restored items + any loot gained this session). From
        // here it is safe for the DB capture to track the client's progression.
        g_restoreComplete.store(true, std::memory_order_release);

        // re-apply the host's CURRENT time + weather now that we are freshly in the
        // host's world. Tied to the restore (not a one-shot at join) so weather matches
        // after planet landings, where weather is regenerated per proc instance.
        Pulsar::TimeWeatherSync::RequestResync();

        // V4.2 - record what we EXPECTED to put back, then diff against the
        // live inventory immediately. This is the diagnostic that answers
        // "does the player hold their own items or the host's?": ALIEN forms
        // are host leftovers, MISSING forms are adds that didn't take.
        {
            std::lock_guard lk(g_expectedMu);
            g_expected.clear();
            for (auto& [form, ci] : cached) {
                if (!form) continue;
                std::uint64_t total = 0;
                for (auto& cs : ci.stacks) total += cs.count;
                g_expected.emplace(
                    form, ExpectedEntry{ form->formID, total, ci.equipped });
            }
        }
        LogLiveDiff("immediate");

        // Arm deferred re-checks. If the +5s / +10s diffs show ALIEN items
        // appearing (or matched items vanishing) AFTER the immediate diff
        // was clean, another sync module (M.2 LootSync / NPCSync) is
        // re-applying host state on top of us post-restore.
        g_restoreTickMs.store(::GetTickCount64(), std::memory_order_release);
        g_postRestoreStage.store(1, std::memory_order_release);

        // V4 - explicitly release the cache here. Holding the smart pointers
        // longer than necessary keeps ExtraDataList objects in memory and
        // would risk double-attachment if RestoreClientState fires twice.
        // (The cache will be re-filled by the next CaptureClientState.)
        cached.clear();
    }

    void Reset()
    {
        std::lock_guard lk(g_cacheMu);
        g_cache.clear();  // smart pointers destruct → DecRef
        g_cacheValid.store(false, std::memory_order_release);
        // Defensive - disconnect/reset should always release any held lock
        // so the next session starts in a clean state.
        g_cacheLocked.store(false, std::memory_order_release);
        g_loadedSaveIsOwn.store(true, std::memory_order_release);  // default: assume own next session
        g_restoreComplete.store(false, std::memory_order_release);
        g_dbAuthoritative.store(false, std::memory_order_release); // re-evaluated per-join by ApplyProfileData
        g_gainsSnapValid.store(false, std::memory_order_release);  // Phase C3 - invalidate gains baseline
        g_gainsSnapPowersValid.store(false, std::memory_order_release);   // B1
        g_gainsSnapResearchValid.store(false, std::memory_order_release); // B1
        { std::lock_guard lk(g_gainsSnapMu); g_gainsSnap.clear(); g_gainsSnapCredits = 0; g_gainsSnapLevel = 0; g_gainsSnapXP = 0.0f; g_gainsSnapPowers.clear(); g_gainsSnapResearch.clear(); }
        { std::lock_guard lk(g_powerMu); g_powerSnap.clear(); }       // Powers - drop seeded set
        { std::lock_guard lk(g_researchMu); g_researchSnap.clear(); } // Research - drop seeded set
        { std::lock_guard lk(g_foreignMu); g_foreignItemSet.clear(); g_foreignPerkSet.clear(); } // #poison - fresh session, no foreign carryover
    }

    void LockCache()
    {
        // A bridge is about to apply host's save → from now on the live
        // inventory is a host-merge; freeze the clean DB-profile auto-capture
        // for the rest of this launch (anti-poison, Bug A).
        g_bridgedThisSession.store(true, std::memory_order_release);
        // The upcoming bridge re-merges host state - the live inventory is no
        // longer the client's until RestoreClientState runs again (Phase A).
        g_restoreComplete.store(false, std::memory_order_release);
        // Phase C3 - a fresh bridge invalidates the gains baseline; a new one is
        // taken at the next +10s. Until then IsGainsSnapshotValid()=false gates
        // the gains upload so we never full-capture mid-restore (would drift).
        g_gainsSnapValid.store(false, std::memory_order_release);
        g_gainsSnapPowersValid.store(false, std::memory_order_release);   // B1
        g_gainsSnapResearchValid.store(false, std::memory_order_release); // B1
        const bool prev = g_cacheLocked.exchange(true, std::memory_order_acq_rel);
        if (!prev) {
            PLOG_INFO(
                "[PULSAR] [DirectRestore] LockCache - captures frozen until restore"sv);
        }
    }

    void UnlockCache()
    {
        const bool prev = g_cacheLocked.exchange(false, std::memory_order_acq_rel);
        if (prev) {
            PLOG_INFO(
                "[PULSAR] [DirectRestore] UnlockCache - captures resume"sv);
        }
    }

    bool IsCacheLocked()
    {
        return g_cacheLocked.load(std::memory_order_acquire);
    }

    std::size_t CachedItemCount()
    {
        std::lock_guard lk(g_cacheMu);
        return g_cache.size();
    }

    bool IsCacheValid()
    {
        return g_cacheValid.load(std::memory_order_acquire);
    }

    bool HasBridgedThisSession()
    {
        return g_bridgedThisSession.load(std::memory_order_acquire);
    }

    void SetLoadedSaveIsOwn(bool a_own)
    {
        const bool prev = g_loadedSaveIsOwn.exchange(a_own, std::memory_order_acq_rel);
        if (prev != a_own) {
            PLOG_INFO("[PULSAR] [DirectRestore] LoadedSaveIsOwn = {} (was {}) - {}"sv,
                      a_own, prev,
                      a_own ? "save-authoritative (own save)"sv
                            : "DB-authoritative (foreign/host save loaded)"sv);
        }
    }

    bool LoadedSaveIsOwn()
    {
        return g_loadedSaveIsOwn.load(std::memory_order_acquire);
    }

    bool RestoreComplete()
    {
        return g_restoreComplete.load(std::memory_order_acquire);
    }

    // V3 Connect-gate. Reads from PlayerCharacter (verified offsets, see
    // `reference_paths.md` / `RawRemoveExceptKeepSet` raw walker):
    //   refr  + 0x0A0 → inventoryList (BSGuarded raw ptr)
    //   refr  + 0x0B0 → parentCell
    //   list  + 0x028 → dataSize (uint32, populated entry count)
    //
    // V3.2 (2026-05-17) - `invList != nullptr` is NOT sufficient: the
    // engine creates the inventoryList struct very early in save load but
    // populates `dataSize` ~10s later (observed gap in 12:13:31→12:13:41
    // run). Without the dataSize check, Connect dequeues immediately,
    // host's bridge arrives in ~600ms, applies, and the capture catches
    // host's items as if they were client's. Requiring `dataSize > 0`
    // delays Connect until items are materialised so the periodic capture
    // sees client's REAL inventory before the bridge fires.
    //
    // Edge case - brand-new character with truly empty inventory: never
    // gates open. Not a practical concern (Starfield starter saves always
    // have suit/weapon/credits). If it ever matters, add a 30s fallback
    // timer after `cell != nullptr` first reports true.
    bool IsPlayerReadyForConnect()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        bool ready = false;
        __try {
            auto* pb = reinterpret_cast<std::uint8_t*>(player);
            auto* invList = *reinterpret_cast<std::uint8_t**>(pb + 0xA0);
            auto* cell    = *reinterpret_cast<void**>(pb + 0xB0);
            if (invList && cell) {
                const auto dataSize =
                    *reinterpret_cast<std::uint32_t*>(invList + 0x28);
                ready = dataSize > 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ready = false;
        }
        return ready;
    }

    // V3 - game-thread periodic capture. Two cadences:
    // Player just became ready (transition false→true): capture
    //     immediately so a Connect attempted ms later already has cache.
    // Steady-state: re-capture every kPeriodMs to keep cache fresh
    //     even across in-game inventory changes.
    void TickPeriodicCapture()
    {
        constexpr std::uint64_t kPeriodMs = 5000;
        static std::atomic<std::uint64_t> s_lastTickMs{ 0 };
        static std::atomic<bool>          s_lastReady { false };

        const bool ready = IsPlayerReadyForConnect();
        const bool prevReady = s_lastReady.exchange(ready, std::memory_order_acq_rel);
        if (!ready) {
            return;  // main menu / loading - nothing to capture
        }

        const std::uint64_t now = ::GetTickCount64();
        const std::uint64_t last = s_lastTickMs.load(std::memory_order_acquire);

        // Force-capture on ready transition or if never captured yet.
        const bool transitioned = !prevReady;
        const bool firstEver    = !g_cacheValid.load(std::memory_order_acquire);
        if (!transitioned && !firstEver && (now - last) < kPeriodMs) {
            return;
        }

        s_lastTickMs.store(now, std::memory_order_release);
        CaptureClientState();
    }

    // GROUND-TRUTH DUMP - what does the ENGINE's own inventory iterator
    // (ForEachInventoryItem, what the menu reads) return AFTER our cleanup? Logs
    // every host-exclusive form still present + its equip state. If hostItems=0
    // here but the user still sees ghosts → pure menu/UI cache (need a UI refresh
    // event). If hostItems>0 → the engine inventory still holds them (removal gap).
    static void DumpInventoryFull(const char* a_tag)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        std::unordered_set<std::uint32_t> foreign;
        { std::lock_guard lk(g_foreignMu); foreign = g_foreignItemSet; }
        std::uint32_t total = 0, hostItems = 0, equippedHost = 0;
        player->ForEachInventoryItem(
            [&](const RE::BGSInventoryItem& it)
                -> RE::BSContainer::ForEachResult {
            if (!it.object) return RE::BSContainer::ForEachResult::kContinue;
            ++total;
            const std::uint32_t fid = it.object->GetFormID();
            const bool eq = it.IsEquipped();
            if (foreign.count(fid)) {
                ++hostItems;
                if (eq) ++equippedHost;
                PLOG_INFO("[PULSAR] [DUMP][{}] HOST form=0x{:08X} equipped={} []"sv,
                          a_tag, fid, eq ? 1 : 0);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        PLOG_INFO("[PULSAR] [DUMP][{}] engine ForEach: total={} hostItems={} "
                  "equippedHost={} []"sv, a_tag, total, hostItems, equippedHost);
    }

    // remove host "ghost" gear from the client's live inventory after a
    // bridge. The bridge load left the client wearing the host's gear; the raw
    // wipe stripped the inventory entries but UnequipObject can't tear down the
    // worn-binding for those forms, so the engine re-streams them as unusable
    // ghosts (worn, can't select/drop, no functional mods). In the SETTLED
    // post-restore window, find every live form that is (a) NOT in the client's
    // expected restore cache AND (b) was confirmed host-EXCLUSIVE at the bridge
    // (g_foreignItemSet), and delete it via the engine RemoveItem (unequip +
    // detach 3D + remove entry, atomically). The g_foreignItemSet gate means a
    // client's own freshly-looted co-op item is NEVER removed: only items the
    // bridge actually carried over from the host. No-op unless a foreign/host
    // save was bridged (LoadedSaveIsOwn==false).
    // DIAG - probe WHY UnequipObject rejects the player's bridged worn
    // gear (cleared=0) while it works on the clone. Runs at +5s BEFORE the
    // RemoveItem pass, so the kept host forms still have entries + worn state.
    // For each pending ghost form, logs: is it in inventory, IsEquipped flag,
    // instance pointer, formType - then tries unequip TWO ways (null instance =
    // the clone's working call, and the inventory instance) and logs each return
    // + the IsEquipped state after. If the null-instance call clears it, that is
    // also the fix (RemoveForeignGhosts then removes the freed entry cleanly).
    static void DiagGhostUnequip(const char* a_tag)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        if (g_pendingGhostUnequip.empty()) return;

        static REL::Relocation<RE::ActorEquipManager**> singleton{ REL::ID(938503) };
        auto* mgr = *singleton;
        using UnequipFn = bool(*)(RE::ActorEquipManager*, RE::Actor*,
            const RE::BGSObjectInstance&, const RE::BGSEquipSlot*,
            bool, bool, bool, bool, const RE::BGSEquipSlot*);
        static REL::Relocation<UnequipFn> unequipFn{ REL::ID(101951) };

        // lambda: current (found, equipped, inst) for a form
        auto probe = [&](RE::TESBoundObject* bo, bool& found, bool& equipped,
                          RE::TBO_InstanceData*& inst) {
            found = false; equipped = false; inst = nullptr;
            player->ForEachInventoryItem(
                [&](const RE::BGSInventoryItem& it)
                    -> RE::BSContainer::ForEachResult {
                if (it.object == bo) {
                    found = true; equipped = it.IsEquipped();
                    inst = it.instanceData.get();
                    return RE::BSContainer::ForEachResult::kStop;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        };

        for (auto* bo : g_pendingGhostUnequip) {
            if (!bo) continue;
            bool found = false, eq0 = false; RE::TBO_InstanceData* inst = nullptr;
            probe(bo, found, eq0, inst);
            const std::uint8_t ft = static_cast<std::uint8_t>(bo->GetFormType());
            PLOG_INFO("[PULSAR] [DIAG][{}] form=0x{:08X} ft={} found={} equipped={} inst={} []"sv,
                      a_tag, bo->GetFormID(), static_cast<int>(ft),
                      found ? 1 : 0, eq0 ? 1 : 0,
                      reinterpret_cast<const void*>(inst));
            if (!mgr) continue;

            // Attempt 1 - null instance (the clone's working call).
            RE::BGSObjectInstance iNull(bo, nullptr);
            const bool rNull = unequipFn(mgr, player, iNull, nullptr,
                                         false, true, false, true, nullptr);
            bool f1 = false, eq1 = false; RE::TBO_InstanceData* i1 = nullptr;
            probe(bo, f1, eq1, i1);

            // Attempt 2 - inventory instance (only if still equipped).
            bool rInst = false, eq2 = eq1;
            if (eq1 && inst) {
                RE::BGSObjectInstance iInv(bo, inst);
                rInst = unequipFn(mgr, player, iInv, nullptr,
                                  false, true, false, true, nullptr);
                bool f2 = false; RE::TBO_InstanceData* i2 = nullptr;
                probe(bo, f2, eq2, i2);
            }
            PLOG_INFO("[PULSAR] [DIAG][{}] form=0x{:08X} unequip(null)={} eqAfter={} "
                      "unequip(inst)={} eqAfter={} []"sv,
                      a_tag, bo->GetFormID(), rNull ? 1 : 0, eq1 ? 1 : 0,
                      rInst ? 1 : 0, eq2 ? 1 : 0);
        }
    }

    static std::uint32_t RemoveForeignGhosts(const char* a_tag)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return 0;
        if (LoadedSaveIsOwn()) return 0;

        std::unordered_map<RE::TESBoundObject*, ExpectedEntry> expected;
        { std::lock_guard lk(g_expectedMu); expected = g_expected; }
        std::unordered_set<std::uint32_t> foreign;
        { std::lock_guard lk(g_foreignMu); foreign = g_foreignItemSet; }
        if (foreign.empty()) return 0;

        // Currently-equipped forms - NEVER swap-remove an equipped entry (it
        // would dangle the worn slot → crash). Host worn items that the client's
        // re-equip swapped out are no longer equipped, so they still get swept;
        // a host item that stayed equipped (slot the client doesn't fill) is
        // skipped here (kept as a benign item rather than risking a crash).
        std::unordered_set<RE::TESBoundObject*> equipped;
        player->ForEachInventoryItem(
            [&](const RE::BGSInventoryItem& it)
                -> RE::BSContainer::ForEachResult {
            if (it.object && it.IsEquipped()) equipped.insert(it.object);
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // EVERY entry obj incl count=0 - the menu renders count=0 host leftovers
        // (raw-wipe zeroes but doesn't collapse them; the diff walk skips them).
        std::vector<RE::TESBoundObject*> allObjs(kMaxItems, nullptr);
        const std::size_t n = WalkAllEntryObjs(
            static_cast<void*>(player), allObjs.data(), allObjs.size());

        std::vector<RE::TESBoundObject*> toRemove;
        std::unordered_set<RE::TESBoundObject*> seen;
        for (std::size_t i = 0; i < n; ++i) {
            auto* obj = allObjs[i];
            if (!obj || !seen.insert(obj).second) continue;
            if (expected.find(obj) != expected.end()) continue;   // client's own
            if (equipped.count(obj)) continue;                    // still worn - skip (safety)
            if (!foreign.count(obj->GetFormID())) continue;       // not a host item
            toRemove.push_back(obj);
        }
        if (toRemove.empty()) return 0;

        // NOTE (v7 REVERTED): an engine-path remove here
        // (AddObjectToContainer with negative delta, to fire the
        // BGSInventoryListEvent the UI-agent cache listens to) DEADLOCKED the game
        // thread even in this settled window (froze right after DIFF[+5s], no
        // sweep log, only the net thread kept logging - same hang as the V4.4
        // mid-bridge mass remove). The negative-delta path is OFF-LIMITS. We use
        // ONLY the raw swap-remove: it keeps the container + DB clean (verified)
        // but cannot touch the BGSInventoryInterface UI-agent cache, so the
        // in-game menu still renders the ghosts - deferred to a UI-RE pass that
        // edits the agent array directly (no engine UI event = no deadlock).
        const bool prevReplay =
            Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);
        std::uint32_t removed = 0;
        for (auto* obj : toRemove) {
            const std::uint32_t collapsed =
                RawSwapRemoveForm(static_cast<void*>(player), obj);
            if (collapsed) {
                removed += collapsed;
                PLOG_INFO(
                    "[PULSAR] [DirectRestore] ghost sweep[{}] form=0x{:08X} "
                    "collapsed={} []"sv, a_tag, obj->GetFormID(), collapsed);
            }
        }
        Pulsar::WorldSync::Loot::SetReplayingFromPacket(prevReplay);
        player->Set3DUpdateFlag(0x400);
        PLOG_INFO(
            "[PULSAR] [DirectRestore] ghost sweep[{}] SUMMARY: candidates={} "
            "collapsed={} []"sv, a_tag, toRemove.size(), removed);
        return removed;
    }

    // V4.2 - game-thread tick. After a restore arms it (g_postRestoreStage=1),
    // re-runs the live-inventory diff at ~+5s and ~+10s, then disarms. Cheap
    // no-op when not armed. Lets us see whether another sync module overwrites
    // our restore shortly after it completes.
    void TickPostRestoreWatch()
    {
        const int stage = g_postRestoreStage.load(std::memory_order_acquire);
        if (stage == 0) return;

        const std::uint64_t start =
            g_restoreTickMs.load(std::memory_order_acquire);
        const std::uint64_t elapsed = ::GetTickCount64() - start;

        if (stage == 1 && elapsed >= 5000) {
            LogLiveDiff("+5s");
            // DIAG - probe the ghost unequip BEFORE Bug-B clears the pending
            // list + before RemoveForeignGhosts removes the entries.
            DiagGhostUnequip("+5s");
            PLOG_INFO("[PULSAR] [DirectRestore] perk watch[+5s]: live perks={}"sv, LivePerkCount());
            // Bug B (2026-06-18) - re-attach equipped gear 3D now that the post-bridge
            // biped rebuild has SETTLED. RestoreClientState equipped items logically
            // (equippedOk=N, IsEquipped=true) but the weapon/armor 3D didn't render -
            // the in-restore re-equip ran while the actor's 3D was still mid-rebuild,
            // so the model never attached ("weapon equipped but invisible"). The
            // in-restore equip-verify sees IsEquipped=true and skips, so it can't fix
            // the render. Here (+5s, 3D stable) force a clean unequip→equip cycle on
            // each currently-worn item + a 3D flag so the model actually attaches.
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                const bool prevReplay =
                    Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);
                std::vector<RE::TESBoundObject*> worn;
                player->ForEachInventoryItem(
                    [&](const RE::BGSInventoryItem& it)
                        -> RE::BSContainer::ForEachResult {
                    if (it.object && it.IsEquipped()) worn.push_back(it.object);
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                std::uint32_t reattached = 0;
                for (auto* f : worn) {
                    DirectUnequipForm(player, f);
                    if (DirectEquipForm(player, f)) ++reattached;
                }
                // ghostfix-v3 - tear down host-exclusive worn gear whose mid-bridge
                // unequip failed. The raw wipe stripped its inventory entry but its
                // worn-3D/equip binding dangled (= an undroppable "ghost" rendered on
                // the character). The engine has settled now, so retry UnequipObject;
                // it can clear the worn slot + detach the model even without an
                // inventory entry. Logs ok/FAIL so the test shows whether it took.
                std::uint32_t ghostCleared = 0;
                for (auto* bo : g_pendingGhostUnequip) {
                    if (!bo) continue;
                    const bool ok = DirectUnequipForm(player, bo);
                    if (ok) ++ghostCleared;
                    if constexpr (kDiagVerbose) PLOG_INFO(
                        "[PULSAR] [DirectRestore] ghost teardown[+5s] form=0x{:08X} -> {}"sv,
                        bo->GetFormID(), ok ? "ok" : "FAIL");
                }
                if (!g_pendingGhostUnequip.empty())
                    PLOG_INFO(
                        "[PULSAR] [DirectRestore] ghost teardown[+5s] SUMMARY: pending={} cleared={}"sv,
                        g_pendingGhostUnequip.size(), ghostCleared);
                g_pendingGhostUnequip.clear();
                player->Set3DUpdateFlag(0x400);
                // O2 : the unequip→equip cycle above resets the O2 BASE
                // (engine recompute on unequip → ~5, observed). Re-assert the
                // client's O2 BASE here, after the re-attach, so it holds; the
                // suit's O2 re-applies on top via the equip (instance data).
                // BASE not Permanent - else the suit double-counts (320 vs host).
                {
                    const float o2Base = g_savedO2Base.load();
                    if (o2Base > 1.0f) {
                        if (auto* o2 = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                                RE::BSFixedString("Oxygen"))) {
                            player->SetBaseActorValue(*o2, o2Base);
                            player->SetActorValue(*o2, o2Base);  // partial refill; suit regens
                            PLOG_INFO("[PULSAR] [DirectRestore] O2 base re-assert[+5s]: → {:.1f} (suit on top)"sv, o2Base);
                        }
                    }
                    // Re-trigger the perk-derived-AV recompute AFTER the re-attach
                    // (which reset the base) so Fitness→O2 registers post-collapse.
                    SafeRefreshDerivedAVs(player);
                }
                Pulsar::WorldSync::Loot::SetReplayingFromPacket(prevReplay);
                PLOG_INFO("[PULSAR] [DirectRestore] Bug-B 3D re-attach[+5s]: worn={} reattached={}"sv,
                          worn.size(), reattached);
            }
            // delete host ghost gear the unequip-teardown can't clear.
            RemoveForeignGhosts("+5s");
            g_postRestoreStage.store(2, std::memory_order_release);
        } else if (stage == 2 && elapsed >= 10000) {
            LogLiveDiff("+10s");
            // backstop: catch any ghost that re-streamed after the +5s pass.
            RemoveForeignGhosts("+10s");
            // GROUND-TRUTH - engine-view of host items AFTER the sweep.
            DumpInventoryFull("+10s");
            PLOG_INFO("[PULSAR] [DirectRestore] perk watch[+10s]: live perks={}"sv, LivePerkCount());
            // O2  - safety net: re-assert the client's O2 BASE once more
            // if a late equip/3D event collapsed it again after the +5s pass.
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                const float o2Base = g_savedO2Base.load();
                if (o2Base > 1.0f) {
                    if (auto* o2 = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                            RE::BSFixedString("Oxygen"))) {
                        const float curBase = player->GetBaseActorValue(*o2);
                        if (curBase < o2Base - 1.0f) {
                            player->SetBaseActorValue(*o2, o2Base);
                            PLOG_INFO("[PULSAR] [DirectRestore] O2 base re-assert[+10s]: {:.1f} → {:.1f}"sv, curBase, o2Base);
                        }
                    }
                }
            }
            // V4.5 - auto-dump the full live inventory at +10s so we capture
            // the container's actual contents during the window the user
            // reports host "ghosts" in the menu (which persist indefinitely).
            // No keypress needed. Compare engineItems against what the menu
            // shows: if the dump lists only client items, the ghost is a
            // UI-cache artifact and the container is clean. (Gated: the manual
            // hotkey dump still works; this auto per-item dump is the biggest spam.)
            if constexpr (kDiagVerbose) DumpLiveInventory("auto+10s");

            // Phase C3 - take the gains baseline snapshot now that the restore has
            // settled. dbAuth only (in non-dbAuth/bootstrap the full live capture is
            // correct, no delta). This is the reference the gains-capture diffs
            // against so only TRUE session changes (loot/spend/credits) persist.
            if (IsDbAuthoritative()) {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    // Phase C3 - re-apply DB credits BEFORE the snapshot (so the
                    // snapshot reflects the applied value → gains delta for credits
                    // is 0 unless truly earned). The raw wipe stripped the credits
                    // item; nothing re-adds it (not in the seed). Now settled.
                    auto readCredits = [&]() -> std::int32_t {
                        std::int32_t c = 0;
                        player->ForEachInventoryItem(
                            [&](const RE::BGSInventoryItem& it)
                                -> RE::BSContainer::ForEachResult {
                            if (it.object && it.object->GetFormID() == kCreditsFormID) {
                                for (auto& s : it.stacks) c += static_cast<std::int32_t>(s.count);
                                return RE::BSContainer::ForEachResult::kStop;
                            }
                            return RE::BSContainer::ForEachResult::kContinue;
                        });
                        return c;
                    };
                    const std::uint32_t wantC = g_savedCredits.load(std::memory_order_acquire);
                    if (wantC > 0) {
                        const std::int32_t haveC = readCredits();
                        const std::int32_t dC = static_cast<std::int32_t>(wantC) - haveC;
                        if (dC != 0) {
                            if (auto* tf = RE::TESForm::LookupByID(kCreditsFormID)) {
                                if (tf->IsBoundObject()) {
                                    player->AddObjectToContainer(
                                        static_cast<RE::TESBoundObject*>(tf), nullptr,
                                        dC, nullptr, RE::ITEM_TRANSFER_REASON::kLoading);
                                }
                            }
                        }
                        PLOG_INFO("[PULSAR] [DirectRestore] credits restore[+10s]: "
                                  "have={} want={} -> now={}"sv,
                                  haveC, wantC, readCredits());
                    }

                    std::unordered_map<std::uint32_t, std::uint32_t> snap;
                    std::uint32_t snapCredits = 0;
                    player->ForEachInventoryItem(
                        [&](const RE::BGSInventoryItem& it)
                            -> RE::BSContainer::ForEachResult {
                        if (it.object) {
                            std::uint32_t tot = 0;
                            for (auto& s : it.stacks) tot += s.count;
                            const std::uint32_t fid = it.object->GetFormID();
                            if (fid == kCreditsFormID) snapCredits += tot;
                            else                       snap[fid] += tot;
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
                    const std::uint32_t snapLevel = SafeReadActorLevel(player);
                    float snapXP = 0.0f;
                    if (auto* xpav = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                            RE::BSFixedString("Experience")))
                        snapXP = player->GetActorValue(*xpav);
                    // Powers - the live power SET right now (= DB powers re-applied +
                    // any foreign powers the loaded save carried). The capture diffs
                    // against this so foreign powers cancel and only session-acquired
                    // powers persist.
                    std::vector<std::uint32_t> snapPowers;
                    const bool powersOk = WalkPowers(player, snapPowers);
                    // Research - completed-project SET right now (= the loaded/foreign
                    // save's research; the capture diffs against this so it cancels).
                    std::vector<std::uint32_t> snapResearch;
                    const bool researchOk = (WalkResearch(snapResearch) >= 0);
                    {
                        std::lock_guard lk(g_gainsSnapMu);
                        g_gainsSnap = std::move(snap);
                        g_gainsSnapCredits  = snapCredits;
                        g_gainsSnapLevel    = snapLevel;
                        g_gainsSnapXP       = snapXP;
                        g_gainsSnapPowers   = std::move(snapPowers);
                        g_gainsSnapResearch = std::move(snapResearch);
                    }
                    // B1: only mark powers/research snapshots valid when their walk
                    // actually succeeded (a faulted = empty walk must NOT enable the
                    // set-delta, else foreign powers/research poison the DB).
                    g_gainsSnapPowersValid.store(powersOk, std::memory_order_release);
                    g_gainsSnapResearchValid.store(researchOk, std::memory_order_release);
                    g_gainsSnapValid.store(true, std::memory_order_release);
                    PLOG_INFO("[PULSAR] [DirectRestore] gains baseline snapshot[+10s]: "
                              "{} forms, credits={}, level={}, xp={:.0f}, powers={}, research={}"sv,
                              g_gainsSnap.size(), snapCredits, snapLevel, snapXP,
                              g_gainsSnapPowers.size(), g_gainsSnapResearch.size());
                }
            }
            g_postRestoreStage.store(0, std::memory_order_release);
        }
    }

    void DumpLiveInventory(const char* a_tag)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            PLOG_WARN("[PULSAR] [DirectRestore] DUMP[{}]: no player"sv, a_tag);
            return;
        }

        // Engine view - what ForEachInventoryItem (the canonical inventory
        // enumeration the menu builds from) actually returns.
        std::uint32_t items = 0, equipped = 0;
        player->ForEachInventoryItem(
            [&](const RE::BGSInventoryItem& item)
                -> RE::BSContainer::ForEachResult {
            if (!item.object) return RE::BSContainer::ForEachResult::kContinue;
            std::int32_t total = 0;
            for (auto& st : item.stacks)
                total += static_cast<std::int32_t>(st.count);
            if (total == 0) return RE::BSContainer::ForEachResult::kContinue;
            const bool eq = item.IsEquipped();
            PLOG_INFO(
                "[PULSAR] [DirectRestore] DUMP[{}] form=0x{:08X} count={}{}"sv,
                a_tag, item.object->formID, total, eq ? " EQUIPPED" : "");
            ++items;
            if (eq) ++equipped;
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // Raw view - BGSInventoryList.dataSize (entries in the BSTArray).
        std::uint32_t rawDataSize = 0;
        {
            auto* pb = reinterpret_cast<std::uint8_t*>(player);
            auto* invList = *reinterpret_cast<std::uint8_t**>(pb + 0xA0);
            if (invList)
                rawDataSize = *reinterpret_cast<std::uint32_t*>(invList + 0x28);
        }

        PLOG_INFO(
            "[PULSAR] [DirectRestore] DUMP[{}] SUMMARY: engineItems={} "
            "equipped={} rawDataSize={}"sv,
            a_tag, items, equipped, rawDataSize);

        // V4.6 - vital ActorValue diagnostic for the O2-on-jump bug. O2/CO2
        // depletion on exertion is driven by encumbrance (InventoryWeight vs
        // CarryWeight) and the suit; if any of these are in a bad post-bridge
        // state the player reds out on the first jump. Log current + permanent
        // (max) for each. PULSAR does NOT restore these AVs today (they come
        // from the bridge save + .psprof apply), so this tells us whether the
        // bug is over-encumbrance (InventoryWeight > CarryWeight), a stuck CO2,
        // or a wrong O2/Health max.
        auto logAV = [&](const char* name) {
            auto* av = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(
                RE::BSFixedString(name));
            if (!av) {
                PLOG_INFO(
                    "[PULSAR] [DirectRestore] AV[{}] {} = <unresolved>"sv,
                    a_tag, name);
                return;
            }
            const float cur = player->GetActorValue(*av);
            const float perm = player->GetPermanentActorValue(*av);
            PLOG_INFO(
                "[PULSAR] [DirectRestore] AV[{}] {} cur={:.1f} max={:.1f}"sv,
                a_tag, name, cur, perm);
        };
        logAV("Oxygen");
        logAV("CarbonDioxide");
        logAV("CarryWeight");
        logAV("InventoryWeight");
        logAV("Health");
    }
}
