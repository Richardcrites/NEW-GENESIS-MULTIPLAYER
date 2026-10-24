using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Pulsar.Services;

// Read/merge helper for the game mod's flat config file PULSAR.ini, the source of
// truth the DLL reads at load. The file is a simple `Key = Value` text file:
//   * keys are case-insensitive
//   * `;` and `#` start comments
//   * `[sections]` are ignored by the DLL (we treat them as plain passthrough lines)
//
// The DLL parses these hotkey/value lines (defaults in parentheses):
//   HotkeyAutoFollow      (F7)   follow partner / co-op fast-travel co-location
//   HotkeyManualBridge    (F8)   manual bridge / join host's location
//   HotkeySaveCharacter   (F10)  save my character now
//   HotkeyRestoreCharacter(F11)  restore my character
//   devHotkeys            (0)    bool gate for developer/diagnostic hotkeys
//
// Value format for hotkeys = a VK name the DLL's ParseVKey understands:
//   F1..F24, single letter A..Z, digit 0..9, Numpad0..Numpad9, or raw 0xNN.
//
// MERGE semantics on write: existing lines for the managed keys are replaced in
// place (case-insensitive match on the key before '='); every other line/comment
// is preserved verbatim; managed keys that are missing are appended.
public static class PulsarIni
{
    private const string HeaderComment =
        "; PULSAR.ini - managed by the Pulsar tool. Hotkeys: F1-F24 / A-Z / 0-9 / Numpad0-9 / 0xNN.";

    // Resolve <gameRoot>\Data\SFSE\Plugins\PULSAR.ini from the configured Starfield
    // exe path (which points at sfse_loader.exe / Starfield.exe in the game root).
    // Returns null when the path is empty or the directory can't be derived.
    public static string? ResolvePath(string? starfieldExePath)
    {
        var exe = starfieldExePath?.Trim();
        if (string.IsNullOrEmpty(exe)) return null;
        string? gameRoot;
        try { gameRoot = Path.GetDirectoryName(exe); }
        catch { return null; }
        if (string.IsNullOrEmpty(gameRoot)) return null;
        return Path.Combine(gameRoot, "Data", "SFSE", "Plugins", "PULSAR.ini");
    }

    // Read all `Key = Value` pairs from an ini file. Returns an empty (case-
    // insensitive) dictionary when the file is missing or unreadable. Last value
    // wins on duplicate keys (mirrors a flat last-write-wins parser).
    public static Dictionary<string, string> Read(string? path)
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return map;
        try
        {
            foreach (var raw in File.ReadAllLines(path))
            {
                var line = raw.Trim();
                if (line.Length == 0) continue;
                if (line[0] is ';' or '#' or '[') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                var key = line[..eq].Trim();
                if (key.Length == 0) continue;
                map[key] = line[(eq + 1)..].Trim();
            }
        }
        catch { /* return whatever parsed so far */ }
        return map;
    }

    // Merge the given key/value pairs into the ini at `path` and write it back.
    // Preserves all unmanaged lines/comments, replaces managed keys in place, and
    // appends any managed key that wasn't present. Creates the file and parent
    // directories (with a header comment) if absent. Throws on IO failure so the
    // caller can surface a status message.
    public static void Merge(string path, IReadOnlyList<KeyValuePair<string, string>> values)
    {
        if (string.IsNullOrEmpty(path)) throw new ArgumentException("empty path", nameof(path));

        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        // Track which managed keys still need to be appended (start with all of them).
        var remaining = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var kv in values) remaining[kv.Key] = kv.Value;

        var output = new List<string>();
        bool freshFile = !File.Exists(path);

        if (!freshFile)
        {
            string[] existing;
            try { existing = File.ReadAllLines(path); }
            catch { existing = Array.Empty<string>(); }

            foreach (var raw in existing)
            {
                var trimmed = raw.TrimStart();
                // Comments, blanks, and section headers pass through untouched.
                if (trimmed.Length == 0 || trimmed[0] is ';' or '#' or '[')
                {
                    output.Add(raw);
                    continue;
                }
                int eq = raw.IndexOf('=');
                if (eq <= 0)
                {
                    output.Add(raw);
                    continue;
                }
                var key = raw[..eq].Trim();
                if (remaining.TryGetValue(key, out var newVal))
                {
                    // Replace this managed key's line in place (canonical formatting).
                    output.Add($"{key} = {newVal}");
                    remaining.Remove(key);
                }
                else
                {
                    output.Add(raw);
                }
            }
        }
        else
        {
            output.Add(HeaderComment);
        }

        // Append any managed keys that weren't already present, in the caller's order.
        if (remaining.Count > 0)
        {
            // Add a blank separator if the file already had content.
            if (output.Count > 0 && output[^1].Trim().Length != 0)
                output.Add("");
            foreach (var kv in values)
            {
                if (remaining.ContainsKey(kv.Key))
                {
                    output.Add($"{kv.Key} = {kv.Value}");
                    remaining.Remove(kv.Key);
                }
            }
        }

        var sb = new StringBuilder();
        foreach (var line in output) sb.Append(line).Append("\r\n");
        File.WriteAllText(path, sb.ToString());
    }
}
