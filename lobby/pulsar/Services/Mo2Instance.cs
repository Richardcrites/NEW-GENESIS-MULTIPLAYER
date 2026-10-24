using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Pulsar.Services;

// Genesis fork: support for a Mod Organizer 2 install, which is how Wabbajack modlists
// such as Star Wars Genesis are delivered.
//
// MO2 changes three things PULSAR otherwise assumes:
//   * The game must be started THROUGH ModOrganizer.exe. Its virtual file system is what
//     makes the modlist's files visible to Starfield; launching sfse_loader.exe directly
//     starts an unmodded game (and PULSAR's mod-parity check then lies to both players).
//   * The active load order lives in the MO2 profile's plugins.txt. MO2 writes it over
//     %LOCALAPPDATA%\Starfield\Plugins.txt at launch, so an entry added there is lost.
//   * Wabbajack lists usually ship a "Stock Game" copy of Starfield inside the modlist
//     folder. That copy (gamePath in ModOrganizer.ini) is the game root, not Steam's.
//
// Only portable instances (ModOrganizer.ini next to ModOrganizer.exe) are handled, which
// is what Wabbajack installs.
public sealed class Mo2Instance
{
    // Set by AppConfig when Mo2Path changes. Static so the static Plugins.txt helpers
    // (PluginsReader / PluginRegistration) can see it without plumbing the config through.
    public static Mo2Instance? Current { get; private set; }

    public string ExePath { get; }
    public string BaseDir { get; }
    public string GamePath { get; }
    public string Profile { get; }
    public string ProfileDir { get; }
    // Title of the MO2 executable entry that runs sfse_loader.exe ("SFSE" by default).
    public string SfseTitle { get; }

    private Mo2Instance(string exe, string baseDir, string gamePath, string profile,
                        string profileDir, string sfseTitle)
    {
        ExePath = exe;
        BaseDir = baseDir;
        GamePath = gamePath;
        Profile = profile;
        ProfileDir = profileDir;
        SfseTitle = sfseTitle;
    }

    public static bool IsMo2Exe(string? path) =>
        !string.IsNullOrWhiteSpace(path) &&
        string.Equals(Path.GetFileName(path.Trim()), "ModOrganizer.exe", StringComparison.OrdinalIgnoreCase);

    public static void SetCurrent(string? mo2ExePath) =>
        Current = string.IsNullOrWhiteSpace(mo2ExePath) ? null : TryLoad(mo2ExePath.Trim());

    // Parse the portable instance next to ModOrganizer.exe. Null when it is not a usable
    // Starfield instance.
    public static Mo2Instance? TryLoad(string mo2ExePath)
    {
        try
        {
            if (!IsMo2Exe(mo2ExePath) || !File.Exists(mo2ExePath)) return null;
            var dir = Path.GetDirectoryName(mo2ExePath)!;
            var iniPath = Path.Combine(dir, "ModOrganizer.ini");
            if (!File.Exists(iniPath)) return null;
            var ini = IniDocument.Load(iniPath);

            var gamePath = Unwrap(ini.GetValue("General", "gamePath"));
            var profile = Unwrap(ini.GetValue("General", "selected_profile"));
            if (gamePath.Length == 0 || profile.Length == 0) return null;

            var baseDir = ExpandBase(Unwrap(ini.GetValue("Settings", "base_directory")), dir, dir);
            var profilesRoot = ExpandBase(Unwrap(ini.GetValue("Settings", "profiles_directory")),
                                          baseDir, Path.Combine(baseDir, "profiles"));

            return new Mo2Instance(mo2ExePath, baseDir, gamePath, profile,
                                   Path.Combine(profilesRoot, profile), FindSfseTitle(ini));
        }
        catch { return null; }
    }

    // <gamePath>\sfse_loader.exe, or Starfield.exe if SFSE is missing (Setup reports that).
    public string GameExe
    {
        get
        {
            var sfse = Path.Combine(GamePath, "sfse_loader.exe");
            return File.Exists(sfse) ? sfse : Path.Combine(GamePath, "Starfield.exe");
        }
    }

    public string PluginsTxt => Path.Combine(ProfileDir, "plugins.txt");

    // A profile with "local game settings" carries its own StarfieldCustom.ini, which MO2
    // maps over the Documents one. Null when the profile uses the global ini.
    public string? ProfileCustomIni
    {
        get
        {
            var p = Path.Combine(ProfileDir, "StarfieldCustom.ini");
            return File.Exists(p) ? p : null;
        }
    }

    // MO2 command line that runs an executable entry inside the VFS. The empty instance
    // name before ':' selects the portable instance of this ModOrganizer.exe.
    public string LaunchArguments => "\"moshortcut://:" + SfseTitle + "\"";

    // Qt writes strings as @ByteArray(...) and doubles every backslash.
    private static string Unwrap(string? raw)
    {
        var v = (raw ?? "").Trim();
        if (v.StartsWith("@ByteArray(", StringComparison.Ordinal) && v.EndsWith(")", StringComparison.Ordinal))
            v = v.Substring(11, v.Length - 12);
        return v.Replace("\\\\", "\\").Trim();
    }

    private static string ExpandBase(string value, string baseDir, string fallback)
    {
        if (value.Length == 0) return fallback;
        value = value.Replace("%BASE_DIR%", baseDir, StringComparison.OrdinalIgnoreCase);
        return Path.IsPathRooted(value) ? value : Path.Combine(baseDir, value);
    }

    // [customExecutables] holds "N\title" / "N\binary" pairs. Wabbajack lists name the
    // SFSE entry differently, so match on the binary rather than the title.
    private static string FindSfseTitle(IniDocument ini)
    {
        var entries = new Dictionary<string, (string Title, string Binary)>();
        foreach (var kv in ini.EnumerateSection("customExecutables"))
        {
            var slash = kv.Key.IndexOf('\\');
            if (slash <= 0) continue;
            var idx = kv.Key.Substring(0, slash);
            var field = kv.Key.Substring(slash + 1);
            entries.TryGetValue(idx, out var e);
            if (field.Equals("title", StringComparison.OrdinalIgnoreCase)) e.Title = Unwrap(kv.Value);
            else if (field.Equals("binary", StringComparison.OrdinalIgnoreCase)) e.Binary = Unwrap(kv.Value);
            entries[idx] = e;
        }
        var hit = entries.Values.FirstOrDefault(e =>
            !string.IsNullOrEmpty(e.Title) &&
            (e.Binary ?? "").Replace('/', '\\').EndsWith("\\sfse_loader.exe", StringComparison.OrdinalIgnoreCase));
        return string.IsNullOrEmpty(hit.Title) ? "SFSE" : hit.Title;
    }
}
