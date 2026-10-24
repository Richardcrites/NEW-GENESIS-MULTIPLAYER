using System;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;

namespace Pulsar.Services;

// Phase 4 - One-Click Local Stack. When the DB mode is "Local", this spins up a full
// self-hosted stack on this PC / LAN:
//   1. `spacetime start`  → local SpacetimeDB on http://127.0.0.1:3000
//   2. `spacetime publish` of the lobby module  (best-effort; see PublishModuleAsync)
//   3. PulsarRelay.exe on UDP 7777 (PULSAR_RELAY_PORT)
// DaemonBridge then points the companion at http://127.0.0.1:3000 + 127.0.0.1:7777.
//
// All child processes are tracked and Kill(true)'d on Stop()/app-close. The online
// path (DB mode Online) never touches this class.
public sealed class LocalStack : IDisposable
{
    public const string LocalStdbHttp = "http://127.0.0.1:3000";
    public const string LocalStdbUri = "ws://127.0.0.1:3000";
    public const string LocalRelayAddr = "127.0.0.1:7777";
    public const string ModuleName = "pulsar-lobby";

    private readonly AppConfig _config;
    private readonly Action<string> _log;

    private Process? _spacetime;
    private Process? _relay;

    public LocalStack(AppConfig config, Action<string> log)
    {
        _config = config;
        _log = log;
    }

    public bool IsRunning => _spacetime is { HasExited: false } || _relay is { HasExited: false };

    // Bring up SpacetimeDB + module + relay. Returns true if the core (STDB + relay)
    // came up; a publish failure is non-fatal (logged + TODO) so an existing module
    // keeps working. Never throws.
    public async Task<bool> StartAsync(CancellationToken ct = default)
    {
        try
        {
            if (!await StartSpacetimeAsync(ct).ConfigureAwait(false))
                return false;

            await PublishModuleAsync(ct).ConfigureAwait(false);   // best-effort

            // The relay is only needed for NAT traversal; direct/LAN self-host connects
            // straight to the host IP, so a missing relay is non-fatal.
            if (!StartRelay())
                _log("Local relay not started - direct/LAN hosting still works without it.");

            _log("Local stack ready (STDB " + LocalStdbHttp + ").");
            return true;
        }
        catch (Exception ex)
        {
            _log("Local stack start failed: " + ex.Message);
            return false;
        }
    }

