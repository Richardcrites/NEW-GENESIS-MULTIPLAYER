using System;
using System.Collections.Generic;

namespace Pulsar.Companion.Voice;

// Seq-ordered playout buffer. Pop() returns the next in-order frame once the
// buffer has filled to targetDepth; drops frames older than the playhead and
// caps memory at capacity (oldest evicted). Thread-safe: the RX thread Pushes
// while the playout-timer thread Pops (SortedDictionary is not safe for
// concurrent mutation, so both take _lock).
public sealed class JitterBuffer
{
    private readonly SortedDictionary<ushort, byte[]> _frames = new();
    private readonly object _lock = new();
    private readonly int _targetDepth;
    private readonly int _capacity;
    private bool _started;
    private ushort _playhead;

    public JitterBuffer(int targetDepth, int capacity)
    {
        _targetDepth = targetDepth;
        _capacity = capacity;
    }

    public void Push(ushort seq, byte[] frame)
    {
        lock (_lock)
        {
            if (_started && SeqOlderOrEqual(seq, _playhead)) return;   // stale, drop
            _frames[seq] = frame;
            while (_frames.Count > _capacity)
            {
                ushort oldest = default;
                foreach (var k in _frames.Keys) { oldest = k; break; }
                _frames.Remove(oldest);
            }
        }
    }

    public byte[]? Pop()
    {
        lock (_lock)
        {
            if (_frames.Count < (_started ? 1 : _targetDepth)) return null;
            ushort next = default;
            byte[] frame = Array.Empty<byte>();
            foreach (var kv in _frames) { next = kv.Key; frame = kv.Value; break; }
            _frames.Remove(next);
            _started = true;
            _playhead = next;
            return frame;
        }
    }

    // wrap-safe: a is older-or-equal to b when the signed 16-bit delta is <= 0
    private static bool SeqOlderOrEqual(ushort a, ushort b) => (short)(a - b) <= 0;
}
