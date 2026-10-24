# Profil-Serialisierung - Design (Teil 1)

> **Status:** Design / Spec. Nächster Schritt = Implementierungsplan (writing-plans).
> **Datum:** 2026-06-15
> **Beziehung:** Fundament für Teil 2 (`docs/design/specs/2026-06-15-spacetimedb-backend-design.md`). Teil 2 setzt die hier definierten Funktionen `SerializeProfile`/`ApplyProfile` voraus.

## Ziel

Ein **storage-agnostisches** Modul, das den kompletten Spieler-Charakter in ein **opakes, versioniertes Blob** serialisiert und ein Blob wieder anwendet - **cross-machine-stabil** (überlebt unterschiedliche Plugin-Ladereihenfolgen zwischen Host und Client). Der Storage selbst (lokale Datei jetzt, STDB später via Companion) bleibt außerhalb dieses Moduls; das Blob ist der Vertrag.

## Scope (v1, User-bestätigt 2026-06-15)

**Drin:** Inventar (inkl. Mods), Equip-State, Credits, O2. **Blob-Format:** JSON v2.
**Bewusst draußen (spätere Folge-Version):** Aussehen (Rasse/Gender/Look), Perks, Skills, weitere Actor-Values, Quest-State.

## Was bereits existiert (wird wiederverwendet, nicht neu gebaut)

| Baustein | Datei | Beitrag |
|---|---|---|
| `Pulsar::Profile::ClientProfile` | `src/Profile/ClientProfile.cpp` + `include/Profile/ClientProfile.h` | Capture-Skelett (`SnapshotFromPlayer`), `ProfileData`-Struct, JSON-Writer/Reader, aktiv im connect-send (Main.cpp) + Save. |
| `PluginLookup` (T34) | `src/NPCSync/PluginLookup.cpp` + Header | FormID↔Plugin-Encoding, projektweit erprobt. Macht Profil-FormIDs portabel. |
| Mod-Primitives | `src/WorldSync/LootSync.cpp` | `ReadItemMods` / `ApplyItemMods` (+ `SafeRestoreModArrayOnInventoryItem`), seit 0cd3ca2 exportiert. |
| `DirectRestore` (L.7 Plan B) | `src/InventorySync/DirectRestore.cpp` + Header | Gehärtetes Live-Apply per Diff-Converge (`AddObjectToContainer`), umgeht den Engine-Bridge; Connect-Gate, periodic capture, Cache-Lock. |

## Datenmodell (`ProfileData` v2)

Erweitert die bestehende Struct. FormIDs werden von rohem `uint32_t` auf **T34-encoded** umgestellt; Waffe/Rüstung verschmelzen in die Inventarliste mit `equipped`-Flag + Mods.

```cpp
struct EncodedFormID {              // T34-portabel statt roher FormID
    std::string plugin;             // z.B. "Starfield.esm" oder Mod-Plugin
    uint32_t    localID = 0;        // FormID maskiert auf plugin-lokal
};

struct ProfileMod {
    EncodedFormID modForm;          // OMOD/Object-Mod-Form (T34)
    uint8_t       slot = 0;         // Mod-Slot-Index
};

struct ProfileItem {
    EncodedFormID form;             // T34-encoded
    int32_t       count    = 0;
    bool          equipped = false; // Equip-State inline
    std::vector<ProfileMod> mods;   // M.2 Mod-Array (Epics)
};

struct ProfileData {
    uint32_t    version = 2;
    std::string playerName;
    uint32_t    credits = 0;
    float       o2      = 0.0f;     // Oxygen-AV
    std::vector<ProfileItem> inventory;  // inkl. Waffen/Rüstung via equipped-Flag
};
```

## Öffentliche Schnittstelle

```cpp
namespace Pulsar::Profile {
    // Live-Spieler → Struct (testbar, ohne IO).
    ProfileData CaptureProfile();

    // Struct → JSON-Bytes / JSON-Bytes → Struct.
    std::vector<uint8_t> EncodeProfile(const ProfileData&);
    bool                 DecodeProfile(std::span<const uint8_t>, ProfileData& out);

    // Struct → Live-Spieler anwenden.
    bool ApplyProfileData(const ProfileData&);

    // Bequemlichkeits-Verkettungen (das ist der Vertrag für Teil 2):
    std::vector<uint8_t> SerializeProfile();                 // = Encode(Capture())
    bool                 ApplyProfile(std::span<const uint8_t>);  // = ApplyProfileData(Decode(blob))

    // Bestehend, bleibt als dünne Wrapper über Blob+Datei erhalten:
    bool Save(const ProfileData&);          // schreibt PULSAR_Profiles/<name>.json
    bool Load(const std::string&, ProfileData&);
    ProfileData SnapshotFromPlayer();        // Alias auf CaptureProfile
}
```

