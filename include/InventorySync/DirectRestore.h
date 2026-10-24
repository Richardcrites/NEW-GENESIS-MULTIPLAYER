#pragma once

// L.7 Plan B - Direct API Restore
//
// V5/V6 mutation-watchpoint evidence proved that the engine bridge load
// writes 300-450 bytes to the live PlayerCharacter struct through a code
// path that bypasses ALL six instrumented engine functions (chunk reader,
// per-form dispatcher, BFCB inner loader, PC::FinishLoadGame, plus the
// claimed PerformGameReset + PlayerRefrReInit). Decompile RVAs misled
// three times in 1.16.236; further blind RE has poor ROI.
//
// This module bypasses the engine apply mechanism entirely:
//   1. Capture client's inventory (+later: AVs + perks) before the bridge
//      into an in-memory cache.
//   2. After the engine bridge completes (and after the existing M.3
//      .psprof apply chain runs in DoApply), enumerate the live player's
//      inventory, diff against the cache, and converge via
//      TESObjectREFR::AddObjectToContainer (negative count = remove).
//
// MVP scope: inventory only. AVs + perks deferred to a follow-up once
// inventory is validated.

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RE { class TESBoundObject; }

namespace Pulsar::InventorySync::DirectRestore
{
    // A unification Phase 3 - seed the restore cache directly from the DB-backed
    // client profile (clean, host-free) at JOIN, before any bridge. Breaks the
    // cache-poisoning bootstrap: the first pre-bridge live-capture used to grab host
    // items if the client wasn't fully its own yet (Bug A: "host inventory leaks to
    // client"). Seeding from the authoritative profile guarantees a clean cache from
    // the start; the periodic capture then maintains it (clean, lock-gated). Seeded
    // Seeded items carry no ExtraData (base forms), but DO carry the M.2 mod
    // array (Phase B /): after the bridge restore re-adds the base form,
    // the mods are re-applied via LootSync::AttachItemModsNative (the engine's
    // own allocate+grow+correct-slot attach core) so DB-restored gear isn't
    // naked (no more engine-rolled random mods; suit regains its O2 mod).
    struct SeedItem {
        RE::TESBoundObject*        form;
        std::int32_t               count;
        bool                       equipped;
        std::vector<std::uint32_t> mods;            // raw same-machine mod-form IDs ("" → none)
        std::uint16_t              modSentinel = 0xFFFF;
    };
    void SeedCacheFromProfile(const std::vector<SeedItem>& items);

    // Phase C (2026-06-19) - DB-AUTHORITATIVE restore. When set true (config
    // `dbAuthoritative` ON AND the DB profile is non-empty), the DB seed is the
    // restore source for EVERY join: CaptureClientState is suppressed (so the
    // seeded items/AVs/perks survive the bridge), and the restore applies the
    // DB's progression regardless of which save is loaded. This is what makes
    // co-op gains survive loading any save (own/stale/host). When false (toggle
    // off OR empty DB) the legacy save-authoritative / bootstrap path runs (the
    // live save is captured and, on an empty DB, uploaded to seed it). Set by
    // ClientProfile::ApplyProfileData at join.
    void SetDbAuthoritative(bool a_on);
    bool IsDbAuthoritative();

    // Phase C - seed the restore AV baselines from the DB profile (so the existing
    // RestoreClientState AV apply uses the DB's values, not a live capture). Values
    // <= threshold are ignored (keep whatever was captured). a_o2Base is the Oxygen
    // BASE (GetBaseActorValue, suit-excluded - mirrors g_savedO2Base, the value the
    // restore actually sets). Call at join when DB-authoritative.
    void SeedAVsFromProfile(float a_o2Base, float a_o2Max,
                            float a_healthBase, float a_carryBase);

    // Phase C3 - seed the DB credits so the post-restore watch can re-apply them
    // to the live player. The raw wipe strips the credits item (0x0000000F) and
    // nothing re-adds it (credits aren't in the item seed), so without this the
    // restored client shows 0 credits. Applied at +10s (settled) in
    // TickPostRestoreWatch via AddObjectToContainer. Call at a dbAuth join.
    void SeedCreditsFromProfile(std::uint32_t a_credits);

