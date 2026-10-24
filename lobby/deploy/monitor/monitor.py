#!/usr/bin/env python3
# PULSAR Lobby - live monitoring dashboard (stdlib only, no deps).
#
# Pulls three live signals and serves them as a self-refreshing page:
#   * Docker Engine API over the mounted unix socket -> container health,
#     CPU%, RAM and per-container network throughput (computed as a rate).
#   * SpacetimeDB HTTP /sql endpoint -> lobbies, accounts, relay grants.
#   * Relay container logs -> recent event feed.
#
# Exposed only via Caddy (basic-auth) on monitor.<DOMAIN>. The docker socket is
# mounted read-only; this is a throwaway test VPS so that exposure is accepted.

import http.client, socket, json, urllib.request, os, threading, time, re, ssl, sqlite3
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DOCKER_SOCK = os.environ.get("DOCKER_SOCK", "/var/run/docker.sock")
STDB_URL    = os.environ.get("STDB_URL", "http://spacetimedb:3000")
DB_NAME     = os.environ.get("DB_NAME", "pulsar-lobby")
CLI_TOML    = os.environ.get("CLI_TOML", "/cli.toml")
RELAY_NAME  = os.environ.get("RELAY_CONTAINER", "pulsar-relay")
RELAY_STATS = os.environ.get("RELAY_STATS_URL", "http://relay:7778/")
CERT_HOST   = os.environ.get("CERT_HOST", "")          # domain to probe for TLS expiry
HOST_ROOT   = os.environ.get("HOST_ROOT", "/host")     # host filesystem mount (disk usage)
CADDY_NAME  = os.environ.get("CADDY_CONTAINER", "pulsar-caddy")
DB_PATH     = os.environ.get("HISTORY_DB", "/data/history.db")
RETAIN_DAYS = int(os.environ.get("HISTORY_RETAIN_DAYS", "30"))
PERSIST_SEC = int(os.environ.get("HISTORY_PERSIST_SEC", "15"))   # write cadence
# Time-series columns persisted for the long-range history / filters.
HIST_COLS   = ["players", "relay_bps", "cpu", "mem", "disk", "sessions", "lobbies"]
# Module owner / publisher identity - appears in st_client as a permanent system
# connection, NOT a real player. Labelled separately in the presence panel.
OWNER_ID    = os.environ.get("OWNER_IDENTITY", "").lower()
WATCH       = [c for c in os.environ.get(
    "WATCH_CONTAINERS",
    "pulsar-stdb,pulsar-caddy,pulsar-relay,pulsar-monitor").split(",") if c]
PORT        = int(os.environ.get("PORT", "8090"))


def read_token():
    try:
        with open(CLI_TOML) as f:
            for line in f:
                m = re.search(r'spacetimedb_token\s*=\s*"([^"]+)"', line)
                if m:
                    return m.group(1)
    except Exception:
        pass
    return ""


TOKEN = read_token()
_prev = {}                 # container name -> last cpu/net sample
_lock = threading.Lock()


# --- Docker Engine API over the unix socket -------------------------------
class UnixHTTPConnection(http.client.HTTPConnection):
    def __init__(self, sock_path):
        super().__init__("localhost", timeout=6)
        self.sock_path = sock_path

    def connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(6)
        s.connect(self.sock_path)
        self.sock = s


def docker_get(path):
    c = UnixHTTPConnection(DOCKER_SOCK)
    try:
        c.request("GET", path)
        r = c.getresponse()
        return r.status, r.read()
    finally:
        c.close()


def demux(data):
    # Docker multiplexes non-TTY logs: 8-byte header [stream,0,0,0,size BE] + payload.
    out, i = [], 0
    while i + 8 <= len(data):
        size = int.from_bytes(data[i + 4:i + 8], "big")
        i += 8
        out.append(data[i:i + size].decode("utf-8", "replace"))
        i += size
    return "".join(out)


def parse_stats(j):
    cpu = j.get("cpu_stats", {}) or {}
    total = cpu.get("cpu_usage", {}).get("total_usage", 0)
    syscpu = cpu.get("system_cpu_usage", 0)
    cpus = cpu.get("online_cpus") or len(cpu.get("cpu_usage", {}).get("percpu_usage") or []) or 1
    mem = j.get("memory_stats", {}) or {}
    inactive = (mem.get("stats", {}) or {}).get("inactive_file", 0)
    mem_usage = max(0, mem.get("usage", 0) - inactive)
    mem_limit = mem.get("limit", 0)
    nets = j.get("networks") or {}
    rx = sum(n.get("rx_bytes", 0) for n in nets.values())
    tx = sum(n.get("tx_bytes", 0) for n in nets.values())
    return total, syscpu, cpus, mem_usage, mem_limit, rx, tx


def container_metrics(byname):
    out, now = [], time.time()
    for name in WATCH:
        c = byname.get(name)
        if not c:
            out.append({"name": name, "state": "missing"})
            continue
        entry = {"name": name, "state": c.get("State"), "status": c.get("Status")}
        if c.get("State") == "running":
            try:
                s, sd = docker_get(f"/containers/{c['Id']}/stats?stream=false&one-shot=true")
                if s == 200:
                    total, syscpu, cpus, memu, meml, rx, tx = parse_stats(json.loads(sd))
                    with _lock:
                        p = _prev.get(name)
                        cpu_pct = rx_rate = tx_rate = 0.0
                        if p:
                            cd, syd, dt = total - p["total"], syscpu - p["sys"], now - p["ts"]
                            if syd > 0 and cd >= 0:
                                cpu_pct = cd / syd * cpus * 100
                            if dt > 0:
                                rx_rate = max(0, rx - p["rx"]) / dt
                                tx_rate = max(0, tx - p["tx"]) / dt
                        _prev[name] = {"total": total, "sys": syscpu, "rx": rx, "tx": tx, "ts": now}
                    entry.update({
                        "cpu": round(cpu_pct, 1),
                        "mem_mb": round(memu / 1048576, 1),
                        "mem_pct": round(memu / meml * 100, 1) if meml else 0,
                        "rx_rate": rx_rate, "tx_rate": tx_rate,
                    })
            except Exception as e:
                entry["stats_error"] = str(e)
        out.append(entry)
    return out


# --- SpacetimeDB SQL ------------------------------------------------------
def stdb_sql(query):
    req = urllib.request.Request(
        f"{STDB_URL}/v1/database/{DB_NAME}/sql",
        data=query.encode(), method="POST",
        headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "text/plain"})
    with urllib.request.urlopen(req, timeout=6) as r:
        res = json.loads(r.read())
    return res[0]["rows"] if res else []


def rows(query, keys):
    return [dict(zip(keys, r)) for r in stdb_sql(query)]


def idval(v):
    # Identity-typed columns come back wrapped as ["0x…"]; raw U256 come plain.
    # Normalise both to the bare hex string.
    while isinstance(v, list) and v:
        v = v[0]
    return v


