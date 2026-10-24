using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Pulsar.Companion.Voice;

// Model + loader for pulsar_voice.json (%LOCALAPPDATA%\PULSAR). The tool writes
// it; the Companion is the single reader and forwards the three gain-relevant
// values (dNear/dFar/alwaysAudible) to the DLL over the loopback.
public sealed class VoiceConfig
{
    [JsonPropertyName("enabled")]          public bool   Enabled { get; set; } = false;
    [JsonPropertyName("mode")]             public string Mode { get; set; } = "ptt";     // "ptt" | "open"
    [JsonPropertyName("pttKey")]           public int    PttKey { get; set; } = 0;
    [JsonPropertyName("alwaysAudible")]    public bool   AlwaysAudible { get; set; } = false;
    [JsonPropertyName("alwaysAudibleKey")] public int    AlwaysAudibleKey { get; set; } = 0;
    [JsonPropertyName("inputDevice")]      public string InputDevice { get; set; } = "";
    [JsonPropertyName("outputDevice")]     public string OutputDevice { get; set; } = "";
    [JsonPropertyName("inputVolume")]      public float  InputVolume { get; set; } = 1.0f;
    [JsonPropertyName("outputVolume")]     public float  OutputVolume { get; set; } = 1.0f;
    [JsonPropertyName("dNear")]            public float  DNear { get; set; } = 600f;
    [JsonPropertyName("dFar")]             public float  DFar { get; set; } = 4000f;

    // Same base dir the Companion already uses for pulsar_lobbies.json / handoff.
    public static string DefaultPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                     "PULSAR", "pulsar_voice.json");

    public static VoiceConfig Load(string path)
    {
        try
        {
            if (!File.Exists(path)) return new VoiceConfig();
            return JsonSerializer.Deserialize<VoiceConfig>(File.ReadAllText(path)) ?? new VoiceConfig();
        }
        catch { return new VoiceConfig(); }
    }
}
