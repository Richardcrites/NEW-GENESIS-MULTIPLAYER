using System;

namespace Pulsar.Companion.Voice;

// Pure pack/parse for the localhost loopback frames. Mirrors the C++
// include/VoiceBridge/VoiceProtocol.h contract. No sockets, no state.
public static class VoiceFraming
{
    public const byte TagAudio       = 0x01;
    public const byte TagSessionUp   = 0x02;
    public const byte TagSessionDown = 0x03;
    public const byte TagSettings    = 0x04;
    public const byte TagHello       = 0x10;
    public const int  LoopbackPort   = 27900;

    // Companion -> DLL mic frame: [tag][seq:2][flags:1][opus...]
    public static byte[] PackMicFrame(ushort seq, byte flags, byte[] opus, int opusLen)
    {
        var b = new byte[1 + 2 + 1 + opusLen];
        b[0] = TagAudio;
        BitConverter.GetBytes(seq).CopyTo(b, 1);
        b[3] = flags;
        Array.Copy(opus, 0, b, 4, opusLen);
        return b;
    }

    // Companion -> DLL settings: [tag][dNear:4][dFar:4][alwaysAudible:1][mode:1][pttKey:4]
    // mode: 0 = open mic, 1 = push-to-talk. PTT is gated in the DLL (in-process),
    // so the DLL needs the mode + key.
    public static byte[] PackSettings(float dNear, float dFar, bool alwaysAudible, byte mode, int pttKey)
    {
        var b = new byte[1 + 4 + 4 + 1 + 1 + 4];
        b[0] = TagSettings;
        BitConverter.GetBytes(dNear).CopyTo(b, 1);
        BitConverter.GetBytes(dFar).CopyTo(b, 5);
        b[9] = (byte)(alwaysAudible ? 1 : 0);
        b[10] = mode;
        BitConverter.GetBytes(pttKey).CopyTo(b, 11);
        return b;
    }

    public static byte[] Hello() => new[] { TagHello };

    // DLL -> Companion peer audio: [tag][gain:4][seq:2][flags:1][opus...]
    public static bool TryParsePeerFrame(byte[] buf, out float gain, out ushort seq,
                                         out byte flags, out byte[] payload)
    {
        gain = 0; seq = 0; flags = 0; payload = Array.Empty<byte>();
        if (buf.Length < 8 || buf[0] != TagAudio) return false;
        gain  = BitConverter.ToSingle(buf, 1);
        seq   = BitConverter.ToUInt16(buf, 5);
        flags = buf[7];
        payload = new byte[buf.Length - 8];
        Array.Copy(buf, 8, payload, 0, payload.Length);
        return true;
    }
}
