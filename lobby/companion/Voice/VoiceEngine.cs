using System;
using NAudio.Wave;

namespace Pulsar.Companion.Voice;

// Wires capture -> Opus -> loopback, and loopback -> jitter -> Opus -> gain ->
// render. Active only between SessionUp and SessionDown.
//
// PTT is NOT gated here: the Companion is a background process and cannot read
// the keyboard/mouse while Starfield holds focus. Instead the Companion always
// captures + sends, and the DLL (in-process with the game, where GetAsyncKeyState
// works) forwards to the peer only while the PTT key is held. Open-mic just keeps
// a silence gate to save bandwidth.
public sealed class VoiceEngine : IDisposable
{
    private readonly VoiceBridgeClient _bridge;
    private readonly OpusCodec _codec = new();
    private readonly JitterBuffer _jitter = new(targetDepth: 4, capacity: 32);
    private readonly object _lock = new();
    private VoiceConfig _cfg;

    private WaveInEvent? _capture;
    private WaveOutEvent? _render;
    private BufferedWaveProvider? _playout;
    private System.Threading.Timer? _playTimer;
    private System.Threading.Timer? _diagTimer;
    private System.Threading.Timer? _settingsTimer;
    private ushort _seq;
    private bool _active;
    private volatile float _lastGain = 1f;

    private readonly byte[] _opus   = new byte[OpusCodec.kMaxOpus];
    private readonly short[] _pcmIn  = new short[OpusCodec.FrameSamples];
    private readonly short[] _pcmOut = new short[OpusCodec.FrameSamples];

    private readonly short[] _accum = new short[OpusCodec.FrameSamples * 16];
    private int _accumCount;
    private int _rawBufs, _framesSent, _lastBytes;

    public VoiceEngine(VoiceConfig cfg)
    {
        _cfg = cfg;
        _bridge = new VoiceBridgeClient();
        _bridge.SessionUp   += Start;
        _bridge.SessionDown += Stop;
        _bridge.PeerAudio   += OnPeerAudio;
        PushSettings();
        // Re-send settings every 2s. The DLL loopback may not be bound yet when the
        // engine starts (the game launches after the companion), and settings are
        // otherwise one-shot -> the DLL would never learn mode/pttKey and would
        // forward audio always (heard without holding PTT).
        _settingsTimer = new System.Threading.Timer(_ => { try { PushSettings(); } catch { } }, null, 2000, 2000);
        VoiceLog.Write($"engine created [build={PttHook.BuildTag}]: enabled={cfg.Enabled} mode={cfg.Mode} pttKey={cfg.PttKey} device='{cfg.InputDevice}'");
    }

    private void PushSettings()
        => _bridge.SendSettings(_cfg.DNear, _cfg.DFar, _cfg.AlwaysAudible,
                                (byte)(_cfg.Mode == "ptt" ? 1 : 0), _cfg.PttKey);

    public void ApplyConfig(VoiceConfig cfg)
    {
        _cfg = cfg;
        PushSettings();
        VoiceLog.Write($"config applied: mode={cfg.Mode} pttKey={cfg.PttKey} always={cfg.AlwaysAudible} device='{cfg.InputDevice}'");
        lock (_lock)
        {
            if (_active) { Stop(); if (cfg.Enabled) Start(); }   // device change
        }
    }

    private void Start()
    {
        lock (_lock)
        {
            if (_active || !_cfg.Enabled) return;
            try
            {
                var fmt = new WaveFormat(OpusCodec.SampleRate, 16, OpusCodec.Channels);
                _playout = new BufferedWaveProvider(fmt) { DiscardOnBufferOverflow = true };
                _render = new WaveOutEvent();
                _render.Init(_playout); _render.Play();

                int dev = ResolveInputDevice();
                string devName = dev < WaveInEvent.DeviceCount ? WaveInEvent.GetCapabilities(dev).ProductName : "?";
                _capture = new WaveInEvent { WaveFormat = fmt, BufferMilliseconds = 20, DeviceNumber = dev };
                _capture.DataAvailable += OnMicData;
                _capture.StartRecording();

                _accumCount = 0;
                _playTimer = new System.Threading.Timer(_ => PumpPlayout(), null, 0, 20);
                _diagTimer = new System.Threading.Timer(_ => DiagTick(), null, 1000, 1000);
                _active = true;
                VoiceLog.Write($"session UP: capture started dev={dev} '{devName}' (of {WaveInEvent.DeviceCount})");
            }
            catch (Exception ex)
            {
                VoiceLog.Write($"session UP FAILED: {ex.GetType().Name}: {ex.Message}");
                try { _capture?.Dispose(); _render?.Dispose(); } catch { }
                _capture = null; _render = null; _playout = null; _active = false;
            }
        }
    }

