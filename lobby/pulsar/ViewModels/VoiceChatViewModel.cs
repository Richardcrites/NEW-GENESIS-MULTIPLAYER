using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json;
using Avalonia.Input;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using NAudio.Wave;

namespace Pulsar.ViewModels;

// Proximity voice settings page. Writes pulsar_voice.json into
// %LOCALAPPDATA%\PULSAR; the Companion is the single reader and applies changes
// live (it polls the file ~1s). This VM has no dependency on the shell.
public partial class VoiceChatViewModel : ViewModelBase
{
    public override string Title => "Voice Chat";
    public override string Glyph => char.ConvertFromUtf32(0xE720); // Microphone

    public ObservableCollection<string> InputDevices { get; } = new();

    [ObservableProperty] private bool _enabled;
    [ObservableProperty] private bool _pushToTalk = true;   // true = PTT, false = open mic
    [ObservableProperty] private int _pttKey;
    [ObservableProperty] private bool _alwaysAudible;
    [ObservableProperty] private string _inputDevice = "";
    [ObservableProperty] private double _inputVolume = 1.0;
    [ObservableProperty] private double _outputVolume = 1.0;
    [ObservableProperty] private double _dNear = 600;
    [ObservableProperty] private double _dFar = 4000;
    [ObservableProperty] private bool _isCapturing;
    [ObservableProperty] private double _micLevel;   // live input meter 0..100

    public string PttKeyLabel => IsCapturing ? "Press key or mouse button..." : KeyName(PttKey);

    private bool _loading;
    private WaveInEvent? _meter;

    public VoiceChatViewModel()
    {
        try
        {
            for (int i = 0; i < WaveInEvent.DeviceCount; i++)
                InputDevices.Add(WaveInEvent.GetCapabilities(i).ProductName);
        }
        catch { /* design-time / no audio device present */ }
        Load();
    }

    // The bind button arms capture; the view routes the next key here.
    [RelayCommand] private void BindPtt() => IsCapturing = true;

    public bool HandleKey(Key key)
    {
        if (!IsCapturing) return false;
        if (key == Key.Escape) { IsCapturing = false; return true; }   // cancel
        int vk = KeyToVk(key);
        if (vk == 0) return false;                                     // unmapped key, keep capturing
        PttKey = vk;
        IsCapturing = false;
        return true;
    }

    // Bind a mouse button (side buttons 4/5 or middle). vk is a Win32 mouse VK
    // (0x04 middle, 0x05 XBUTTON1, 0x06 XBUTTON2) matching the Companion hook.
    public bool HandleMouse(int vk)
    {
        if (!IsCapturing || vk == 0) return false;
        PttKey = vk;
        IsCapturing = false;
        return true;
    }

    partial void OnIsCapturingChanged(bool value) => OnPropertyChanged(nameof(PttKeyLabel));
    partial void OnPttKeyChanged(int value) { OnPropertyChanged(nameof(PttKeyLabel)); Save(); }
    partial void OnEnabledChanged(bool value) => Save();
    partial void OnPushToTalkChanged(bool value) => Save();
    partial void OnAlwaysAudibleChanged(bool value) => Save();
    partial void OnInputDeviceChanged(string value) { Save(); if (_meter != null) StartMeter(); }
    partial void OnInputVolumeChanged(double value) => Save();
    partial void OnOutputVolumeChanged(double value) => Save();
    partial void OnDNearChanged(double value) => Save();
    partial void OnDFarChanged(double value) => Save();

