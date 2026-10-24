using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Pulsar.Services;

// Reads Starfield's active plugin list (Plugins.txt) and derives the set of MODS
// (everything active that is not a base-game master or PULSAR's own ESP). Used for the
// lobby "mods" badge and the on-join parity check. Pure read-only: PULSAR never copies
// or distributes mod files, it only compares load orders and tells the user what is missing.
public static class PluginsReader
{
    // Base-game / official content that does NOT count as a "mod".
    private static readonly HashSet<string> Vanilla = new(StringComparer.OrdinalIgnoreCase)
    {
        "Starfield.esm", "Constellation.esm", "OldMars.esm", "ShatteredSpace.esm",
        "BlueprintShips-Starfield.esm",
        "SFBGS003.esm", "SFBGS004.esm", "SFBGS006.esm", "SFBGS007.esm", "SFBGS008.esm",
        "PULSAR.esp",   // our own co-op plugin (master-flagged) is part of the mod, not a user mod
    };

    // Candidate locations of Plugins.txt, in preference order (Starfield uses
    // %LOCALAPPDATA%\Starfield; some setups mirror it under Documents\My Games\Starfield).
    // Public so the registration writer (PluginRegistration) targets the same files this
    // reader inspects.
    public static IEnumerable<string> Candidates()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (!string.IsNullOrEmpty(local))
            yield return Path.Combine(local, "Starfield", "Plugins.txt");
        var docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        if (!string.IsNullOrEmpty(docs))
            yield return Path.Combine(docs, "My Games", "Starfield", "Plugins.txt");
    }

    // Ordered list of active mod plugin filenames (load order preserved).
    public static List<string> ActiveMods()
    {
        var mods = new List<string>();
        try
        {
            var path = Candidates().FirstOrDefault(File.Exists);
            if (path == null) return mods;
            foreach (var raw in File.ReadAllLines(path))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("#")) continue;
                // Starfield marks enabled plugins with a leading '*'. Lines without it
                // are present-but-disabled; ignore those.
                if (!line.StartsWith("*")) continue;
                var name = line.Substring(1).Trim();
                if (name.Length == 0 || Vanilla.Contains(name)) continue;
                if (!mods.Contains(name, StringComparer.OrdinalIgnoreCase))
                    mods.Add(name);
            }
        }
        catch { /* unreadable / missing -> treat as no mods */ }
        return mods;
    }

    // (count, "modA.esp|modB.esp|...") for transport in the lobby row. '|'-joined so it
    // survives JSON as a single string; order is the host load order.
    public static (uint Count, string List) Snapshot()
    {
        var mods = ActiveMods();
        return ((uint)mods.Count, string.Join("|", mods));
    }

    // Compare a host's mod list (pipe-joined) against the local active mods.
    // Returns mods the local player is MISSING and whether the relative order differs.
    public static (List<string> Missing, bool OrderDiffers) Compare(string hostList)
    {
        var host  = (hostList ?? "").Split('|', StringSplitOptions.RemoveEmptyEntries).ToList();
        var local = ActiveMods();
        var localSet = new HashSet<string>(local, StringComparer.OrdinalIgnoreCase);
        var missing = host.Where(m => !localSet.Contains(m)).ToList();

        // Order check only over the plugins both sides actually have (we never touch the
        // local player's extra mods). Different relative order of shared plugins can still
        // shift FormIDs, so we flag it.
        var hostSet = new HashSet<string>(host, StringComparer.OrdinalIgnoreCase);
        var hostShared  = host.Where(hostSet.Contains).Where(localSet.Contains).ToList();
        var localShared = local.Where(hostSet.Contains).ToList();
        bool orderDiffers = !hostShared.SequenceEqual(localShared, StringComparer.OrdinalIgnoreCase);

        return (missing, orderDiffers);
    }
}
