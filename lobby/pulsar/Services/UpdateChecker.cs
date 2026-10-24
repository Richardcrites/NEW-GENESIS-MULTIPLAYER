using System;
using System.IO;
using System.Net.Http;
using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Pulsar.Services;

// Result of a startup update check. NewTool / NewMod is true when the corresponding
// repo's latest release is ahead of what is installed locally.
public sealed record UpdateInfo(
    bool NewTool,
    bool NewMod,
    string ToolVersion,
    string ModVersion,
    string HtmlUrl);

// Latest-release detail for ONE repo. Tool and mod ship as SEPARATE artifacts in
// SEPARATE repos, so the version is taken from the release TAG (e.g. "v0.2"); the first
// .zip asset is the downloadable package (used for the mod auto-update).
public sealed record ReleaseInfo(
    string Tag,
    string Notes,
    string HtmlUrl,
    string ZipUrl,
    Version? TagVersion);

// Phase 5: queries the public GitHub release of a repo. Tool and mod are checked
// INDEPENDENTLY (own repo each). Fully silent on offline / 404 / parse failure - every
// path is wrapped so a network hiccup never surfaces a dialog or crashes startup.
public static class UpdateChecker
{
    private static readonly HttpClient s_http = CreateClient();

    private static HttpClient CreateClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
        // GitHub's API rejects requests without a User-Agent.
        c.DefaultRequestHeaders.UserAgent.ParseAdd("Pulsar");
        c.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return c;
    }

    // Mod version (optional) read from a small local file written after a mod update.
    private static string ModVersionPath =>
        Path.Combine(AppConfig.SharedDir, "mod_version.txt");

    // Check BOTH channels independently: tool (toolRepo's latest tag vs the running
    // assembly) and mod (modRepo's latest tag vs the local mod_version.txt). Returns an
    // UpdateInfo only when something newer exists; null otherwise (incl. any failure).
    public static async Task<UpdateInfo?> CheckAsync(string toolRepo, string modRepo)
    {
        bool newTool = false, newMod = false;
        string toolV = "", modV = "", url = "";

        var tool = await FetchLatestAsync(toolRepo).ConfigureAwait(false);
        if (tool?.TagVersion != null && tool.TagVersion > LocalToolVersion())
        {
            newTool = true;
            toolV = tool.TagVersion.ToString();
            url = tool.HtmlUrl;
        }

        var mod = await FetchLatestAsync(modRepo).ConfigureAwait(false);
        var localMod = ReadLocalModVersion();
        // Only flag a mod update when we KNOW the local version (file present).
        if (mod?.TagVersion != null && localMod != null && mod.TagVersion > localMod)
        {
            newMod = true;
            modV = mod.TagVersion.ToString();
            if (url.Length == 0) url = mod.HtmlUrl;
        }

        if (!newTool && !newMod) return null;
        return new UpdateInfo(newTool, newMod, toolV, modV, url);
    }

    // Full latest-release fetch for ONE repo: notes + the .zip asset URL + the tag version
    // whenever the request succeeds (regardless of up-to-date). null on offline/404/parse.
    public static async Task<ReleaseInfo?> FetchLatestAsync(string repo)
    {
        try
        {
            repo = (repo ?? "").Trim().Trim('/');
            if (repo.Length == 0 || !repo.Contains('/')) return null;

            var url = $"https://api.github.com/repos/{repo}/releases/latest";
            using var resp = await s_http.GetAsync(url).ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode) return null;

            var body = await resp.Content.ReadAsStringAsync().ConfigureAwait(false);
            using var doc = JsonDocument.Parse(body);
            var root = doc.RootElement;

            string tag = Str(root, "tag_name");
            string notes = Str(root, "body");
            string htmlUrl = Str(root, "html_url");

            // First .zip asset = the release package (per package_release.ps1).
            string zipUrl = "";
            if (root.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
            {
                foreach (var a in assets.EnumerateArray())
                {
                    if (Str(a, "name").EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                    {
                        zipUrl = Str(a, "browser_download_url");
                        break;
                    }
                }
            }

            return new ReleaseInfo(tag, notes, htmlUrl, zipUrl, ParseVersion(tag));
        }
        catch { return null; }
    }

    // Exposed so the Updates page can show installed versions + record the new mod version
    // after an in-tool update.
    public static Version LocalToolVersion() =>
        Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 0);

    public static Version? LocalModVersion() => ReadLocalModVersion();

    public static void WriteLocalModVersion(string version)
    {
        try { File.WriteAllText(ModVersionPath, (version ?? "").Trim()); } catch { /* best-effort */ }
    }

    // Extract the first dotted numeric version from a free-form string (tag).
    // "tool-v0.2" -> 0.2 ; "v1.3.1" -> 1.3.1 ; returns null if none.
    private static Version? ParseVersion(string s)
    {
        if (string.IsNullOrWhiteSpace(s)) return null;
        var m = Regex.Match(s, @"(\d+(?:\.\d+){0,3})");
        if (!m.Success) return null;
        return Version.TryParse(Normalize(m.Groups[1].Value), out var v) ? v : null;
    }

    private static string Str(JsonElement e, string p) =>
        e.TryGetProperty(p, out var v) && v.ValueKind == JsonValueKind.String ? (v.GetString() ?? "") : "";

    private static Version? ReadLocalModVersion()
    {
        try
        {
            if (!File.Exists(ModVersionPath)) return null;
            var raw = File.ReadAllText(ModVersionPath).Trim();
            return ParseVersion(raw);
        }
        catch { return null; }
    }

    // System.Version needs at least major.minor; pad a bare "2" to "2.0".
    private static string Normalize(string v) => v.Contains('.') ? v : v + ".0";
}
