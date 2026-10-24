using System;
using System.IO;

namespace Pulsar.Companion.Voice;

// Voice diagnostics to a file. The companion console is spawned hidden by the
// tool and its stdout is not visible, so voice state goes here instead.
// %LOCALAPPDATA%\PULSAR\pulsar_voice.log (append, timestamped).
public static class VoiceLog
{
    private static readonly object Lock = new();
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "PULSAR", "pulsar_voice.log");

    public static void Write(string msg)
    {
        try
        {
            lock (Lock)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
                File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} {msg}{Environment.NewLine}");
            }
        }
        catch { }
    }
}
