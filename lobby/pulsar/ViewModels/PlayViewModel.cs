using System;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Pulsar.ViewModels;

// The Co-op hub: the single page a casual user needs. The tool connects automatically,
// so there is no "daemon" to start. From here you set your name once and either host a
// game (you get an invite code to share) or join with a friend's code. Merges the old
// Play + Host/Join pages.
public partial class PlayViewModel : ViewModelBase
{
    private readonly MainWindowViewModel _shell;

    public PlayViewModel(MainWindowViewModel shell) => _shell = shell;

    public override string Title => "Co-op";
    public override string Glyph => char.ConvertFromUtf32(0xE716); // People

    public MainWindowViewModel Shell => _shell;

    // Editable any time. Saving re-claims the name if already connected (no-op offline).
    public string PlayerName
    {
        get => _shell.Config.PlayerName;
        set
        {
            if (_shell.Config.PlayerName != value)
            {
                _shell.Config.PlayerName = value;
                _shell.SaveConfig();
                OnPropertyChanged();
                _shell.Claim();
            }
        }
    }

    public string StarfieldPath
    {
        get => _shell.Config.StarfieldPath;
        set
        {
            if (_shell.Config.StarfieldPath != value)
            {
                _shell.Config.StarfieldPath = value;
                _shell.SaveConfig();
                OnPropertyChanged();
                OnPropertyChanged(nameof(GameNotFound));
            }
        }
    }

    // Drives the "Game & launch" section: when the install path is still empty the
    // section auto-expands so a first-run user can't miss Detect/Browse; once a game is
    // found it stays collapsed and out of the way.
    public bool GameNotFound => string.IsNullOrWhiteSpace(StarfieldPath);

