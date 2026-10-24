using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;

namespace Pulsar.Services;

// Registers PULSAR's plugin in Starfield's Plugins.txt - the modern, coexistence-friendly
// load mechanism.
//
// Why not sTestFile: defining any sTestFileN in StarfieldCustom.ini makes Starfield ignore
// Plugins.txt entirely, so the player's other mods (which live in Plugins.txt) stop loading.
// A Plugins.txt entry loads PULSAR alongside every other enabled mod. Native engine support
// since Starfield 1.12.30 (the "Plugins.txt Enabler" is obsolete); PULSAR targets 1.16.x.
//
// PULSAR ships as PULSAR.esp with the ESM master flag SET (a master needs the flag, not the
// .esm extension). Keeping the .esp filename means existing saves still find it - no "missing
// content" warning - while the flag still makes it a real master other mods can build on.
//
// Refs: blog.ortham.net "Load order in Starfield" (sTestFile bypasses Plugins.txt);
// nexusmods Plugins.txt Enabler posts (obsolete since 1.12.30).
public static class PluginRegistration
{
    public const string PluginName = "PULSAR.esp";
    private const string AltName = "PULSAR.esm";   // collapse any stray .esm entry onto the .esp

    // Starfield marks enabled plugins with a leading '*'. Match a line (with or without the
    // marker) against a plugin name, case-insensitively.
    private static bool LineNames(string line, string name) =>
        string.Equals(line.TrimStart('*').Trim(), name, StringComparison.OrdinalIgnoreCase);

    private static bool IsPulsarLine(string line) =>
        LineNames(line, PluginName) || LineNames(line, AltName);

    // Does an sTestFileN VALUE name PULSAR's plugin (PULSAR.esp or a stray PULSAR.esm)?
    public static bool IsPulsarPlugin(string? sTestFileValue) =>
        !string.IsNullOrWhiteSpace(sTestFileValue) && IsPulsarLine(sTestFileValue);

    // %USERPROFILE%\Documents\My Games\Starfield\StarfieldCustom.ini (OneDrive-aware via
    // SpecialFolder.MyDocuments). The single source of truth for this path.
    public static string StarfieldCustomIniPath =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            "My Games", "Starfield", "StarfieldCustom.ini");

    // The Plugins.txt the game actually reads (first existing candidate), or null if none yet.
    public static string? ActivePath() => PluginsReader.Candidates().FirstOrDefault(File.Exists);

    // Where to write: the existing file if there is one, else the preferred default location.
    private static string WritePath() =>
        PluginsReader.Candidates().FirstOrDefault(File.Exists)
        ?? PluginsReader.Candidates().First();

    // True if PULSAR.esp is present AND enabled (leading '*') in Plugins.txt.
    public static bool IsEnabled()
    {
        var path = ActivePath();
        return path != null && IsEnabledAt(path);
    }

    // Ensure exactly one canonical, enabled "*PULSAR.esp" line, dropping any prior PULSAR line
    // (disabled, or a stray PULSAR.esm). Every unrelated line keeps its place and order.
    // Backs the file up once (.pulsar.bak) before the first edit. Returns true on success.
    public static bool Enable()
    {
        var path = WritePath();
        BackupOnce(path);
        return EnableAt(path);
    }

    // Delete every sTestFileN (N=1..10) in [General] that points at PULSAR's plugin. Returns
    // true if any line was removed. Removal (not blanking) matters: an empty sTestFileN= still
    // counts as a defined property and would keep Starfield ignoring Plugins.txt.
    public static bool RemovePulsarTestFiles(IniDocument doc)
    {
        bool any = false;
        for (int n = 1; n <= 10; n++)
        {
            var key = "sTestFile" + n.ToString(CultureInfo.InvariantCulture);
            if (IsPulsarPlugin(doc.GetValue("General", key)))
                any |= doc.RemoveKey("General", key);
        }
        return any;
    }

    // Full migration to the Plugins.txt load mechanism: enable *PULSAR.esp AND strip PULSAR's
    // own sTestFile entries from StarfieldCustom.ini (so they no longer bypass Plugins.txt).
    // Self-contained (own load/save + backup), idempotent, never throws. Used by the auto-
    // updater so an updated install is never left half-migrated; the Setup page uses the
    // doc-based helpers directly so all its fixes share one ini round-trip.
    public static void EnsurePluginsTxtLoad()
    {
        try
        {
            Enable();
            var ini = StarfieldCustomIniPath;
            if (!File.Exists(ini)) return;
            var doc = IniDocument.Load(ini);
            if (RemovePulsarTestFiles(doc))
            {
                BackupOnce(ini);
                doc.Save(ini);
            }
        }
        catch { /* best-effort; the Setup page still surfaces any remaining issue */ }
    }

    // Copy <file> -> <file>.pulsar.bak once (never clobber an existing backup, no-op if the
    // file does not exist yet). Same convention the Setup page uses for StarfieldCustom.ini.
    private static void BackupOnce(string path)
    {
        try
        {
            if (!File.Exists(path)) return;
            var bak = path + ".pulsar.bak";
            if (!File.Exists(bak)) File.Copy(path, bak);
        }
        catch { /* backup is best-effort */ }
    }

    // --- path-injectable cores (also the unit-test surface) ----------------------------------

    internal static bool IsEnabledAt(string path)
    {
        try
        {
            if (!File.Exists(path)) return false;
            foreach (var raw in File.ReadAllLines(path))
            {
                var line = raw.Trim();
                if (line.StartsWith("*") && LineNames(line, PluginName)) return true;
            }
        }
        catch { /* unreadable -> treat as not enabled */ }
        return false;
    }

    internal static bool EnableAt(string path)
    {
        try
        {
            var lines = File.Exists(path)
                ? new List<string>(File.ReadAllLines(path))
                : new List<string>();

            lines.RemoveAll(l => l.Trim().Length > 0 && IsPulsarLine(l));
            lines.Add("*" + PluginName);

            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.WriteAllLines(path, lines);   // UTF-8 without BOM (game-readable)
            return true;
        }
        catch { return false; }
    }
}