## Capture-Flow (`CaptureProfile`)

1. `ForEachInventoryItem` über den Spieler: pro Item
   - FormID → **T34-encode** via `PluginLookup` → `EncodedFormID`.
   - `count` aus den Stacks summieren.
   - `equipped` aus `ForEachEquippedItem`-Set bestimmen.
   - **Mods** via `ReadItemMods` → `ProfileMod[]`.
2. **Credits** auslesen (Credits-MiscObject-Form; exakte Form-ID in der RE-Phase des Plans bestätigen).
3. **O2** = Oxygen-Actor-Value lesen (AV-Index in der RE-Phase bestätigen).

## Apply-Flow (`ApplyProfileData`)

1. **T34-decode** jedes `EncodedFormID` → live FormID über `PluginLookup` (löst Plugin+localID gegen die aktuelle Ladereihenfolge dieser Maschine auf). Nicht-auflösbare Forms werden geloggt + übersprungen (defense in depth).
2. **Inventar** über `DirectRestore`-Diff-Converge angleichen (add/remove gegen Ist-Zustand) - wiederverwendet die gehärtete Logik, die den Engine-Bridge umgeht.
3. Pro Item mit Mods: `SafeRestoreModArrayOnInventoryItem`.
4. Items mit `equipped == true` re-equippen.
5. **Credits** per Diff setzen, **O2-AV** setzen.

**Vorbehalt (dokumentiert):** Mod-Restore klemmt aktuell auf `commonSlots = min(local, cached)` (Crash-Schutz). Hat ein Epic mehr Mods als lokal allokiert, geht Legendary verloren (grau). Vollständiger Epic-Erhalt setzt den Array-Realloc-Fix aus voraus - bis dahin gilt der Clamp als bekannte Grenze.

## Modul-Grenzen & Abhängigkeiten

- Lebt in `Pulsar::Profile`. Ruft `PluginLookup`, die LootSync-Mod-Primitives und `DirectRestore` auf.
- **Kein Storage im Modul.** Es liefert/konsumiert nur Blobs. Datei-Persistenz bleibt in `Save`/`Load`; die STDB-Anbindung macht ausschließlich der Companion (Teil 2, Ansatz A) - die DLL schreibt/liest nur Blob-Dateien.
- Falls `ReadItemMods`/`ApplyItemMods` sauberer geteilt werden sollen, in der Plan-Phase in einen gemeinsamen Header extrahieren (statt aus LootSync zu ziehen).

## Fehlerbehandlung

| Fall | Verhalten |
|---|---|
| FormID nicht auflösbar (Plugin fehlt auf Zielmaschine) | Item geloggt + übersprungen, Rest des Profils wird angewandt. |
| Mod-Slot-Mismatch (Epic) | Clamp auf `commonSlots` (Grenze), geloggt. |
| Blob fehlt/korrupt/ältere version | `Decode` gibt false → Aufrufer fällt auf bestehendes lokales Verhalten zurück. |
| Player null / nicht spawn-bereit | Capture/Apply no-op (analog `DirectRestore::IsPlayerReadyForConnect`). |

## Teststrategie

- **Round-trip (unit-nah):** `CaptureProfile` → `EncodeProfile` → `DecodeProfile` → Struct-Gleichheit (FormIDs, counts, equipped, mods, credits, o2).
- **T34-Stabilität:** Encode/Decode löst dieselbe Form über (simulierte) abweichende Ladereihenfolge korrekt auf.
- **Apply (in-game, manuell):** Item löschen → `ApplyProfile` → Item zurück, korrekt equipped, Mods dran; Credits + O2 stimmen.
- **Epic-Erhalt (in-game):** Epic-Waffe capturen → auf frisches Profil applien → Legendary erhalten (vorbehaltlich).

## Offene Punkte für den Implementierungsplan

- Exakte Credits-MiscObject-FormID und Oxygen-AV-Index (RE bestätigen).
- Ob `ReadItemMods`/`ApplyItemMods` aus LootSync in einen gemeinsamen Header extrahiert werden.
- Migration des bestehenden connect-send-Pfads (Main.cpp `ClientProfileHeader`) auf das v2-Blob - oder vorerst koexistieren lassen, bis Teil 2 die Lobby-Übergabe liefert.
- Verhalten bei v1-Profilen auf Disk (alte JSON ohne Mods/Credits/O2): version-Feld → tolerantes Laden mit Defaults.
