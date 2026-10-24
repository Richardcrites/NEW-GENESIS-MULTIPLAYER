using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Pulsar.Services;

// Section-aware ini editor for StarfieldCustom.ini.
//
// PulsarIni is a FLAT key=value helper for the mod's own PULSAR.ini (sections are
// passthrough). The game's StarfieldCustom.ini, by contrast, is genuinely
// SECTIONED ([Archive], [General], [Papyrus], …) and the same key can legitimately
// appear in different sections - so we need a parser that tracks the current
// section.
//
// Design goals:
//   * preserve ORDER, comments, and blank lines verbatim
//   * preserve every unrelated section AND every unrelated key inside a section
//   * GetValue(section, key) → current value (case-insensitive section + key)
//   * SetValue(section, key, value) → replace in place, or create the key (and the
//     section if missing) without disturbing anything else
//   * value may be empty (e.g. sResourceDataDirsFinal=) - empty is a real value
//
// We model the file as an ordered list of physical lines and remember which line
// index a given section/key lives on. Section membership ends at the next
// `[Section]` header (or EOF).
public sealed class IniDocument
{
    // Physical lines, kept verbatim apart from edited/inserted ones.
    private readonly List<string> _lines = new();

    private IniDocument() { }

    public static IniDocument Parse(string? text)
    {
        var doc = new IniDocument();
        if (!string.IsNullOrEmpty(text))
        {
            // Split on \n and strip a trailing \r so we don't care about CRLF vs LF
            // on input; we always write CRLF back (game-native).
            foreach (var raw in text.Replace("\r\n", "\n").Split('\n'))
                doc._lines.Add(raw);
            // A trailing newline in the source produces a final empty element; drop
            // it so we don't accumulate blank lines on every round-trip.
            if (doc._lines.Count > 0 && doc._lines[^1].Length == 0)
                doc._lines.RemoveAt(doc._lines.Count - 1);
        }
        return doc;
    }

    public static IniDocument Load(string path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path))
            return new IniDocument();
        return Parse(File.ReadAllText(path));
    }

    // True if a line is a `[Section]` header. Returns the trimmed section name.
    private static bool IsSectionHeader(string line, out string name)
    {
        name = "";
        var t = line.Trim();
        if (t.Length >= 2 && t[0] == '[' && t[^1] == ']')
        {
            name = t[1..^1].Trim();
            return true;
        }
        return false;
    }

    // True if a line is a `key = value` assignment (not a comment). Outputs the
    // trimmed key and the raw value (untrimmed-right is fine, we trim on read).
    private static bool IsKeyValue(string line, out string key, out string value)
    {
        key = "";
        value = "";
        var t = line.TrimStart();
        if (t.Length == 0 || t[0] is ';' or '#' or '[') return false;
        int eq = line.IndexOf('=');
        if (eq <= 0) return false;
        key = line[..eq].Trim();
        if (key.Length == 0) return false;
        value = line[(eq + 1)..].Trim();
        return true;
    }

    // Find the physical line index of `key` within `section`. Returns -1 if the
    // section or key is absent. Both comparisons are case-insensitive.
    private int FindKeyLine(string section, string key)
    {
        bool inSection = false;
        for (int i = 0; i < _lines.Count; i++)
        {
            if (IsSectionHeader(_lines[i], out var name))
            {
                inSection = string.Equals(name, section, StringComparison.OrdinalIgnoreCase);
                continue;
            }
            if (!inSection) continue;
            if (IsKeyValue(_lines[i], out var k, out _) &&
                string.Equals(k, key, StringComparison.OrdinalIgnoreCase))
                return i;
        }
        return -1;
    }

    // Read the current value for section/key, or null if missing. Empty string is a
    // legitimate value (key present with nothing after '=').
    public string? GetValue(string section, string key)
    {
        int i = FindKeyLine(section, key);
        if (i < 0) return null;
        IsKeyValue(_lines[i], out _, out var v);
        return v;
    }

    // Case-insensitive scan of every key in a section. Yields (key, value) for each
    // assignment line. Used for the sTestFileN family.
    public IEnumerable<KeyValuePair<string, string>> EnumerateSection(string section)
    {
        bool inSection = false;
        foreach (var line in _lines)
        {
            if (IsSectionHeader(line, out var name))
            {
                inSection = string.Equals(name, section, StringComparison.OrdinalIgnoreCase);
                continue;
            }
            if (!inSection) continue;
            if (IsKeyValue(line, out var k, out var v))
                yield return new KeyValuePair<string, string>(k, v);
        }
    }

    // Set section/key to value. If the key exists in the section, replace it in
    // place. Else, if the section exists, append the key at the END of that section
    // (just before the next header / EOF). Else, append a new section block.
    public void SetValue(string section, string key, string value)
    {
        int keyLine = FindKeyLine(section, key);
        if (keyLine >= 0)
        {
            _lines[keyLine] = $"{key}={value}";
            return;
        }

        // Locate the section's body so we can append the key at its tail.
        int sectionHeader = -1;
        for (int i = 0; i < _lines.Count; i++)
        {
            if (IsSectionHeader(_lines[i], out var name) &&
                string.Equals(name, section, StringComparison.OrdinalIgnoreCase))
            {
                sectionHeader = i;
                break;
            }
        }

        if (sectionHeader >= 0)
        {
            // Find the last line that still belongs to this section (the line before
            // the next header, or the last line of the file).
            int insertAt = _lines.Count;
            for (int i = sectionHeader + 1; i < _lines.Count; i++)
            {
                if (IsSectionHeader(_lines[i], out _))
                {
                    insertAt = i;
                    break;
                }
            }
            // Skip back over trailing blank lines so the new key hugs the section's
            // existing content rather than landing after a gap.
            while (insertAt - 1 > sectionHeader && _lines[insertAt - 1].Trim().Length == 0)
                insertAt--;
            _lines.Insert(insertAt, $"{key}={value}");
            return;
        }

        // Section absent: append a fresh block (blank separator if file has content).
        if (_lines.Count > 0 && _lines[^1].Trim().Length != 0)
            _lines.Add("");
        _lines.Add($"[{section}]");
        _lines.Add($"{key}={value}");
    }

    // Remove section/key entirely (drops the physical line). Returns true if a line was
    // removed. Unlike SetValue(section, key, "") - which leaves `key=` behind - this deletes
    // the assignment, which matters for keys whose mere presence is significant (e.g. an
    // sTestFileN entry makes Starfield ignore Plugins.txt even when its value is empty).
    public bool RemoveKey(string section, string key)
    {
        int i = FindKeyLine(section, key);
        if (i < 0) return false;
        _lines.RemoveAt(i);
        return true;
    }

    // Serialize back to CRLF text (game-native), with a trailing newline.
    public override string ToString()
    {
        var sb = new StringBuilder();
        foreach (var line in _lines)
            sb.Append(line).Append("\r\n");
        return sb.ToString();
    }

    public void Save(string path)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(path, ToString());
    }
}
