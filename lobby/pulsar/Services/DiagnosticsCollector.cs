using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace Pulsar.Services;

// Result of a diagnostics collection: where the bundle landed and a short human summary.
public sealed record DiagResult(bool Ok, string ZipPath, string Summary);

// Gathers the small, shareable artifacts a tester needs to file a useful bug report and
// zips them onto the Desktop. By design this bundles only TEXT-sized files:
// PULSAR.log + the SFSE log(s)            (the sanitized PLOG_DIAG channel)
// Bethesda ".crash" metadata             (cell / error message / loaded modules)
// an info.txt                            (tool + OS + game path + what was skipped)
// The multi-GB full memory dumps are deliberately NOT bundled (they cannot be attached to
// a GitHub issue / Discord); info.txt records where they are so a dev can request one.
public static class DiagnosticsCollector
{
    // Single source of truth for the bug-tracker URL (used by the ZIP's info.txt AND the
    // "Open issue tracker" button) so the two can never point at different repos. Derived
    // from the configured repo; falls back to the GitHub home if it is unset/malformed.
    public static string IssueTrackerUrl(string? repo)
    {
        var r = (repo ?? "").Trim().Trim('/');
        return r.Contains('/') ? $"https://github.com/{r}/issues" : "https://github.com";
    }

    public static DiagResult Collect(string? gamePath, string toolVersion, string? reportRepo)
    {
        try
        {
            var docs   = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            var sfse   = Path.Combine(docs, "My Games", "Starfield", "SFSE");
            var crashD = Path.Combine(Path.GetTempPath(), "Dumps");                 // Bethesda handler
            var werD   = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CrashDumps");

            // Collect text logs (SFSE folder, recursive; PULSAR.log lives here or under Logs\).
            var logs = SafeEnumerate(sfse, "*.log").ToList();

            // Collect Bethesda .crash text metadata from the last 14 days (newest first, cap 5).
            var crashes = SafeEnumerate(crashD, "*.crash")
                .Select(p => new FileInfo(p))
                .Where(SafeRecent)
                .OrderByDescending(SafeWriteTime)
                .Take(5)
                .Select(fi => fi.FullName)
                .ToList();

            if (logs.Count == 0 && crashes.Count == 0)
            {
                return new DiagResult(false, "",
                    "No log or crash files found yet. Start the game once (so PULSAR writes a log) and try again.");
            }

            var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            if (string.IsNullOrEmpty(desktop) || !Directory.Exists(desktop)) desktop = docs;
            var zipPath = Path.Combine(desktop, $"PULSAR-diagnostics-{stamp}.zip");
            if (File.Exists(zipPath)) File.Delete(zipPath);

            int added = 0;
            using (var fs = new FileStream(zipPath, FileMode.CreateNew))
            using (var zip = new ZipArchive(fs, ZipArchiveMode.Create))
            {
                foreach (var f in logs)    if (AddFile(zip, f, "logs"))    added++;
                foreach (var f in crashes) if (AddFile(zip, f, "crashes")) added++;

                var info = BuildInfo(toolVersion, gamePath, sfse, logs, crashes, werD, crashD, reportRepo);
                var entry = zip.CreateEntry("info.txt", CompressionLevel.Optimal);
                using var w = new StreamWriter(entry.Open(), new UTF8Encoding(false));
                w.Write(info);
            }

            return new DiagResult(true, zipPath,
                $"Saved {added} file(s) to your Desktop: {Path.GetFileName(zipPath)}. " +
                "Attach this ZIP to your bug report.");
        }
        catch (Exception ex)
        {
            return new DiagResult(false, "", "Could not collect diagnostics: " + ex.Message);
        }
    }

    private static string BuildInfo(string toolVersion, string? gamePath, string sfse,
        List<string> logs, List<string> crashes, string werDir, string crashDir, string? reportRepo)
    {
        var sb = new StringBuilder();
        sb.AppendLine("PULSAR diagnostics bundle");
        sb.AppendLine("Created: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
        sb.AppendLine("Tool version: " + toolVersion);
        sb.AppendLine("OS: " + Environment.OSVersion.VersionString + " (64-bit: " + Environment.Is64BitOperatingSystem + ")");
        sb.AppendLine("Game path: " + (string.IsNullOrEmpty(gamePath) ? "(not set)" : gamePath));
        sb.AppendLine("SFSE folder: " + sfse);
        sb.AppendLine();
        sb.AppendLine("Included logs (" + logs.Count + "):");
        foreach (var f in logs) sb.AppendLine("  " + f);
        sb.AppendLine("Included crash metadata (" + crashes.Count + "):");
        foreach (var f in crashes) sb.AppendLine("  " + f);
        sb.AppendLine();
        sb.AppendLine("NOT included (too large to attach - a developer may request one):");
        sb.AppendLine("  Full memory dumps: " + werDir + "\\Starfield.exe.*.dmp");
        sb.AppendLine("                     " + crashDir + "\\*.dmp");
        sb.AppendLine();
        sb.AppendLine("Report at: " + IssueTrackerUrl(reportRepo));
        return sb.ToString();
    }

    private static bool AddFile(ZipArchive zip, string path, string folder)
    {
        try
        {
            var name = folder + "/" + Path.GetFileName(path);
            var entry = zip.CreateEntry(name, CompressionLevel.Optimal);
            // Copy with FileShare.ReadWrite: the game may still hold the log open.
            using var src = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var dst = entry.Open();
            src.CopyTo(dst);
            return true;
        }
        catch { return false; }
    }

    private static IEnumerable<string> SafeEnumerate(string dir, string pattern)
    {
        try
        {
            if (!Directory.Exists(dir)) return Array.Empty<string>();
            return Directory.EnumerateFiles(dir, pattern, SearchOption.AllDirectories);
        }
        catch { return Array.Empty<string>(); }
    }

    private static bool SafeRecent(FileInfo fi)
    {
        try { return fi.LastWriteTime > DateTime.Now.AddDays(-14); } catch { return false; }
    }

    private static DateTime SafeWriteTime(FileInfo fi)
    {
        try { return fi.LastWriteTime; } catch { return DateTime.MinValue; }
    }
}
