using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using SpacetimeDB;
using SpacetimeDB.Types;
using Pulsar.Companion.Voice;

// PULSAR companion core (Teil 2B/2C) - headless SpacetimeDB client for the local
// pulsar-lobby module. Persists a stable identity token, uploads/downloads the
// opaque Teil-1 profile blob, brokers lobbies, and writes the handoff files the
// PULSAR DLL (Teil 2C) reads on launch. Shared dir: %LOCALAPPDATA%\PULSAR\.
//
// Commands:
//   register <name>
//   push-profile [blobfile] <version>   upsert_profile (default file = profile-out)
//   pull-profile [outfile]              download own profile (default = profile-in)
//   list                               print lobbies (server browser)
//   host <name> <addr> <max> [--launch <exe>]   register + create_lobby + handoff;
//                                               STAYS CONNECTED so the lobby stays
//                                               open (until Q, or the game exits)
//   join <lobbyId> [--launch <exe>]     join + pull profile -> profile-in + handoff
internal static class Program
{
    // Endpoint is env-overridable for the VPS (PULSAR_STDB_URI=wss://host or
    // https://host - the SDK upgrades https->wss). Defaults to local dev.
    private static readonly string Uri =
        Environment.GetEnvironmentVariable("PULSAR_STDB_URI") ?? "http://127.0.0.1:3000";
    private static readonly string ModuleName =
        Environment.GetEnvironmentVariable("PULSAR_STDB_DB") ?? "pulsar-lobby";

    // NAT-free relay (lobby/relay). Empty = direct/LAN mode (handoff has no relay).
    private static readonly string RelayAddr =
        Environment.GetEnvironmentVariable("PULSAR_RELAY_ADDR") ?? "";

    // Only pick verified relays when hosting (mirrors the tool's "Show community relays"
    // being OFF). Default verified-only.
    private static readonly bool RelayVerifiedOnly =
        (Environment.GetEnvironmentVariable("PULSAR_RELAY_VERIFIED_ONLY") ?? "1") != "0";

    // Host-authoritative relay selection from the LIVE master-list: prefer verified +
    // accepting, then lowest load. "" if none (caller falls back to the env relay addr).
    // The joiner reuses the host's choice (stored in the lobby), so both always meet.
    private static string PickRelay(DbConnection c)
    {
        Relay? best = null;
        foreach (var r in c.Db.Relay.Iter())
        {
            if (!r.Accepting) continue;
            if (RelayVerifiedOnly && !r.Verified) continue;
            if (best is null) { best = r; continue; }
            bool better = (r.Verified && !best.Verified)
                       || (r.Verified == best.Verified && r.LoadPct < best.LoadPct);
            if (better) best = r;
        }
        return best?.PublicHost ?? "";
    }

    private static string NewSessionHex()
    {
        var b = new byte[16];
        System.Security.Cryptography.RandomNumberGenerator.Fill(b);
        return Convert.ToHexString(b);   // 32 uppercase hex chars
    }

    // Per-lobby E2E auth secret (anti-MITM): 32 random bytes -> 64 hex. Delivered to both
    // peers over the trusted DB (the relay never sees it) and mixed into the key derivation
    // so a malicious relay cannot MITM the key exchange.
    private static string NewSecretHex()
    {
        var b = new byte[32];
        System.Security.Cryptography.RandomNumberGenerator.Fill(b);
        return Convert.ToHexString(b);   // 64 uppercase hex chars
    }

    private static string s_pendingRegister = "";   // cmdId awaiting an OnRegisterAccount result

    private static string GuessLanAddr()
    {
        try
        {
            foreach (var ip in System.Net.Dns.GetHostAddresses(System.Net.Dns.GetHostName()))
                if (ip.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork &&
                    !System.Net.IPAddress.IsLoopback(ip))
                    return ip + ":7777";
        }
        catch { /* fall through */ }
        return "127.0.0.1:7777";
    }

    private static long GetMs() => Environment.TickCount64;
    // Password is hashed in the GUI (plaintext never reaches the companion / cmd file);
    // the companion just forwards the hash to the reducer.

    // Async join state: JoinLobby verifies the password, then we read the session
    // from the my_relay view (relay mode) before writing the handoff.
    private static string s_joinCmd = "";
    private static ulong  s_joinLobby = 0;
    private static string s_joinDirectAddr = "";   // non-empty = direct lobby (no session needed)
    private static uint   s_joinVersion = 0;
    private static bool   s_joinAwaitSession = false;
    private static long   s_joinDeadline = 0;

    // Deferred host ack: the host's lobby (with its generated Code) appears in the
    // local cache only after CreateLobby commits, so the ack waits for it.
    private static Identity s_identity;
    private static string   s_hostCmd = "";
    private static long     s_hostDeadline = 0;

    // Auto-save: watch profile-out (written by the DLL on disconnect) and upload it.
    private static bool s_autoSave = (Environment.GetEnvironmentVariable("PULSAR_AUTOSAVE") ?? "1") != "0";
    private static long s_profileOutSig = -1;

    private static bool s_done;
    private static int s_exit;
    private static Process? s_gameProc;

    private static string LobbyDir()
    {
        var d = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PULSAR");
        Directory.CreateDirectory(d);
        return d;
    }
    private static string HandoffPath => Path.Combine(LobbyDir(), "pulsar_handoff.json");
    private static string ProfileInPath => Path.Combine(LobbyDir(), "pulsar_profile_in.json");
    private static string ProfileOutPath => Path.Combine(LobbyDir(), "pulsar_profile_out.json");
    private static string TokenPath => Path.Combine(LobbyDir(), "companion.token");
    private static string LobbiesPath => Path.Combine(LobbyDir(), "pulsar_lobbies.json");
    private static string RelaysPath => Path.Combine(LobbyDir(), "pulsar_relays.json");
    private static string CmdPath => Path.Combine(LobbyDir(), "pulsar_cmd.json");
    private static string AckPath => Path.Combine(LobbyDir(), "pulsar_ack.json");