    private void Stop()
    {
        lock (_lock)
        {
            if (!_active) return;
            try { _playTimer?.Dispose(); } catch { }
            try { _diagTimer?.Dispose(); } catch { }
            try { _capture?.StopRecording(); _capture?.Dispose(); } catch { }
            try { _render?.Stop(); _render?.Dispose(); } catch { }
            _playTimer = null; _diagTimer = null; _capture = null; _render = null; _playout = null;
            _active = false;
            VoiceLog.Write("session DOWN: capture stopped");
        }
    }

    private void OnMicData(object? _, WaveInEventArgs e)
    {
        _rawBufs++;
        _lastBytes = e.BytesRecorded;

        int incoming = e.BytesRecorded / 2;
        for (int i = 0; i < incoming && _accumCount < _accum.Length; i++)
            _accum[_accumCount++] = (short)(e.Buffer[i * 2] | (e.Buffer[i * 2 + 1] << 8));

        while (_accumCount >= OpusCodec.FrameSamples)
        {
            Array.Copy(_accum, 0, _pcmIn, 0, OpusCodec.FrameSamples);
            int rem = _accumCount - OpusCodec.FrameSamples;
            Array.Copy(_accum, OpusCodec.FrameSamples, _accum, 0, rem);
            _accumCount = rem;

            if (_cfg.Mode == "open")   // open-mic silence gate; PTT is gated in the DLL
            {
                long sum = 0;
                for (int i = 0; i < OpusCodec.FrameSamples; i++) sum += (long)_pcmIn[i] * _pcmIn[i];
                if (Math.Sqrt(sum / (double)OpusCodec.FrameSamples) < 350) continue;
            }

            ApplyGainInPlace(_pcmIn, _cfg.InputVolume);
            int n = _codec.Encode(_pcmIn, _opus);
            _bridge.SendMic(_seq++, flags: 1, _opus, n);
            _framesSent++;
        }
    }

    private void DiagTick()
    {
        VoiceLog.Write($"diag active={_active} mode={_cfg.Mode} pttKey={_cfg.PttKey} " +
                       $"rawBufs/s={_rawBufs} lastBytes={_lastBytes} framesSent/s={_framesSent} (PTT gated in DLL)");
        _rawBufs = 0; _framesSent = 0;
    }

    private void OnPeerAudio(float gain, ushort seq, byte flags, byte[] opus)
    {
        _lastGain = gain;
        _jitter.Push(seq, opus);
    }

    private void PumpPlayout()
    {
        BufferedWaveProvider? sink;
        lock (_lock) { sink = _active ? _playout : null; }
        if (sink == null) return;
        var frame = _jitter.Pop();
        if (frame == null) return;
        int got = _codec.Decode(frame, frame.Length, _pcmOut);
        if (got <= 0) return;
        ApplyGainInPlace(_pcmOut, _lastGain * _cfg.OutputVolume);
        var outBytes = new byte[OpusCodec.FrameSamples * 2];
        Buffer.BlockCopy(_pcmOut, 0, outBytes, 0, outBytes.Length);
        sink.AddSamples(outBytes, 0, outBytes.Length);
    }

    private static void ApplyGainInPlace(short[] pcm, float gain)
    {
        if (Math.Abs(gain - 1.0f) < 0.001f) return;
        for (int i = 0; i < pcm.Length; i++)
            pcm[i] = (short)Math.Clamp((int)(pcm[i] * gain), short.MinValue, short.MaxValue);
    }

    private int ResolveInputDevice()
    {
        if (string.IsNullOrEmpty(_cfg.InputDevice)) return 0;
        for (int i = 0; i < WaveInEvent.DeviceCount; i++)
            if (WaveInEvent.GetCapabilities(i).ProductName.Contains(_cfg.InputDevice, StringComparison.OrdinalIgnoreCase))
                return i;
        return 0;
    }

    public void Dispose()
    {
        try { _settingsTimer?.Dispose(); } catch { }
        Stop();
        _bridge.Dispose();
    }
}
