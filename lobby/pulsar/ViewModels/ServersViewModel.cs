using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Media;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Pulsar.Services;

namespace Pulsar.ViewModels;

// Servers / relay master-list page (Phase 2 GUI). The daemon (when serving) writes
// %LOCALAPPDATA%\PULSAR\pulsar_relays.json; the shell's DaemonBridge polls it ~1s and
// raises RelaysUpdated, which this VM mirrors into a bound ObservableCollection. Each
// row is ICMP-pinged client-side; rows sort by ping ascending (unreachable last).
public partial class ServersViewModel : ViewModelBase
{
    private readonly MainWindowViewModel _shell;

    // Background re-ping loop.
    private readonly CancellationTokenSource _cts = new();
    private const int PingTimeoutMs = 1000;
    private static readonly TimeSpan RepingInterval = TimeSpan.FromSeconds(10);

    public ServersViewModel(MainWindowViewModel shell)
    {
        _shell = shell;
        _showCommunity = shell.Config.ShowCommunityRelays;          // init without firing OnChanged
        _communityWarning = _showCommunity ? CommunityWarningText : "";
        _shell.Bridge.RelaysUpdated += list => Post(() => ApplyRelays(list));
        _ = RepingLoopAsync(_cts.Token);
    }

    // SECURITY (F4): show ONLY verified relays unless the user opts in to community
    // (unverified) ones - whose operator can observe traffic routed through them until
    // end-to-end encryption lands. The opt-in is persisted and surfaced with a warning.
    [ObservableProperty] private bool _showCommunity;
    [ObservableProperty] private string _communityWarning = "";

    private const string CommunityWarningText =
        "Community relays are UNVERIFIED - their operator can observe traffic routed " +
        "through them until end-to-end encryption lands. Only connect to relays you trust.";

    // Last raw list from the daemon, kept so toggling the filter re-applies in place.
    private IReadOnlyList<RelayInfo> _lastRaw = System.Array.Empty<RelayInfo>();

    partial void OnShowCommunityChanged(bool value)
    {
        _shell.Config.ShowCommunityRelays = value;
        _shell.SaveConfig();
        CommunityWarning = value ? CommunityWarningText : "";
        ApplyRelays(_lastRaw);   // re-filter the current list
    }

    public override string Title => "Relays";
    public override string Glyph => char.ConvertFromUtf32(0xE774); // Globe

    // The relay master-list bound in the view (kept sorted by ping).
    public ObservableCollection<ServerRowViewModel> Servers { get; } = new();

    public bool HasServers => Servers.Count > 0;

    [ObservableProperty] private ServerRowViewModel? _selectedServer;

    // Shown when the user picks a relay (reminds them DB / character is unaffected).
    [ObservableProperty] private string _connectNote = "";

    // ── relay-list sync (from the daemon's pulsar_relays.json) ───────────────────
    private void ApplyRelays(IReadOnlyList<RelayInfo> list)
    {
        _lastRaw = list;
        // SECURITY (F4): verified-only by default; community/unverified only when opted in.
        var filtered = ShowCommunity ? list : list.Where(r => r.Verified).ToList();

        // Reconcile by publicHost so we keep existing ping values for rows that stay.
        var byHost = Servers.ToDictionary(s => s.PublicHost, StringComparer.OrdinalIgnoreCase);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var r in filtered)
        {
            seen.Add(r.PublicHost);
            if (byHost.TryGetValue(r.PublicHost, out var existing))
            {
                existing.UpdateFrom(r);
            }
            else
            {
                var row = new ServerRowViewModel(r);
                Servers.Add(row);
                _ = PingRowAsync(row, _cts.Token);   // ping freshly-added rows immediately
            }
        }

        // Drop rows no longer in the published list.
        for (int i = Servers.Count - 1; i >= 0; i--)
            if (!seen.Contains(Servers[i].PublicHost))
                Servers.RemoveAt(i);