    private const string TokenMarker = "DPAPI1:";   // prefix for DPAPI-encrypted token files

#pragma warning disable CA1416 // DPAPI is Windows-only; the companion is Windows-only by design
    private static string? LoadToken()
    {
        if (!File.Exists(TokenPath)) return null;
        var raw = File.ReadAllText(TokenPath).Trim();
        if (raw.Length == 0) return null;
        if (!raw.StartsWith(TokenMarker)) return raw;   // legacy plaintext → migrate on next save
        try
        {
            var cipher = Convert.FromBase64String(raw.Substring(TokenMarker.Length));
            var plain  = ProtectedData.Unprotect(cipher, null, DataProtectionScope.CurrentUser);
            return System.Text.Encoding.UTF8.GetString(plain);
        }
        catch { return null; }   // unreadable (e.g. different user) → re-auth
    }

    private static void SaveToken(string token)
    {
        try
        {
            var plain  = System.Text.Encoding.UTF8.GetBytes(token);
            var cipher = ProtectedData.Protect(plain, null, DataProtectionScope.CurrentUser);
            File.WriteAllText(TokenPath, TokenMarker + Convert.ToBase64String(cipher));
        }
        catch { /* best-effort */ }
    }
#pragma warning restore CA1416

    private static string? TakeLaunch(ref string[] args)
    {
        for (int i = 0; i < args.Length - 1; i++)
            if (args[i] == "--launch") return args[i + 1];
        return null;
    }

    private static void MaybeLaunch(string? exe)
    {
        if (string.IsNullOrEmpty(exe)) return;
        try
        {
            exe = ResolveSfseLaunch(exe);   // co-op needs SFSE; never launch Starfield.exe directly
            KillLeftoverStarfields();       // issue #1 double-instance guard (headless: enforce, no prompt)
            s_gameProc = Process.Start(new ProcessStartInfo { FileName = exe, UseShellExecute = true });
            Console.WriteLine($"[companion] launched {exe}");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[companion] launch failed: {ex.Message}");
        }
    }

    // issue #1 root cause: a leftover Starfield.exe (killed on the loading screen)
    // becomes a SECOND game instance on the next host/join, which self-connects over
    // LAN and kills the real remote friend's relay session (reason-0 reconnect loop).
    // The tool asks the user first; the companion is headless, so here we just enforce
    // a clean single launch by closing any leftover Starfield before starting the game.
    private static void KillLeftoverStarfields()
    {
        try
        {
            var procs = Process.GetProcessesByName("Starfield");
            if (procs.Length == 0) return;
            int killed = 0;
            foreach (var p in procs) { try { p.Kill(); killed++; } catch { /* gone / no rights */ } }
            for (int i = 0; i < 25 && Process.GetProcessesByName("Starfield").Length > 0; i++)
                Thread.Sleep(200);
            Console.WriteLine($"[companion] closed {killed} leftover Starfield process(es) before launch (issue #1 guard)");
        }
        catch (Exception ex) { Console.WriteLine($"[companion] starfield guard failed: {ex.Message}"); }
    }

    // Co-op needs SFSE. Launching Starfield.exe directly bypasses SFSE and the PULSAR
    // mod never injects. If sfse_loader.exe sits next to the configured exe, launch it.
    private static string ResolveSfseLaunch(string exe)
    {
        try
        {
            if (string.Equals(Path.GetFileName(exe), "sfse_loader.exe", StringComparison.OrdinalIgnoreCase))
                return exe;
            var dir = Path.GetDirectoryName(exe);
            if (string.IsNullOrEmpty(dir)) return exe;
            var sfse = Path.Combine(dir, "sfse_loader.exe");
            if (File.Exists(sfse)) return sfse;
        }
        catch { /* fall through to the original path */ }
        return exe;
    }

