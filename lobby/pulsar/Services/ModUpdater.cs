using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Threading.Tasks;

namespace Pulsar.Services;

// Downloads the latest release ZIP from GitHub and applies the MOD half (mod/Data) into the
// Starfield install, reusing ModInstaller for the copy. The tool itself is not self-updated
// (a running exe cannot cleanly replace itself) - for that we point the user at the release
// page. Never throws; returns a (ok, message) result.
public static class ModUpdater
{
    public sealed record Result(bool Ok, string Message);

    public static async Task<Result> DownloadAndApplyAsync(string zipUrl, string gameExePath, string? modVersion)
    {
        if (string.IsNullOrWhiteSpace(zipUrl))
            return new Result(false, "No downloadable package on the latest release.");
        if (string.IsNullOrWhiteSpace(gameExePath) || !File.Exists(gameExePath))
            return new Result(false, "Set your Starfield path first (on the Co-op page or in Setup).");
        if (IsGameRunning())
            return new Result(false, "Close Starfield first, then update.");

        var gameRoot = Path.GetDirectoryName(gameExePath);
        if (string.IsNullOrEmpty(gameRoot))
            return new Result(false, "Could not resolve the game folder.");
        var destData = Path.Combine(gameRoot, "Data");

        var tmpRoot = Path.Combine(Path.GetTempPath(), "pulsar_update_" + Guid.NewGuid().ToString("N"));
        var tmpZip = tmpRoot + ".zip";
        try
        {
            Directory.CreateDirectory(tmpRoot);

            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) })
            {
                http.DefaultRequestHeaders.UserAgent.ParseAdd("Pulsar");
                var bytes = await http.GetByteArrayAsync(zipUrl).ConfigureAwait(false);
                await File.WriteAllBytesAsync(tmpZip, bytes).ConfigureAwait(false);
            }

            ZipFile.ExtractToDirectory(tmpZip, tmpRoot, overwriteFiles: true);

            var modData = FindModData(tmpRoot);
            if (modData == null)
                return new Result(false, "The downloaded package does not contain the mod files.");

            // Re-check right before touching Data: the download can take minutes, and the
            // user may have launched Starfield in the meantime. Writing into a loaded Data
            // folder can corrupt the install, so refuse if the game came up mid-download.
            if (IsGameRunning())
                return new Result(false, "Starfield was started during the download. Close it and update again.");

            var res = ModInstaller.Install(modData, destData);
            if (!res.Ok) return new Result(false, res.Message);

            if (!string.IsNullOrWhiteSpace(modVersion))
                UpdateChecker.WriteLocalModVersion(modVersion!);

            // Keep the load mechanism correct: PULSAR must load via Plugins.txt, not sTestFile
            // (sTestFile makes Starfield ignore Plugins.txt, so the player's other mods stop
            // loading). Enable *PULSAR.esp and strip any PULSAR sTestFile entries, so a mod-only
            // update is never left on the old sTestFile path.
            PluginRegistration.EnsurePluginsTxtLoad();

            return new Result(true, res.Message + " Load order updated (Plugins.txt).");
        }
        catch (HttpRequestException ex)
        {
            return new Result(false, "Download failed: " + ex.Message);
        }
        catch (Exception ex)
        {
            return new Result(false, "Update failed: " + ex.Message);
        }
        finally
        {
            try { if (File.Exists(tmpZip)) File.Delete(tmpZip); } catch { }
            try { if (Directory.Exists(tmpRoot)) Directory.Delete(tmpRoot, true); } catch { }
        }
    }

    // The release ZIP holds mod/Data/... at its root (per package_release.ps1); some zips
    // wrap everything in a single top-level folder, so check one level down too.
    private static string? FindModData(string root)
    {
        var direct = Path.Combine(root, "mod", "Data");
        if (Directory.Exists(direct)) return direct;
        try
        {
            foreach (var sub in Directory.EnumerateDirectories(root))
            {
                var c = Path.Combine(sub, "mod", "Data");
                if (Directory.Exists(c)) return c;
            }
        }
        catch { }
        return null;
    }

    private static bool IsGameRunning()
    {
        try { return Process.GetProcessesByName("Starfield").Length > 0; }
        catch { return false; }
    }
}