    // Phase C - seed the perk snapshot + unspent skill points from the DB profile,
    // so RestoreClientState's ApplyPerksReplace restores the DB's perks/skills
    // instead of the loaded save's. Each pair = (perk formID, rank). Call at join
    // when DB-authoritative. Empty list clears nothing (no-op) to avoid stripping.
    void SeedPerksFromProfile(
        const std::vector<std::pair<std::uint32_t, std::int8_t>>& a_perks,
        std::uint32_t a_skillPoints);

    // Powers (2026-06-19, gap #2) - seed the DB's Starborn/lesser/voice powers so
    // RestoreClientState re-adds them ADDITIVELY (Actor::AddSpell, HasSpell-guarded;
    // never strips, so foreign/quest spells are untouched). Raw base-game SPEL
    // formIDs. Empty list = no-op. Call at a DB-authoritative join.
    void SeedPowersFromProfile(const std::vector<std::uint32_t>& a_powers);

    // Research (2026-06-19, gap #1) - seed the DB's completed-research formIDs so
    // RestoreClientState replays the engine recipe-unlock for each (crash-safe). Raw
    // base-game RSPJ formIDs. Empty list = no-op. Call at a DB-authoritative join.
    void SeedResearchFromProfile(const std::vector<std::uint32_t>& a_research);

    // Game-thread. Walks player->inventoryList and stores form→count map
    // in an internal cache. Safe to call repeatedly - overwrites the cache.
    // No-op if player or inventoryList is null.
    void CaptureClientState();

    // Game-thread. If a cache is valid, enumerate the live player's
    // current inventory, compute diff against the cache, and apply the
    // diff via AddObjectToContainer(±count). No-op if cache is invalid
    // or player is null.
    void RestoreClientState();

    // Invalidate the cache (e.g., on disconnect when no further restores
    // should happen).
    void Reset();

    // For diagnostics - number of items currently in the cache.
    std::size_t CachedItemCount();

    // True if a captured cache is currently held.
    bool IsCacheValid();

    // A unification Phase 3 - true once a host save-bridge has been applied this
    // game launch (set in LockCache, sticky until process restart). While true,
    // the live player inventory is a host-merge and is NOT a safe source for the
    // authoritative DB profile (it may carry host-leaked items - Bug A). The
    // clean DB-profile auto-capture (Lobby::TickCleanProfileCapture) only runs
    // while this is false, i.e. from the client's OWN loaded save pre-bridge.
    bool HasBridgedThisSession();

    // 2026-06-18 - whether the loaded save is the client's OWN character (true)
    // or a foreign/host save (false, e.g. the co-op auto-save = the host's
    // bridged character). Set by ClientProfile::ApplyProfileData at join
    // (Health-base signature vs the DB profile). When false, CaptureClientState
    // is suppressed so the DB-profile seed (the client's loadout) is the restore
    // source instead of the host items the loaded save carries, and the
    // clean-capture won't upload the host save to the DB. Default true.
    void SetLoadedSaveIsOwn(bool a_own);
    bool LoadedSaveIsOwn();

    // Phase A (co-op gain persistence) - true once RestoreClientState has
    // completed a bridge restore: host items are wiped, the live inventory is
    // the client's own (restored items + loot gained this session), so it is
    // safe to capture to the DB. Cleared on the next bridge (LockCache) + Reset.
    bool RestoreComplete();

    // V3 Connect-gate - true if the player exists AND is fully spawned into
    // a cell with an initialised inventoryList. False on main-menu and
    // during loading. Cheap pointer-only check; safe from any thread.
    bool IsPlayerReadyForConnect();

    // V3 - game-thread tick. Rate-limited (default 5s) periodic
    // CaptureClientState call when the player is ready. Keeps the cache
    // fresh so OnSaveReceived's bridge always has a recent snapshot to
    // restore from. Cheap when player is not ready or rate-limit hasn't
    // elapsed.
    void TickPeriodicCapture();

