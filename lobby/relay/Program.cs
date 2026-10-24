using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using SpacetimeDB;
using SpacetimeDB.Types;

namespace PulsarRelay;

// Transparent UDP relay for PULSAR co-op. Pairs a host and a client by a 16-byte
// session and cross-forwards every non-control datagram between them, so neither
// peer needs an inbound/forwarded port. ENet runs unmodified on top.
internal static class Program
{
    // Wire protocol - MUST match include/Network/RelayProtocol.h.
    private static readonly byte[] Magic = "PULSARLY"u8.ToArray(); // 8 bytes
    private const int  SessionBytes = 16;
    private const int  ControlLen   = 8 + 1 + SessionBytes; // 25
    private const byte TypeRegister = 1;
    private const byte TypeJoin     = 2;
    private const byte TypePeer     = 3;
    private const byte TypePing     = 4;   // tool→relay liveness/latency probe (echoed as Pong)
    private const byte TypePong     = 5;   // relay→tool reply (same nonce)
    private const long SessionTtlMs = 10 * 60 * 1000;
    // SECURITY (F1) - unpaired/half-open sessions die fast so a control-packet flood
    // (spoofable source, 1 packet per distinct session key) can't lock the 4096-slot
    // table for the full 10-min TTL. Keyed off CreatedAt (NOT LastSeen) so re-sending
    // control can't keep a half-open slot alive. A real host→client handshake pairs in
    // seconds, so 60 s is generous. Paired/forwarding sessions keep the full TTL.
    private const long UnpairedTtlMs = 60_000;
    private const int  MaxSessions  = 4096;     // memory-DoS cap
    private const int  MaxUnpairedPerIp = 64;   // SECURITY - one source IP can't fill the table with half-open sessions
    private const long PinStaleMs   = 30_000;   // a slot may be taken over only after this idle (NAT rebind / restart)
    private const int  DefaultStatsPort = 7778; // internal read-only stats HTTP (monitoring)

    private const int  MaxPeerSends = 3;        // bounded PEER resends per (re)pairing - beats loss, caps reflection

    // SECURITY (F3) - per-session forward-rate cap: the relay drops a session's
    // datagrams once it exceeds this many forwarded bytes in a 1 s window. Caps a
    // single (possibly spoof-set-up) session from hogging relay bandwidth or being
    // used to reflect a high-rate flood at a victim. Generous vs real co-op traffic
    // (~tens of KB/s; the multi-MB save-bridge burst still fits in ~1 s) and ENet
    // reliable simply adapts to drops, so legit play is unaffected. 0 = unlimited.
    // Override with PULSAR_RELAY_MAXBPS.
    private static readonly long MaxBytesPerSec =
        long.TryParse(Environment.GetEnvironmentVariable("PULSAR_RELAY_MAXBPS"), out var _mbps) && _mbps >= 0
            ? _mbps : 4_000_000;

    // LOAD REPORTING - a "comfortable" concurrent-session ceiling for THIS box (the
    // single-thread forward loop). Past it the relay reports Accepting=false in its
    // heartbeat so the tool routes new sessions to a less-busy relay. This is a soft,
    // operator-tunable hint, NOT the hard MaxSessions memory cap. 0 = disable the
    // session term. Override with PULSAR_RELAY_SOFT_CAP.
    private static readonly int SoftCap =
        int.TryParse(Environment.GetEnvironmentVariable("PULSAR_RELAY_SOFT_CAP"), out var _sc) && _sc >= 0
            ? _sc : 200;

    // Optional bandwidth budget (Mbit/s of forwarded egress) factored into the load%.
    // 0 = ignore (load is session-based only). Override with PULSAR_RELAY_BW_BUDGET_MBPS.
    private static readonly long BwBudgetBps =
        long.TryParse(Environment.GetEnvironmentVariable("PULSAR_RELAY_BW_BUDGET_MBPS"), out var _bw) && _bw > 0
            ? _bw * 1_000_000L / 8 : 0;