# Relayed bytes/s above which we call a session "relayed" (vs direct/idle). Game
# traffic is tens of KB/s, so anything past ~1 KB/s is clearly flowing via relay.
RELAY_ACTIVE_BPS = 1024


def relay_sessions(now):
    # Pull the relay's per-session counters + lifetime summary. The relay self-
    # measures the bytes/s flowing THROUGH it ("bps") and the NAT type ("nat").
    try:
        with urllib.request.urlopen(RELAY_STATS, timeout=4) as r:
            obj = json.loads(r.read())
    except Exception as e:
        return {"error": str(e), "list": []}
    out = []
    direct = relayed = connecting = 0
    for s in obj.get("sessions", []):
        rate = s.get("bps", 0)
        if not s.get("both"):
            mode = "connecting"; connecting += 1
        elif rate > RELAY_ACTIVE_BPS:
            mode = "relayed"; relayed += 1
        elif s.get("peer"):
            mode = "direct"; direct += 1   # rendezvous done + relay idle → peers talk directly
        else:
            mode = "idle"
        out.append({**s, "rate": rate, "mode": mode})
    # P2P success = of the currently-classified sessions, share going direct.
    decided = direct + relayed
    p2p_rate = round(direct / decided * 100) if decided else None
    return {"list": out, "cap": obj.get("cap", 0), "count": obj.get("count", 0),
            "peak": obj.get("peak", 0), "total": obj.get("total", 0),
            "rendezvous": obj.get("rendezvous", 0),
            "direct": direct, "relayed": relayed, "connecting": connecting,
            "p2p_rate": p2p_rate}


# --- security / infra helpers --------------------------------------------
def age_min(iso):
    try:
        t = datetime.fromisoformat(str(iso)).astimezone(timezone.utc)
        return (datetime.now(timezone.utc) - t).total_seconds() / 60
    except Exception:
        return None


def host_metrics():
    out = {}
    try:
        with open("/proc/loadavg") as f:
            la = f.read().split()
        out["load"] = [float(la[0]), float(la[1]), float(la[2])]
        out["cpus"] = os.cpu_count() or 1
    except Exception:
        pass
    try:
        mt = ma = 0
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):     mt = int(line.split()[1]) * 1024
                elif line.startswith("MemAvailable:"): ma = int(line.split()[1]) * 1024
        if mt:
            out["mem_total_mb"] = round(mt / 1048576)
            out["mem_used_pct"] = round((mt - ma) / mt * 100, 1)
    except Exception:
        pass
    try:
        st = os.statvfs(HOST_ROOT)
        tot, free = st.f_blocks * st.f_frsize, st.f_bavail * st.f_frsize
        out["disk_total_gb"] = round(tot / 1073741824, 1)
        out["disk_used_pct"] = round((tot - free) / tot * 100, 1) if tot else 0
    except Exception:
        pass
    return out


def docker_df():
    try:
        s, d = docker_get("/system/df")
        if s != 200:
            return {}
        j = json.loads(d)
        vols = j.get("Volumes") or []
        total = sum((v.get("UsageData") or {}).get("Size", 0) for v in vols)
        stdb = next((v for v in vols if "stdb-data" in (v.get("Name") or "")), None)
        return {
            "volumes_mb": round(max(0, total) / 1048576, 1),
            "stdb_mb": round((stdb.get("UsageData") or {}).get("Size", 0) / 1048576, 1) if stdb else None,
            "images_mb": round(sum(i.get("Size", 0) for i in (j.get("Images") or [])) / 1048576, 1),
        }
    except Exception:
        return {}


def container_health(byname):
    out = {}
    for name in WATCH:
        c = byname.get(name)
        if not c:
            continue
        try:
            s, d = docker_get(f"/containers/{c['Id']}/json")
            if s != 200:
                continue
            st = json.loads(d).get("State", {}) or {}
            out[name] = {"restarts": st.get("RestartCount", 0),
                         "oom": st.get("OOMKilled", False),
                         "health": (st.get("Health") or {}).get("Status")}
        except Exception:
            pass
    return out


_cert_cache = {"days": None, "ts": 0.0}
def cert_days():
    # SSL handshake to the public domain; read the leaf cert expiry. Cached ~10 min.
    if _cert_cache["days"] is not None and time.time() - _cert_cache["ts"] < 600:
        return _cert_cache["days"]
    if not CERT_HOST:
        return None
    try:
        ctx = ssl.create_default_context()
        with socket.create_connection((CERT_HOST, 443), timeout=5) as sk:
            with ctx.wrap_socket(sk, server_hostname=CERT_HOST) as ss:
                exp = ssl.cert_time_to_seconds(ss.getpeercert()["notAfter"])
        _cert_cache.update(days=round((exp - time.time()) / 86400, 1), ts=time.time())
    except Exception:
        _cert_cache["ts"] = time.time()   # back off on failure, keep last value
    return _cert_cache["days"]


def count_identity_reqs(byname):
    # Rough sybil signal: anonymous /identity logins seen in the recent caddy log tail.
    c = byname.get(CADDY_NAME)
    if not c:
        return None
    try:
        s, d = docker_get(f"/containers/{c['Id']}/logs?stdout=1&stderr=1&tail=500")
        return demux(d).count("/identity") if s == 200 else None
    except Exception:
        return None


def compute_alerts(p):
    a = []
    if not p.get("server", {}).get("online"):
        a.append({"lvl": "err", "msg": "SpacetimeDB nicht erreichbar"})
    lat = p.get("server", {}).get("latency_ms")
    if lat and lat > 500:
        a.append({"lvl": "warn", "msg": f"hohe DB-Latenz: {lat} ms"})
    inf = p.get("infra", {})
    disk = inf.get("host", {}).get("disk_used_pct")
    if disk is not None and disk > 85:
        a.append({"lvl": "err" if disk > 95 else "warn", "msg": f"Disk {disk}% belegt"})
    load = inf.get("host", {}).get("load")
    cpus = inf.get("host", {}).get("cpus", 1)
    if load and load[0] > cpus * 2:
        a.append({"lvl": "warn", "msg": f"hohe Host-Last: {load[0]} ({cpus} CPUs)"})
    cd = inf.get("cert_days")
    if cd is not None and cd < 14:
        a.append({"lvl": "err" if cd < 3 else "warn", "msg": f"TLS-Cert läuft in {cd} Tagen ab"})
    for n, h in (inf.get("health") or {}).items():
        if h.get("oom"):
            a.append({"lvl": "err", "msg": f"{n}: OOM-Killed"})
        if h.get("restarts", 0) > 0:
            a.append({"lvl": "warn", "msg": f"{n}: {h['restarts']} Restart(s)"})
    rl = p.get("relay", {})
    if rl.get("cap") and rl.get("count", 0) > rl["cap"] * 0.8:
        a.append({"lvl": "warn", "msg": f"Relay nahe Kapazität: {rl['count']}/{rl['cap']}"})
    sec = p.get("security", {})
    locked = sec.get("grant_status", {}).get("locked", 0)
    if locked:
        a.append({"lvl": "warn", "msg": f"{locked} Lobby-Login(s) gesperrt (Brute-Force)"})
    return a


