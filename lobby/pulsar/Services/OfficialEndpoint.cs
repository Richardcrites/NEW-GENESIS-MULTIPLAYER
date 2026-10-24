using System;
using System.IO;

namespace Pulsar.Services;

// Where the "official" lobby lives.
//
// NOTHING is compiled into this build. PULSAR is self-host first: if no endpoint
// is configured anywhere, the tool falls back to the local stack and the user can
// point it wherever they like in Settings. That keeps the project independent of
// any single operator, so it does not stop working when one server goes away.
//
// An operator who wants to ship a default has two ways to supply one, checked in
// order:
//
//   1. The PULSAR_OFFICIAL_STDB_URI environment variable.
//   2. A file official_endpoint.txt next to the executable. First line that is
//      neither blank nor a # comment wins.
//
// Deliberately NOT a compiled-in constant: baking an address into the binary
// made it impossible to hand the project to somebody else without also handing
// over the server.
internal static class OfficialEndpoint
{
    public static string Uri { get; } = Resolve();

    public static bool IsConfigured => Uri.Length > 0;

    private const string EnvVar   = "PULSAR_OFFICIAL_STDB_URI";
    private const string FileName = "official_endpoint.txt";

    private static string Resolve()
    {
        try
        {
            var env = Environment.GetEnvironmentVariable(EnvVar);
            if (!string.IsNullOrWhiteSpace(env))
                return env.Trim();

            var sideCar = Path.Combine(AppContext.BaseDirectory, FileName);
            if (File.Exists(sideCar))
            {
                foreach (var raw in File.ReadAllLines(sideCar))
                {
                    var line = raw.Trim();
                    if (line.Length == 0 || line.StartsWith("#", StringComparison.Ordinal))
                        continue;
                    return line;
                }
            }
        }
        catch
        {
            // Best effort. An unreadable side-car must never keep the tool from
            // starting: the user can always enter an address in Settings.
        }

        return "";
    }
}
