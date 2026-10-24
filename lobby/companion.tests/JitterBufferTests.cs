using Pulsar.Companion.Voice;
using Xunit;

public class JitterBufferTests
{
    [Fact]
    public void ReordersOutOfOrderFrames()
    {
        var jb = new JitterBuffer(targetDepth: 2, capacity: 8);
        jb.Push(2, new byte[] { 2 });
        jb.Push(1, new byte[] { 1 });
        jb.Push(3, new byte[] { 3 });
        Assert.Equal((byte)1, jb.Pop()![0]);
        Assert.Equal((byte)2, jb.Pop()![0]);
        Assert.Equal((byte)3, jb.Pop()![0]);
    }

    [Fact]
    public void DropsStaleFramesBelowPlayhead()
    {
        var jb = new JitterBuffer(targetDepth: 1, capacity: 8);
        jb.Push(5, new byte[] { 5 });
        Assert.Equal((byte)5, jb.Pop()![0]);
        jb.Push(4, new byte[] { 4 });     // older than playhead -> dropped
        Assert.Null(jb.Pop());
    }

    [Fact]
    public void ReturnsNullWhenBelowTargetDepth()
    {
        var jb = new JitterBuffer(targetDepth: 3, capacity: 8);
        jb.Push(1, new byte[] { 1 });
        Assert.Null(jb.Pop());            // still filling
    }

    [Fact]
    public void EvictsOldestBeyondCapacity()
    {
        var jb = new JitterBuffer(targetDepth: 1, capacity: 2);
        jb.Push(1, new byte[] { 1 });
        jb.Push(2, new byte[] { 2 });
        jb.Push(3, new byte[] { 3 });     // capacity 2 -> seq 1 evicted
        Assert.Equal((byte)2, jb.Pop()![0]);
        Assert.Equal((byte)3, jb.Pop()![0]);
        Assert.Null(jb.Pop());
    }
}