    private static string ConfigPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "PULSAR", "pulsar_voice.json");

    private void Load()
    {
        _loading = true;
        try
        {
            if (File.Exists(ConfigPath))
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(ConfigPath));
                var r = doc.RootElement;
                Enabled = GetB(r, "enabled", false);
                PushToTalk = GetS(r, "mode", "ptt") == "ptt";
                PttKey = GetI(r, "pttKey", 0);
                AlwaysAudible = GetB(r, "alwaysAudible", false);
                InputDevice = GetS(r, "inputDevice", "");
                InputVolume = GetD(r, "inputVolume", 1.0);
                OutputVolume = GetD(r, "outputVolume", 1.0);
                DNear = GetD(r, "dNear", 600);
                DFar = GetD(r, "dFar", 4000);
            }
        }
        catch { }
        finally { _loading = false; }
    }

    public void Save()
    {
        if (_loading) return;   // don't rewrite the file while applying loaded values
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ConfigPath)!);
            var obj = new
            {
                enabled = Enabled,
                mode = PushToTalk ? "ptt" : "open",
                pttKey = PttKey,
                alwaysAudible = AlwaysAudible,
                inputDevice = InputDevice,
                inputVolume = (float)InputVolume,
                outputVolume = (float)OutputVolume,
                dNear = (float)DNear,
                dFar = (float)DFar
            };
            File.WriteAllText(ConfigPath,
                JsonSerializer.Serialize(obj, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { }
    }

    private static bool GetB(JsonElement r, string k, bool d)
        => r.TryGetProperty(k, out var v) && v.ValueKind is JsonValueKind.True or JsonValueKind.False ? v.GetBoolean() : d;
    private static int GetI(JsonElement r, string k, int d)
        => r.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetInt32() : d;
    private static double GetD(JsonElement r, string k, double d)
        => r.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : d;
    private static string GetS(JsonElement r, string k, string d)
        => r.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? (v.GetString() ?? d) : d;

    // Avalonia logical Key -> Win32 virtual-key code (what the Companion's
    // WH_KEYBOARD_LL hook reports). Covers the common PTT choices.
    private static int KeyToVk(Key k)
    {
        if (k >= Key.A && k <= Key.Z) return 0x41 + (k - Key.A);
        if (k >= Key.F1 && k <= Key.F12) return 0x70 + (k - Key.F1);
        if (k >= Key.D0 && k <= Key.D9) return 0x30 + (k - Key.D0);
        return k switch
        {
            Key.Space => 0x20,
            Key.LeftCtrl => 0xA2,
            Key.RightCtrl => 0xA3,
            Key.LeftShift => 0xA0,
            Key.RightShift => 0xA1,
            Key.LeftAlt => 0xA4,
            Key.RightAlt => 0xA5,
            Key.Tab => 0x09,
            Key.OemTilde => 0xC0,   // ` - common PTT key
            _ => 0
        };
    }

    private static string KeyName(int vk) => vk switch
    {
        0 => "Click to bind",
        >= 0x41 and <= 0x5A => ((char)vk).ToString(),   // A-Z
        >= 0x30 and <= 0x39 => ((char)vk).ToString(),   // 0-9
        >= 0x70 and <= 0x7B => "F" + (vk - 0x6F),        // F1-F12
        0x20 => "Space",
        0xA0 => "L-Shift", 0xA1 => "R-Shift",
        0xA2 => "L-Ctrl",  0xA3 => "R-Ctrl",
        0xA4 => "L-Alt",   0xA5 => "R-Alt",
        0x09 => "Tab",     0xC0 => "`",
        0x04 => "Mouse Mid", 0x05 => "Mouse 4", 0x06 => "Mouse 5",
        _ => "Key " + vk
    };

    // ── live mic input meter (tool-side; shows "there's input" right in the tab) ──
    // The tool opens its OWN capture of the selected device just for the meter, so
    // it works whether or not a co-op session is running. Runs while the tab is open.
    public void StartMeter()
    {
        StopMeter();
        try
        {
            _meter = new WaveInEvent
            {
                WaveFormat = new WaveFormat(48000, 16, 1),
                BufferMilliseconds = 50,
                DeviceNumber = ResolveInputDevice()
            };
            _meter.DataAvailable += OnMeterData;
            _meter.StartRecording();
        }
        catch { _meter = null; }
    }

    public void StopMeter()
    {
        try
        {
            if (_meter != null)
            {
                _meter.DataAvailable -= OnMeterData;
                _meter.StopRecording();
                _meter.Dispose();
            }
        }
        catch { }
        _meter = null;
        Dispatcher.UIThread.Post(() => MicLevel = 0);
    }

    private void OnMeterData(object? sender, WaveInEventArgs e)
    {
        int samples = e.BytesRecorded / 2;
        if (samples == 0) return;
        long sum = 0;
        for (int i = 0; i + 1 < e.BytesRecorded; i += 2)
        {
            short s = (short)(e.Buffer[i] | (e.Buffer[i + 1] << 8));
            sum += (long)s * s;
        }
        double rms = Math.Sqrt(sum / (double)samples);
        double lvl = Math.Min(100.0, rms / 25.0);
        // quick attack, slow decay so the bar reads naturally
        Dispatcher.UIThread.Post(() => MicLevel = Math.Max(lvl, MicLevel * 0.75));
    }

    private int ResolveInputDevice()
    {
        if (string.IsNullOrEmpty(InputDevice)) return 0;
        for (int i = 0; i < WaveInEvent.DeviceCount; i++)
            if (WaveInEvent.GetCapabilities(i).ProductName.Contains(InputDevice, StringComparison.OrdinalIgnoreCase))
                return i;
        return 0;
    }
}
