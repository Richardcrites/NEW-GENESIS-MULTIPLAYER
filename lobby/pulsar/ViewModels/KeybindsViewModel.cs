using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using Avalonia.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Pulsar.Services;

namespace Pulsar.ViewModels;

// One remappable player hotkey row (label + current key + capture state).
public partial class KeybindRowViewModel : ObservableObject
{
    public string IniKey { get; }
    public string Label { get; }
    public string Description { get; }

    [ObservableProperty] private string _keyName;

    // True while this row is waiting for the next key press.
    [ObservableProperty] private bool _isCapturing;

    public KeybindRowViewModel(string iniKey, string label, string description, string keyName)
    {
        IniKey = iniKey;
        Label = label;
        Description = description;
        _keyName = keyName;
    }

    // Label shown on the rebind button: "Rebind" normally, "Press a key… (Esc to cancel)"
    // while capturing.
    public string ButtonText => IsCapturing ? "Press a key…" : "Rebind";

    partial void OnIsCapturingChanged(bool value) => OnPropertyChanged(nameof(ButtonText));
}

// Keybinds page: remaps the game mod's player hotkeys by writing PULSAR.ini, the
// flat config the DLL reads at load. Bindings live in PULSAR.ini (not pulsar_gui.json);
// changes apply on the next game start.
public partial class KeybindsViewModel : ViewModelBase
{
    private readonly MainWindowViewModel _shell;

    public override string Title => "Keybinds";
    public override string Glyph => char.ConvertFromUtf32(0xE765); // Keyboard

    public ObservableCollection<KeybindRowViewModel> Bindings { get; } = new();

    // Developer/diagnostic hotkey gate (devHotkeys = 0/1 in the ini).
    [ObservableProperty] private bool _devHotkeys;

    // Resolved PULSAR.ini path or a warning when the Starfield path isn't set.
    [ObservableProperty] private string _statusText = "";

    // Transient note for unsupported-key presses during capture.
    [ObservableProperty] private string _captureNote = "";

    // True when Save is possible (the Starfield path resolves to a plugins dir).
    [ObservableProperty] private bool _canSave;

    // Default player hotkeys (defaults the DLL also uses when a key is absent).
    private static readonly (string Key, string Label, string Desc, string Default)[] s_defs =
    {
        ("HotkeyAutoFollow",      "Auto-follow",      "Follow your partner / co-op fast-travel co-location.", "F7"),
        ("HotkeyManualBridge",    "Manual bridge",    "Manually bridge to / join the host's location.",       "F8"),
        ("HotkeySaveCharacter",   "Save character",   "Save my character now.",                               "F10"),
        ("HotkeyRestoreCharacter","Restore character","Restore my character.",                                "F11"),
    };

    private KeybindRowViewModel? _capturing;

    // Designer ctor.
    public KeybindsViewModel() : this(null!) { }

    public KeybindsViewModel(MainWindowViewModel shell)
    {
        _shell = shell;
        Load();
    }

    private string? IniPath => PulsarIni.ResolvePath(_shell?.Config.StarfieldPath);

    // (Re)load bindings + dev gate from PULSAR.ini, defaulting per-key when absent.
    public void Load()
    {
        var path = IniPath;
        var map = PulsarIni.Read(path);

        Bindings.Clear();
        foreach (var d in s_defs)
        {
            var val = map.TryGetValue(d.Key, out var v) && !string.IsNullOrWhiteSpace(v)
                ? v.Trim()
                : d.Default;
            Bindings.Add(new KeybindRowViewModel(d.Key, d.Label, d.Desc, val));
        }

        // devHotkeys: treat "1"/"true"/"yes"/"on" as enabled.
        DevHotkeys = map.TryGetValue("devHotkeys", out var dv) && IsTruthy(dv);

        if (path == null)
        {
            CanSave = false;
            StatusText = "Set the Starfield path in Settings first.";
        }
        else
        {
            CanSave = true;
            StatusText = "PULSAR.ini: " + path;
        }
    }

    private static bool IsTruthy(string s)
    {
        s = s.Trim();
        return s == "1"
            || s.Equals("true", StringComparison.OrdinalIgnoreCase)
            || s.Equals("yes", StringComparison.OrdinalIgnoreCase)
            || s.Equals("on", StringComparison.OrdinalIgnoreCase);
    }

    // ── capture ──────────────────────────────────────────────────────────────────

    [RelayCommand]
    private void StartCapture(KeybindRowViewModel? row)
    {
        if (row == null) return;
        CaptureNote = "";
        // Only one row captures at a time.
        if (_capturing != null && _capturing != row) _capturing.IsCapturing = false;

        if (row.IsCapturing)
        {
            // Toggle off (acts as cancel).
            row.IsCapturing = false;
            _capturing = null;
            return;
        }
        row.IsCapturing = true;
        _capturing = row;
    }

    public bool IsCapturing => _capturing != null;

    // Called by the View's KeyDown handler. Returns true if the key was consumed.
    // Esc cancels; a supported key sets the binding; an unsupported key shows a note
    // and keeps capturing.
    public bool HandleKey(Key key)
    {
        if (_capturing == null) return false;

        if (key == Key.Escape)
        {
            _capturing.IsCapturing = false;
            _capturing = null;
            CaptureNote = "";
            return true;
        }

        // Ignore bare modifier presses so the user can hold/combine before the real key.
        if (key is Key.LeftShift or Key.RightShift or Key.LeftCtrl or Key.RightCtrl
                or Key.LeftAlt or Key.RightAlt or Key.LWin or Key.RWin or Key.None)
            return true;

        var name = KeyName.FromAvalonia(key);
        if (name == null)
        {
            CaptureNote = "Unsupported key - try F1-F24, A-Z, 0-9 or Numpad0-9.";
            return true; // stay in capture mode
        }

        _capturing.KeyName = name;
        _capturing.IsCapturing = false;
        _capturing = null;
        CaptureNote = "";
        Save();
        return true;
    }

    partial void OnDevHotkeysChanged(bool value)
    {
        // Avoid writing during initial Load() (CanSave reflects a valid path).
        if (CanSave) Save();
    }

    // ── persist ──────────────────────────────────────────────────────────────────

    [RelayCommand]
    public void Save()
    {
        var path = IniPath;
        if (path == null)
        {
            StatusText = "Set the Starfield path in Settings first.";
            CanSave = false;
            return;
        }

        var values = new List<KeyValuePair<string, string>>();
        foreach (var b in Bindings)
            values.Add(new KeyValuePair<string, string>(b.IniKey, b.KeyName));
        values.Add(new KeyValuePair<string, string>("devHotkeys", DevHotkeys ? "1" : "0"));

        try
        {
            PulsarIni.Merge(path, values);
            StatusText = "Saved → " + path + "  (applies on next game start)";
            CanSave = true;
            _shell?.Log("Keybinds saved to PULSAR.ini.");
        }
        catch (Exception ex)
        {
            StatusText = "Save failed: " + ex.Message;
            _shell?.Log("Keybinds save failed: " + ex.Message);
        }
    }

    // Re-read the ini + path (the Starfield path may have changed in Settings).
    public void RaiseStateChanged() => Load();
}
