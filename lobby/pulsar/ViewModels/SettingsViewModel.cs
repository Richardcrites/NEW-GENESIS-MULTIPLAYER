using System;
using Pulsar.Services;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Pulsar.ViewModels;

public partial class SettingsViewModel : ViewModelBase
{
    private readonly MainWindowViewModel _shell;

    public SettingsViewModel(MainWindowViewModel shell)
    {
        _shell = shell;
        _isOnline = shell.Config.DbMode == DbMode.Online;
        // Result of a character wipe (fires on the UI thread via the daemon poll).
        _shell.Bridge.ResetAck += (ok, err) =>
            WipeStatus = ok
                ? "Character wiped. Load a solo save once to rebuild it from that save."
                : ("Wipe failed: " + (string.IsNullOrWhiteSpace(err) ? "is the tool connected?" : err));
    }

    public override string Title => "Settings";
    public override string Glyph => char.ConvertFromUtf32(0xE713); // Settings

    public MainWindowViewModel Shell => _shell;

    // ── character backup (export/import). The file dialogs live in the code-behind,
    // which raises these events; the actual blob work is in the shell. ──────────────
    [ObservableProperty] private string _backupStatus = "";

    public event Action? ExportRequested;
    public event Action? ImportRequested;

    [RelayCommand] private void ExportCharacter() => ExportRequested?.Invoke();
    [RelayCommand] private void ImportCharacter() => ImportRequested?.Invoke();

    // ── character wipe (danger zone) ────────────────────────────────────────────────
    // The checkbox arms the button. Wiping deletes ONLY your own co-op character: the
    // DB profile + the local backup mirror. Next solo load rebuilds it from that save.
    [ObservableProperty] private bool _wipeConfirm;
    [ObservableProperty] private string _wipeStatus = "";

    [RelayCommand]
    private void WipeCharacter()
    {
        if (!WipeConfirm) return;
        WipeStatus = "Wiping character ...";
        WipeConfirm = false;
        _shell.Bridge.ResetProfile();
    }

    // DB selector: Online (official) vs Local. Bound to two radio buttons.
    [ObservableProperty] private bool _isOnline;

    partial void OnIsOnlineChanged(bool value)
    {
        _shell.Config.DbMode = value ? DbMode.Online : DbMode.Local;
        _shell.SaveConfig();
        OnPropertyChanged(nameof(IsLocal));
    }

    public bool IsLocal
    {
        get => !IsOnline;
        set => IsOnline = !value;
    }

    public string OfficialStdbUri
    {
        get => _shell.Config.OfficialStdbUri;
        set
        {
            if (_shell.Config.OfficialStdbUri != value)
            {
                _shell.Config.OfficialStdbUri = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }

    public string LocalStdbUri
    {
        get => _shell.Config.LocalStdbUri;
        set
        {
            if (_shell.Config.LocalStdbUri != value)
            {
                _shell.Config.LocalStdbUri = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }

    public bool Advanced
    {
        get => _shell.Config.Advanced;
        set
        {
            if (_shell.Config.Advanced != value)
            {
                _shell.Config.Advanced = value;
                _shell.SaveConfig();
                OnPropertyChanged();
                _shell.RebuildNav();   // show/hide Servers + Keybinds in the nav
            }
        }
    }

    public string RelayOverride
    {
        get => _shell.Config.RelayOverride;
        set
        {
            if (_shell.Config.RelayOverride != value)
            {
                _shell.Config.RelayOverride = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }

    // Update-repos (Phase 5) - tool and mod are separate repos / release channels.
    public string ToolRepo
    {
        get => _shell.Config.ToolRepo;
        set
        {
            if (_shell.Config.ToolRepo != value)
            {
                _shell.Config.ToolRepo = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }

    public string ModRepo
    {
        get => _shell.Config.ModRepo;
        set
        {
            if (_shell.Config.ModRepo != value)
            {
                _shell.Config.ModRepo = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }

    // spacetime CLI path for the one-click local stack (Phase 4).
    public string SpacetimeExePath
    {
        get => _shell.Config.SpacetimeExePath;
        set
        {
            if (_shell.Config.SpacetimeExePath != value)
            {
                _shell.Config.SpacetimeExePath = value;
                _shell.SaveConfig();
                OnPropertyChanged();
            }
        }
    }
}
