using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FluentAvalonia.UI.Controls;
using Pulsar.Services;

namespace Pulsar.ViewModels;

// The shell view-model. Owns the DaemonBridge, the shared connection/session state,
// the poll timer, and the navigation. Page view-models hold a reference back here so
// all daemon interaction is centralized (mirrors the single WinForms MainForm).
public partial class MainWindowViewModel : ObservableObject
{
    private readonly AppConfig _config;
    private readonly DaemonBridge _bridge;
    private readonly DispatcherTimer _timer;
    private readonly LocalStack _localStack;
    private bool _autoClaimDone;
    private bool _startingDaemon;

    public AppConfig Config => _config;
    public DaemonBridge Bridge => _bridge;

    // ── shared session state (bound across pages) ───────────────────────────────
    [ObservableProperty] private bool _isConnected;
    [ObservableProperty] private bool _nameClaimed;
    [ObservableProperty] private bool _isHosting;
    [ObservableProperty] private string _connectionStatus = "Disconnected";
    [ObservableProperty] private string _nameStatus = "not claimed";
    [ObservableProperty] private string _lastSaved = "";
    [ObservableProperty] private string _logText = "";
    [ObservableProperty] private string _hostInviteCode = "";

    // Lobbies = raw published list; FilteredLobbies = what the browser shows (name filter).
    public ObservableCollection<LobbyRowViewModel> Lobbies { get; } = new();
    public ObservableCollection<LobbyRowViewModel> FilteredLobbies { get; } = new();
    [ObservableProperty] private bool _hasLobbies;     // any open lobby at all
    [ObservableProperty] private bool _noLobbyMatches; // lobbies exist but none match the search
    [ObservableProperty] private string _lobbySearch = "";

    partial void OnLobbySearchChanged(string value) => RebuildFilteredLobbies();

    private void RebuildFilteredLobbies()
    {
        var q = (LobbySearch ?? "").Trim();
        FilteredLobbies.Clear();
        foreach (var l in Lobbies)
            if (q.Length == 0 || l.Name.Contains(q, StringComparison.OrdinalIgnoreCase))
                FilteredLobbies.Add(l);
        HasLobbies = Lobbies.Count > 0;
        NoLobbyMatches = Lobbies.Count > 0 && FilteredLobbies.Count == 0;
    }