    private sealed class Session
    {
        public IPEndPoint? Host; public IPEndPoint? Client;
        public long HostSeen; public long ClientSeen; public long LastSeen; public long CreatedAt;
        public long H2C; public long C2H;   // bytes FORWARDED through the relay each way
        public int  PeerSends;              // PEER resends spent on the current pairing (>0 ⇒ rendezvous done)
        public long RateWindowMs; public long RateBytes;   // F3: current 1 s forward-rate window
        public long LastPubTot; public long LastPubMs; public double Bps;  // self-measured relay throughput
    }

    // Latest sessions snapshot as JSON, refreshed ~1/s by the loop, served by the
    // stats thread. Reference assignment is atomic → no lock needed.
    private static volatile string _stats = "{\"sessions\":[]}";
    private static long _totalSessions, _rendezvous, _peak;   // lifetime counters

    // Live load snapshot, refreshed by the loop thread (PublishStats ~1/s) and read by
    // the registration thread for the heartbeat. Single writer + single reader; long
    // reads/writes are atomic on x64, so no lock is needed for a best-effort stat.
    private static volatile int _curSessions;
    private static long _curAggBps;

    private static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--selftest") return SelfTest();
        if (args.Length > 1 && args[0] == "--probe") return Probe(args[1]);
        int port = int.TryParse(Environment.GetEnvironmentVariable("PULSAR_RELAY_PORT"), out var p) ? p : 7777;
        Run(port);
        return 0;
    }

    private static void Run(int port)
    {
        int statsPort = int.TryParse(Environment.GetEnvironmentVariable("PULSAR_STATS_PORT"), out var sp) ? sp : DefaultStatsPort;
        new Thread(() => StatsServer(statsPort)) { IsBackground = true }.Start();
        // Phase 2: opt-in self-registration into the STDB relay master-list. Runs on
        // its OWN task and never touches the UDP path - a registration failure must
        // never take the relay down. Disabled unless PULSAR_RELAY_REGISTER=1.
        MaybeStartRegistration();
        using var sock = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        sock.Bind(new IPEndPoint(IPAddress.Any, port));
        Console.WriteLine($"[relay] listening on udp/{port}");
        Loop(sock);
    }

    // Pulled out so the self-test can run the exact same logic on a background thread.
    private static void Loop(Socket sock)
    {
        var sessions = new Dictionary<string, Session>();
        var bySrc    = new Dictionary<IPEndPoint, (string key, bool isHost)>();
        var sw = Stopwatch.StartNew();
        long lastSweep = 0, lastPublish = 0;
        var buf = new byte[65535];   // full UDP datagram - never truncate a legit large (save-bridge) packet
        EndPoint remote = new IPEndPoint(IPAddress.Any, 0);
        try { sock.ReceiveTimeout = 1000; } catch { /* ignore */ }   // wake ~1/s when idle to publish stats/sweep

        while (true)
        {
            int n = -1;
            try { n = sock.ReceiveFrom(buf, ref remote); }
            catch (SocketException) { n = -1; }   // idle-wake timeout or ICMP port-unreachable
            long now = sw.ElapsedMilliseconds;

            // SECURITY/robustness: a single malformed datagram or a transient SendTo/
            // MapToIPv4 exception must NEVER escape the loop - that would leave a
            // dead-but-running relay (process up, forwarding nothing) for everyone. Guard
            // the entire per-iteration body; log and keep forwarding.
            try
            {
            // Liveness/latency probe: the tool sends Magic+TypePing+16-byte nonce; we echo
            // it straight back as TypePong. Stateless, no session - proves the relay is
            // reachable on its REAL UDP port and lets the tool measure RTT (ICMP is blocked
            // on most VPS). Reply is the same size to the sender only → no amplification.
            if (n == ControlLen && buf[8] == TypePing && MagicMatches(buf))
            {
                buf[8] = TypePong;
                try { sock.SendTo(buf, n, SocketFlags.None, remote); } catch { /* best-effort */ }
                continue;
            }

            if (n > 0 && IsControl(buf, n, out byte type, out string key))
            {
                if (!sessions.TryGetValue(key, out var s) && sessions.Count < MaxSessions
                    && UnpairedFromIp(sessions, ((IPEndPoint)remote).Address) < MaxUnpairedPerIp)
                {
                    s = new Session { CreatedAt = now }; sessions[key] = s;
                    _totalSessions++;
                    if (sessions.Count > _peak) _peak = sessions.Count;
                }
                if (s != null)
                {
                    var src = (IPEndPoint)remote;
                    var ep = new IPEndPoint(src.Address, src.Port);
                    // Endpoint-pinning: a live slot can't be hijacked by a different endpoint;
                    // takeover only after the current one goes idle (NAT rebind / restart).
                    bool epChanged = false;
                    if (type == TypeRegister)
                    {
                        // Takeover of an idle slot is allowed only from the SAME IP (a NAT
                        // rebind keeps the IP, changes the port). A different IP that learned
                        // the session key therefore cannot hijack/redirect the pair.
                        if (s.Host == null || s.Host.Equals(ep)
                            || (now - s.HostSeen > PinStaleMs && s.Host.Address.Equals(ep.Address)))
                        {
                            if (s.Host == null || !s.Host.Equals(ep)) epChanged = true;
                            if (s.Host != null && !s.Host.Equals(ep)) bySrc.Remove(s.Host);
                            s.Host = ep; s.HostSeen = now; bySrc[ep] = (key, true);
                        }
                    }
                    else
                    {
                        if (s.Client == null || s.Client.Equals(ep)
                            || (now - s.ClientSeen > PinStaleMs && s.Client.Address.Equals(ep.Address)))
                        {
                            if (s.Client == null || !s.Client.Equals(ep)) epChanged = true;
                            if (s.Client != null && !s.Client.Equals(ep)) bySrc.Remove(s.Client);
                            s.Client = ep; s.ClientSeen = now; bySrc[ep] = (key, false);
                        }
                    }
                    // Rendezvous: once both endpoints are known, tell each peer the other's
                    // public endpoint so they can hole-punch + connect directly. Send ONLY on
                    // a (new) pairing / endpoint change, capped at MaxPeerSends, so a flood of
                    // control packets can't turn the relay into a reflection/amplification
                    // source. A fresh endpoint (NAT rebind) resets the budget.
                    if (epChanged) s.PeerSends = 0;
                    if (s.Host != null && s.Client != null && s.PeerSends < MaxPeerSends)
                    {
                        if (s.PeerSends == 0) _rendezvous++;   // count one rendezvous per pairing
                        s.PeerSends++;
                        var sessBytes = Convert.FromHexString(key);
                        sock.SendTo(PeerMsg(sessBytes, s.Client), s.Host);
                        sock.SendTo(PeerMsg(sessBytes, s.Host), s.Client);
                    }
                    s.LastSeen = now;
                }
            }
            else if (n > 0 && bySrc.TryGetValue((IPEndPoint)remote, out var owner) && sessions.TryGetValue(owner.key, out var sess))
            {
                sess.LastSeen = now;
                if (owner.isHost) sess.HostSeen = now; else sess.ClientSeen = now;
                var dst = owner.isHost ? sess.Client : sess.Host;
                if (dst != null)
                {
                    // F3: per-session forward-rate cap. Reset the window each second;
                    // drop datagrams once over budget (ENet reliable retransmits, so
                    // legit traffic just throttles; abuse/reflection bandwidth is bounded).
                    if (now - sess.RateWindowMs >= 1000) { sess.RateWindowMs = now; sess.RateBytes = 0; }
                    if (MaxBytesPerSec == 0 || sess.RateBytes + n <= MaxBytesPerSec)
                    {
                        sock.SendTo(buf, n, SocketFlags.None, dst);
                        sess.RateBytes += n;
                        if (owner.isHost) sess.H2C += n; else sess.C2H += n;   // count relayed game bytes
                    }
                    // else: over the per-session cap this window - drop.
                }
            }
            // else: idle wake, or unknown source with no control yet - drop.

            if (now - lastPublish >  1_000) { lastPublish = now; PublishStats(sessions, now); }
            if (now - lastSweep   > 15_000) { lastSweep   = now; Sweep(sessions, bySrc, now); }  // F1: sweep often enough to evict unpaired floods
            }
            catch (Exception ex) { Console.WriteLine($"[relay] loop error (ignored, keep forwarding): {ex.Message}"); }
        }
    }

    // Build a JSON snapshot of live sessions for the monitoring page. Runs on the
    // loop thread ~1/s; the stats server thread serves the latest string.
    private static void PublishStats(Dictionary<string, Session> sessions, long now)
    {
        var sb = new System.Text.StringBuilder("{\"cap\":").Append(MaxSessions)
            .Append(",\"count\":").Append(sessions.Count)
            .Append(",\"peak\":").Append(_peak)
            .Append(",\"total\":").Append(_totalSessions)
            .Append(",\"rendezvous\":").Append(_rendezvous)
            .Append(",\"sessions\":[");
        bool first = true;
        double aggBps = 0;   // sum of per-session throughput → the box's current load
        foreach (var kv in sessions)
        {
            var s = kv.Value;
            // Self-measured throughput over the (real) publish interval - robust
            // regardless of how often the monitor polls us.
            long tot = s.H2C + s.C2H, dt = now - s.LastPubMs;
            if (s.LastPubMs > 0 && dt > 0) s.Bps = (tot - s.LastPubTot) * 1000.0 / dt;
            s.LastPubTot = tot; s.LastPubMs = now;
            aggBps += s.Bps;
            // NAT hint: same public IP on both ends → same network (LAN/hairpin);
            // different IPs → a genuine cross-NAT traversal.
            string nat = (s.Host != null && s.Client != null)
                ? (s.Host.Address.Equals(s.Client.Address) ? "same" : "cross") : "";

            if (!first) sb.Append(',');
            first = false;
            sb.Append("{\"id\":\"").Append(kv.Key.Substring(0, 8))
              .Append("\",\"host\":\"").Append(FormatEp(s.Host))
              .Append("\",\"client\":\"").Append(FormatEp(s.Client))
              .Append("\",\"nat\":\"").Append(nat)
              .Append("\",\"both\":").Append(s.Host != null && s.Client != null ? "true" : "false")
              .Append(",\"peer\":").Append(s.PeerSends > 0 ? "true" : "false")
              .Append(",\"h2c\":").Append(s.H2C)
              .Append(",\"c2h\":").Append(s.C2H)
              .Append(",\"bps\":").Append((long)s.Bps)
              .Append(",\"age_ms\":").Append(now - s.LastSeen)
              .Append('}');
        }
        sb.Append("]}");
        _stats = sb.ToString();

        // Publish the load snapshot for the heartbeat (registration thread reads these).
        _curSessions = sessions.Count;
        _curAggBps   = (long)aggBps;
    }

    private static string FormatEp(IPEndPoint? ep) => ep == null ? "" : ep.Address + ":" + ep.Port;

    // Minimal read-only HTTP endpoint: any GET returns the sessions JSON. Internal
    // only (never published); the monitoring container reads it over the compose net.
    private static void StatsServer(int port)
    {
        TcpListener listener;
        try { listener = new TcpListener(IPAddress.Any, port); listener.Start(); }
        catch (Exception e) { Console.WriteLine($"[relay] stats server off: {e.Message}"); return; }
        Console.WriteLine($"[relay] stats on tcp/{port}");
        while (true)
        {
            try
            {
                using var c = listener.AcceptTcpClient();
                c.SendTimeout = 2000; c.ReceiveTimeout = 2000;  // F6: a stuck/slowloris client can't block the stats loop
                using var st = c.GetStream();
                var body = System.Text.Encoding.UTF8.GetBytes(_stats);
                var head = System.Text.Encoding.ASCII.GetBytes(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
                    "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: " +
                    body.Length + "\r\n\r\n");
                st.Write(head, 0, head.Length);
                st.Write(body, 0, body.Length);
            }
            catch { /* keep serving */ }
        }
    }

    // True if the first 8 bytes are our magic. Cheap discriminator for control/ping.
    private static bool MagicMatches(byte[] b)
    {
        for (int i = 0; i < Magic.Length; i++) if (b[i] != Magic[i]) return false;
        return true;
    }

    private static bool IsControl(byte[] b, int n, out byte type, out string key)
    {
        type = 0; key = "";
        if (n != ControlLen) return false;
        if (!MagicMatches(b)) return false;
        type = b[8];
        if (type != TypeRegister && type != TypeJoin) return false;
        key = Convert.ToHexString(b, 9, SessionBytes);
        return true;
    }

    // Count still-unpaired sessions that already own an endpoint from this IP. Used to
    // stop ONE source IP from filling the table with half-open sessions before the
    // unpaired-TTL sweep can evict them (per-IP flood cap).
    private static int UnpairedFromIp(Dictionary<string, Session> sessions, IPAddress ip)
    {
        int c = 0;
        foreach (var s in sessions.Values)
            if ((s.Host == null || s.Client == null)
                && ((s.Host != null && s.Host.Address.Equals(ip)) ||
                    (s.Client != null && s.Client.Address.Equals(ip))))
                c++;
        return c;
    }

    private static void Sweep(Dictionary<string, Session> sessions,
                              Dictionary<IPEndPoint, (string, bool)> bySrc, long now)
    {
        // Evict: (a) any session idle past the full TTL, OR (b) a still-unpaired
        // session (missing host or client) older than UnpairedTtlMs - the F1 anti-flood
        // rule, keyed off CreatedAt so refreshing control packets can't keep it alive.
        foreach (var kv in sessions.Where(kv =>
                     now - kv.Value.LastSeen > SessionTtlMs ||
                     ((kv.Value.Host == null || kv.Value.Client == null) &&
                      now - kv.Value.CreatedAt > UnpairedTtlMs)).ToList())
        {
            if (kv.Value.Host   != null) bySrc.Remove(kv.Value.Host);
            if (kv.Value.Client != null) bySrc.Remove(kv.Value.Client);
            sessions.Remove(kv.Key);
            Console.WriteLine($"[relay] session {kv.Key[..8]}… expired");
        }
    }

    private static byte[] Control(byte type, byte[] session16)
    {
        var b = new byte[ControlLen];
        Array.Copy(Magic, b, 8);
        b[8] = type;
        Array.Copy(session16, 0, b, 9, SessionBytes);
        return b;
    }

    // PEER (relay -> peer): 25-byte control + peer endpoint (4-byte IPv4 net order + 2-byte port BE).
    private static byte[] PeerMsg(byte[] session16, IPEndPoint peer)
    {
        var b = new byte[ControlLen + 6];
        Array.Copy(Magic, b, 8);
        b[8] = TypePeer;
        Array.Copy(session16, 0, b, 9, SessionBytes);
        var ip = peer.Address.MapToIPv4().GetAddressBytes(); // 4 bytes, network order
        Array.Copy(ip, 0, b, 25, 4);
        b[29] = (byte)((peer.Port >> 8) & 0xFF); // big-endian
        b[30] = (byte)(peer.Port & 0xFF);
        return b;
    }

    // ---- Phase 2: STDB relay master-list self-registration --------------------------
    //
    // Entirely additive. If PULSAR_RELAY_REGISTER!=1 we return immediately and the
    // relay behaves exactly as before. Otherwise we connect to the STDB lobby DB on a
    // background task, RegisterRelay once connected, then RelayHeartbeat every 15 s.
    // EVERYTHING here is wrapped so a failure (no DB, bad key, network blip) only logs
    // and is retried - it can never block or crash the UDP relay.
    private const int RelayHeartbeatSeconds = 15;

    private static void MaybeStartRegistration()
    {
        if ((Environment.GetEnvironmentVariable("PULSAR_RELAY_REGISTER") ?? "") != "1")
            return;

        string uri    = Environment.GetEnvironmentVariable("PULSAR_STDB_URI")        ?? "http://127.0.0.1:3000";
        string db     = Environment.GetEnvironmentVariable("PULSAR_STDB_DB")         ?? "pulsar-lobby";
        string name   = Environment.GetEnvironmentVariable("PULSAR_RELAY_NAME")      ?? "Pulsar Relay";
        string region = Environment.GetEnvironmentVariable("PULSAR_RELAY_REGION")    ?? "unknown";
        string host   = Environment.GetEnvironmentVariable("PULSAR_RELAY_PUBLIC_HOST") ?? "";
        string apiKey = Environment.GetEnvironmentVariable("PULSAR_RELAY_APIKEY")    ?? "";

        if (string.IsNullOrWhiteSpace(host))
        {
            Console.WriteLine("[relay] register: PULSAR_RELAY_PUBLIC_HOST not set - skipping master-list registration");
            return;
        }
        if (string.IsNullOrWhiteSpace(apiKey))
        {
            Console.WriteLine("[relay] register: PULSAR_RELAY_APIKEY not set - skipping master-list registration");
            return;
        }

        var t = new Thread(() => RegistrationLoop(uri, db, name, region, host, apiKey))
        {
            IsBackground = true,   // must never keep the process alive on its own
            Name = "stdb-register",
        };
        t.Start();
        Console.WriteLine($"[relay] register: master-list registration enabled (db={db} region={region} host={host})");
    }

    // Thread-safe connection state shared between the FrameTick loop and the SDK
    // callbacks (OnConnect/OnDisconnect/OnConnectError may fire on the network thread).
    private sealed class ConnState
    {
        public volatile bool Registered;   // RegisterRelay has been sent on this connection
        public volatile bool Dead;         // connection dropped or errored - tear down + reconnect
    }

    private const int ReconnectMinMs = 1_000;    // first retry delay after a drop
    private const int ReconnectMaxMs = 30_000;   // backoff ceiling

    private static void RegistrationLoop(string uri, string db, string name, string region,
                                         string host, string apiKey)
    {
        // Outer loop: the STDB server can close the WebSocket at any time (its restart, a
        // timeout, a network blip). The SDK does NOT reconnect on its own, so we rebuild the
        // whole connection on every drop, with exponential backoff. This loop only ever
        // exits if the process exits - registration must self-heal without a container restart.
        int backoffMs = ReconnectMinMs;
        while (true)
        {
            var st = new ConnState();
            DbConnection conn;
            try
            {
                conn = DbConnection.Builder()
                    .WithUri(uri)
                    .WithDatabaseName(db)
                    .OnConnect((c, identity, _) =>
                    {
                        try
                        {
                            c.Reducers.RegisterRelay(name, region, host, apiKey);
                            st.Registered = true;
                            Console.WriteLine($"[relay] register: connected ({identity}) - RegisterRelay sent");
                        }
                        catch (Exception ex) { Console.WriteLine($"[relay] register: RegisterRelay failed: {ex.Message}"); }
                    })
                    .OnConnectError(e =>
                    {
                        Console.WriteLine($"[relay] register: connect error: {e.Message}");
                        st.Dead = true;
                    })
                    .OnDisconnect((_, err) =>
                    {
                        Console.WriteLine(err is not null
                            ? $"[relay] register: disconnected: {err}"
                            : "[relay] register: disconnected");
                        st.Dead = true;
                    })
                    .Build();
            }
            catch (Exception ex)
            {
                // Build itself failed (e.g. DNS/connect refused) - back off and retry.
                Console.WriteLine($"[relay] register: connect failed: {ex.Message} - retry in {backoffMs / 1000}s");
                Thread.Sleep(backoffMs);
                backoffMs = Math.Min(ReconnectMaxMs, backoffMs * 2);
                continue;
            }

            var sw = Stopwatch.StartNew();
            long lastBeat = 0;
            // Inner loop: tick + heartbeat until the connection dies, then fall through to reconnect.
            while (!st.Dead)
            {
                try { conn.FrameTick(); }
                catch (Exception ex)
                {
                    Console.WriteLine($"[relay] register: tick error: {ex.Message}");
                    st.Dead = true;
                    break;
                }
                long now = sw.ElapsedMilliseconds;
                if (st.Registered && now - lastBeat >= RelayHeartbeatSeconds * 1000)
                {
                    lastBeat = now;
                    int  sess = _curSessions;
                    long bps  = _curAggBps;
                    int  loadPct = 0;
                    if (SoftCap > 0)     loadPct = (int)Math.Min(100, sess * 100L / SoftCap);
                    if (BwBudgetBps > 0) loadPct = Math.Max(loadPct, (int)Math.Min(100, bps * 100 / BwBudgetBps));
                    bool accepting = (SoftCap <= 0 || sess < SoftCap)
                                  && (BwBudgetBps <= 0 || bps < BwBudgetBps);
                    try { conn.Reducers.RelayHeartbeat((uint)sess, (uint)Math.Max(0, SoftCap), (uint)loadPct, accepting); }
                    catch (Exception ex) { Console.WriteLine($"[relay] register: heartbeat failed: {ex.Message}"); }
                }
                // A connection that stayed up long enough to register is healthy: reset backoff.
                if (st.Registered) backoffMs = ReconnectMinMs;
                Thread.Sleep(100);
            }

            // Connection dropped - close it cleanly and reconnect after a backoff.
            try { conn.Disconnect(); } catch { /* already dead, best-effort */ }
            Console.WriteLine($"[relay] register: reconnecting in {backoffMs / 1000}s");
            Thread.Sleep(backoffMs);
            backoffMs = Math.Min(ReconnectMaxMs, backoffMs * 2);
        }
    }

    // Self-test: run the relay on an ephemeral port, simulate host+client, and
    // assert a datagram crosses in BOTH directions. Exit 0 on PASS, 1 on FAIL.
    private static int SelfTest()
    {
        var relaySock = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        relaySock.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        var relayEp = (IPEndPoint)relaySock.LocalEndPoint!;
        new Thread(() => Loop(relaySock)) { IsBackground = true }.Start();

        var session = new byte[SessionBytes];
        for (byte i = 0; i < SessionBytes; i++) session[i] = (byte)(i + 1);

        using var host   = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        using var client = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        host.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        client.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        host.ReceiveTimeout = 2000; client.ReceiveTimeout = 2000;

        host.SendTo(Control(TypeRegister, session), relayEp);
        client.SendTo(Control(TypeJoin, session), relayEp);
        Thread.Sleep(100);   // let the relay register both endpoints

        // Both peers should receive a PEER carrying the other's endpoint (rendezvous).
        var pbuf = new byte[64];
        EndPoint pfrom = new IPEndPoint(IPAddress.Any, 0);
        try
        {
            int hpn = host.ReceiveFrom(pbuf, ref pfrom);
            if (hpn != 31 || pbuf[8] != TypePeer) { Console.WriteLine("[selftest] FAIL: host PEER malformed"); return 1; }
            int cpn = client.ReceiveFrom(pbuf, ref pfrom);
            if (cpn != 31 || pbuf[8] != TypePeer) { Console.WriteLine("[selftest] FAIL: client PEER malformed"); return 1; }
        }
        catch (SocketException) { Console.WriteLine("[selftest] FAIL: PEER not delivered"); return 1; }
        Console.WriteLine("[selftest] PEER delivered both ways");

        var hello = "HELLO-FROM-HOST"u8.ToArray();
        host.SendTo(hello, relayEp);
        var rbuf = new byte[64];
        EndPoint from = new IPEndPoint(IPAddress.Any, 0);
        try
        {
            int rn = client.ReceiveFrom(rbuf, ref from);
            if (rn != hello.Length || !rbuf.AsSpan(0, rn).SequenceEqual(hello))
            { Console.WriteLine("[selftest] FAIL: host->client payload mismatch"); return 1; }
        }
        catch (SocketException) { Console.WriteLine("[selftest] FAIL: host->client not forwarded"); return 1; }

        var back = "HELLO-FROM-CLIENT"u8.ToArray();
        client.SendTo(back, relayEp);
        try
        {
            int rn = host.ReceiveFrom(rbuf, ref from);
            if (rn != back.Length || !rbuf.AsSpan(0, rn).SequenceEqual(back))
            { Console.WriteLine("[selftest] FAIL: client->host payload mismatch"); return 1; }
        }
        catch (SocketException) { Console.WriteLine("[selftest] FAIL: client->host not forwarded"); return 1; }

        Console.WriteLine("[selftest] PASS");
        return 0;
    }

    // Probe a LIVE relay (e.g. the VPS): two local sockets register/join and we
    // assert a datagram crosses in BOTH directions through it. Proves the relay
    // is reachable + forwarding over the real network. Usage: --probe host:port
    private static int Probe(string addr)
    {
        int colon = addr.LastIndexOf(':');
        if (colon <= 0) { Console.WriteLine("[probe] usage: --probe host:port"); return 2; }
        string host = addr.Substring(0, colon);
        int port = int.Parse(addr.Substring(colon + 1));
        IPAddress ip = IPAddress.TryParse(host, out var parsed)
            ? parsed
            : Array.Find(Dns.GetHostAddresses(host), a => a.AddressFamily == AddressFamily.InterNetwork)
              ?? throw new Exception("cannot resolve " + host);
        var relayEp = new IPEndPoint(ip, port);
        Console.WriteLine($"[probe] target relay {relayEp}");

        var session = new byte[SessionBytes];
        for (byte i = 0; i < SessionBytes; i++) session[i] = (byte)(0xA0 + i);

        using var hostSock   = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        using var clientSock = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        hostSock.ReceiveTimeout = 3000; clientSock.ReceiveTimeout = 3000;

        // Register host + join client a few times (beat any loss), then test both ways.
        for (int i = 0; i < 3; i++)
        {
            hostSock.SendTo(Control(TypeRegister, session), relayEp);
            clientSock.SendTo(Control(TypeJoin, session), relayEp);
            Thread.Sleep(50);
        }

        var rbuf = new byte[64];
        EndPoint from = new IPEndPoint(IPAddress.Any, 0);

        var h2c = "PROBE-H2C"u8.ToArray();
        hostSock.SendTo(h2c, relayEp);
        try
        {
            int rn = clientSock.ReceiveFrom(rbuf, ref from);
            if (!rbuf.AsSpan(0, rn).SequenceEqual(h2c)) { Console.WriteLine("[probe] FAIL: host->client payload mismatch"); return 1; }
        }
        catch (SocketException) { Console.WriteLine("[probe] FAIL: host->client not forwarded (relay down or udp/" + port + " blocked?)"); return 1; }

        var c2h = "PROBE-C2H"u8.ToArray();
        clientSock.SendTo(c2h, relayEp);
        try
        {
            int rn = hostSock.ReceiveFrom(rbuf, ref from);
            if (!rbuf.AsSpan(0, rn).SequenceEqual(c2h)) { Console.WriteLine("[probe] FAIL: client->host payload mismatch"); return 1; }
        }
        catch (SocketException) { Console.WriteLine("[probe] FAIL: client->host not forwarded"); return 1; }

        Console.WriteLine("[probe] PASS - live relay reachable and forwarding both directions");
        return 0;
    }
}