    // V3.3 - bridge transition lockout. WorldSync::OnSaveReceived calls
    // `LockCache()` AFTER its final pre-bridge capture; the engine then
    // applies the bridge (replacing the player's inventory with host's).
    // While locked, all `CaptureClientState()` calls (periodic ticks +
    // any future caller) become no-ops so the cache stays frozen at the
    // last known-good client snapshot. `RestoreClientState()` calls
    // `UnlockCache()` at the end so periodic captures resume during
    // normal play. Without this lock, the periodic tick fired between
    // bridge-applied and restore-fires, overwriting the cache with
    // host's items (observed 2026-05-17 V3.2 logs: second F8 had
    // cached=23 curBefore=23 = no-op).
    void LockCache();
    void UnlockCache();
    bool IsCacheLocked();

    // V4.2 - game-thread tick. After RestoreClientState completes it arms a
    // deferred verification; this re-walks the live inventory at ~+5s and
    // ~+10s and logs a diff against what we restored. Catches other sync
    // modules (M.2 LootSync / NPCSync) re-applying host state post-restore.
    // Cheap no-op until armed. Call alongside TickPeriodicCapture().
    void TickPostRestoreWatch();

    // Phase C3 (co-op gains) - the live inventory snapshot taken the moment the
    // restore settled (+10s, dbAuth only). ClientProfile's delta gains-capture
    // uses it as the reference: (later capture − this snapshot) = pure session
    // gains/losses, because the restore's own count artifacts AND the host-save's
    // credits are present in BOTH sides and cancel. Valid only after the +10s
    // post-restore watch ran in a DB-authoritative session; cleared on Reset/bridge.
    bool IsGainsSnapshotValid();
    bool GetGainsSnapshot(std::unordered_map<std::uint32_t, std::uint32_t>& a_outCounts,
                          std::uint32_t& a_outCredits, std::uint32_t& a_outLevel,
                          float& a_outXP);

    // Powers (2026-06-19) - the live power-type spell SET captured at the +10s
    // post-restore snapshot (dbAuth only). ClientProfile's power gains-capture uses
    // it as the reference: (later capture − this set) = powers acquired THIS session
    // (the host/foreign save's powers are in BOTH and cancel). Returns the formIDs.
    bool GetGainsSnapshotPowers(std::vector<std::uint32_t>& a_outPowers);

    // Research (2026-06-19, gap #1) - the completed-research formID SET captured at
    // the +10s post-restore snapshot (dbAuth only). Same set-delta reference as
    // powers: (later capture − this set) = research completed THIS session, so the
    // host/foreign save's research cancels and only the client's own accrues.
    bool GetGainsSnapshotResearch(std::vector<std::uint32_t>& a_outResearch);

    // #poison-guard (overnight 2026-06-24) - host-EXCLUSIVE item forms the bridge
    // briefly placed in the client's live inventory (recorded across every bridge
    // this session, before each wipe). ClientProfile's DB gains delta-merge
    // excludes these formIDs so a host item that leaks back into live AFTER the
    // +10s gains snapshot can never be uploaded into the client's DB as a fake
    // "session gain" (the inventory analogue of the powers/research foreign guard).
    void GetForeignItemSet(std::unordered_set<std::uint32_t>& a_out);
    // SKILLS/PERKS analogue - host-exclusive perks seen during bridges this session.
    void GetForeignPerkSet(std::unordered_set<std::uint32_t>& a_out);

    // V4.5 diagnostic - on-demand full inventory dump. Logs EVERY item the
    // engine's ForEachInventoryItem returns (formID + count + equipped +
    // resolved name) AND the raw BGSInventoryList dataSize, tagged so it can
    // be correlated with a screenshot. Wired to a hotkey so the user can fire
    // it while the inventory menu shows the "host ghosts": if the dump lists
    // only the client's items while the menu shows host items, the ghost is
    // a UI-cache artifact (container is clean); if the dump lists the host
    // items, they are really in the container. Resolves the menu-vs-container
    // question that alien=0 alone can't (alien only compares live==cache).
    void DumpLiveInventory(const char* a_tag);
}