    private static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("usage: companion <register|push-profile|pull-profile|reset-profile|list|host|join|serve> ...");
            return 2;
        }
        string cmd = args[0];
        if (cmd == "voiceprobe")   // local audio self-test (no DB, no network)
        {
            VoiceSelfLoopProbe.Run();
            return 0;
        }
        string? launch = TakeLaunch(ref args);

        var builder = DbConnection.Builder()
            .WithUri(Uri)
            .WithDatabaseName(ModuleName);

        var existingToken = LoadToken();
        if (!string.IsNullOrEmpty(existingToken))
            builder = builder.WithToken(existingToken);

        var conn = builder
            .OnConnect((c, identity, token) =>
            {
                s_identity = identity;
                SaveToken(token);
                Console.WriteLine($"[companion] connected identity={identity}");
                try { Dispatch(c, cmd, args, launch); }
                catch (Exception ex)
                {
                    Console.WriteLine($"[companion] command error: {ex.Message}");
                    s_exit = 1; s_done = true;
                }
            })
            .OnConnectError((Exception e) =>
            {
                Console.WriteLine($"[companion] connect ERROR: {e.Message}");
                // A stale identity token after a server --delete-data is rejected with
                // 401. Clear it so the NEXT start gets a fresh identity (just press
                // Start again) - no hunting for a hidden token file.
                if (e.Message.Contains("401") || e.Message.Contains("NotAWebSocket"))
                {
                    try { File.Delete(TokenPath); Console.WriteLine("[companion] cleared stale token - press Start again"); }
                    catch { /* best-effort */ }
                }
                s_exit = 1; s_done = true;
            })
            .OnDisconnect((ctx, err) =>
            {
                if (err is not null) Console.WriteLine($"[companion] disconnected: {err}");
            })
            .Build();

        if (cmd == "host" || cmd == "serve")
        {
            // Keep-alive: the connection MUST stay up, otherwise the module's
            // ClientDisconnected hook closes hosted lobbies / drops memberships.
            // 'serve' additionally publishes the lobby list and services the
            // DLL command file. Runs until the game exits or 'Q'.
            Console.WriteLine($"[companion] {cmd} - staying connected. Press Q to stop.");
            string lastLobbies = "";
            string lastRelays = "";
            // proximity voice: build the engine when enabled; poll the config
            // file (~1s) so tool changes (mode, PTT key, range, always-audible)
            // apply live without a restart. serve only (the in-game session).
            VoiceEngine? voice = null;
            DateTime voiceCfgStamp = DateTime.MinValue;
            int voicePollTick = 0;
            void RefreshVoice()
            {
                try
                {
                    var vpath = VoiceConfig.DefaultPath;
                    var stamp = File.Exists(vpath) ? File.GetLastWriteTimeUtc(vpath) : DateTime.MinValue;
                    if (stamp == voiceCfgStamp) return;
                    voiceCfgStamp = stamp;
                    var cfg = VoiceConfig.Load(vpath);
                    if (voice == null)
                    {
                        if (cfg.Enabled) { voice = new VoiceEngine(cfg); Console.WriteLine("[companion] voice: engine up (waiting for peer)"); }
                    }
                    else voice.ApplyConfig(cfg);
                }
                catch (Exception ex) { Console.WriteLine($"[companion] voice cfg error: {ex.Message}"); }
            }
            if (cmd == "serve") RefreshVoice();
            while (!s_done)
            {
                conn.FrameTick();
                if (cmd == "serve")
                {
                    lastLobbies = WriteLobbies(conn, lastLobbies);
                    lastRelays = WriteRelays(conn, lastRelays);
                    HandleCommandFile(conn);
                    ResolveAwaitedJoin(conn);
                    ResolveAwaitedHost(conn);
                    AutoUploadProfile(conn);
                    if (++voicePollTick >= 20) { voicePollTick = 0; RefreshVoice(); }
                }
                if (s_gameProc is { HasExited: true }) { Console.WriteLine("[companion] game exited"); break; }
                if (!Console.IsInputRedirected && Console.KeyAvailable &&
                    Console.ReadKey(true).Key == ConsoleKey.Q) break;
                Thread.Sleep(50);
            }
            try { voice?.Dispose(); } catch { }
        }
        else
        {
            var sw = Stopwatch.StartNew();
            while (!s_done && sw.Elapsed.TotalSeconds < 20)
            {
                conn.FrameTick();
                Thread.Sleep(50);
            }
            if (!s_done) { Console.WriteLine("[companion] TIMEOUT"); s_exit = 1; }
        }

        try { conn.Disconnect(); } catch { /* already closed */ }
        for (int i = 0; i < 20 && conn.IsActive; i++) { conn.FrameTick(); Thread.Sleep(20); }
        return s_exit;
    }

    private static void Dispatch(DbConnection c, string cmd, string[] args, string? launch)
    {
        switch (cmd)
        {
            case "register":
            {
                string name = args.Length > 1 ? args[1] : "Player";
                c.Reducers.OnRegisterAccount += (ctx, dn) =>
                {
                    Console.WriteLine($"[companion] register_account applied: {dn}");
                    s_done = true;
                };
                c.Reducers.RegisterAccount(name);
                break;
            }

            case "push-profile":
            {
                // push-profile [file] <version>   (file defaults to the profile-out)
                string file; uint version;
                if (args.Length >= 3) { file = args[1]; version = uint.Parse(args[2]); }
                else if (args.Length == 2) { file = ProfileOutPath; version = uint.Parse(args[1]); }
                else { Console.WriteLine("usage: push-profile [file] <version>"); s_done = true; break; }
                var blob = new List<byte>(File.ReadAllBytes(file));
                c.Reducers.OnUpsertProfile += (ctx, b, v) =>
                {
                    Console.WriteLine($"[companion] upsert_profile applied: {b.Count} bytes v{v}");
                    s_done = true;
                };
                c.Reducers.UpsertProfile(blob, version);
                break;
            }

            case "pull-profile":
            {
                string outFile = args.Length > 1 ? args[1] : ProfileInPath;
                c.SubscriptionBuilder()
                    .OnApplied(ctx =>
                    {
                        var mine = FindMyProfile(c);
                        if (mine is null)
                            Console.WriteLine("[companion] no profile stored for this identity");
                        else
                        {
                            File.WriteAllBytes(outFile, mine.Blob.ToArray());
                            WriteMirror(mine.Version, mine.Blob.ToArray());   // keep local backup fresh
                            Console.WriteLine($"[companion] wrote {mine.Blob.Count} bytes (v{mine.Version}) -> {outFile}");
                        }
                        s_done = true;
                    })
                    .Subscribe(new[] { "SELECT * FROM my_profile" });
                break;
            }

            case "reset-profile":
            {
                // RECOVERY - wipe THIS identity's stored DB profile so the
                // next SOLO join re-bootstraps a clean profile from the player's
                // own save. Run on the machine whose profile is poisoned (e.g. the
                // client that got the host's items), THEN load your REAL save SOLO
                // (no co-op join) so the bootstrap re-seeds the DB cleanly.
                c.Reducers.OnResetMyProfile += (ctx) =>
                {
                    Console.WriteLine("[companion] reset_my_profile applied - DB profile WIPED. "
                        + "Now load your REAL save SOLO (no co-op) so it re-seeds clean, "
                        + "then rejoin co-op.");
                    s_done = true;
                };
                c.Reducers.ResetMyProfile();
                break;
            }

            case "list":
            {
                c.SubscriptionBuilder()
                    .OnApplied(ctx =>
                    {
                        int n = 0;
                        foreach (var l in c.Db.Lobby.Iter())
                        {
                            Console.WriteLine($"[companion] lobby {l.LobbyId} '{l.Name}' status={l.Status} addr={l.HostAddr} max={l.MaxPlayers}");
                            n++;
                        }
                        Console.WriteLine($"[companion] {n} lobbies");
                        s_done = true;
                    })
                    .Subscribe(new[] { "SELECT * FROM lobby" });
                break;
            }

            case "serve":
            {
                // GUI daemon: stay connected, publish the lobby list, and service
                // the DLL command file (host/join). Optional display name arg.
                string? sname = args.Length > 1 ? args[1] : null;
                c.SubscriptionBuilder()
                    .OnApplied(_ => Console.WriteLine("[companion] serve subscribed (lobby + my_profile + my_relay + relay)"))
                    .Subscribe(new[] { "SELECT * FROM lobby", "SELECT * FROM my_profile", "SELECT * FROM my_relay", "SELECT * FROM relay" });
                if (!string.IsNullOrEmpty(sname)) c.Reducers.RegisterAccount(sname);
                // Don't upload a profile-out left over from a previous run; only NEW
                // captures during this session should auto-upload.
                try { if (File.Exists(ProfileOutPath)) { var fi = new FileInfo(ProfileOutPath); s_profileOutSig = fi.LastWriteTimeUtc.Ticks ^ (fi.Length << 1); } } catch { }
                if (!s_autoSave) Console.WriteLine("[companion] serve: auto-save disabled (PULSAR_AUTOSAVE=0)");
                // Name-claim result → ack for the pending register command.
                c.Reducers.OnRegisterAccount += (ectx, dn) =>
                {
                    if (s_pendingRegister.Length == 0) return;
                    var rcmd = s_pendingRegister; s_pendingRegister = "";
                    bool rok = ectx.Event.Status is SpacetimeDB.Status.Committed;
                    WriteAck(rcmd, rok, rok ? null : "name taken or invalid", null);
                    Console.WriteLine($"[companion] serve: register '{dn}' -> {(rok ? "ok" : "REJECTED")}");
                };
                // Join result: the reducer no longer THROWS on wrong-password/full/locked
                // (a throw would roll back the recorded brute-force fail count). So a
                // Committed status now means only "the reducer ran" - the real outcome is
                // in the per-user RelayGrant.Status, read via ResolveAwaitedJoin. A Failed
                // status now means only lobby-not-found / not-open.
                c.Reducers.OnJoinLobby += (ectx, lid, pwh) =>
                {
                    if (s_joinCmd.Length == 0 || lid != s_joinLobby) return;
                    if (ectx.Event.Status is not SpacetimeDB.Status.Committed)
                    {
                        WriteAck(s_joinCmd, false, "lobby unavailable", null);
                        Console.WriteLine($"[companion] serve: join {lid} FAILED (not found / closed)");
                        s_joinCmd = ""; s_joinAwaitSession = false; return;
                    }
                    // Outcome (ok/denied/locked/full) is in the grant - resolve next frames.
                    s_joinAwaitSession = true; s_joinDeadline = GetMs() + 6000;
                };
                // Do NOT set s_done - Main's serve loop runs until the game exits / Q.
                break;
            }

            case "host":
            {
                if (args.Length < 4) { Console.WriteLine("usage: host <name> <addr> <max> [--launch <exe>]"); s_done = true; break; }
                string name = args[1], addr = args[2];
                uint max = uint.Parse(args[3]);
                c.Reducers.OnCreateLobby += (ctx, n, a, m, s, p, rh, mcnt, mlist) =>
                {
                    WriteHandoff("host", addr, 0, "", "");
                    Console.WriteLine($"[companion] create_lobby applied; handoff (host) -> {HandoffPath}");
                    MaybeLaunch(launch);
                    // NOTE: do NOT set s_done - the Main loop keeps the connection
                    // (and thus the lobby) alive until the game exits or Q.
                };
                c.Reducers.RegisterAccount(name);   // idempotent
                c.Reducers.CreateLobby(name, addr, max, "", "", "", 0, "");   // CLI host = direct, no password, no relay, mods unknown
                break;
            }

            case "join":
            {
                if (args.Length < 2) { Console.WriteLine("usage: join <lobbyId> [--launch <exe>]"); s_done = true; break; }
                ulong lobbyId = ulong.Parse(args[1]);
                c.SubscriptionBuilder()
                    .OnApplied(ctx =>
                    {
                        Lobby? lobby = null;
                        foreach (var l in c.Db.Lobby.Iter())
                            if (l.LobbyId == lobbyId) { lobby = l; break; }
                        if (lobby is null)
                        {
                            Console.WriteLine($"[companion] lobby {lobbyId} not found");
                            s_exit = 1; s_done = true; return;
                        }
                        var mine = FindMyProfile(c);
                        uint version = ApplyProfileDbFirst(c, mine);   // DB-first + local mirror, newest-wins
                        Console.WriteLine($"[companion] profile staged v{version} -> {ProfileInPath}");
                        c.Reducers.JoinLobby(lobbyId, "");
                        WriteHandoff("client", lobby.HostAddr, version, "", "");
                        Console.WriteLine($"[companion] join_lobby sent; handoff (client, {lobby.HostAddr}) -> {HandoffPath}");
                        MaybeLaunch(launch);
                        s_done = true;   // client may exit; the DLL drives the ENet connection
                    })
                    .Subscribe(new[] { "SELECT * FROM lobby", "SELECT * FROM my_profile" });
                break;
            }

            default:
                Console.WriteLine($"[companion] unknown command: {cmd}");
                s_exit = 1; s_done = true;
                break;
        }
    }

    // serve: publish the open-lobby list to pulsar_lobbies.json (only on change).
    private static string WriteLobbies(DbConnection c, string last)
    {
        var sb = new StringBuilder("[");
        bool first = true;
        foreach (var l in c.Db.Lobby.Iter())
        {
            if (l.Status != "open") continue;
            if (!first) sb.Append(',');
            first = false;
            sb.Append("{\"lobbyId\":").Append(l.LobbyId)
              .Append(",\"name\":").Append(JsonEsc(l.Name))
              .Append(",\"status\":").Append(JsonEsc(l.Status))
              .Append(",\"hasPassword\":").Append(l.HasPassword ? "true" : "false")
              .Append(",\"memberCount\":").Append(l.MemberCount)
              .Append(",\"code\":").Append(JsonEsc(l.Code ?? ""))
              .Append(",\"maxPlayers\":").Append(l.MaxPlayers)
              .Append(",\"modCount\":").Append(l.ModCount)
              .Append(",\"modList\":").Append(JsonEsc(l.ModList ?? ""))
              .Append('}');
        }
        sb.Append(']');
        var json = sb.ToString();
        if (json != last)
        {
            try { File.WriteAllText(LobbiesPath, json); } catch { /* best-effort */ }
        }
        return json;
    }

    // serve: publish the live relay master-list to pulsar_relays.json (only on
    // change). Mirrors WriteLobbies: same throttle/only-on-change style. The module
    // prunes stale (>60 s) relays itself; we additionally HIDE any row whose LastSeen
    // is older than 60 s here, so a slow prune never surfaces a dead relay in the GUI.
    private static string WriteRelays(DbConnection c, string last)
    {
        long nowMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var sb = new StringBuilder("[");
        bool first = true;
        foreach (var r in c.Db.Relay.Iter())
        {
            long lastSeenMs = r.LastSeen.MicrosecondsSinceUnixEpoch / 1000;
            if (nowMs - lastSeenMs > 60_000) continue;   // hide stale relays (>60 s)
            if (!first) sb.Append(',');
            first = false;
            sb.Append("{\"name\":").Append(JsonEsc(r.Name))
              .Append(",\"region\":").Append(JsonEsc(r.Region))
              .Append(",\"publicHost\":").Append(JsonEsc(r.PublicHost))
              .Append(",\"verified\":").Append(r.Verified ? "true" : "false")
              .Append(",\"sessions\":").Append(r.Sessions)
              .Append(",\"softCap\":").Append(r.SoftCap)
              .Append(",\"loadPct\":").Append(r.LoadPct)
              .Append(",\"accepting\":").Append(r.Accepting ? "true" : "false")
              .Append(",\"lastSeenUnixMs\":").Append(lastSeenMs)
              .Append('}');
        }
        sb.Append(']');
        var json = sb.ToString();
        if (json != last)
        {
            try { File.WriteAllText(RelaysPath, json); } catch { /* best-effort */ }
        }
        return json;
    }

    // serve: after a JoinLobby commit, read the per-user grant (RelayGrant.Status,
    // released via the my_relay view) and act on the outcome. "ok" → handoff + ack;
    // "denied"/"locked"/"full" → friendly rejection. Waits for the grant up to the
    // deadline. The grant for THIS join was applied in the same transaction whose
    // OnJoinLobby fired, so we never read a stale outcome.
    private static void ResolveAwaitedJoin(DbConnection c)
    {
        if (!s_joinAwaitSession) return;
        RelayGrant? grant = null;
        foreach (var r in c.Db.MyRelay.Iter())
            if (r.LobbyId == s_joinLobby) { grant = r; break; }

        if (grant is null)
        {
            if (GetMs() > s_joinDeadline)
            {
                WriteAck(s_joinCmd, false, "join timed out", null);
                Console.WriteLine($"[companion] serve: join {s_joinLobby} - grant timeout");
                s_joinAwaitSession = false; s_joinCmd = "";
            }
            return;
        }

        switch (grant.Status)
        {
            case "ok":
                if (s_joinDirectAddr.Length > 0)
                {
                    WriteHandoff("client", s_joinDirectAddr, s_joinVersion, RelayAddr, "");
                    Console.WriteLine($"[companion] serve: joined {s_joinLobby} (direct)");
                }
                else
                {
                    // Host-authoritative relay: use the HOST's relay (stored in the lobby),
                    // NOT our own pick - otherwise the session never pairs. Fall back to the
                    // env relay only if the lobby has none (older host / direct mode).
                    string relayHost = RelayAddr;
                    foreach (var l in c.Db.Lobby.Iter())
                        if (l.LobbyId == s_joinLobby)
                        {
                            if (!string.IsNullOrEmpty(l.RelayHost)) relayHost = l.RelayHost;
                            break;
                        }
                    // grant.Session carries "relaySession|cryptoSecret" (the DB delivered the
                    // per-lobby E2E auth secret alongside the relay session). Split them; the
                    // DLL uses the pure session for the relay and the secret for anti-MITM.
                    string gs = grant.Session ?? "";
                    int bar = gs.IndexOf('|');
                    string pureSession = bar >= 0 ? gs.Substring(0, bar) : gs;
                    string cryptoSecret = bar >= 0 ? gs.Substring(bar + 1) : "";
                    WriteHandoff("client", "", s_joinVersion, relayHost, pureSession, cryptoSecret);
                    Console.WriteLine($"[companion] serve: joined {s_joinLobby} (relay {relayHost}, session resolved, auth={(cryptoSecret.Length>0)})");
                }
                WriteAck(s_joinCmd, true, null, null);
                break;
            case "denied":
                WriteAck(s_joinCmd, false, "wrong password", null);
                Console.WriteLine($"[companion] serve: join {s_joinLobby} denied (wrong password)");
                break;
            case "locked":
                WriteAck(s_joinCmd, false, "too many attempts - wait 30s and retry", null);
                Console.WriteLine($"[companion] serve: join {s_joinLobby} locked (brute-force throttle)");
                break;
            case "full":
                WriteAck(s_joinCmd, false, "lobby full", null);
                Console.WriteLine($"[companion] serve: join {s_joinLobby} full");
                break;
            default:
                if (GetMs() > s_joinDeadline)   // unknown/empty status - wait, then time out
                {
                    WriteAck(s_joinCmd, false, "join timed out", null);
                    s_joinAwaitSession = false; s_joinCmd = "";
                }
                return;
        }
        s_joinAwaitSession = false; s_joinCmd = "";
    }

    // serve: after a CreateLobby commit, the host's lobby (with its generated Code)
    // appears in the local cache; surface the code in the host ack. Times out safely.
    private static void ResolveAwaitedHost(DbConnection c)
    {
        if (s_hostCmd.Length == 0) return;
        foreach (var l in c.Db.Lobby.Iter())
            if (l.HostIdentity == s_identity && l.Status == "open")
            {
                WriteAck(s_hostCmd, true, null, null, l.Code);
                Console.WriteLine($"[companion] serve: hosted '{l.Name}' code={l.Code}");
                s_hostCmd = ""; return;
            }
        if (GetMs() > s_hostDeadline)
        {
            WriteAck(s_hostCmd, true, null, null);   // hosted, code just not visible yet
            Console.WriteLine("[companion] serve: host ack (code unresolved)");
            s_hostCmd = "";
        }
    }

    // serve: watch profile-out (written by the DLL on disconnect) and upload it.
    // version = unix seconds so the module's LWW is always monotonic regardless of source.
    private static void AutoUploadProfile(DbConnection c)
    {
        if (!s_autoSave) return;
        // Do nothing until connected (real identity): otherwise a save made before the
        // connection lands would consume the change-signature without being mirrored or
        // uploaded, and would be lost until the next save. Leaving early keeps it pending.
        if (!IdentityReady()) return;
        try
        {
            if (!File.Exists(ProfileOutPath)) return;
            var fi = new FileInfo(ProfileOutPath);
            long sig = fi.LastWriteTimeUtc.Ticks ^ (fi.Length << 1);
            if (sig == s_profileOutSig) return;
            s_profileOutSig = sig;
            var blob = new List<byte>(File.ReadAllBytes(ProfileOutPath));
            if (blob.Count == 0) return;
            uint version = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            // Local backup FIRST so an outage during the DB upload can never lose this save;
            // newest-wins reconcile on the next load pushes it up if the upload didn't land.
            WriteMirror(version, blob.ToArray());
            c.Reducers.UpsertProfile(blob, version);
            Console.WriteLine($"[companion] serve: auto-saved profile ({blob.Count} bytes) v{version}");
        }
        catch (Exception ex) { Console.WriteLine($"[companion] serve: auto-save error: {ex.Message}"); }
    }

    // serve: pick up a DLL-written command (host/join), run the reducer, ack.
    private static void HandleCommandFile(DbConnection c)
    {
        if (!File.Exists(CmdPath)) return;
        string text;
        try { text = File.ReadAllText(CmdPath); } catch { return; }
        try { File.Delete(CmdPath); } catch { /* best-effort */ }

        string cmdId = "", action = "";
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            action = root.TryGetProperty("action", out var a) ? (a.GetString() ?? "") : "";
            cmdId  = root.TryGetProperty("cmdId", out var i) ? (i.GetString() ?? "") : "";

            if (action == "host")
            {
                string name = root.TryGetProperty("name", out var n) ? (n.GetString() ?? "Host") : "Host";
                uint max = root.TryGetProperty("max", out var m) ? m.GetUInt32() : 4;
                string pwHash = root.TryGetProperty("passwordHash", out var pw) ? (pw.GetString() ?? "") : "";
                // Mod parity: the tool reads the host's active mod list and passes it here.
                // NAMES ONLY (for the browser badge + the joiner's parity check), never files.
                uint modCount = root.TryGetProperty("modCount", out var mcp) ? mcp.GetUInt32() : 0;
                string modList = root.TryGetProperty("modList", out var mlp) ? (mlp.GetString() ?? "") : "";
                // Host-authoritative relay: pick the best from the LIVE master-list; fall
                // back to the env relay only if the list is empty. The joiner reuses this
                // exact relay (stored in the lobby), so host + client always meet.
                string relayHost = PickRelay(c);
                if (relayHost.Length == 0) relayHost = RelayAddr;
                string session = relayHost.Length > 0 ? NewSessionHex() : "";
                // E2E relay-auth secret (anti-MITM): one per relay lobby, delivered to both
                // peers via the DB. Carried in the relaySession arg as "session|secret" so no
                // reducer-signature / client-binding change is needed; the DB passes it
                // through opaquely (the relay never sees the DB channel), the joiner splits it.
                string cryptoSecret = session.Length > 0 ? NewSecretHex() : "";
                string sessionArg = cryptoSecret.Length > 0 ? session + "|" + cryptoSecret : session;
                string lanAddr = GuessLanAddr();                      // local, for the direct/LAN handoff only
                string pubAddr = session.Length > 0 ? "" : lanAddr;   // never publish an IP in relay mode
                // NOTE: do NOT RegisterAccount here - `name` is the LOBBY name; the
                // player's own name was already claimed via the register cmd. Calling
                // RegisterAccount(lobbyName) would rename the player to the lobby name.
                c.Reducers.CreateLobby(name, pubAddr, max, sessionArg, pwHash, relayHost, modCount, modList);
                WriteHandoff("host", lanAddr, 0, relayHost, session, cryptoSecret);
                s_hostCmd = cmdId; s_hostDeadline = GetMs() + 6000;   // ack written by ResolveAwaitedHost (with code)
                Console.WriteLine($"[companion] serve: host requested '{name}' pw={pwHash.Length>0} relay={relayHost}");
            }
            else if (action == "join")
            {
                ulong lobbyId = root.TryGetProperty("lobbyId", out var li) ? li.GetUInt64() : 0;
                string pwHash = root.TryGetProperty("passwordHash", out var jp) ? (jp.GetString() ?? "") : "";
                string addr = "";
                foreach (var l in c.Db.Lobby.Iter())
                    if (l.LobbyId == lobbyId) { addr = l.HostAddr ?? ""; break; }
                var mine = FindMyProfile(c);
                uint version = ApplyProfileDbFirst(c, mine);   // DB-first + local mirror, newest-wins
                // async: ack written by OnJoinLobby (fail) / ResolveAwaitedJoin (relay session)
                s_joinCmd = cmdId; s_joinLobby = lobbyId; s_joinDirectAddr = addr; s_joinVersion = version;
                s_joinAwaitSession = false;
                c.Reducers.JoinLobby(lobbyId, pwHash);
                Console.WriteLine($"[companion] serve: join requested {lobbyId} pw={pwHash.Length>0}");
            }
            else if (action == "joinCode")
            {
                string code = (root.TryGetProperty("code", out var cc) ? (cc.GetString() ?? "") : "")
                    .Trim().ToUpperInvariant();
                string pwHash = root.TryGetProperty("passwordHash", out var jc) ? (jc.GetString() ?? "") : "";
                ulong lobbyId = 0; string addr = "";
                foreach (var l in c.Db.Lobby.Iter())
                    if (l.Status == "open" && (l.Code ?? "").ToUpperInvariant() == code)
                    { lobbyId = l.LobbyId; addr = l.HostAddr ?? ""; break; }
                if (lobbyId == 0)
                {
                    WriteAck(cmdId, false, "code not found", null);
                    Console.WriteLine($"[companion] serve: joinCode '{code}' not found");
                }
                else
                {
                    var mine = FindMyProfile(c); uint version = ApplyProfileDbFirst(c, mine);   // DB-first + local mirror, newest-wins
                    s_joinCmd = cmdId; s_joinLobby = lobbyId; s_joinDirectAddr = addr; s_joinVersion = version;
                    s_joinAwaitSession = false;
                    c.Reducers.JoinLobby(lobbyId, pwHash);   // reuses OnJoinLobby + ResolveAwaitedJoin
                    Console.WriteLine($"[companion] serve: joinCode '{code}' -> lobby {lobbyId}");
                }
            }
            else if (action == "hostDirect")
            {
                // Self-host, pure direct: no DB lobby, no relay, no session. The DLL
                // StartHosts on its game port; the friend connects straight to the host's
                // IP:port (the tool shows the reachable IP as the invite). Ack immediately.
                string lanAddr = GuessLanAddr();
                WriteHandoff("host", lanAddr, 0, "", "");
                WriteAck(cmdId, true, null, lanAddr);
                Console.WriteLine($"[companion] serve: host-direct -> handoff (direct, {lanAddr})");
            }
            else if (action == "joinDirect")
            {
                // Join by IP: connect straight to host_addr, no relay/session, no DB lobby.
                // Still stage the joiner's OWN profile from the connected DB (online or local).
                string addr = (root.TryGetProperty("addr", out var da) ? (da.GetString() ?? "") : "").Trim();
                if (addr.Length == 0 || !addr.Contains(':'))
                {
                    WriteAck(cmdId, false, "bad address", null);
                    Console.WriteLine($"[companion] serve: join-direct bad addr '{addr}'");
                }
                else
                {
                    var mine = FindMyProfile(c); uint version = ApplyProfileDbFirst(c, mine);   // DB-first + local mirror, newest-wins
                    WriteHandoff("client", addr, version, "", "");
                    WriteAck(cmdId, true, null, addr);
                    Console.WriteLine($"[companion] serve: join-direct -> {addr} (profile staged={mine is not null})");
                }
            }
            else if (action == "register")
            {
                string name = root.TryGetProperty("name", out var nm) ? (nm.GetString() ?? "") : "";
                s_pendingRegister = cmdId;             // ack written by OnRegisterAccount
                c.Reducers.RegisterAccount(name);
                Console.WriteLine($"[companion] serve: register requested '{name}'");
            }
            else if (action == "close")
            {
                c.Reducers.CloseLobby();
                WriteAck(cmdId, true, null, null);
                Console.WriteLine("[companion] serve: closed hosted lobby");
            }
            else if (action == "resetProfile")
            {
                // Character wipe (GUI button): delete THIS identity's DB profile + the
                // local mirror so the next solo load re-bootstraps from the save (the
                // recovery flow). Optimistic ack: a self-delete reducer doesn't fail;
                // it applies on the next FrameTick.
                c.Reducers.ResetMyProfile();
                try { var mp = ProfileMirrorPath; if (File.Exists(mp)) File.Delete(mp); } catch { /* best-effort */ }
                WriteAck(cmdId, true, null, null);
                Console.WriteLine("[companion] serve: resetProfile - DB profile WIPED + local mirror cleared");
            }
            else
            {
                WriteAck(cmdId, false, "unknown action: " + action, null);
            }
        }
        catch (Exception ex)
        {
            WriteAck(cmdId, false, ex.Message, null);
            Console.WriteLine($"[companion] serve: command error: {ex.Message}");
        }
    }

    private static void WriteAck(string cmdId, bool ok, string? error, string? hostAddr, string? code = null)
    {
        var sb = new StringBuilder("{\"cmdId\":").Append(JsonEsc(cmdId))
            .Append(",\"ok\":").Append(ok ? "true" : "false");
        if (error is not null) sb.Append(",\"error\":").Append(JsonEsc(error));
        if (hostAddr is not null) sb.Append(",\"hostAddr\":").Append(JsonEsc(hostAddr));
        if (code is not null) sb.Append(",\"code\":").Append(JsonEsc(code));
        sb.Append('}');
        try { File.WriteAllText(AckPath, sb.ToString()); } catch { /* best-effort */ }
    }

    private static string JsonEsc(string s) =>
        "\"" + (s ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";

    private static Profile? FindMyProfile(DbConnection c)
    {
        // The my_profile view contains only the caller's own row (the profile
        // table itself is private), so the first row - if any - is ours.
        foreach (var p in c.Db.MyProfile.Iter())
            return p;
        return null;
    }

    // ── Local profile mirror: DB-first, local fallback, newest-wins ───────────────
    // The DB stays authoritative, but we mirror the profile locally WITH its version
    // (version = unix seconds = the same monotonic value the module uses for LWW). On a
    // pull we take the newest of {DB, mirror}; if the local mirror is strictly newer (a
    // save that never reached the DB) we push it up so it is never silently lost. The
    // mirror is keyed by IDENTITY so switching accounts can never cross-contaminate a
    // character (the poison class). Best-effort throughout; never throws.
    // True only once OnConnect has assigned a REAL (non-zero) identity. Before that
    // s_identity is default (all-zero), and touching the mirror would write/read an
    // all-zero-identity file: orphaned, and shared across users = the
    // cross-contamination class. An identity has at least one non-zero hex digit.
    private static bool IdentityReady()
    {
        var s = s_identity.ToString();
        if (string.IsNullOrEmpty(s)) return false;
        foreach (var ch in s)
            if (ch is (>= '1' and <= '9') or (>= 'a' and <= 'f') or (>= 'A' and <= 'F'))
                return true;
        return false;
    }

    private static string MirrorId()
    {
        var id = s_identity.ToString() ?? "";
        if (string.IsNullOrWhiteSpace(id)) return "anon";
        foreach (var ch in Path.GetInvalidFileNameChars()) id = id.Replace(ch, '_');
        return id;
    }
    private static string ProfileMirrorPath =>
        Path.Combine(LobbyDir(), $"pulsar_profile_mirror_{MirrorId()}.json");

    private static void WriteMirror(uint version, byte[] blob)
    {
        if (blob == null || blob.Length == 0) return;
        if (!IdentityReady()) return;   // never write under the all-zero identity
        try
        {
            // Never move the backup backwards: an explicit pull or a stale write must
            // not clobber a newer local save that has not reached the DB yet.
            if (TryReadMirror(out var curVer, out _) && version < curVer) return;

            var json = JsonSerializer.Serialize(new { version, blob = Convert.ToBase64String(blob) });
            // Atomic replace (temp + rename) so a crash mid-write can never corrupt the
            // last-resort backup. NTFS rename on the same volume is atomic.
            var path = ProfileMirrorPath;
            var tmp  = path + ".tmp";
            File.WriteAllText(tmp, json);
            File.Move(tmp, path, overwrite: true);
        }
        catch { /* best-effort */ }
    }

    private static bool TryReadMirror(out uint version, out byte[] blob)
    {
        version = 0; blob = Array.Empty<byte>();
        if (!IdentityReady()) return false;   // no real identity yet -> no per-user mirror
        try
        {
            if (!File.Exists(ProfileMirrorPath)) return false;
            using var doc = JsonDocument.Parse(File.ReadAllText(ProfileMirrorPath));
            var r = doc.RootElement;
            version = r.TryGetProperty("version", out var v) ? v.GetUInt32() : 0;
            var b64 = r.TryGetProperty("blob", out var bb) ? (bb.GetString() ?? "") : "";
            if (b64.Length == 0) return false;
            blob = Convert.FromBase64String(b64);
            return blob.Length > 0;
        }
        catch { version = 0; blob = Array.Empty<byte>(); return false; }
    }

    // Resolve the profile to load (-> profile-in), DB-first with local-mirror fallback +
    // newest-wins. Returns the version written (0 = nothing available anywhere).
    private static uint ApplyProfileDbFirst(DbConnection c, Profile? mine)
    {
        bool haveMirror = TryReadMirror(out var mVer, out var mBlob);

        if (mine is not null)
        {
            // Local mirror strictly newer than the DB = a save that never reached the DB.
            // Push it up (module LWW keeps the newer one) and load it.
            if (haveMirror && mVer > mine.Version)
            {
                try { c.Reducers.UpsertProfile(new List<byte>(mBlob), mVer); } catch { /* offline */ }
                File.WriteAllBytes(ProfileInPath, mBlob);
                WriteMirror(mVer, mBlob);
                Console.WriteLine($"[companion] local mirror newer (v{mVer} > DB v{mine.Version}) - pushed up + loaded local");
                return mVer;
            }
            var dbBlob = mine.Blob.ToArray();
            File.WriteAllBytes(ProfileInPath, dbBlob);
            WriteMirror(mine.Version, dbBlob);
            return mine.Version;
        }

        // No DB profile (none stored, or DB unreachable) - fall back to the local mirror
        // so the player can still load their last character.
        if (haveMirror)
        {
            File.WriteAllBytes(ProfileInPath, mBlob);
            Console.WriteLine($"[companion] DB profile unavailable - loaded LOCAL mirror v{mVer} ({mBlob.Length} bytes)");
            return mVer;
        }
        return 0;
    }

    private static void WriteHandoff(string role, string hostAddr, uint version,
                                     string relayAddr, string relaySession, string cryptoSecret = "")
    {
        string json =
            "{\n" +
            $"  \"role\": \"{role}\",\n" +
            $"  \"host_addr\": \"{hostAddr}\",\n" +
            $"  \"relay_addr\": \"{relayAddr}\",\n" +
            $"  \"relay_session\": \"{relaySession}\",\n" +
            $"  \"crypto_secret\": \"{cryptoSecret}\",\n" +
            $"  \"version\": {version}\n" +
            "}\n";
        File.WriteAllText(HandoffPath, json);
    }
}