# --- aggregate ------------------------------------------------------------
def gather():
    p = {"ts": time.time()}
    try:
        st, data = docker_get("/containers/json?all=1")
        conts = json.loads(data) if st == 200 else []
    except Exception as e:
        conts, p["docker_error"] = [], str(e)
    byname = {}
    for c in conts:
        for n in c.get("Names", []):
            byname[n.lstrip("/")] = c
    p["containers"] = container_metrics(byname)

    t0 = time.time()
    try:
        p["lobbies"] = rows(
            "SELECT lobby_id, name, status, code, member_count, max_players, "
            "has_password, host_identity, host_addr, created_at FROM lobby",
            ["id", "name", "status", "code", "members", "max", "pw", "host", "host_addr", "created"])
        for l in p["lobbies"]:
            l["host"] = idval(l["host"])
        p["server"] = {"online": True, "latency_ms": round((time.time() - t0) * 1000, 1)}
    except Exception as e:
        p["server"] = {"online": False, "error": str(e)}
        p["lobbies"] = []
    try:
        p["accounts"] = rows("SELECT identity, display_name, created_at FROM account",
                             ["identity", "name", "created"])
        for a in p["accounts"]:
            a["identity"] = idval(a["identity"])
    except Exception as e:
        p["accounts"], p["accounts_error"] = [], str(e)

    # Live presence: st_client holds the currently-connected clients (active
    # websockets). Join names from account; flag the owner/system connection.
    name_by_id = {str(a["identity"]).lower(): a["name"] for a in p.get("accounts", [])}
    try:
        clients = []
        for r in stdb_sql("SELECT identity, connection_id FROM st_client"):
            ident = str(idval(r[0])).lower()
            is_owner = bool(OWNER_ID) and ident == OWNER_ID
            clients.append({"identity": ident, "name": name_by_id.get(ident),
                            "owner": is_owner})
        p["clients"] = clients
        p["players_live"] = sum(1 for c in clients if not c["owner"])
    except Exception as e:
        p["clients"], p["players_live"], p["clients_error"] = [], 0, str(e)
    try:
        p["grants"] = rows("SELECT member, lobby_id, session, status FROM relay_grant",
                           ["member", "lobby_id", "session", "status"])
        for g in p["grants"]:
            ident = str(idval(g["member"])).lower()
            g["member"] = ident
            g["name"] = name_by_id.get(ident)
            # Never ship the raw relay session secret to the browser. It is the
            # relay access token (kept in a PRIVATE module table, released only to
            # verified members); a short prefix is enough to correlate a grant.
            s = str(g.get("session") or "")
            g["session"] = (s[:4] + "…") if s else ""
    except Exception as e:
        p["grants"], p["grants_error"] = [], str(e)

    p["relay"] = relay_sessions(p["ts"])

    # security signals
    try:
        ja = rows("SELECT lobby_id, fails, window_start FROM join_attempt",
                  ["lobby", "fails", "since"])
    except Exception:
        ja = []
    status_counts = {}
    for g in p.get("grants", []):
        status_counts[g["status"]] = status_counts.get(g["status"], 0) + 1
    recent = sum(1 for a in p.get("accounts", [])
                 if (age_min(a.get("created")) or 999) < 10)
    p["security"] = {"join_attempts": ja, "grant_status": status_counts,
                     "recent_accounts": recent, "identity_reqs": count_identity_reqs(byname)}

    # infrastructure
    p["infra"] = {"host": host_metrics(), "df": docker_df(),
                  "health": container_health(byname), "cert_days": cert_days()}

    rc = byname.get(RELAY_NAME)
    if rc:
        try:
            s, d = docker_get(f"/containers/{rc['Id']}/logs?stdout=1&stderr=1&tail=40")
            p["relay_log"] = demux(d)[-4000:] if s == 200 else ""
        except Exception as e:
            p["relay_log_error"] = str(e)
    return p


# --- persistent time-series (SQLite) for long-range history + filters -------
_db = None
_db_lock = threading.Lock()


def db_init():
    global _db
    try:
        os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    except Exception:
        pass
    _db = sqlite3.connect(DB_PATH, check_same_thread=False)
    cols = ", ".join(f"{c} REAL" for c in HIST_COLS)
    _db.execute(f"CREATE TABLE IF NOT EXISTS samples (ts INTEGER PRIMARY KEY, {cols})")
    _db.commit()


def db_insert(ts, vals):
    if _db is None:
        return
    cols, qs = ",".join(HIST_COLS), ",".join("?" for _ in HIST_COLS)
    with _db_lock:
        _db.execute(f"INSERT OR REPLACE INTO samples (ts,{cols}) VALUES (?,{qs})",
                    [int(ts)] + [vals.get(c) for c in HIST_COLS])
        _db.commit()


def db_prune():
    if _db is None:
        return
    with _db_lock:
        _db.execute("DELETE FROM samples WHERE ts < ?", (int(time.time()) - RETAIN_DAYS * 86400,))
        _db.commit()


