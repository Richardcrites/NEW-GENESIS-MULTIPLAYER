using System;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace Pulsar.Services;

// Address helpers for self-hosting. The relay already binds 0.0.0.0; the work here is
// telling the joiner the host's REACHABLE address (LAN IP for same-network friends, the
// public IP for internet friends) instead of 127.0.0.1, plus formatting/parsing the
// direct-IP invite. SpacetimeDB stays on localhost and is never advertised.
public static class NetworkAddress
{
    public const string InvitePrefix = "pulsar://join/";

    // ── LAN IP ────────────────────────────────────────────────────────────────────
    // The host's primary private IPv4. Prefers an Up, non-loopback, non-virtual adapter
    // that has a default gateway (i.e. the one actually carrying traffic). Returns null
    // if nothing private is found (e.g. no network).
    public static string? GetLanIpv4()
    {
        try
        {
            (string ip, bool hasGateway)? best = null;
            foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (nic.OperationalStatus != OperationalStatus.Up) continue;
                if (nic.NetworkInterfaceType is NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel) continue;
                if (LooksVirtual(nic.Name) || LooksVirtual(nic.Description)) continue;

                var props = nic.GetIPProperties();
                bool hasGateway = props.GatewayAddresses
                    .Any(g => g.Address.AddressFamily == AddressFamily.InterNetwork &&
                              !g.Address.Equals(IPAddress.Any));

                foreach (var ua in props.UnicastAddresses)
                {
                    if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    var ip = ua.Address.ToString();
                    if (!IsPrivateIpv4(ua.Address)) continue;   // skips loopback + APIPA + public

                    // First private hit wins, but a gateway-bearing adapter beats a non-gateway one.
                    if (best == null || (hasGateway && !best.Value.hasGateway))
                        best = (ip, hasGateway);
                }
            }
            return best?.ip;
        }
        catch { return null; }
    }

    private static bool LooksVirtual(string s)
    {
        if (string.IsNullOrEmpty(s)) return false;
        s = s.ToLowerInvariant();
        return s.Contains("virtual") || s.Contains("vethernet") || s.Contains("vmware") ||
               s.Contains("virtualbox") || s.Contains("hyper-v") || s.Contains("loopback") ||
               s.Contains("tailscale") || s.Contains("zerotier") || s.Contains("wireguard") ||
               s.Contains("tap-") || s.Contains("docker");
    }

    private static bool IsPrivateIpv4(IPAddress addr)
    {
        var b = addr.GetAddressBytes();
        if (b.Length != 4) return false;
        if (b[0] == 10) return true;                          // 10.0.0.0/8
        if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;  // 172.16.0.0/12
        if (b[0] == 192 && b[1] == 168) return true;          // 192.168.0.0/16
        return false;                                          // public, loopback(127), APIPA(169.254) all excluded
    }

    // ── Invite string ───────────────────────────────────────────────────────────────
    public static string FormatInvite(string ip, int port) => $"{InvitePrefix}{ip}:{port}";

    // Accepts "pulsar://join/<host>:<port>" or a bare "<host>:<port>". host = IPv4 or
    // hostname. Returns false on anything malformed.
    public static bool TryParseInvite(string? text, out string host, out int port)
    {
        host = "";
        port = 0;
        if (string.IsNullOrWhiteSpace(text)) return false;

        var s = text.Trim();
        if (s.StartsWith(InvitePrefix, StringComparison.OrdinalIgnoreCase))
            s = s.Substring(InvitePrefix.Length);
        s = s.Trim().TrimEnd('/');

        int colon = s.LastIndexOf(':');
        if (colon <= 0 || colon == s.Length - 1) return false;

        var h = s.Substring(0, colon).Trim();
        var portStr = s.Substring(colon + 1).Trim();
        if (!int.TryParse(portStr, out var p) || p < 1 || p > 65535) return false;
        if (!IsValidHost(h)) return false;

        host = h;
        port = p;
        return true;
    }

    private static bool IsValidHost(string h)
    {
        if (string.IsNullOrWhiteSpace(h)) return false;
        if (IPAddress.TryParse(h, out _)) return true;
        // Basic hostname sanity: letters/digits/dot/hyphen, must contain at least one letter.
        if (h.Length > 253) return false;
        if (!h.All(c => char.IsLetterOrDigit(c) || c == '.' || c == '-')) return false;
        return h.Any(char.IsLetter);
    }

    // ── Public IP (async, cached) ────────────────────────────────────────────────────
    private static string? _cachedPublicIp;

    public static async Task<string?> GetPublicIpAsync(CancellationToken ct = default)
    {
        if (!string.IsNullOrEmpty(_cachedPublicIp)) return _cachedPublicIp;
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(3) };
            var text = (await http.GetStringAsync("https://api.ipify.org", ct).ConfigureAwait(false)).Trim();
            if (IPAddress.TryParse(text, out var ip) && ip.AddressFamily == AddressFamily.InterNetwork)
            {
                _cachedPublicIp = text;
                return text;
            }
        }
        catch { /* offline / blocked -> null, caller shows manual entry */ }
        return null;
    }
}
