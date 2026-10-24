using Concentus.Enums;
using Concentus.Structs;

namespace Pulsar.Companion.Voice;

// 48kHz mono 20ms frames (960 samples). Classic Concentus array API (1.1.7).
public sealed class OpusCodec
{
    public const int SampleRate   = 48000;
    public const int Channels     = 1;
    public const int FrameSamples = 960;   // 20ms @ 48kHz
    public const int kMaxOpus     = 400;   // encoded VOIP frame ceiling

    private readonly OpusEncoder _enc;
    private readonly OpusDecoder _dec;

    public OpusCodec()
    {
        _enc = new OpusEncoder(SampleRate, Channels, OpusApplication.OPUS_APPLICATION_VOIP)
        {
            Bitrate = 24000
        };
        _dec = new OpusDecoder(SampleRate, Channels);
    }

    // pcm: FrameSamples shorts -> returns opus byte length written into outBuf.
    public int Encode(short[] pcm, byte[] outBuf)
        => _enc.Encode(pcm, 0, FrameSamples, outBuf, 0, outBuf.Length);

    // opus[0..len) -> FrameSamples shorts into outPcm. Returns samples decoded.
    public int Decode(byte[] opus, int len, short[] outPcm)
        => _dec.Decode(opus, 0, len, outPcm, 0, FrameSamples, false);
}