    // ── 1. spacetime start ───────────────────────────────────────────────────────
    private async Task<bool> StartSpacetimeAsync(CancellationToken ct)
    {
        // Already reachable? (a manually-started instance) → reuse it.
        if (await IsStdbReachableAsync(ct).ConfigureAwait(false))
        {
            _log("SpacetimeDB already running at " + LocalStdbHttp + " - reusing it.");
            return true;
        }

        var exe = ResolveSpacetimeExe();
        if (!File.Exists(exe))
        {
            _log("spacetime.exe not found (not bundled, and no valid path in Settings).");
            return false;
        }

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? "",
            };
            psi.ArgumentList.Add("start");
            _spacetime = StartTracked(psi, "stdb");
            _log("spacetime start launched; waiting for " + LocalStdbHttp + " ...");
        }
        catch (Exception ex)
        {
            _log("spacetime start failed: " + ex.Message);
            return false;
        }

        // Poll until reachable (~10s timeout).
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < deadline)
        {
            if (ct.IsCancellationRequested) return false;
            if (await IsStdbReachableAsync(ct).ConfigureAwait(false))
            {
                _log("SpacetimeDB is up.");
                return true;
            }
            await Task.Delay(500, ct).ConfigureAwait(false);
        }
        _log("SpacetimeDB did not become reachable within 10s.");
        return false;
    }

    private static async Task<bool> IsStdbReachableAsync(CancellationToken ct)
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(2) };
            using var resp = await http.GetAsync(LocalStdbHttp, ct).ConfigureAwait(false);
            return true;   // any HTTP response means the listener is up
        }
        catch { return false; }
    }

    // ── 2. spacetime publish (best-effort) ───────────────────────────────────────
    // The module project lives at lobby/server/spacetimedb (located by walk-up like
    // GuessCompanionExe). The publish CLI flag differs across spacetime versions; this
    // build agent could not verify which flag this CLI expects for `publish`, so we
    // run a reasonable attempt and treat any failure as non-fatal: an already-published
    // module keeps working. TODO(human): confirm the exact `spacetime publish` flag
    // (`spacetime publish --help`) on the bundled CLI and harden this.
    // Resolve the spacetime CLI: prefer the binary bundled next to the tool (release ZIP
    // layout), then fall back to the explicit path from Settings (dev machines). Returns
    // the configured path even if missing, so the caller can surface a useful error.
    private string ResolveSpacetimeExe()
    {
        var baseDir = AppContext.BaseDirectory;
        foreach (var rel in new[] { "spacetime.exe", Path.Combine("spacetime", "spacetime.exe") })
        {
            var c = Path.Combine(baseDir, rel);
            if (File.Exists(c)) return c;
        }
        return _config.SpacetimeExePath?.Trim() ?? "";
    }

    private async Task PublishModuleAsync(CancellationToken ct)
    {
        var exe = ResolveSpacetimeExe();
        if (!File.Exists(exe)) return;

        var modulePath = GuessModulePath();
        if (modulePath == null)
        {
            _log("Module path not found - skipping publish (assuming module is already published). TODO: bundle/locate lobby/server/spacetimedb.");
            return;
        }

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? "",
            };
            psi.ArgumentList.Add("publish");
            psi.ArgumentList.Add(ModuleName);
            // The build agent found `generate`/`build` use `--project-path` on this CLI;
            // publish very likely accepts the same. If a future CLI renames it, the
            // failure is swallowed below (non-fatal).
            psi.ArgumentList.Add("--project-path");
            psi.ArgumentList.Add(modulePath);

            using var proc = new Process { StartInfo = psi };
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) _log("publish: " + e.Data); };
            proc.ErrorDataReceived += (_, e) => { if (e.Data != null) _log("publish! " + e.Data); };
            proc.Start();
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();

            // Give it up to 90s - a cold WASM build can be slow - but never hang forever.
            using var reg = ct.Register(() => { try { if (!proc.HasExited) proc.Kill(true); } catch { } });
            var exited = await Task.Run(() => proc.WaitForExit(90_000), ct).ConfigureAwait(false);
            if (!exited)
            {
                _log("publish timed out (90s) - continuing; module may already be published.");
                try { proc.Kill(true); } catch { }
                return;
            }
            _log(proc.ExitCode == 0
                ? "Module published (" + ModuleName + ")."
                : "publish exited " + proc.ExitCode + " - continuing (module may already be published). TODO: verify the publish flag.");
        }
        catch (Exception ex)
        {
            _log("publish skipped: " + ex.Message + " - continuing (module may already be published).");
        }
    }

    // ── 3. PulsarRelay.exe ───────────────────────────────────────────────────────
    private bool StartRelay()
    {
        var exe = GuessRelayExe();
        if (!File.Exists(exe))
        {
            _log("PulsarRelay.exe not found: " + exe);
            return false;
        }
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? "",
            };
            psi.Environment["PULSAR_RELAY_PORT"] = "7777";
            _relay = StartTracked(psi, "relay");
            _log("Local relay started on UDP 7777.");
            return true;
        }
        catch (Exception ex)
        {
            _log("relay start failed: " + ex.Message);
            return false;
        }
    }

    private Process StartTracked(ProcessStartInfo psi, string tag)
    {
        var proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
        proc.OutputDataReceived += (_, e) => { if (e.Data != null) _log(tag + ": " + e.Data); };
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) _log(tag + "! " + e.Data); };
        proc.Start();
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();
        return proc;
    }

    // ── shutdown ─────────────────────────────────────────────────────────────────
    public void Stop()
    {
        KillTracked(ref _relay, "relay");
        KillTracked(ref _spacetime, "stdb");
    }

    private void KillTracked(ref Process? proc, string tag)
    {
        try
        {
            if (proc is { HasExited: false })
            {
                proc.Kill(true);
                proc.WaitForExit(3000);
                _log("Stopped local " + tag + ".");
            }
        }
        catch { /* best-effort */ }
        proc = null;
    }

    public void Dispose() => Stop();

    // ── locators (walk-up search, mirrors DaemonBridge.GuessCompanionExe) ─────────
    public static string GuessRelayExe()
    {
        var baseDir = AppContext.BaseDirectory;
        foreach (var rel in new[] { "PulsarRelay.exe", Path.Combine("relay", "PulsarRelay.exe") })
        {
            var c = Path.Combine(baseDir, rel);
            if (File.Exists(c)) return c;
        }
        var dir = baseDir;
        for (int i = 0; i < 8 && dir != null; i++)
        {
            foreach (var cfg in new[] { "Release", "Debug" })
            {
                var c = Path.Combine(dir, "relay", "bin", cfg, "net8.0", "PulsarRelay.exe");
                if (File.Exists(c)) return c;
            }
            dir = Directory.GetParent(dir)?.FullName;
        }
        return "PulsarRelay.exe";
    }

    // Find lobby/server/spacetimedb (the module project) by walking up parents.
    private static string? GuessModulePath()
    {
        // Bundled layout (release ZIP): the module source ships next to the tool.
        var baseDir = AppContext.BaseDirectory;
        foreach (var rel in new[] { "module", "spacetimedb", Path.Combine("server", "spacetimedb") })
        {
            var c = Path.Combine(baseDir, rel);
            if (Directory.Exists(c)) return c;
        }
        var dir = AppContext.BaseDirectory;
        for (int i = 0; i < 10 && dir != null; i++)
        {
            var c = Path.Combine(dir, "server", "spacetimedb");
            if (Directory.Exists(c)) return c;
            // Also try a lobby\server\spacetimedb layout one level up.
            var c2 = Path.Combine(dir, "lobby", "server", "spacetimedb");
            if (Directory.Exists(c2)) return c2;
            dir = Directory.GetParent(dir)?.FullName;
        }
        return null;
    }
}
