using System;
using Pulsar.Companion.Voice;
using Xunit;

public class VoiceFramingTests
{
    [Fact]
    public void MicFrame_HasTagAndLayout()
    {
        var opus = new byte[] { 1, 2, 3, 4 };
        var wire = VoiceFraming.PackMicFrame(seq: 7, flags: 1, opus, opus.Length);
        Assert.Equal(VoiceFraming.TagAudio, wire[0]);
        Assert.Equal(7, BitConverter.ToUInt16(wire, 1));
        Assert.Equal(1, wire[3]);
        Assert.Equal(new byte[] { 1, 2, 3, 4 }, wire[4..]);
    }

    [Fact]
    public void PeerFrame_RoundTrips_GainSeqFlagsOpus()
    {
        var opus = new byte[] { 9, 8, 7 };
        var wire = BuildPeerWire(gain: 0.5f, seq: 7, flags: 1, opus);

        bool ok = VoiceFraming.TryParsePeerFrame(wire, out float gain, out ushort seq,
                                                 out byte flags, out byte[] payload);
        Assert.True(ok);
        Assert.Equal(0.5f, gain, 3);
        Assert.Equal((ushort)7, seq);
        Assert.Equal((byte)1, flags);
        Assert.Equal(opus, payload);
    }

    [Fact]
    public void PeerFrame_RejectsShortOrWrongTag()
    {
        Assert.False(VoiceFraming.TryParsePeerFrame(new byte[] { 0x01, 0, 0 }, out _, out _, out _, out _));
        Assert.False(VoiceFraming.TryParsePeerFrame(new byte[] { 0x02, 0, 0, 0, 0, 0, 0, 0 }, out _, out _, out _, out _));
    }

    [Fact]
    public void Settings_PacksExpectedBytes()
    {
        var wire = VoiceFraming.PackSettings(600f, 4000f, true, mode: 1, pttKey: 0x45);
        Assert.Equal(VoiceFraming.TagSettings, wire[0]);
        Assert.Equal(600f, BitConverter.ToSingle(wire, 1), 3);
        Assert.Equal(4000f, BitConverter.ToSingle(wire, 5), 3);
        Assert.Equal(1, wire[9]);
        Assert.Equal(1, wire[10]);
        Assert.Equal(0x45, BitConverter.ToInt32(wire, 11));
    }

    private static byte[] BuildPeerWire(float gain, ushort seq, byte flags, byte[] opus)
    {
        var b = new byte[1 + 4 + 3 + opus.Length];
        b[0] = VoiceFraming.TagAudio;
        BitConverter.GetBytes(gain).CopyTo(b, 1);
        BitConverter.GetBytes(seq).CopyTo(b, 5);
        b[7] = flags;
        opus.CopyTo(b, 8);
        return b;
    }
}
