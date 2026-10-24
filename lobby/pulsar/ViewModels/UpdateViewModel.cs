using System.Threading.Tasks;
using Avalonia.Controls;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Pulsar.Services;

namespace Pulsar.ViewModels;

// The Updates page: shows installed vs latest version, the patch notes pulled straight from
// the GitHub release, and a one-click "Update mod" that downloads + applies the release.
public partial class UpdateViewModel : ViewModelBase
{
    private readonly MainWindowViewModel _shell;

    public UpdateViewModel(MainWindowViewModel shell)
    {
        _shell = shell;
        RefreshInstalled();
        // Auto-populate the page on open. Skip in the XAML designer (no network there).
        if (!Design.IsDesignMode)
            _ = CheckNow();   // fire-and-forget; fully silent on failure
    }

    public override string Title => "Updates";
    public override string Glyph => char.ConvertFromUtf32(0xE896); // Download

    [ObservableProperty] private string _installedVersions = "";
    [ObservableProperty] private string _latestVersion = "Not checked yet.";
    [ObservableProperty] private string _patchNotes = "";
    [ObservableProperty] private string _status = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanUpdateMod))]
    private bool _isBusy;

    private string _zipUrl = "";
    private string _htmlUrl = "";
    private string _latestModVersion = "";

    // The update/reinstall button is usable whenever a downloadable package exists and we
    // are idle. We deliberately do NOT require knowing the local version (a fresh Setup
    // install does not record one yet), so capable users can always (re)install the latest.
    public bool CanUpdateMod => !IsBusy && _zipUrl.Length > 0;

    public void RefreshInstalled()
    {
        var tool = UpdateChecker.LocalToolVersion().ToString(3);
        var mod = UpdateChecker.LocalModVersion();
        InstalledVersions = $"Installed: tool {tool}, mod {(mod?.ToString() ?? "unknown")}";
    }

    [RelayCommand]
    private async Task CheckNow()
    {
        RefreshInstalled();
        IsBusy = true;
        Status = "Checking GitHub for the latest tool + mod releases...";

        // Tool and mod are SEPARATE repos / channels, checked independently.
        var tool = await UpdateChecker.FetchLatestAsync(_shell.Config.ToolRepo);
        var mod  = await UpdateChecker.FetchLatestAsync(_shell.Config.ModRepo);

        var toolTag = tool != null && tool.Tag.Length > 0 ? tool.Tag : "unknown";
        var modTag  = mod  != null && mod.Tag.Length  > 0 ? mod.Tag  : "unknown";
        LatestVersion = $"Latest: tool {toolTag}, mod {modTag}";

        // Mod = the auto-updatable one (zip asset). Tool = notify + release-page link only.
        _zipUrl = mod?.ZipUrl ?? "";
        _latestModVersion = mod?.TagVersion?.ToString() ?? "";
        _htmlUrl = !string.IsNullOrWhiteSpace(tool?.HtmlUrl) ? tool!.HtmlUrl : (mod?.HtmlUrl ?? "");

        // Patch notes: the mod's (that's what you can update here); fall back to the tool's.
        PatchNotes = !string.IsNullOrWhiteSpace(mod?.Notes) ? mod!.Notes
                   : (!string.IsNullOrWhiteSpace(tool?.Notes) ? tool!.Notes
                   : "(No patch notes in the latest releases.)");

        var localTool = UpdateChecker.LocalToolVersion();
        var localMod  = UpdateChecker.LocalModVersion();
        var msgs = new System.Collections.Generic.List<string>();

        if (tool == null) msgs.Add("Tool: no release found (offline or repo empty in Settings).");
        else if (tool.TagVersion != null && tool.TagVersion > localTool)
            msgs.Add($"Tool update {tool.TagVersion} available - use Open release page (the tool can't self-update).");

        if (mod == null) msgs.Add("Mod: no release found (offline or repo empty in Settings).");
        else if (mod.TagVersion == null) msgs.Add("Mod: latest release has no version tag, can't compare.");
        else if (localMod == null) msgs.Add($"Mod {mod.TagVersion} available - your installed version is unknown, you can update to be sure.");
        else if (mod.TagVersion > localMod) msgs.Add("A mod update is available.");

        Status = msgs.Count > 0 ? string.Join("  •  ", msgs) : "You are up to date.";
        if (mod != null && _zipUrl.Length == 0)
            Status += " (Mod release has no downloadable .zip - use Open release page.)";

        IsBusy = false;
    }

    // Entry point for the main-window update banner's button: refresh the release info,
    // then install the mod in one go, with this page's normal status display. Lets the
    // banner do a real "Update mod" (download + install) instead of just opening a browser.
    public async Task UpdateModFromBannerAsync()
    {
        await CheckNow();
        if (CanUpdateMod) await UpdateMod();
    }

    [RelayCommand]
    private async Task UpdateMod()
    {
        IsBusy = true;
        Status = "Downloading and installing the mod update...";
        var res = await ModUpdater.DownloadAndApplyAsync(_zipUrl, _shell.Config.StarfieldPath, _latestModVersion);
        Status = res.Message;
        if (res.Ok) RefreshInstalled();
        IsBusy = false;
    }

    [RelayCommand]
    private void OpenReleasePage()
    {
        if (string.IsNullOrWhiteSpace(_htmlUrl)) return;
        OpenUrl(_htmlUrl);
    }

    // Source repos where releases are published. Shown on the Updates page so users
    // know where the tool + mod come from. Derived from the configured repos, so
    // they stay in sync with the update check. The repos are still private for now,
    // so the links will 404 in a browser until they are flipped public.
    public string ToolRepoLabel => (_shell.Config.ToolRepo ?? "").Trim().Trim('/');
    public string ModRepoLabel  => (_shell.Config.ModRepo  ?? "").Trim().Trim('/');
    public string ToolRepoUrl => RepoUrl(_shell.Config.ToolRepo);
    public string ModRepoUrl  => RepoUrl(_shell.Config.ModRepo);

    private static string RepoUrl(string? repo)
    {
        repo = (repo ?? "").Trim().Trim('/');
        return (repo.Length == 0 || !repo.Contains('/')) ? "" : $"https://github.com/{repo}";
    }

    [RelayCommand]
    private void OpenUrl(string? url)
    {
        if (string.IsNullOrWhiteSpace(url)) return;
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = url,
                UseShellExecute = true
            });
        }
        catch { /* best-effort */ }
    }
}
