using System.IO;
using Pulsar.Companion.Voice;
using Xunit;

public class VoiceConfigTests
{
    [Fact]
    public void ParsesJson_WithDefaultsForMissingFields()
    {
        var path = Path.GetTempFileName();
        File.WriteAllText(path, "{ \"enabled\": true, \"mode\": \"ptt\", \"pttKey\": 84 }");
        var cfg = VoiceConfig.Load(path);
        Assert.True(cfg.Enabled);
        Assert.Equal("ptt", cfg.Mode);
        Assert.Equal(84, cfg.PttKey);
        Assert.Equal(600f, cfg.DNear);       // default
        Assert.Equal(4000f, cfg.DFar);       // default
        Assert.False(cfg.AlwaysAudible);     // default
        File.Delete(path);
    }

    [Fact]
    public void ParsesFullConfig()
    {
        var path = Path.GetTempFileName();
        File.WriteAllText(path,
            "{ \"enabled\": true, \"mode\": \"open\", \"alwaysAudible\": true, " +
            "\"inputVolume\": 0.5, \"dNear\": 300, \"dFar\": 2000 }");
        var cfg = VoiceConfig.Load(path);
        Assert.Equal("open", cfg.Mode);
        Assert.True(cfg.AlwaysAudible);
        Assert.Equal(0.5f, cfg.InputVolume, 3);
        Assert.Equal(300f, cfg.DNear);
        Assert.Equal(2000f, cfg.DFar);
        File.Delete(path);
    }

    [Fact]
    public void MissingFile_ReturnsDisabledDefault()
    {
        var cfg = VoiceConfig.Load(Path.Combine(Path.GetTempPath(), "does_not_exist_voice.json"));
        Assert.False(cfg.Enabled);
    }
}