    public bool AutoLaunch
    {
        get => _shell.Config.AutoLaunch;
        set
        {
            if (_shell.Config.AutoLaunch != value)
            {
                _shell.Config.AutoLaunch = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }

    public bool AutoSave
    {
        get => _shell.Config.AutoSave;
        set
        {
            if (_shell.Config.AutoSave != value)
            {
                _shell.Config.AutoSave = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }

    // ── host / join ─────────────────────────────────────────────────────────────
    [ObservableProperty] private string _hostLobbyName = "My game";
    [ObservableProperty] private string _hostPassword = "";

    // ALPHA: 2 is fully supported; 3-4 is experimental (ENet allows it, but the relay
    // pairs only 1 client and the sync currently handles 1 partner). Testers report back.
    [ObservableProperty] private int _maxPlayers = 2;
    public int[] MaxPlayerOptions { get; } = { 2, 3, 4 };
    [ObservableProperty] private string _joinCode = "";
    [ObservableProperty] private string _joinCodePassword = "";

    // Public-games list selection + its password prompt.
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanJoinSelected))]
    [NotifyPropertyChangedFor(nameof(ModParityMessage))]
    [NotifyPropertyChangedFor(nameof(HasModParityMessage))]
    private LobbyRowViewModel? _selectedLobby;
    [ObservableProperty] private string _joinPassword = "";

    // Mod parity for the selected lobby: compares the host's mod list to ours. We never
    // copy mods, we just tell the joiner what to install + whether the order differs.
    public bool HasModParityMessage => SelectedLobby?.HasMods == true;
    public string ModParityMessage
    {
        get
        {
            var lob = SelectedLobby;
            if (lob == null || !lob.HasMods) return "";
            var (missing, orderDiffers) = Pulsar.Services.PluginsReader.Compare(lob.ModList);
            if (missing.Count == 0 && !orderDiffers) return "Mods match this host. Good to go.";
            var parts = new System.Collections.Generic.List<string>();
            if (missing.Count > 0) parts.Add("You are missing: " + string.Join(", ", missing));
            if (orderDiffers) parts.Add("Your load order differs from the host.");
            return string.Join("   ", parts);
        }
    }

    public string HostButtonText => _shell.IsHosting ? "Stop hosting" : "Host a game";
    public bool CanHost => _shell.IsConnected && _shell.NameClaimed;
    public bool CanJoin => _shell.IsConnected && _shell.NameClaimed && !_shell.IsHosting;
    public bool CanJoinSelected => CanJoin && SelectedLobby != null;
    public bool IsOffline => !_shell.IsConnected;

    [RelayCommand]
    private void HostToggle()
    {
        if (_shell.IsHosting) _shell.CloseLobby();
        else _shell.Host(HostLobbyName, HostPassword, MaxPlayers);
    }

    [RelayCommand]
    private void JoinWithCode() => _shell.JoinByCode(JoinCode, JoinCodePassword);

    [RelayCommand]
    private void JoinSelected() => _shell.Join(SelectedLobby, JoinPassword);

    // ── self-host (direct, no relay) ─────────────────────────────────────────────
    // Host on your own IP and share the invite; or join a friend's IP directly. The game
    // port is fixed (the DLL listens on 7777). For internet, the host port-forwards UDP.
    private const int GamePort = 7777;

    [ObservableProperty] private string _joinIp = "";          // friend's "ip:port" or pulsar:// invite
    [ObservableProperty] private bool _internetHost;           // use public IP in the invite (port-forward needed)

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasInvite))]
    private string _inviteAddress = "";                        // shown + copyable after hosting

    public bool HasInvite => !string.IsNullOrEmpty(InviteAddress);
    public bool CanDirect => _shell.IsConnected;

    partial void OnInternetHostChanged(bool value) => _ = RefreshInviteAsync();

    [RelayCommand]
    private async Task HostDirect()
    {
        _shell.HostDirect();
        await RefreshInviteAsync();
    }

    [RelayCommand]
    private void JoinByIp()
    {
        if (!Pulsar.Services.NetworkAddress.TryParseInvite(JoinIp, out var host, out var port))
        {
            _shell.Log("Enter a valid address like 1.2.3.4:7777 or a pulsar:// invite.");
            return;
        }
        _shell.JoinDirect(host, port);
    }

    // Compute the address a friend should connect to: LAN IP by default, public IP when
    // hosting over the internet. Public lookup is async; LAN is the fallback.
    private async Task RefreshInviteAsync()
    {
        string? ip = InternetHost
            ? (await Pulsar.Services.NetworkAddress.GetPublicIpAsync() ?? Pulsar.Services.NetworkAddress.GetLanIpv4())
            : Pulsar.Services.NetworkAddress.GetLanIpv4();
        InviteAddress = ip != null
            ? Pulsar.Services.NetworkAddress.FormatInvite(ip, GamePort)
            : "(no network address found)";
    }

    [RelayCommand]
    private void Reconnect()
    {
        if (!_shell.IsConnected) _shell.ToggleDaemonCommand.Execute(null);
    }

    [RelayCommand]
    private void Launch() => _shell.LaunchGameNow();

    [RelayCommand]
    private void Browse() => BrowseRequested?.Invoke();

    [RelayCommand]
    private void AutoDetect() => _shell.TryAutoDetectGame();

    public event Action? BrowseRequested;

    // Refresh the bound path after the shell auto-detected the install.
    public void RaiseGamePathChanged()
    {
        OnPropertyChanged(nameof(StarfieldPath));
        OnPropertyChanged(nameof(GameNotFound));
    }

    // Called by the shell when connection/claim/host state changes so bound props refresh.
    public void RaiseStateChanged()
    {
        OnPropertyChanged(nameof(HostButtonText));
        OnPropertyChanged(nameof(CanHost));
        OnPropertyChanged(nameof(CanJoin));
        OnPropertyChanged(nameof(CanJoinSelected));
        OnPropertyChanged(nameof(IsOffline));
        OnPropertyChanged(nameof(CanDirect));
    }
}