    // Anti-spam: the button disables for a few seconds after each manual refresh (the
    // list also auto-polls ~1s, so a manual refresh is only for immediacy).
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(RefreshLobbiesCommand))]
    private bool _canRefreshLobbies = true;

    [RelayCommand(CanExecute = nameof(CanRefreshLobbies))]
    private void RefreshLobbies()
    {
        _bridge.RefreshLobbiesNow();
        CanRefreshLobbies = false;
        var cooldown = new Avalonia.Threading.DispatcherTimer
        {
            Interval = System.TimeSpan.FromSeconds(3)
        };
        cooldown.Tick += (_, _) => { cooldown.Stop(); CanRefreshLobbies = true; };
        cooldown.Start();
    }

    // ── update banner (Phase 5) ─────────────────────────────────────────────────
    [ObservableProperty] private bool _updateAvailable;
    [ObservableProperty] private string _updateText = "";
    // True when the available update is the MOD (auto-installable in-tool). Drives the
    // banner button: "Update mod" (direct download+install) vs "Open release page" (a
    // running tool can't replace its own exe, so that one stays a manual download).
    [ObservableProperty] private bool _modUpdateAvailable;
    [ObservableProperty] private string _updateButtonText = "Open release page";
    private string _updateUrl = "";

    // ── navigation ──────────────────────────────────────────────────────────────
    public ObservableCollection<ViewModelBase> Pages { get; } = new();

    [ObservableProperty] private ViewModelBase? _selectedPage;

    public PlayViewModel PlayPage { get; }
    public SetupViewModel SetupPage { get; }
    public UpdateViewModel UpdatePage { get; }
    public HelpViewModel HelpPage { get; }
    public BugReportViewModel BugReportPage { get; }
    public ServersViewModel ServersPage { get; }
    public SettingsViewModel SettingsPage { get; }
    public VoiceChatViewModel VoiceChatPage { get; }
    public KeybindsViewModel KeybindsPage { get; }
    public AboutViewModel AboutPage { get; }

    // Parameterless ctor for the XAML designer only.
    public MainWindowViewModel() : this(new AppConfig(), new DaemonBridge(new AppConfig())) { }

    public MainWindowViewModel(AppConfig config, DaemonBridge bridge)
    {
        _config = config;
        _bridge = bridge;
        _localStack = new LocalStack(config, Log);

        PlayPage = new PlayViewModel(this);
        SetupPage = new SetupViewModel(this);
        UpdatePage = new UpdateViewModel(this);
        HelpPage = new HelpViewModel(this);
        BugReportPage = new BugReportViewModel(this);
        ServersPage = new ServersViewModel(this);
        SettingsPage = new SettingsViewModel(this);
        VoiceChatPage = new VoiceChatViewModel();
        KeybindsPage = new KeybindsViewModel(this);
        AboutPage = new AboutViewModel(this);

        RebuildNav();

        WireBridge();

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) => Poll();
    }

    public void Start()
    {
        _timer.Start();
        // Auto-find the Starfield install so the user does not have to browse for it.
        TryAutoDetectGame();
        Log("Connecting...");
        // Auto-connect on launch: the user never presses a "start daemon" button.
        _ = AutoConnectAsync();
        // Fire-and-forget update check; never blocks startup, fully silent on failure.
        _ = CheckForUpdatesAsync();
    }

    private async Task AutoConnectAsync()
    {
        try { if (!_bridge.IsConnected) await ToggleDaemon(); }
        catch { /* the status pill shows offline; the Reconnect button retries */ }
    }

    // Build the nav list. Casual users see only Co-op / Setup / Settings / About;
    // the relay browser and key remapping appear only with Advanced enabled.
    public void RebuildNav()
    {
        var keep = SelectedPage;
        Pages.Clear();
        Pages.Add(PlayPage);
        Pages.Add(SetupPage);
        Pages.Add(UpdatePage);
        if (_config.Advanced) Pages.Add(ServersPage);
        Pages.Add(SettingsPage);
        Pages.Add(VoiceChatPage);
        if (_config.Advanced) Pages.Add(KeybindsPage);
        Pages.Add(HelpPage);
        Pages.Add(BugReportPage);
        Pages.Add(AboutPage);
        SelectedPage = (keep != null && Pages.Contains(keep)) ? keep : PlayPage;
    }

    // Public launch entry point for the Co-op hub's "Launch Starfield" button.
    public void LaunchGameNow() => _ = LaunchGameAsync();

    // Auto-detect the Starfield install (Steam libraries + common paths). Only fills the
    // path when it is empty or stale, so a manual choice is never overwritten.
    public void TryAutoDetectGame()
    {
        var cur = _config.StarfieldPath?.Trim() ?? "";
        if (cur.Length > 0 && File.Exists(cur)) return;   // already valid, leave it
        var found = GameLocator.FindStarfield();
        if (found != null)
        {
            _config.StarfieldPath = found;
            _config.Save();
            Log("Found Starfield: " + found);
            PlayPage.RaiseGamePathChanged();
        }
        else
        {
            Log("Starfield not found automatically. Set the path in Co-op under Game, or run Setup.");
        }
    }

    // ── update check (Phase 5) ──────────────────────────────────────────────────
    private async Task CheckForUpdatesAsync()
    {
        try
        {
            var info = await UpdateChecker.CheckAsync(_config.ToolRepo, _config.ModRepo);
            if (info == null) return;
            Post(() =>
            {
                var parts = new List<string>();
                if (info.NewTool) parts.Add("Tool v" + info.ToolVersion);
                if (info.NewMod) parts.Add("Mod v" + info.ModVersion);
                _updateUrl = info.HtmlUrl;
                UpdateText = "Update available: " + string.Join(" / ", parts);
                ModUpdateAvailable = info.NewMod;
                // A mod update installs in-tool; a tool-only update opens the release page.
                UpdateButtonText = info.NewMod ? "Update mod" : "Open release page";
                UpdateAvailable = true;
            });
        }
        catch { /* silent */ }
    }

    [RelayCommand]
    private async Task DownloadUpdate()
    {
        // Mod update: do the real in-tool download + install (same as the Updates page),
        // not just open a browser. Jump to the Updates page so its status/progress shows.
        if (ModUpdateAvailable)
        {
            SelectedPage = UpdatePage;
            UpdateAvailable = false;   // dismiss the banner; the page now drives it
            await UpdatePage.UpdateModFromBannerAsync();
            return;
        }
        // Tool-only update: a running exe can't cleanly replace itself -> open the release.
        var url = _updateUrl;
        if (string.IsNullOrWhiteSpace(url)) return;
        try
        {
            Process.Start(new ProcessStartInfo { FileName = url, UseShellExecute = true });
        }
        catch (Exception ex) { Log("Open download page failed: " + ex.Message); }
    }

    [RelayCommand]
    private void DismissUpdate() => UpdateAvailable = false;

    public void SaveConfig() => _config.Save();

    // Called on window close: stop background loops + tear down local processes.
    public void Shutdown()
    {
        _timer.Stop();
        ServersPage.Shutdown();
        _localStack.Stop();
    }

    // ── bridge wiring (all callbacks marshalled onto the UI thread) ─────────────
    private void WireBridge()
    {
        _bridge.Log += m => Post(() => Log(m));
        _bridge.Connected += () => Post(() => { IsConnected = true; _autoClaimDone = false; UpdateState(); });
        _bridge.Disconnected += () => Post(() =>
        {
            IsConnected = false;
            NameClaimed = false;
            IsHosting = false;
            HostInviteCode = "";
            _autoClaimDone = false;
            UpdateState();
        });
        _bridge.DaemonReady += () => Post(UpdateState);
        _bridge.ProfileSaved += t => Post(() => { LastSaved = "Last saved: " + t; BackupCurrentProfile(); });
        _bridge.LobbiesUpdated += list => Post(() => ApplyLobbies(list));

        _bridge.RegisterAck += (ok, err) => Post(() =>
        {
            NameClaimed = ok;
            if (ok) Log("Name claimed.");
            else Log("Claim rejected: " + (err.Length > 0 ? err : "name taken - choose another."));
            UpdateState();
        });
        _bridge.HostAck += (ok, err, code) => Post(() =>
        {
            if (ok)
            {
                IsHosting = true;
                HostInviteCode = code;
                Log(code.Length > 0
                    ? "Lobby hosted. Invite code: " + code
                      + " - share it with your friend and wait for them to join."
                    : "Lobby hosted - waiting for a friend to join.");
            }
            else Log("host failed: " + err);
            UpdateState();
        });
        _bridge.DirectHostAck += (ok, err) => Post(() =>
        {
            Log(ok ? "Direct host ready - share your invite address with your friend."
                   : "Direct host failed: " + err);
        });
        _bridge.CloseAck += (ok, err) => Post(() =>
        {
            IsHosting = false;
            HostInviteCode = "";
            Log(ok ? "Lobby closed." : "Close failed: " + err);
            UpdateState();
        });
        _bridge.JoinAck += (ok, err, kind) => Post(() =>
        {
            if (ok)
                Log("Joined the lobby. Starfield is launching now. The co-op link "
                    + "forms IN-GAME once you BOTH load a save - look for your friend "
                    + "near you. The lobby showing 2/2 only means you reached the "
                    + "lobby, not that the game link is up yet.");
            else
                Log(kind + " could not connect: "
                    + (err.Length > 0 ? err : "wrong password, or the lobby is full / gone."));
            UpdateState();
        });
        _bridge.LaunchRequested += () => Post(() => _ = LaunchGameAsync());
    }

    private static void Post(Action a)
    {
        if (Dispatcher.UIThread.CheckAccess()) a();
        else Dispatcher.UIThread.Post(a);
    }

    // ── polling ─────────────────────────────────────────────────────────────────
    private void Poll()
    {
        IsConnected = _bridge.IsConnected;

        // Auto-claim once the daemon is ready (mirrors MainForm Poll auto-claim):
        // the name is bound to identity, so no manual claim is needed each restart.
        if (_bridge.IsDaemonReady && !NameClaimed && !_autoClaimDone && IsConnected)
        {
            var n = _config.PlayerName?.Trim() ?? "";
            if (n.Length is >= 1 and <= 32)
            {
                _autoClaimDone = true;
                _bridge.Register(n);
            }
        }

        _bridge.Poll();
        UpdateState();
    }

    private void ApplyLobbies(IReadOnlyList<LobbyInfo> list)
    {
        Lobbies.Clear();
        foreach (var l in list)
            Lobbies.Add(new LobbyRowViewModel(l));
        RebuildFilteredLobbies();
    }

    // ── commands invoked by pages ───────────────────────────────────────────────
    [RelayCommand]
    public async Task ToggleDaemon()
    {
        if (_bridge.IsConnected)
        {
            _bridge.Stop();
            _localStack.Stop();   // tear down the local stack too (no-op if Online)
            IsConnected = false;
            NameClaimed = false;
            IsHosting = false;
            HostInviteCode = "";
            UpdateState();
            return;
        }

        if (_startingDaemon) return;
        _startingDaemon = true;
        try
        {
            _config.Save();

            // One-Click Local Stack (Phase 4): when hosting on this PC / LAN, spin up
            // SpacetimeDB + the local relay before the daemon connects to them.
            if (_config.DbMode == DbMode.Local)
            {
                Log("Starting local stack (SpacetimeDB + relay) …");
                bool ok = await _localStack.StartAsync();
                if (!ok)
                {
                    Log("Local stack failed to start - daemon not launched.");
                    UpdateState();
                    return;
                }
            }

            // Load-aware auto-pick (Phase 2): in default Online mode (no manual relay
            // override) choose the least-loaded accepting relay from the master-list so
            // players spread across relays instead of piling onto one. Empty → the
            // daemon falls back to the DB host's :7777.
            bool manualRelay = _config.Advanced && (_config.RelayOverride?.Trim().Length ?? 0) > 0;
            if (_config.DbMode == DbMode.Online && !manualRelay)
            {
                var best = ServersPage.PickBestHost();
                _bridge.AutoRelayAddr = best ?? "";
                if (!string.IsNullOrEmpty(best)) Log("Auto-picked relay: " + best);
            }
            else
            {
                _bridge.AutoRelayAddr = "";
            }

            if (!_bridge.Start(out var error))
            {
                if (error.Length > 0) Log(error);
                _localStack.Stop();   // roll back the stack if the daemon can't start
            }
            UpdateState();
        }
        finally
        {
            _startingDaemon = false;
        }
    }

    public void Claim()
    {
        if (!_bridge.IsConnected) return;
        var name = _config.PlayerName?.Trim() ?? "";
        if (name.Length is < 1 or > 32)
        {
            Log("Name must be 1..32 characters.");
            return;
        }
        _bridge.Register(name);
    }

    public void Host(string lobbyName, string password, int maxPlayers = 2)
    {
        if (!_bridge.IsConnected || !NameClaimed) return;
        if (string.IsNullOrWhiteSpace(lobbyName))
        {
            Log("Enter a lobby name.");
            return;
        }
        // ALPHA: 2 is fully supported; 3-4 is experimental (see PlayViewModel).
        int max = maxPlayers < 2 ? 2 : (maxPlayers > 4 ? 4 : maxPlayers);
        // Mod parity: publish our active mod list (names only) so the browser shows a
        // "Mods N" badge and joiners can run the parity check. We never share mod files.
        var (modCount, modList) = Pulsar.Services.PluginsReader.Snapshot();
        if (modCount > 0) Log($"Hosting with {modCount} mod(s) - joiners will be told to match them.");
        Log("Creating your lobby...");
        _bridge.Host(lobbyName.Trim(), password ?? "", max, _config.AutoLaunch, modCount, modList);
    }

    public void CloseLobby() => _bridge.Close();

    public void Join(LobbyRowViewModel? row, string password)
    {
        if (row == null || !_bridge.IsConnected || !NameClaimed) return;
        Log($"Joining '{row.Name}' - connecting...");
        _bridge.Join(row.LobbyId, password ?? "", _config.AutoLaunch);
    }

    public void JoinByCode(string code, string password)
    {
        if (!_bridge.IsConnected || !NameClaimed) return;
        if (string.IsNullOrWhiteSpace(code) || code.Trim().Length < 4)
        {
            Log("Enter a lobby code.");
            return;
        }
        Log("Joining with that code - connecting...");
        _bridge.JoinByCode(code.Trim(), password ?? "", _config.AutoLaunch);
    }

    // ── self-host (pure direct, no relay/DB lobby) ───────────────────────────────
    // The host plays on their own IP; the friend connects with a shared IP invite.
    // Works in either DB mode (the character still comes from the connected DB).
    public void HostDirect()
    {
        if (!_bridge.IsConnected) { Log("Not connected yet."); return; }
        // Direct host has no DB lobby, so it must NOT set the cloud IsHosting state (that
        // drives the cloud invite-code card + the Host button toggle). The direct card
        // shows its own IP invite instead.
        _bridge.HostDirect(_config.AutoLaunch);
    }

    public void JoinDirect(string ip, int port)
    {
        if (!_bridge.IsConnected) { Log("Not connected yet."); return; }
        Log($"Connecting directly to {ip}:{port}...");
        _bridge.JoinDirect(ip, port, _config.AutoLaunch);
    }

    // ── local character backup (rolling) + export / import ───────────────────────
    private static string ProfileOutPath => Path.Combine(AppConfig.SharedDir, "pulsar_profile_out.json");

    // Mirror the last-saved profile blob into a rolling local backup (keeps the newest
    // N) so a DB wipe never loses the character. Called on each daemon auto-save report.
    private void BackupCurrentProfile()
    {
        try
        {
            if (!File.Exists(ProfileOutPath)) return;
            var blob = File.ReadAllText(ProfileOutPath);
            if (string.IsNullOrEmpty(blob)) return;
            var id = string.IsNullOrWhiteSpace(_config.PlayerName) ? "character" : _config.PlayerName.Trim();
            CharacterBackup.BackupBlob(id, blob);
        }
        catch { /* best-effort */ }
    }

    // Export the current character (last-saved profile blob) to a .pulsarchar file.
    public bool ExportCharacter(string destPath, out string error)
    {
        error = "";
        try
        {
            if (!File.Exists(ProfileOutPath))
            {
                error = "No saved character yet. Play once so it auto-saves, then export.";
                return false;
            }
            var blob = File.ReadAllText(ProfileOutPath);
            if (string.IsNullOrEmpty(blob)) { error = "The saved character is empty."; return false; }
            var header = new CharacterBackup.Header
            {
                Name = _config.PlayerName?.Trim() ?? "",
                Level = 0,
                Ts = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                Mode = _config.DbMode == DbMode.Local ? "local" : "online",
            };
            CharacterBackup.Export(blob, destPath, header);
            Log("Character exported to " + destPath);
            return true;
        }
        catch (Exception ex) { error = ex.Message; return false; }
    }

    // Import a .pulsarchar: stage it as profile-out so the daemon uploads it to the active
    // DB (online or local) on its next sync. Reuses the existing auto-upload path, so it
    // needs the daemon connected with auto-save on (both default while playing).
    public bool ImportCharacter(string srcPath, out string error)
    {
        error = "";
        try
        {
            if (!CharacterBackup.TryReadExport(srcPath, out var header, out var blob))
            {
                error = "That is not a valid character file.";
                return false;
            }
            File.WriteAllText(ProfileOutPath, blob);
            var who = header.Name.Length > 0 ? header.Name : "character";
            Log($"Imported {who} - it will upload to your DB on the next sync.");
            return true;
        }
        catch (Exception ex) { error = ex.Message; return false; }
    }

    private async Task LaunchGameAsync()
    {
        var exe = _config.StarfieldPath?.Trim() ?? "";
        if (!File.Exists(exe))
        {
            Log("Starfield not found: " + exe);
            return;
        }
        // issue #1 root-cause guard: a leftover Starfield.exe (common after killing
        // the game on the loading screen) would make THIS launch a SECOND game
        // instance. That second instance hits the host's port-in-use fallback,
        // self-connects over LAN as a DIRECT peer, and trips the host's
        // "DIRECT peer -> stop relay keepalive" path, which kills the real remote
        // friend's relay session (endless reason-0 reconnect loop). Ask, then close.
        if (!await EnsureNoGhostStarfieldAsync()) return;
        // Co-op REQUIRES SFSE. Launching Starfield.exe directly bypasses SFSE, so the
        // PULSAR mod never injects and co-op silently does nothing. Whatever the path
        // points at (stale cache, manual pick, or locator fallback), always launch
        // through sfse_loader.exe when it is present next to the game.
        var launchExe = ResolveSfseLaunch(exe);
        if (!string.Equals(launchExe, exe, StringComparison.OrdinalIgnoreCase))
            Log("Launching through sfse_loader.exe so the co-op mod loads.");
        exe = launchExe;
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = exe,
                UseShellExecute = true,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? ""
            });
            Log("Starfield launched.");
            // When the game later exits, auto-close our lobby so it does not linger open.
            _ = WatchGameExitAsync();
        }
        catch (Exception ex) { Log("Launch failed: " + ex.Message); }
    }

    // Returns true if it is safe to launch (no Starfield running, or the user agreed
    // to close the leftover one). Returns false only if the user cancels.
    private async Task<bool> EnsureNoGhostStarfieldAsync()
    {
        Process[] procs;
        try { procs = Process.GetProcessesByName("Starfield"); }
        catch { return true; }   // can't enumerate -> don't block the launch
        if (procs.Length == 0) return true;

        var dialog = new ContentDialog
        {
            Title = "Starfield is already running",
            Content = "A Starfield process is still running. This usually means a "
                    + "leftover instance, for example after closing the game on the "
                    + "loading screen. Co-op has to launch the game itself, so this "
                    + "one must be closed first. Otherwise a second game instance "
                    + "starts and the co-op link never stays up.\n\nClose it and continue?",
            PrimaryButtonText = "Close it and continue",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary
        };
        var result = await dialog.ShowAsync();
        if (result != ContentDialogResult.Primary)
        {
            Log("Launch cancelled - close the running Starfield first, then try again.");
            return false;
        }

        int killed = 0;
        foreach (var p in procs)
        {
            try { p.Kill(); killed++; } catch { /* already gone / access denied */ }
        }
        // Wait up to ~5s for the process(es) to actually disappear before launching.
        for (int i = 0; i < 25 && GameRunning(); i++) await Task.Delay(200);
        Log($"Closed {killed} leftover Starfield process(es). Launching cleanly.");
        return true;
    }

    // Co-op needs SFSE. If the configured exe is Starfield.exe (or anything) but
    // sfse_loader.exe sits in the same folder, return the loader so the mod loads.
    // Never launch Starfield.exe directly when SFSE is available next to it.
    private static string ResolveSfseLaunch(string exe)
    {
        try
        {
            if (string.Equals(Path.GetFileName(exe), "sfse_loader.exe", StringComparison.OrdinalIgnoreCase))
                return exe;
            var dir = Path.GetDirectoryName(exe);
            if (string.IsNullOrEmpty(dir)) return exe;
            var sfse = Path.Combine(dir, "sfse_loader.exe");
            if (File.Exists(sfse)) return sfse;
        }
        catch { /* fall through to the original path */ }
        return exe;
    }

    private bool _gameWatchActive;

    // Watch the Starfield process; once it has started and then exits, close our lobby
    // (the host's play session is over). Launched via sfse_loader the loader exits right
    // away, so we track Starfield.exe by name, not the launched process handle.
    private async Task WatchGameExitAsync()
    {
        if (_gameWatchActive) return;
        _gameWatchActive = true;
        try
        {
            // Wait for the game to actually appear (cold start can take a while).
            bool appeared = false;
            for (int i = 0; i < 90; i++)
            {
                if (GameRunning()) { appeared = true; break; }
                await Task.Delay(1000);
            }
            if (!appeared) return;   // never saw it (odd setup / failed launch)

            while (GameRunning())
                await Task.Delay(2000);

            Post(() =>
            {
                if (IsHosting)
                {
                    Log("Game closed - closing the lobby.");
                    CloseLobby();
                }
            });
        }
        catch { /* best-effort */ }
        finally { _gameWatchActive = false; }
    }

    private static bool GameRunning()
    {
        try { return Process.GetProcessesByName("Starfield").Length > 0; }
        catch { return false; }
    }

    // ── state propagation to pages ──────────────────────────────────────────────
    private void UpdateState()
    {
        bool c = _bridge.IsConnected;
        IsConnected = c;
        ConnectionStatus = c ? "Connected" : "Disconnected";
        if (NameClaimed)
            NameStatus = $"You are '{_config.PlayerName?.Trim()}'";
        else
            NameStatus = c ? "claiming your name…" : "not claimed";

        PlayPage.RaiseStateChanged();
    }

    public void Log(string m)
    {
        var line = $"[{DateTime.Now:HH:mm:ss}] {m}{Environment.NewLine}";
        LogText += line;
        // Trim to keep the box light.
        if (LogText.Length > 8000)
            LogText = LogText[^6000..];
    }
}
