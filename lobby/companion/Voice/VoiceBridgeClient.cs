using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;

namespace Pulsar.Companion.Voice;

// Companion side of the localhost loopback to the DLL. Sends HELLO on start,
// sends mic frames + settings, and raises events for peer audio + session
// state pushed by the DLL. One background receive thread.
//
// The DLL binds its loopback socket at game start, which is AFTER the Companion
// launches the game. So a single HELLO would be lost. A 2s heartbeat re-announces
// our endpoint (1 byte, idempotent on the DLL side) until the game is up.
public sealed class VoiceBridgeClient : IDisposable
{
    private readonly UdpClient _udp;
    private readonly IPEndPoint _dll = new(IPAddress.Loopback, VoiceFraming.LoopbackPort);
    private readonly Thread _rx;
    private readonly Timer _helloTimer;
    private volatile bool _run = true;

    public event Action<float, ushort, byte, byte[]>? PeerAudio;   // gain, seq, flags, opus
    public event Action? SessionUp;
    public event Action? SessionDown;

    public VoiceBridgeClient()
    {
        _udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));  // ephemeral
        SendHello();
        _rx = new Thread(RxLoop) { IsBackground = true, Name = "voice-rx" };
        _rx.Start();
        _helloTimer = new Timer(_ => SendHello(), null, 2000, 2000);
    }

    private void SendHello()
    {
        try { _udp.Send(VoiceFraming.Hello(), 1, _dll); } catch { }
    }

    public void SendMic(ushort seq, byte flags, byte[] opus, int opusLen)
    {
        var f = VoiceFraming.PackMicFrame(seq, flags, opus, opusLen);
        try { _udp.Send(f, f.Length, _dll); } catch { }
    }

    public void SendSettings(float dNear, float dFar, bool alwaysAudible, byte mode, int pttKey)
    {
        var f = VoiceFraming.PackSettings(dNear, dFar, alwaysAudible, mode, pttKey);
        try { _udp.Send(f, f.Length, _dll); } catch { }
    }

    private void RxLoop()
    {
        var any = new IPEndPoint(IPAddress.Any, 0);
        while (_run)
        {
            byte[] buf;
            try { buf = _udp.Receive(ref any); }
            catch { if (!_run) break; continue; }
            if (buf.Length == 0) continue;
            switch (buf[0])
            {
                case VoiceFraming.TagAudio:
                    if (VoiceFraming.TryParsePeerFrame(buf, out var g, out var s, out var fl, out var p))
                        PeerAudio?.Invoke(g, s, fl, p);
                    break;
                case VoiceFraming.TagSessionUp:   SessionUp?.Invoke();   break;
                case VoiceFraming.TagSessionDown: SessionDown?.Invoke(); break;
            }
        }
    }

    public void Dispose()
    {
        _run = false;
        try { _helloTimer.Dispose(); } catch { }
        try { _udp.Close(); } catch { }
        try { _rx.Join(500); } catch { }
    }
}
