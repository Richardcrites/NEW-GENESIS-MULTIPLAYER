#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Pulsar::Profile {

    struct ProfileItem {
        // --- existing fields (kept; used by the live connect-send path) ---
        uint32_t formID = 0;     // current-machine FormID (resolved id)
        int32_t  count  = 0;

        // --- v2 additions ---
        std::string plugin;                 // T34: owning plugin filename ("" if base/unresolved)
        uint32_t    localID  = 0;           // T34: plugin-local id
        bool        equipped = false;       // equip-state captured inline
        std::vector<uint32_t> mods;         // M.2 mod array (raw, same-machine)
        uint16_t    modSentinel = 0;        // M.2 mod sentinel
    };

    // Phase C0 (2026-06-19) - a player perk/skill (the Starfield skill tree IS
    // perks). Captured so the DB profile holds the client's progression, not just
    // items. Raw formID + rank for now (base-game perks live in Starfield.esm →
    // stable formID; plugin/localID reserved for future cross-machine T34).
    struct ProfilePerk {
        uint32_t formID  = 0;
        int8_t   rank    = 0;
        std::string plugin;                 // reserved (T34); unused in C0
        uint32_t    localID = 0;            // reserved (T34); unused in C0
    };

    struct ProfileData {
        // --- existing fields (kept) ---
        std::string playerName;
        uint32_t    playerLevel  = 0;
        std::string raceName;
        uint16_t    gender       = 0;
        uint32_t    weaponFormID = 0;       // legacy; still written by old path
        std::vector<uint32_t> armorFormIDs; // legacy; still written by old path
        std::vector<ProfileItem> inventory;

        // --- v2 additions ---
        uint32_t version = 2;
        uint32_t credits = 0;               // form 0x0000000F count
        float    o2      = 0.0f;            // Oxygen ActorValue (current)

        // --- v3 additions (A unification Phase 1): level-derived survivability so
        // the DB profile is the COMPLETE client state (matches DirectRestore's set).
        float    o2Max           = 0.0f;    // Oxygen permanent (base + permanent mods)
        float    healthBase      = 0.0f;    // Health base (level-derived, no mods)
        float    carryWeightBase = 0.0f;    // CarryWeight base
        float    o2Base          = 0.0f;    // Oxygen BASE (suit-excluded; mirrors DirectRestore g_savedO2Base)

        // --- Phase C0 additions (2026-06-19): the rest of "progress" so the DB is
        // a COMPLETE restore source (Phase C: DB-authoritative). Additive - captured
        // + round-tripped now; consumed once the DB-authoritative restore flips on.
        std::vector<ProfilePerk> perks;     // skill tree + perks (rank per perk)
        uint32_t skillPoints = 0;           // unspent skill points (player + 0xF60)

        // --- V3 (2026-06-19): character level + XP. XP is the "Experience"
        // ActorValue (verified via `player.modav experience` console cmd) - read
        // like O2 via GetActorValue. Captured as a co-op DELTA (live − +10s snapshot
        // cancels the host body's value). Capture-only for now (apply = option B).
        float xp = 0.0f;                    // "Experience" ActorValue

        // --- Powers (2026-06-19, gap #2): Starborn powers + lesser/voice powers.
        // The actor's added-spell array (Actor+0x2c8..+0x2d0, SpellItem* entries)
        // filtered to power-type spells (kPower/kLesserPower/kVoicePower) so gear/
        // quest abilities (kAbility/kEnchantment) never leak. Raw base-game formIDs
        // (Starfield.esm → stable). Captured as a SET-DELTA (baseline ∪ (live − +10s
        // snapshot)) and applied additively (Actor::AddSpell, HasSpell-guarded).
        std::vector<uint32_t> powers;

        // --- Research / crafting recipes (2026-06-19, gap #1): completed research
        // projects (RSPJ). The research-manager singleton holds a std::map of every
        // project keyed by formID; a project's state byte (value+0x08) == 2 means
        // "researched" (unlocks its crafting recipes). Captured as a monotonic
        // SET-DELTA (baseline + session-gained); applied by replaying the engine's
        // own per-project unlock.
        std::vector<uint32_t> research;
    };

    // ---- existing API (unchanged behaviour) ----
    bool Save(const ProfileData& a_data);
    bool Load(const std::string& a_playerName, ProfileData& a_out);
    ProfileData SnapshotFromPlayer();   // alias to CaptureProfile after Task 3

    // ---- v2 codec API (the contract Teil 2 consumes) ----
    ProfileData CaptureProfile();                                   // Task 3
    std::vector<uint8_t> EncodeProfile(const ProfileData&);         // Task 2
    bool DecodeProfile(std::span<const uint8_t>, ProfileData& out); // Task 2
    bool ApplyProfileData(const ProfileData&);                      // Task 4
    ///restore the client's latest clean snapshot after a both-down local
    // game-over reload (preserves within-session progress). Returns false if no clean
    // snapshot / DB baseline exists this session.
    bool ReapplyCachedDbProfile();
    ///call on the loadMenu RISING edge: a save-load just started, so suppress
    // clean-snapshot caching for ~15s (the live state becomes the host-bridged save).
    void NotifySaveLoadStarted();
    std::vector<uint8_t> SerializeProfile();                        // Task 5 = Encode(Capture())
    bool ApplyProfile(std::span<const uint8_t>);                    // Task 5 = ApplyProfileData(Decode())

    // ---- dev self-test (load-time codec check + hotkey-driven apply check) ----
    bool RunSelfTest();                  // Task 2: synthetic Encode->Decode round-trip, logs PASS/FAIL
    void RunInGameRoundTripTest();       // Task 5: Capture->Encode->Decode->Apply in-game, logs diff
    void SaveProfileBlobTest();          // F10: SerializeProfile() into an in-memory snapshot blob
    void RestoreProfileBlobTest();       // F11: ApplyProfile(snapshot) - restores the F10 loadout
}
