using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Pulsar.Services;

// Local safety net for the DB-stored character (the JSON-v2 profile blob: items+mods,
// credits, O2, perks, skills, level, XP, powers, research). Two jobs:
//   1. rolling auto-backup on every profile save (keep the newest N) -> survives a DB wipe
//   2. manual export/import (.pulsarchar) -> portable between online and local characters
// It never touches Starfield .sfs saves; it only mirrors the co-op profile blob.
public static class CharacterBackup
{
    public const int KeepVersions = 10;
    public const string ExportExtension = ".pulsarchar";

    // Small, human-readable descriptor so the import dialog can show what a file holds.
    public sealed class Header
    {
        [JsonPropertyName("name")]  public string Name { get; set; } = "";
        [JsonPropertyName("level")] public int Level { get; set; }
        [JsonPropertyName("ts")]    public long Ts { get; set; }       // unix seconds
        [JsonPropertyName("mode")]  public string Mode { get; set; } = ""; // "online" | "local"
    }

    private sealed class Envelope
    {
        [JsonPropertyName("header")] public Header Header { get; set; } = new();
        [JsonPropertyName("blob")]   public string Blob { get; set; } = "";
    }

    public static string BackupRoot =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                     "PULSAR", "backups");

    // ── 1. rolling auto-backup ────────────────────────────────────────────────────
    // Writes char-<unixtime>.json under backups/<identity>/ and prunes to KeepVersions.
    // identity = STDB identity short id, or "local". Best-effort: never throws.
    public static void BackupBlob(string identity, string blob)
    {
        if (string.IsNullOrEmpty(blob)) return;
        try
        {
            var dir = Path.Combine(BackupRoot, Sanitize(identity));
            Directory.CreateDirectory(dir);

            // Unix MILLISECONDS (13 digits, fixed width for centuries): unique even for
            // back-to-back saves, and still lexicographically sortable for Prune().
            var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var path = Path.Combine(dir, $"char-{ts}.json");
            File.WriteAllText(path, blob);

            Prune(dir);
        }
        catch { /* best-effort backup */ }
    }

    private static void Prune(string dir)
    {
        try
        {
            var files = Directory.GetFiles(dir, "char-*.json")
                                 .OrderByDescending(f => f)   // char-<ts>.json sorts by time
                                 .ToList();
            foreach (var old in files.Skip(KeepVersions))
            {
                try { File.Delete(old); } catch { }
            }
        }
        catch { }
    }

    // ── 2. export / import ────────────────────────────────────────────────────────
    public static void Export(string blob, string destPath, Header header)
    {
        var env = new Envelope { Header = header, Blob = blob };
        var json = JsonSerializer.Serialize(env, new JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(destPath, json);
    }

    // Returns false (out defaults) if the file is missing/garbled/empty-blob.
    public static bool TryReadExport(string path, out Header header, out string blob)
    {
        header = new Header();
        blob = "";
        try
        {
            if (!File.Exists(path)) return false;
            var env = JsonSerializer.Deserialize<Envelope>(File.ReadAllText(path));
            if (env == null || string.IsNullOrEmpty(env.Blob)) return false;
            header = env.Header ?? new Header();
            blob = env.Blob;
            return true;
        }
        catch
        {
            header = new Header();
            blob = "";
            return false;
        }
    }

    private static string Sanitize(string s)
    {
        if (string.IsNullOrWhiteSpace(s)) return "local";
        foreach (var c in Path.GetInvalidFileNameChars())
            s = s.Replace(c, '_');
        return s;
    }
}