        Resort();
        OnPropertyChanged(nameof(HasServers));
    }

    // Sort by ping ascending; unreachable (no ping value) sinks to the bottom; within
    // equal-ish ping, verified relays come first.
    private void Resort()
    {
        var ordered = Servers
            .OrderBy(s => s.PingMs ?? int.MaxValue)
            .ThenByDescending(s => s.Verified)
            .ThenBy(s => s.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();

        for (int i = 0; i < ordered.Count; i++)
        {
            int cur = Servers.IndexOf(ordered[i]);
            if (cur != i) Servers.Move(cur, i);
        }
    }

    // ── client-side latency probe (background; never touches the UI thread except via Post) ──
    // Uses a real UDP PING/PONG to the relay's game port (see PingRowAsync) - accurate
    // and reachability-correct, unlike ICMP (blocked on most VPS).
    private async Task RepingLoopAsync(CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                await Task.Delay(RepingInterval, ct).ConfigureAwait(false);
                // Snapshot the current rows on the UI thread, then ping off-thread.
                ServerRowViewModel[] rows = Array.Empty<ServerRowViewModel>();
                await Dispatcher.UIThread.InvokeAsync(() => rows = Servers.ToArray());
                foreach (var row in rows)
                    await PingRowAsync(row, ct).ConfigureAwait(false);
                Post(Resort);
            }
        }
        catch (OperationCanceledException) { /* shutting down */ }
        catch { /* ping loop is best-effort */ }
    }

    // Relay protocol probe (mirrors lobby/relay Program.cs: Magic+Type+nonce).
    private static readonly byte[] RelayMagic = System.Text.Encoding.ASCII.GetBytes("PULSARLY");
    private const byte RelayTypePing = 4;
    private const byte RelayTypePong = 5;
    private const int  RelayCtrlLen  = 8 + 1 + 16;   // magic + type + 16-byte nonce

    // Latency probe over UDP to the relay's REAL game port: send Magic+Ping+nonce, the
    // relay echoes Magic+Pong+nonce, we measure RTT. This replaces ICMP - most VPS block
    // ping, so ICMP showed a misleading "timeout" for a perfectly healthy relay. No pong
    // (old relay / blocked UDP) → "-", never "timeout".
    private static async Task PingRowAsync(ServerRowViewModel row, CancellationToken ct)
    {
        if (!TryParseHostPort(row.PublicHost, out var host, out var port))
        {
            Post(() => row.SetPing(null, "-"));
            return;
        }
        try
        {
            var addrs = await Dns.GetHostAddressesAsync(host, ct).ConfigureAwait(false);
            if (addrs.Length == 0) { Post(() => row.SetPing(null, "-")); return; }
            var ep = new IPEndPoint(addrs[0], port);

            var nonce = Guid.NewGuid().ToByteArray();          // 16 bytes
            var packet = new byte[RelayCtrlLen];
            Array.Copy(RelayMagic, packet, RelayMagic.Length);
            packet[8] = RelayTypePing;
            Array.Copy(nonce, 0, packet, 9, 16);

            using var udp = new UdpClient();
            var sw = Stopwatch.StartNew();
            await udp.SendAsync(packet, packet.Length, ep).ConfigureAwait(false);

            var recv = udp.ReceiveAsync();
            var done = await Task.WhenAny(recv, Task.Delay(PingTimeoutMs, ct)).ConfigureAwait(false);
            if (done != recv) { Post(() => row.SetPing(null, "-")); return; }   // no pong in time
            var resp = (await recv.ConfigureAwait(false)).Buffer;
            sw.Stop();

            bool ok = resp.Length >= RelayCtrlLen && resp[8] == RelayTypePong
                      && MagicOk(resp) && NonceOk(resp, nonce);
            if (ok)
            {
                int ms = (int)sw.ElapsedMilliseconds;
                Post(() => row.SetPing(ms, $"{ms} ms"));
            }
            else Post(() => row.SetPing(null, "-"));
        }
        catch
        {
            Post(() => row.SetPing(null, "-"));
        }
    }

    private static bool MagicOk(byte[] b)
    {
        if (b.Length < RelayMagic.Length) return false;
        for (int i = 0; i < RelayMagic.Length; i++) if (b[i] != RelayMagic[i]) return false;
        return true;
    }

    private static bool NonceOk(byte[] resp, byte[] nonce)
    {
        if (resp.Length < 9 + nonce.Length) return false;
        for (int i = 0; i < nonce.Length; i++) if (resp[9 + i] != nonce[i]) return false;
        return true;
    }

    // Parse "host:port" (default 7777). Strips an optional scheme/path.
    private static bool TryParseHostPort(string publicHost, out string host, out int port)
    {
        host = ""; port = 7777;
        var h = (publicHost ?? "").Trim();
        if (h.Length == 0) return false;
        int scheme = h.IndexOf("://", StringComparison.Ordinal);
        if (scheme >= 0) h = h[(scheme + 3)..];
        int slash = h.IndexOf('/');
        if (slash >= 0) h = h[..slash];
        int colon = h.LastIndexOf(':');
        if (colon >= 0)
        {
            if (int.TryParse(h[(colon + 1)..], out var p) && p > 0 && p <= 65535) port = p;
            h = h[..colon];
        }
        host = h.Trim();
        return host.Length > 0;
    }

    // ── Connect: persist the chosen relay for the NEXT daemon start ──────────────
    [RelayCommand]
    private void Connect()
    {
        var row = SelectedServer;
        if (row == null) return;

        // DaemonBridge uses RelayOverride as PULSAR_RELAY_ADDR only when Advanced=true.
        _shell.Config.RelayOverride = row.PublicHost;
        _shell.Config.Advanced = true;
        _shell.SaveConfig();

        // Picking a relay does NOT change your character: the DB (Online/Local) is
        // entirely separate - relays are just interchangeable traffic brokers.
        ConnectNote = $"Relay set to '{row.Name}' ({row.PublicHost}). Your character is unchanged - the DB is separate.";
        _shell.Log($"Relay selected: {row.Name} ({row.PublicHost}).");

        if (_shell.Bridge.IsConnected)
            _shell.Log("Reconnect: stop & start daemon to apply relay.");
    }

    // Load-aware auto-pick: the best relay for a NEW session. Skips relays past their
    // soft cap (Accepting=false); prefers ones below the "busy" line, then nearest
    // (verified first), only falling back to the least-bad option when all are busy.
    // Returns null when the (already F4-filtered) list is empty → caller uses its
    // default. Called by the shell right before starting the daemon.
    private const int BusyLine = 70;   // LoadPct at/above which a relay is "busy"

    public string? PickBestHost()
    {
        var candidates = Servers.Where(s => s.Accepting).ToList();
        if (candidates.Count == 0) return null;

        var notBusy = candidates.Where(s => s.LoadPct < BusyLine).ToList();
        var pool = notBusy.Count > 0 ? notBusy : candidates;

        var best = pool
            .OrderByDescending(s => s.Verified)
            .ThenBy(s => s.PingMs ?? int.MaxValue)
            .ThenBy(s => s.LoadPct)
            .FirstOrDefault();
        return best?.PublicHost;
    }

    private static void Post(Action a)
    {
        if (Dispatcher.UIThread.CheckAccess()) a();
        else Dispatcher.UIThread.Post(a);
    }

    public void Shutdown() => _cts.Cancel();
}

