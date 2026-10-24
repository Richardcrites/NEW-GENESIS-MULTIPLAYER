using System;
using NAudio.Wave;

namespace Pulsar.Companion.Voice;

// `companion voiceprobe` - captures the default mic, encodes to Opus, decodes,
// and plays it back so you hear yourself with a short delay. Confirms NAudio +
// Concentus work on THIS machine before the 2-player test. No network, no DB.
public static class VoiceSelfLoopProbe
{
    public static void Run()
    {
        var codec = new OpusCodec();
        var fmt = new WaveFormat(OpusCodec.SampleRate, 16, OpusCodec.Channels);
        var play = new BufferedWaveProvider(fmt) { DiscardOnBufferOverflow = true };
        using var outp = new WaveOutEvent();
        outp.Init(play);
        outp.Play();

        var opus = new byte[OpusCodec.kMaxOpus];
        var pcmIn = new short[OpusCodec.FrameSamples];
        var pcmOut = new short[OpusCodec.FrameSamples];

        using var cap = new WaveInEvent { WaveFormat = fmt, BufferMilliseconds = 20 };
        cap.DataAvailable += (_, e) =>
        {
            if (e.BytesRecorded < OpusCodec.FrameSamples * 2) return;
            Buffer.BlockCopy(e.Buffer, 0, pcmIn, 0, OpusCodec.FrameSamples * 2);
            int n = codec.Encode(pcmIn, opus);
            int got = codec.Decode(opus, n, pcmOut);
            if (got <= 0) return;
            var outBytes = new byte[OpusCodec.FrameSamples * 2];
            Buffer.BlockCopy(pcmOut, 0, outBytes, 0, outBytes.Length);
            play.AddSamples(outBytes, 0, outBytes.Length);
        };
        cap.StartRecording();
        Console.WriteLine("voiceprobe: talk into the mic - you should hear yourself. Press Enter to stop.");
        Console.ReadLine();
        cap.StopRecording();
    }
}