def db_history(range_sec, points=300):
    # Downsample to ~`points` buckets so any range returns a similar payload size.
    out = {"cols": HIST_COLS, "series": {c: [] for c in HIST_COLS}, "range": range_sec}
    if _db is None:
        return out
    bucket = max(1, range_sec // points)
    out["bucket"] = bucket
    sel = ", ".join(f"AVG({c})" for c in HIST_COLS)
    with _db_lock:
        rows = _db.execute(
            f"SELECT (ts/{bucket})*{bucket} AS b, {sel} FROM samples WHERE ts >= ? "
            "GROUP BY b ORDER BY b", (int(time.time()) - range_sec,)).fetchall()
    for r in rows:
        for i, c in enumerate(HIST_COLS):
            if r[i + 1] is not None:
                out["series"][c].append([r[0], round(r[i + 1], 2)])
    return out


def db_csv(range_sec):
    header = "ts_iso,ts," + ",".join(HIST_COLS)
    if _db is None:
        return header + "\n"
    with _db_lock:
        rows = _db.execute(
            f"SELECT ts,{','.join(HIST_COLS)} FROM samples WHERE ts >= ? ORDER BY ts",
            (int(time.time()) - range_sec,)).fetchall()
    lines = [header]
    for r in rows:
        iso = datetime.fromtimestamp(r[0], timezone.utc).isoformat()
        lines.append(iso + "," + ",".join("" if v is None else str(v) for v in r))
    return "\n".join(lines) + "\n"


# --- background sampler: compute once, serve to all, persist for history ----
_latest = {"ts": 0, "server": {"online": False}}
_last_persist = 0.0
_last_prune = 0.0


def hist_values(p):
    stdb = next((c for c in p.get("containers", []) if c["name"] == "pulsar-stdb"), {})
    host = p.get("infra", {}).get("host", {})
    return {
        "players":   p.get("players_live", 0),
        "relay_bps": sum(s.get("bps", 0) for s in (p.get("relay", {}).get("list") or [])),
        "cpu":       stdb.get("cpu", 0),
        "mem":       host.get("mem_used_pct"),
        "disk":      host.get("disk_used_pct"),
        "sessions":  p.get("relay", {}).get("count", 0),
        "lobbies":   sum(1 for l in p.get("lobbies", []) if l.get("status") == "open"),
    }


def bg_loop():
    global _latest, _last_persist, _last_prune
    while True:
        try:
            p = gather()
            p["alerts"] = compute_alerts(p)
            _latest = p
            now = time.time()
            if now - _last_persist >= PERSIST_SEC:
                _last_persist = now
                db_insert(now, hist_values(p))
            if now - _last_prune >= 3600:
                _last_prune = now
                db_prune()
        except Exception as e:
            _latest = {"ts": time.time(), "server": {"online": False, "error": str(e)}}
        time.sleep(2)


# --- HTTP server ----------------------------------------------------------
PAGE = r"""<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PULSAR Lobby - Live Monitor</title>
<style>
:root{--bg:#0a0e17;--card:#121826;--card2:#1a2235;--bd:#243049;--fg:#e6edf6;
--mut:#8b9bb4;--ok:#34d399;--warn:#fbbf24;--err:#f87171;--acc:#60a5fa;--pur:#a78bfa}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
header{display:flex;align-items:center;gap:14px;padding:16px 22px;
border-bottom:1px solid var(--bd);background:linear-gradient(180deg,#0d1320,#0a0e17)}
h1{font-size:17px;margin:0;letter-spacing:.5px}
.dot{width:11px;height:11px;border-radius:50%;background:var(--mut);box-shadow:0 0 0 0 rgba(0,0,0,0)}
.dot.on{background:var(--ok);animation:pulse 2s infinite}
.dot.off{background:var(--err)}
@keyframes pulse{0%{box-shadow:0 0 0 0 rgba(52,211,153,.5)}70%{box-shadow:0 0 0 7px rgba(52,211,153,0)}100%{box-shadow:0 0 0 0 rgba(52,211,153,0)}}
.spacer{flex:1}.meta{color:var(--mut);font-size:12px}
main{padding:18px 22px;display:grid;gap:18px;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));max-width:1500px;margin:0 auto}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;overflow:hidden}
.card.wide{grid-column:1/-1}
.ch{padding:11px 16px;border-bottom:1px solid var(--bd);font-size:12px;
text-transform:uppercase;letter-spacing:1px;color:var(--mut);display:flex;gap:8px;align-items:center}
.cb{padding:6px 0}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{padding:8px 16px;text-align:left;border-bottom:1px solid #1b2436;white-space:nowrap}
th{color:var(--mut);font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:.5px}
tr:last-child td{border-bottom:none}
.tiles{display:grid;grid-template-columns:1fr 1fr;gap:1px;background:var(--bd)}
.tile{background:var(--card);padding:13px 16px}
.tile .n{color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.5px;display:flex;justify-content:space-between}
.tile .v{font-size:20px;margin-top:3px;font-weight:600}
.tile .v small{font-size:12px;color:var(--mut);font-weight:400}
.bar{height:4px;background:#1b2436;border-radius:2px;margin-top:7px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--acc);border-radius:2px;transition:width .4s}
.bar i.hi{background:var(--warn)}.bar i.crit{background:var(--err)}
.badge{padding:2px 9px;border-radius:20px;font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.5px}
.b-open{background:rgba(52,211,153,.15);color:var(--ok)}
.b-closed{background:rgba(139,155,180,.15);color:var(--mut)}
.b-relayed{background:rgba(167,139,250,.18);color:var(--pur)}
.b-direct{background:rgba(52,211,153,.18);color:var(--ok)}
.b-connecting{background:rgba(251,191,36,.18);color:var(--warn)}
.b-idle{background:rgba(139,155,180,.15);color:var(--mut)}
.b-same{background:rgba(251,191,36,.15);color:var(--warn)}
.b-cross{background:rgba(52,211,153,.15);color:var(--ok)}
#alerts{max-width:1500px;margin:0 auto;padding:0 22px}
.alert{margin-top:12px;padding:10px 14px;border-radius:10px;font-size:13px;border:1px solid;display:flex;gap:8px;align-items:center}
.alert.err{background:rgba(248,113,113,.12);border-color:rgba(248,113,113,.4);color:#fecaca}
.alert.warn{background:rgba(251,191,36,.10);border-color:rgba(251,191,36,.35);color:#fde68a}
.spark-row{display:flex;align-items:center;gap:14px;padding:11px 16px;border-bottom:1px solid #1b2436}
.spark-row:last-child{border:none}.spark-lbl{width:140px;font-size:12px}
.spark-val{width:96px;text-align:right;font-weight:600}
svg.spark{flex:1;height:38px;min-width:0}
.histctl{display:flex;flex-wrap:wrap;gap:7px;padding:12px 16px;border-bottom:1px solid var(--bd);align-items:center}
.rbtn,.mtog{cursor:pointer;user-select:none;padding:4px 11px;border-radius:7px;border:1px solid var(--bd);background:#0f1626;color:var(--mut);font-size:12px}
.rbtn:hover,.mtog:hover{border-color:#3a4a68}
.rbtn.on{background:rgba(96,165,250,.18);color:var(--acc);border-color:rgba(96,165,250,.5)}
.mtog.on{color:var(--fg)}
.mtog .sw{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:6px;vertical-align:middle}
.sep{width:1px;height:18px;background:var(--bd);margin:0 5px}
.ftog{cursor:pointer;user-select:none;padding:4px 11px;border-radius:7px;border:1px solid var(--bd);background:#0f1626;color:var(--mut);font-size:12px}
.ftog:hover{border-color:#3a4a68}
.ftog.on{color:var(--fg);background:rgba(96,165,250,.12);border-color:rgba(96,165,250,.4)}
.histctl input,.histctl select{background:#0f1626;border:1px solid var(--bd);color:var(--fg);border-radius:7px;padding:4px 9px;font:inherit;font-size:12px}
.histctl .lbl{color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.5px}
.lnk{cursor:pointer;color:var(--acc);font-size:12px;border:1px solid rgba(96,165,250,.4);padding:4px 11px;border-radius:7px}
#settings{max-width:1500px;margin:0 auto;padding:0 22px}
.setbox{margin-top:12px;background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px 16px;display:flex;flex-wrap:wrap;gap:16px;align-items:flex-end}
.setbox label{font-size:11px;color:var(--mut);display:flex;flex-direction:column;gap:4px;text-transform:uppercase;letter-spacing:.5px}
.setbox input{width:84px;background:#0f1626;border:1px solid var(--bd);color:var(--fg);border-radius:6px;padding:5px 8px;font:inherit}
.b-run{background:rgba(52,211,153,.15);color:var(--ok)}
.b-dead{background:rgba(248,113,113,.15);color:var(--err)}
.code{font-weight:700;letter-spacing:2px;color:var(--pur)}
.lock{color:var(--warn)}.muted{color:var(--mut)}
.big{display:flex;gap:26px;padding:16px}
.big .x{flex:1}.big .x .n{color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.5px}
.big .x .v{font-size:26px;font-weight:700;margin-top:2px}
.up{color:var(--acc)}.down{color:var(--pur)}
pre{margin:0;padding:14px 16px;font-size:12px;line-height:1.5;color:#aeb9cc;
max-height:240px;overflow:auto;white-space:pre-wrap;word-break:break-word}
.empty{padding:18px 16px;color:var(--mut);font-style:italic}
.pill{display:inline-block;padding:1px 8px;border-radius:6px;background:#1b2436;color:var(--acc);font-size:12px}
.flash{animation:flash .8s}@keyframes flash{0%{background:rgba(96,165,250,.18)}100%{background:transparent}}
</style></head><body>
<header>
  <span class="dot" id="dot"></span>
  <h1>PULSAR&nbsp;LOBBY <span class="muted" style="font-weight:400">live monitor</span></h1>
  <span class="spacer"></span>
  <span class="meta" id="meta">verbinde…</span>
  <span id="gear" title="Einstellungen" style="cursor:pointer;font-size:16px;margin-left:6px">⚙️</span>
</header>
<div id="settings" style="display:none"></div>
<div id="alerts"></div>
<main>
  <div class="card"><div class="ch">⬤ Server</div><div id="server"></div></div>
  <div class="card"><div class="ch">🟢 Live verbunden</div><div id="presence"></div></div>
  <div class="card"><div class="ch">📡 Relay-Traffic</div><div id="relay"></div></div>
  <div class="card"><div class="ch">🎯 P2P / Hole-Punching</div><div id="p2p"></div></div>
  <div class="card"><div class="ch">🔐 Sicherheit / Joins</div><div id="security"></div></div>
  <div class="card"><div class="ch">🛡️ Infrastruktur</div><div id="infra"></div></div>
  <div class="card wide"><div class="ch">📈 Verlauf <span id="histmeta" class="muted" style="font-weight:400;text-transform:none;letter-spacing:0"></span></div>
    <div id="histctl" class="histctl"></div>
    <div id="history"></div></div>
  <div class="card wide"><div class="ch">🐳 Container</div><div class="cb"><table id="cont"></table></div></div>
  <div class="card wide"><div class="ch">🎮 Lobbys</div><div id="lobctl" class="histctl"></div><div id="lobbies"></div></div>
  <div class="card"><div class="ch">👤 Spieler / Accounts</div><div id="accounts"></div></div>
  <div class="card"><div class="ch">🔗 Aktive Relay-Sessions</div><div id="grants"></div></div>
  <div class="card wide"><div class="ch">🛰️ Relay-Sessions / Modus (direct vs relayed)</div><div id="rsctl" class="histctl"></div><div id="rsess"></div></div>
  <div class="card wide"><div class="ch">📜 Relay-Log</div><pre id="log"></pre></div>
</main>
<script>
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));
function fmtRate(b){if(b<1024)return b.toFixed(0)+' <small>B/s</small>';
 if(b<1048576)return (b/1024).toFixed(1)+' <small>KB/s</small>';return (b/1048576).toFixed(2)+' <small>MB/s</small>';}
function short(h){if(!h)return '-';h=String(h).replace(/^0x/,'');return h.slice(0,6)+'…'+h.slice(-4);}
function ago(iso){if(!iso)return '';let t=Date.parse(String(iso).replace(/\+00:00$/,'Z'));if(isNaN(t))return '';
 let s=(Date.now()-t)/1000;if(s<60)return Math.floor(s)+'s';if(s<3600)return Math.floor(s/60)+'m';return Math.floor(s/3600)+'h';}
let lastLog='', LD=null;

// settings (alert thresholds + refresh interval) persisted in localStorage
const DEF={refresh:2000,diskWarn:85,diskCrit:95,certWarn:14,loadFactor:2,latWarn:500};
let SETTINGS=Object.assign({},DEF,JSON.parse(localStorage.getItem('pulsarMon')||'{}'));
const saveSettings=()=>localStorage.setItem('pulsarMon',JSON.stringify(SETTINGS));

// filter state for relay-sessions + lobbies (survives the 2s tick)
const RS_MODES=['direct','relayed','connecting','idle'],RS_NATS=['same','cross'],LOB_ST=['open','in_game'];
const rsState={mode:new Set(RS_MODES),nat:new Set(RS_NATS)};
const lobState={status:new Set(LOB_ST),pw:new Set(['yes','no']),occ:new Set(['free','full'])};

function renderAlerts(){
 const d=LD;if(!d)return;const A=[],inf=d.infra||{},h=inf.host||{};
 if(!(d.server&&d.server.online))A.push(['err','SpacetimeDB nicht erreichbar']);
 const lat=d.server&&d.server.latency_ms;if(lat&&lat>SETTINGS.latWarn)A.push(['warn','hohe DB-Latenz: '+lat+' ms']);
 const dk=h.disk_used_pct;if(dk!=null&&dk>SETTINGS.diskWarn)A.push([dk>SETTINGS.diskCrit?'err':'warn','Disk '+dk+'% belegt']);
 if(h.load&&h.cpus&&h.load[0]>h.cpus*SETTINGS.loadFactor)A.push(['warn','hohe Host-Last: '+h.load[0]+' ('+h.cpus+' CPUs)']);
 const cd=inf.cert_days;if(cd!=null&&cd<SETTINGS.certWarn)A.push([cd<3?'err':'warn','TLS-Cert läuft in '+Math.round(cd)+' Tagen ab']);
 const he=inf.health||{};for(const n in he){if(he[n].oom)A.push(['err',n+': OOM-Killed']);if(he[n].restarts>0)A.push(['warn',n+': '+he[n].restarts+' Restart(s)']);}
 const rl=d.relay||{};if(rl.cap&&rl.count>rl.cap*0.8)A.push(['warn','Relay nahe Kapazität: '+rl.count+'/'+rl.cap]);
 const lk=(d.security&&d.security.grant_status&&d.security.grant_status.locked)||0;if(lk)A.push(['warn',lk+' Lobby-Login(s) gesperrt (Brute-Force)']);
 $('alerts').innerHTML=A.map(a=>'<div class="alert '+a[0]+'">'+(a[0]==='err'?'⛔':'⚠️')+' '+esc(a[1])+'</div>').join('');
}

function renderLobbies(){
 const d=LD;if(!d)return;
 // Closing a lobby hard-deletes the row now, so the table only ever holds
 // open / in_game lobbies - no "closed" state to handle here.
 const L=d.lobbies||[];
 const q=($('lobq')?$('lobq').value:'').toLowerCase().trim();
 const F=L.filter(l=>lobState.status.has(l.status)&&lobState.pw.has(l.pw?'yes':'no')
   &&lobState.occ.has(l.members>=l.max?'full':'free')
   &&(!q||((l.code||'')+' '+(l.name||'')).toLowerCase().includes(q)));
 $('lobbies').innerHTML=(F.length?'<table><tr><th>Code</th><th>Name</th><th>Status</th><th>Spieler</th><th>Host</th><th>Adresse</th><th>Alter</th></tr>'+
  F.map(l=>'<tr><td><span class="code">'+esc(l.code)+'</span></td><td>'+esc(l.name)+(l.pw?' <span class="lock">🔒</span>':'')+'</td>'+
   '<td><span class="badge '+(l.status==='open'?'b-open':'b-connecting')+'">'+esc(l.status)+'</span></td>'+
   '<td>'+l.members+' / '+l.max+'</td><td class="muted">'+short(l.host)+'</td><td class="muted">'+esc(l.host_addr||'-')+'</td>'+
   '<td class="muted">'+ago(l.created)+'</td></tr>').join('')+'</table>'
  :'<div class="empty">Keine Lobby (für aktuellen Filter)</div>')+
  '<div style="padding:8px 16px;border-top:1px solid #1b2436" class="muted">'+F.length+' von '+L.length+' Lobbys angezeigt</div>';
}

function renderRsess(){
 const d=LD;if(!d)return;
 if(d.relay&&d.relay.error){$('rsess').innerHTML='<div class="empty">Relay-Stats nicht erreichbar: '+esc(d.relay.error)+'</div>';return;}
 const lbl={relayed:'⇄ RELAYED',direct:'⇢ DIRECT (P2P)',connecting:'… CONNECTING',idle:'• IDLE'};
 const all=(d.relay&&d.relay.list)||[];
 const RS=all.filter(s=>rsState.mode.has(s.mode)&&(!s.nat||rsState.nat.has(s.nat)));
 $('rsess').innerHTML=(RS.length?'<table><tr><th>Session</th><th>Modus</th><th>NAT</th><th>Host</th><th>Client</th><th>Relay-Durchsatz</th><th>Idle</th></tr>'+
   RS.map(s=>'<tr><td class="pill">'+esc(s.id)+'</td>'+
    '<td><span class="badge b-'+s.mode+'">'+(lbl[s.mode]||s.mode)+'</span></td>'+
    '<td>'+(s.nat?'<span class="badge b-'+s.nat+'">'+(s.nat==='same'?'same-LAN':'cross-NAT')+'</span>':'-')+'</td>'+
    '<td class="muted">'+esc(s.host||'-')+'</td><td class="muted">'+esc(s.client||'-')+'</td>'+
    '<td>'+fmtRate(s.rate||0)+'</td>'+
    '<td class="muted">'+((s.age_ms||0)/1000).toFixed(1)+'s</td></tr>').join('')+'</table>'+
    '<div style="padding:8px 16px;border-top:1px solid #1b2436" class="muted">'+RS.length+' von '+all.length+' Sessions · ⇄ relayed = über Server · ⇢ direct = P2P · same-LAN/cross-NAT</div>'
   :'<div class="empty">Keine Session (für aktuellen Filter)</div>');
}

const ftog=(set,val,lbl)=>'<span class="ftog'+(set.has(val)?' on':'')+'" data-v="'+val+'">'+lbl+'</span>';
function initFilters(){
 const r=$('rsctl');
 r.innerHTML='<span class="lbl">Modus</span>'+RS_MODES.map(m=>ftog(rsState.mode,m,m)).join('')+
  '<span class="sep"></span><span class="lbl">NAT</span>'+RS_NATS.map(n=>ftog(rsState.nat,n,n==='same'?'same-LAN':'cross-NAT')).join('');
 r.querySelectorAll('.ftog').forEach(b=>b.onclick=()=>{const v=b.dataset.v,set=RS_MODES.includes(v)?rsState.mode:rsState.nat;set.has(v)?set.delete(v):set.add(v);b.classList.toggle('on');renderRsess();});
 const l=$('lobctl');
 l.innerHTML='<span class="lbl">Status</span>'+LOB_ST.map(s=>ftog(lobState.status,s,s)).join('')+
  '<span class="sep"></span><span class="lbl">PW</span>'+ftog(lobState.pw,'yes','🔒 mit')+ftog(lobState.pw,'no','ohne')+
  '<span class="sep"></span><span class="lbl">Belegung</span>'+ftog(lobState.occ,'free','frei')+ftog(lobState.occ,'full','voll')+
  '<span class="sep"></span><input id="lobq" placeholder="Code/Name suchen…" style="width:160px">';
 l.querySelectorAll('.ftog').forEach(b=>b.onclick=()=>{const v=b.dataset.v;let set;if(LOB_ST.includes(v))set=lobState.status;else if(v==='yes'||v==='no')set=lobState.pw;else set=lobState.occ;set.has(v)?set.delete(v):set.add(v);b.classList.toggle('on');renderLobbies();});
 $('lobq').oninput=renderLobbies;
}

// settings panel + configurable refresh interval
let tickTimer=null;
function applyRefresh(){if(tickTimer)clearInterval(tickTimer);tickTimer=setInterval(tick,Math.max(500,SETTINGS.refresh));}
const SKEYS=['refresh','diskWarn','diskCrit','certWarn','loadFactor','latWarn'];
const SLBL={refresh:'Refresh (ms)',diskWarn:'Disk warn %',diskCrit:'Disk krit %',certWarn:'Cert warn (Tage)',loadFactor:'Last-Faktor (×CPUs)',latWarn:'DB-Latenz warn (ms)'};
function renderSettings(){
 $('settings').innerHTML='<div class="setbox">'+SKEYS.map(k=>'<label>'+SLBL[k]+'<input id="s_'+k+'" type="number" step="any" value="'+SETTINGS[k]+'"></label>').join('')+
  '<span class="lnk" id="s_save">Speichern</span><span class="lnk" id="s_reset">Standard</span></div>';
 $('s_save').onclick=()=>{SKEYS.forEach(k=>{const el=$('s_'+k);if(el)SETTINGS[k]=parseFloat(el.value)||DEF[k];});saveSettings();applyRefresh();renderAlerts();};
 $('s_reset').onclick=()=>{SETTINGS=Object.assign({},DEF);saveSettings();renderSettings();applyRefresh();renderAlerts();};
}
$('gear').onclick=()=>{const s=$('settings');const show=s.style.display==='none';s.style.display=show?'block':'none';if(show)renderSettings();};

async function tick(){
 let d;try{d=await (await fetch('api/stats',{cache:'no-store'})).json();}catch(e){
  $('dot').className='dot off';$('meta').textContent='offline - '+e;return;}
 LD=d;
 const on=d.server&&d.server.online;
 $('dot').className='dot '+(on?'on':'off');
 $('meta').textContent=new Date().toLocaleTimeString()+(on?'  •  '+d.server.latency_ms+' ms':'  •  DB nicht erreichbar');

 // server
 const openN=(d.lobbies||[]).filter(l=>l.status==='open').length;
 $('server').innerHTML='<div class="big"><div class="x"><div class="n">Status</div><div class="v" style="color:'+(on?'var(--ok)':'var(--err)')+'">'+(on?'ONLINE':'OFFLINE')+'</div></div>'+
  '<div class="x"><div class="n">DB-Latenz</div><div class="v">'+(on?d.server.latency_ms+' <small>ms</small>':'-')+'</div></div>'+
  '<div class="x"><div class="n">Spieler live</div><div class="v" style="color:'+((d.players_live||0)>0?'var(--ok)':'')+'">'+(d.players_live||0)+'</div></div>'+
  '<div class="x"><div class="n">Lobbys offen</div><div class="v">'+openN+' <small>/ '+(d.lobbies?d.lobbies.length:0)+'</small></div></div></div>'+
  (on?'':'<div class="empty">'+esc(d.server.error||'')+'</div>');

 // presence (st_client = aktive Websockets)
 const C=d.clients||[];const players=C.filter(c=>!c.owner);
 $('presence').innerHTML=(players.length?'<table><tr><th>Spieler</th><th>Identity</th></tr>'+
  players.map(c=>'<tr><td>'+(c.name?'🟢 '+esc(c.name):'<span class="muted">🟢 (ohne Name)</span>')+'</td><td class="muted">'+short(c.identity)+'</td></tr>').join('')+'</table>'
  :'<div class="empty">Gerade kein Spieler verbunden</div>')+
  (C.some(c=>c.owner)?'<div style="padding:8px 16px;border-top:1px solid #1b2436" class="muted">＋ 1 System-/Owner-Verbindung (kein Spieler)</div>':'');

 // relay traffic (sum rates of relay container)
 const rc=(d.containers||[]).find(c=>c.name.includes('relay'))||{};
 const open=(d.lobbies||[]).filter(l=>l.status==='open').length;
 const sess=(d.grants||[]).length;
 $('relay').innerHTML='<div class="big"><div class="x"><div class="n up">▲ raus</div><div class="v up">'+fmtRate(rc.tx_rate||0)+'</div></div>'+
  '<div class="x"><div class="n down">▼ rein</div><div class="v down">'+fmtRate(rc.rx_rate||0)+'</div></div>'+
  '<div class="x"><div class="n">Sessions</div><div class="v">'+sess+'</div></div></div>';

 // containers
 $('cont').innerHTML='<tr><th>Container</th><th>Status</th><th>CPU</th><th>RAM</th><th>Netz ▲/▼</th></tr>'+
  (d.containers||[]).map(c=>{
   const run=c.state==='running';
   const cpu=c.cpu||0,mem=c.mem_pct||0;
   const cls=v=>v>80?'crit':v>50?'hi':'';
   return '<tr><td>'+esc(c.name)+'</td>'+
    '<td><span class="badge '+(run?'b-run':'b-dead')+'">'+esc(c.state||'-')+'</span></td>'+
    '<td>'+(run?cpu.toFixed(1)+'% <div class="bar"><i class="'+cls(cpu)+'" style="width:'+Math.min(100,cpu)+'%"></i></div>':'-')+'</td>'+
    '<td>'+(run?(c.mem_mb||0)+' MB <div class="bar"><i class="'+cls(mem)+'" style="width:'+Math.min(100,mem)+'%"></i></div>':'-')+'</td>'+
    '<td>'+(run?'<span class="up">'+fmtRate(c.tx_rate||0)+'</span> / <span class="down">'+fmtRate(c.rx_rate||0)+'</span>':'-')+'</td></tr>';
  }).join('');

 renderLobbies();

 // accounts
 const A=d.accounts||[];
 $('accounts').innerHTML=A.length?'<table><tr><th>Name</th><th>Identity</th><th>Seit</th></tr>'+
  A.map(a=>'<tr><td>'+esc(a.name)+'</td><td class="muted">'+short(a.identity)+'</td><td class="muted">'+ago(a.created)+'</td></tr>').join('')+'</table>'
  :'<div class="empty">Keine Accounts</div>';

 // grants
 const G=d.grants||[];
 $('grants').innerHTML=G.length?'<table><tr><th>Lobby</th><th>Member</th><th>Session</th><th>Status</th></tr>'+
  G.map(g=>'<tr><td>#'+g.lobby_id+'</td><td>'+(g.name?esc(g.name):'<span class="muted">'+short(g.member)+'</span>')+'</td><td class="pill">'+esc(String(g.session).slice(0,10))+'…</td><td><span class="badge b-open">'+esc(g.status)+'</span></td></tr>').join('')+'</table>'
  :'<div class="empty">Keine aktiven Relay-Sessions</div>';

 renderRsess();

 // alerts banner (client-side, thresholds from settings)
 renderAlerts();

 // p2p / hole-punching
 const rl=d.relay||{};const p2p=rl.p2p_rate;
 $('p2p').innerHTML='<div class="big">'+
  '<div class="x"><div class="n">P2P-Quote</div><div class="v" style="color:'+(p2p>=50?'var(--ok)':(p2p==null?'':'var(--warn)'))+'">'+(p2p==null?'-':p2p+'%')+'</div></div>'+
  '<div class="x"><div class="n">direct / relayed</div><div class="v"><span style="color:var(--ok)">'+(rl.direct||0)+'</span> <small>/</small> <span style="color:var(--pur)">'+(rl.relayed||0)+'</span></div></div>'+
  '<div class="x"><div class="n">Sessions</div><div class="v">'+(rl.count||0)+' <small>/ '+(rl.cap||0)+'</small></div></div></div>'+
  '<div style="padding:8px 16px;border-top:1px solid #1b2436" class="muted">Peak '+(rl.peak||0)+' gleichzeitig · '+(rl.total||0)+' Sessions gesamt · '+(rl.rendezvous||0)+' Rendezvous</div>';

 // security / joins
 const sec=d.security||{};const gs=sec.grant_status||{};const ja=sec.join_attempts||[];
 $('security').innerHTML='<div class="big">'+
  '<div class="x"><div class="n">Joins ok / fail</div><div class="v"><span style="color:var(--ok)">'+(gs.ok||0)+'</span> <small>/</small> <span style="color:var(--err)">'+((gs.denied||0)+(gs.locked||0)+(gs.full||0))+'</span></div></div>'+
  '<div class="x"><div class="n">gesperrt</div><div class="v" style="color:'+((gs.locked||0)?'var(--err)':'')+'">'+(gs.locked||0)+'</div></div>'+
  '<div class="x"><div class="n">neue Accounts</div><div class="v">'+(sec.recent_accounts||0)+'</div></div></div>'+
  (ja.length?'<table><tr><th>Lobby</th><th>Fehlversuche</th></tr>'+ja.map(a=>'<tr><td>#'+a.lobby+'</td><td style="color:var(--warn)">'+a.fails+'</td></tr>').join('')+'</table>'
   :'<div class="empty">Keine aktiven Brute-Force-Versuche</div>');

 // infrastructure
 const inf=d.infra||{};const h=inf.host||{};const df=inf.df||{};
 const pc=(v,hi)=>v==null?'-':'<span style="color:'+(v>(hi||85)?'var(--err)':(v>60?'var(--warn)':'var(--fg)'))+'">'+v+'%</span>';
 $('infra').innerHTML='<div class="big">'+
  '<div class="x"><div class="n">Host-Last</div><div class="v">'+(h.load?h.load[0].toFixed(2):'-')+' <small>/'+(h.cpus||'?')+'</small></div></div>'+
  '<div class="x"><div class="n">RAM</div><div class="v">'+pc(h.mem_used_pct)+'</div></div>'+
  '<div class="x"><div class="n">Disk</div><div class="v">'+pc(h.disk_used_pct)+' <small>'+(h.disk_total_gb||'?')+'G</small></div></div>'+
  '<div class="x"><div class="n">TLS-Cert</div><div class="v" style="color:'+(inf.cert_days!=null&&inf.cert_days<14?'var(--warn)':'')+'">'+(inf.cert_days!=null?Math.round(inf.cert_days)+' <small>Tage</small>':'-')+'</div></div></div>'+
  '<div style="padding:8px 16px;border-top:1px solid #1b2436" class="muted">DB-Volume '+(df.stdb_mb!=null?df.stdb_mb+' MB':'-')+' · Volumes '+(df.volumes_mb||0)+' MB · Images '+(df.images_mb||0)+' MB</div>';

 // log
 const lg=d.relay_log||'';
 if(lg!==lastLog){lastLog=lg;const e=$('log');e.textContent=lg.trim()||'(leer)';e.scrollTop=e.scrollHeight;}
}
initFilters();tick();applyRefresh();

// --- history: range + metric filters, served from persistent SQLite ---------
const METRICS=[
 {k:'players',label:'Spieler',color:'#34d399',fmt:v=>(+v).toFixed(0)},
 {k:'relay_bps',label:'Relay-Durchsatz',color:'#a78bfa',fmt:fmtRate},
 {k:'cpu',label:'stdb CPU',color:'#60a5fa',fmt:v=>(+v).toFixed(1)+'%'},
 {k:'mem',label:'Host-RAM',color:'#f59e0b',fmt:v=>(+v).toFixed(0)+'%'},
 {k:'disk',label:'Disk',color:'#fb7185',fmt:v=>(+v).toFixed(1)+'%'},
 {k:'sessions',label:'Relay-Sessions',color:'#22d3ee',fmt:v=>(+v).toFixed(0)},
 {k:'lobbies',label:'Lobbys offen',color:'#a3e635',fmt:v=>(+v).toFixed(0)},
];
const RANGES=[['15m',900],['1h',3600],['6h',21600],['24h',86400],['7d',604800],['30d',2592000]];
let curRange=3600, histData=null;
const selM=new Set(['players','relay_bps','cpu']);
function initHist(){
 const ctl=$('histctl');
 ctl.innerHTML=RANGES.map(r=>'<span class="rbtn'+(r[1]===curRange?' on':'')+'" data-r="'+r[1]+'">'+r[0]+'</span>').join('')+
  '<span class="sep"></span>'+
  METRICS.map(m=>'<span class="mtog'+(selM.has(m.k)?' on':'')+'" data-m="'+m.k+'"><span class="sw" style="background:'+(selM.has(m.k)?m.color:'#333c4f')+'"></span>'+m.label+'</span>').join('')+
  '<span class="sep"></span><span class="lnk" id="histcsv">⭳ CSV</span>';
 ctl.querySelectorAll('.rbtn').forEach(b=>b.onclick=()=>{curRange=+b.dataset.r;initHist();loadHist();});
 ctl.querySelectorAll('.mtog').forEach(b=>b.onclick=()=>{const k=b.dataset.m;selM.has(k)?selM.delete(k):selM.add(k);initHist();renderHist();});
 $('histcsv').onclick=()=>window.open('api/history.csv?range='+curRange,'_blank');
}
const sparkR=(data,color)=>{
 if(!data||data.length<2)return '<span class="muted">zu wenig Daten…</span>';
 const ys=data.map(p=>p[1]),max=Math.max(...ys),min=Math.min(...ys),t0=data[0][0],t1=data[data.length-1][0],W=600,H=38,span=(t1-t0)||1;
 const pts=data.map(p=>((p[0]-t0)/span*W).toFixed(1)+','+(H-((p[1]-min)/((max-min)||1))*(H-5)-2.5).toFixed(1)).join(' ');
 return '<svg class="spark" viewBox="0 0 '+W+' '+H+'" preserveAspectRatio="none"><polyline fill="none" stroke="'+color+'" stroke-width="1.5" points="'+pts+'"/></svg>';
};
function renderHist(){
 if(!histData){$('history').innerHTML='<div class="empty">lade…</div>';return;}
 const s=histData.series||{},sel=METRICS.filter(m=>selM.has(m.k));
 $('history').innerHTML=sel.length?sel.map(m=>{
  const data=s[m.k]||[],last=data.length?data[data.length-1][1]:null;
  return '<div class="spark-row"><div class="spark-lbl" style="color:'+m.color+'">'+m.label+'</div>'+sparkR(data,m.color)+'<div class="spark-val">'+(last==null?'-':m.fmt(last))+'</div></div>';
 }).join(''):'<div class="empty">keine Metrik gewählt</div>';
 const n=Math.max(0,...METRICS.map(m=>(s[m.k]||[]).length)),rl=(RANGES.find(r=>r[1]===curRange)||[])[0]||'';
 $('histmeta').textContent=' · '+rl+' · '+n+' Punkte'+(n<2?' (sammelt noch - alle 15s ein Wert)':'');
}
async function loadHist(){
 try{histData=await (await fetch('api/history?range='+curRange,{cache:'no-store'})).json();}catch(e){histData=null;}
 renderHist();
}
initHist();loadHist();setInterval(loadHist,10000);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send(200, PAGE.encode(), "text/html; charset=utf-8")
        elif path == "/api/stats":
            # served from the background sampler - every request is cheap
            self._send(200, json.dumps(_latest).encode(), "application/json")
        elif path == "/api/history":
            q = {}
            if "?" in self.path:
                for kv in self.path.split("?", 1)[1].split("&"):
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        q[k] = v
            try:
                rng = max(60, min(RETAIN_DAYS * 86400, int(q.get("range", "3600"))))
            except ValueError:
                rng = 3600
            self._send(200, json.dumps(db_history(rng)).encode(), "application/json")
        elif path == "/api/history.csv":
            rng = 3600
            if "?" in self.path:
                m = re.search(r"range=(\d+)", self.path)
                if m:
                    rng = max(60, min(RETAIN_DAYS * 86400, int(m.group(1))))
            body = db_csv(rng).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Disposition", f'attachment; filename="pulsar-history-{rng}s.csv"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/healthz":
            self._send(200, b"ok", "text/plain")
        else:
            self._send(404, b"not found", "text/plain")


if __name__ == "__main__":
    print(f"[monitor] token={'set' if TOKEN else 'MISSING'} stdb={STDB_URL} db={DB_NAME} watching={WATCH}", flush=True)
    db_init()
    threading.Thread(target=bg_loop, daemon=True).start()
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