// A row in the relay master-list. Mutable so the ping value can update in place while
// the row stays referenced by the bound collection.
public sealed partial class ServerRowViewModel : ObservableObject
{
    public ServerRowViewModel(RelayInfo info) => UpdateFrom(info);

    public string PublicHost { get; private set; } = "";

    [ObservableProperty] private string _name = "";
    [ObservableProperty] private string _region = "";
    [ObservableProperty] private bool _verified;

    // Self-reported load (from the relay heartbeat). Accepting defaults true so an
    // older relay that doesn't report load is still usable.
    public int Sessions { get; private set; }
    public int SoftCap { get; private set; }
    public int LoadPct { get; private set; }
    public bool Accepting { get; private set; } = true;

    // PingMs is the numeric value used for sorting (null = unreachable). Ping is the
    // display string ("12 ms" / "timeout" / "-" / "…").
    public int? PingMs { get; private set; }
    [ObservableProperty] private string _ping = "…";

    // Load shown in the browser: "FULL" past the soft cap, "12/200" with a cap, bare
    // count otherwise, "-" when nothing is reported.
    public string LoadText =>
        !Accepting ? "FULL"
        : SoftCap > 0 ? $"{Sessions}/{SoftCap}"
        : Sessions > 0 ? Sessions.ToString()
        : "-";

    // Traffic-light colour: red = full, amber = busy, green = ok, grey = no data.
    public IBrush LoadBrush =>
        !Accepting ? FullBrush
        : LoadPct >= BusyPct ? BusyBrush
        : (Sessions > 0 || SoftCap > 0) ? OkBrush
        : NoDataBrush;

    private const int BusyPct = 70;
    private static readonly IBrush OkBrush     = new SolidColorBrush(Color.FromRgb(0x4C, 0xAF, 0x50));
    private static readonly IBrush BusyBrush   = new SolidColorBrush(Color.FromRgb(0xFF, 0xB3, 0x00));
    private static readonly IBrush FullBrush   = new SolidColorBrush(Color.FromRgb(0xE5, 0x39, 0x35));
    private static readonly IBrush NoDataBrush = new SolidColorBrush(Color.FromRgb(0x9E, 0x9E, 0x9E));

    public void UpdateFrom(RelayInfo info)
    {
        PublicHost = info.PublicHost ?? "";
        Name = string.IsNullOrWhiteSpace(info.Name) ? PublicHost : info.Name;
        Region = info.Region ?? "";
        Verified = info.Verified;
        Sessions = info.Sessions;
        SoftCap = info.SoftCap;
        LoadPct = info.LoadPct;
        Accepting = info.Accepting;
        // Recompute the derived load display (plain getters, not [ObservableProperty]).
        OnPropertyChanged(nameof(LoadText));
        OnPropertyChanged(nameof(LoadBrush));
    }

    public void SetPing(int? ms, string display)
    {
        PingMs = ms;
        Ping = display;
    }
}
