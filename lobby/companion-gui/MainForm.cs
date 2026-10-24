using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Windows.Forms;

namespace PulsarCompanionGui;

// PULSAR Lobby - v2 front-end over the companion `serve` daemon (JSON protocol in
// %LOCALAPPDATA%\PULSAR). One Server field auto-derives the relay; player names are
// claimed (unique); no host IP is ever shown. Settings persist in lobbygui.json.
internal sealed class MainForm : Form
{
    private readonly string _sharedDir, _lobbiesPath, _cmdPath, _ackPath, _cfgPath;

    private Process? _daemon;
    private string _lastLobbiesJson = "";
    private string _lastAckCmdId = "";
    private string _pendingCmdId = "";
    private string _pendingKind = "";      // host | join | register
    private bool _launchAfterPending;
    private bool _nameClaimed;
    private bool _hosting;
    private bool _daemonReady;
    private bool _autoClaimDone;
    private Cfg _cfg = new();

    private sealed class Cfg
    {
        public string PlayerName { get; set; } = "Player";
        public List<string> Servers { get; set; } = new();
        public List<string> Relays { get; set; } = new();
        public string StarfieldPath { get; set; } = "";
        public bool AutoLaunch { get; set; } = true;
        public bool Advanced { get; set; } = false;
        public bool AutoSave { get; set; } = true;
    }

    private readonly record struct LobbyRow(ulong Id, bool HasPw);

    private readonly TextBox _name = new();
    private readonly Button _claim = new();
    private readonly Label _nameStatus = new();
    private readonly ComboBox _server = new();
    private readonly CheckBox _advanced = new();
    private readonly Label _relayLabel = new();
    private readonly ComboBox _relay = new();
    private readonly TextBox _starfield = new();
    private readonly Button _browse = new();
    private readonly CheckBox _launch = new();
    private readonly CheckBox _autoSave = new();
    private readonly Label    _lastSaved = new();
    private readonly Button _start = new();
    private readonly Label _status = new();
    private readonly ListView _lobbies = new();
    private readonly Button _join = new();
    private readonly TextBox _codeBox = new();
    private readonly TextBox _codePw = new();
    private readonly Button  _joinCode = new();
    private readonly Label   _hostCodeLbl = new();
    private readonly TextBox _hostCode = new();
    private readonly Button  _copyCode = new();
    private readonly TextBox _hostName = new();
    private readonly TextBox _hostPw = new();
    private readonly NumericUpDown _max = new();
    private readonly Button _host = new();
    private readonly TextBox _log = new();
    private readonly System.Windows.Forms.Timer _timer = new();

    public MainForm()
    {
        _sharedDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PULSAR");
        Directory.CreateDirectory(_sharedDir);
        _lobbiesPath = Path.Combine(_sharedDir, "pulsar_lobbies.json");
        _cmdPath     = Path.Combine(_sharedDir, "pulsar_cmd.json");
        _ackPath     = Path.Combine(_sharedDir, "pulsar_ack.json");
        _cfgPath     = Path.Combine(_sharedDir, "lobbygui.json");

        LoadCfg();
        BuildUi();
        ApplyCfgToUi();
        SetState();

        _timer.Interval = 1000;
        _timer.Tick += (_, _) => Poll();
        _timer.Start();
        FormClosing += (_, _) => StopDaemon();
        Log("Ready. Start the daemon, then claim your name.");
    }

    private void BuildUi()
    {
        Text = "PULSAR Lobby";
        try { Icon = System.Drawing.Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { /* fall back to default icon */ }
        ClientSize = new Size(700, 588);
        MinimumSize = new Size(640, 500);
        Font = new Font("Segoe UI", 9f);

        var lblName = new Label { Text = "Player name:", Location = new Point(12, 16), AutoSize = true };
        _name.Location = new Point(95, 12); _name.Width = 160; _name.PlaceholderText = "Spielername";
        _claim.Location = new Point(265, 11); _claim.Size = new Size(90, 26); _claim.Text = "Claim";
        _claim.Click += (_, _) => DoClaim();
        _nameStatus.Location = new Point(365, 16); _nameStatus.AutoSize = true; _nameStatus.Text = "not claimed";
        _nameStatus.ForeColor = Color.Gray;

        var lblServer = new Label { Text = "Server:", Location = new Point(12, 50), AutoSize = true };
        _server.Location = new Point(95, 46); _server.Width = 360; _server.Anchor = AnchorAll();
        _server.DropDownStyle = ComboBoxStyle.DropDown;
        _advanced.Location = new Point(465, 47); _advanced.AutoSize = true; _advanced.Text = "Advanced"; _advanced.Anchor = AnchorTR();
        _advanced.CheckedChanged += (_, _) => { _cfg.Advanced = _advanced.Checked; RelayVisible(_advanced.Checked); SaveCfg(); };

        _relayLabel.Text = "Relay:"; _relayLabel.Location = new Point(12, 80); _relayLabel.AutoSize = true;
        _relay.Location = new Point(95, 76); _relay.Width = 360; _relay.Anchor = AnchorAll();
        _relay.DropDownStyle = ComboBoxStyle.DropDown;

        var lblSf = new Label { Text = "Starfield:", Location = new Point(12, 112), AutoSize = true };
        _starfield.Location = new Point(95, 108); _starfield.Width = 470; _starfield.Anchor = AnchorAll();
        _browse.Location = new Point(573, 107); _browse.Size = new Size(90, 24); _browse.Text = "Browse"; _browse.Anchor = AnchorTR();
        _browse.Click += (_, _) => { using var d = new OpenFileDialog { Filter = "EXE|*.exe" }; if (d.ShowDialog(this) == DialogResult.OK) { _starfield.Text = d.FileName; SaveCfg(); } };
        _launch.Location = new Point(95, 138); _launch.AutoSize = true; _launch.Text = "Launch Starfield after host/join";
        _autoSave.Location = new Point(95, 160); _autoSave.AutoSize = true; _autoSave.Text = "Auto-save my character";
        _autoSave.CheckedChanged += (_, _) => { _cfg.AutoSave = _autoSave.Checked; SaveCfg(); Log("Auto-save " + (_autoSave.Checked ? "on" : "off") + " (applies on next daemon start)."); };
        _lastSaved.Location = new Point(290, 161); _lastSaved.AutoSize = true; _lastSaved.Text = ""; _lastSaved.ForeColor = Color.Gray;

        _start.Location = new Point(380, 134); _start.Size = new Size(130, 28); _start.Text = "Start daemon"; _start.Anchor = AnchorTR();
        _start.Click += (_, _) => ToggleDaemon();
        _status.Location = new Point(516, 140); _status.AutoSize = true; _status.Text = "disconnected"; _status.ForeColor = Color.Firebrick; _status.Anchor = AnchorTR();

        var lblList = new Label { Text = "Open lobbies:", Location = new Point(12, 172), AutoSize = true };
        _lobbies.Location = new Point(12, 192); _lobbies.Size = new Size(560, 210);
        _lobbies.View = View.Details; _lobbies.FullRowSelect = true; _lobbies.GridLines = true;
        _lobbies.MultiSelect = false; _lobbies.HideSelection = false; _lobbies.Anchor = AnchorTLBR();
        _lobbies.Columns.Add("Name", 360);
        _lobbies.Columns.Add("Players", 80);
        _lobbies.Columns.Add("Status", 90);
        _lobbies.Columns.Add("PW", 40);
        _lobbies.DoubleClick += (_, _) => DoJoin();
        _join.Location = new Point(582, 192); _join.Size = new Size(100, 30); _join.Text = "Join"; _join.Anchor = AnchorTR();
        _join.Click += (_, _) => DoJoin();

        var lblOr = new Label { Text = "or by code:", Location = new Point(582, 230), AutoSize = true, Anchor = AnchorTR() };
        _codeBox.Location = new Point(582, 250); _codeBox.Width = 100; _codeBox.CharacterCasing = CharacterCasing.Upper;
        _codeBox.PlaceholderText = "Lobby-Code";
        _codeBox.MaxLength = 8; _codeBox.Anchor = AnchorTR();
        _codePw.Location = new Point(582, 276); _codePw.Width = 100; _codePw.UseSystemPasswordChar = true;
        _codePw.PlaceholderText = "Passwort";
        _codePw.Anchor = AnchorTR();   // optional password for code-join
        _joinCode.Location = new Point(582, 302); _joinCode.Size = new Size(100, 28); _joinCode.Text = "Join code";
        _joinCode.Anchor = AnchorTR();
        _joinCode.Click += (_, _) => DoJoinByCode();

        var grp = new GroupBox { Text = "Host a new lobby", Location = new Point(12, 414), Size = new Size(676, 98) };
        grp.Anchor = AnchorBLR();
        var lblHN = new Label { Text = "Name:", Location = new Point(12, 28), AutoSize = true };
        _hostName.Location = new Point(58, 24); _hostName.Width = 150; _hostName.Text = "My Lobby";
        var lblMax = new Label { Text = "Max:", Location = new Point(220, 28), AutoSize = true };
        // Co-op is 2-player (ENet MAX_CLIENTS=1; the relay pairs exactly host+client).
        // Advertising more would let a 3rd join corrupt the pairing, so fix Max at 2
        // until N-player co-op exists. Disabled so it can't be raised.
        _max.Location = new Point(258, 24); _max.Width = 45; _max.Minimum = 2; _max.Maximum = 2; _max.Value = 2;
        _max.Enabled = false;
        var lblPw = new Label { Text = "Password:", Location = new Point(315, 28), AutoSize = true };
        _hostPw.Location = new Point(380, 24); _hostPw.Width = 110; _hostPw.UseSystemPasswordChar = true;
        _hostPw.PlaceholderText = "Passwort (optional)";
        _host.Location = new Point(500, 22); _host.Size = new Size(166, 30); _host.Text = "Host lobby";
        _host.Click += (_, _) => DoHostToggle();
        grp.Controls.AddRange(new Control[] { lblHN, _hostName, lblMax, _max, lblPw, _hostPw, _host });

        _hostCodeLbl.Text = "Invite code:"; _hostCodeLbl.Location = new Point(12, 62); _hostCodeLbl.AutoSize = true;
        _hostCode.Location = new Point(95, 58); _hostCode.Width = 110; _hostCode.ReadOnly = true;
        _hostCode.Font = new Font("Consolas", 11f, FontStyle.Bold); _hostCode.Text = "";
        _copyCode.Location = new Point(215, 57); _copyCode.Size = new Size(70, 24); _copyCode.Text = "Copy";
        _copyCode.Click += (_, _) => { if (_hostCode.Text.Length > 0) { Clipboard.SetText(_hostCode.Text); Log("Invite code copied."); } };
        _hostCodeLbl.Visible = false; _hostCode.Visible = false; _copyCode.Visible = false;
        grp.Controls.AddRange(new Control[] { _hostCodeLbl, _hostCode, _copyCode });

        _log.Location = new Point(12, 492); _log.Size = new Size(676, 60);
        _log.Multiline = true; _log.ReadOnly = true; _log.ScrollBars = ScrollBars.Vertical;
        _log.BackColor = Color.FromArgb(245, 245, 245); _log.Anchor = AnchorBLR();

        Controls.AddRange(new Control[]
        {
            lblName, _name, _claim, _nameStatus,
            lblServer, _server, _advanced, _relayLabel, _relay,
            lblSf, _starfield, _browse, _launch, _autoSave, _lastSaved, _start, _status,
            lblList, _lobbies, _join, lblOr, _codeBox, _codePw, _joinCode, grp, _log
        });
    }

    private static AnchorStyles AnchorAll()  => AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
    private static AnchorStyles AnchorTR()   => AnchorStyles.Top | AnchorStyles.Right;
    private static AnchorStyles AnchorTLBR() => AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
    private static AnchorStyles AnchorBLR()  => AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;

    // ── config ────────────────────────────────────────────────────────────────
    private void LoadCfg()
    {
        try { if (File.Exists(_cfgPath)) _cfg = JsonSerializer.Deserialize<Cfg>(File.ReadAllText(_cfgPath)) ?? new(); }
        catch { _cfg = new(); }
    }
    private void SaveCfg()
    {
        try { File.WriteAllText(_cfgPath, JsonSerializer.Serialize(_cfg, new JsonSerializerOptions { WriteIndented = true })); }
        catch { /* best-effort */ }
    }
    private void ApplyCfgToUi()
    {
        _name.Text = _cfg.PlayerName;
        _server.Items.Clear(); _server.Items.AddRange(_cfg.Servers.ToArray());
        if (_cfg.Servers.Count > 0) _server.Text = _cfg.Servers[0];
        _relay.Items.Clear(); _relay.Items.AddRange(_cfg.Relays.ToArray());
        if (_cfg.Relays.Count > 0) _relay.Text = _cfg.Relays[0];
        _starfield.Text = _cfg.StarfieldPath.Length > 0 ? _cfg.StarfieldPath : GuessStarfieldExe();
        _launch.Checked = _cfg.AutoLaunch;
        _autoSave.Checked = _cfg.AutoSave;
        _advanced.Checked = _cfg.Advanced;
        RelayVisible(_cfg.Advanced);
    }
    private void RelayVisible(bool on) { _relayLabel.Visible = on; _relay.Visible = on; }
    private static void Remember(List<string> list, string v)
    {
        v = v.Trim(); if (v.Length == 0) return;
        list.RemoveAll(x => string.Equals(x, v, StringComparison.OrdinalIgnoreCase));
        list.Insert(0, v);
        if (list.Count > 8) list.RemoveRange(8, list.Count - 8);
    }

    // ── daemon ────────────────────────────────────────────────────────────────
    private void ToggleDaemon()
    {
        if (_daemon is { HasExited: false }) { StopDaemon(); SetState(); return; }
        var exe = GuessCompanionExe();
        if (!File.Exists(exe)) { MessageBox.Show(this, "PulsarCompanion.exe not found:\n" + exe, "PULSAR Lobby"); return; }

        var serverHost = _server.Text.Trim();
        if (serverHost.Length == 0) { MessageBox.Show(this, "Enter a server (e.g. my.host or wss://my.host).", "PULSAR Lobby"); return; }
        var name = _name.Text.Trim().Length > 0 ? _name.Text.Trim() : "Player";
        var stdbUri = serverHost.Contains("://") ? serverHost : "wss://" + serverHost;
        var relay   = _advanced.Checked && _relay.Text.Trim().Length > 0 ? _relay.Text.Trim() : HostOnly(serverHost) + ":7777";

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? _sharedDir,
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
            };
            psi.ArgumentList.Add("serve");
            psi.ArgumentList.Add(name);   // ArgumentList quotes safely (name may contain spaces/quotes)
            psi.Environment["PULSAR_STDB_URI"]   = stdbUri;
            psi.Environment["PULSAR_RELAY_ADDR"] = relay;
            psi.Environment["PULSAR_AUTOSAVE"]   = _autoSave.Checked ? "1" : "0";
            _daemon = new Process { StartInfo = psi, EnableRaisingEvents = true };
            _daemon.OutputDataReceived += (_, e) => { if (e.Data != null) { Log("daemon: " + e.Data); if (e.Data.Contains("subscribed")) BeginInvoke(() => _daemonReady = true); if (e.Data.Contains("auto-saved profile")) BeginInvoke(() => _lastSaved.Text = "Last saved: " + DateTime.Now.ToString("HH:mm")); } };
            _daemon.ErrorDataReceived  += (_, e) => { if (e.Data != null) Log("daemon! " + e.Data); };
            _daemon.Exited += (_, _) => BeginInvoke(() => { _nameClaimed = false; _hosting = false; SetState(); Log("Daemon stopped."); });
            _daemonReady = false; _autoClaimDone = false;
            _daemon.Start(); _daemon.BeginOutputReadLine(); _daemon.BeginErrorReadLine();

            _cfg.PlayerName = name; Remember(_cfg.Servers, serverHost);
            if (_advanced.Checked) Remember(_cfg.Relays, relay);
            _cfg.StarfieldPath = _starfield.Text.Trim(); _cfg.AutoLaunch = _launch.Checked; SaveCfg(); ApplyCfgToUi();
            SetState();
            Log($"Daemon started (server={stdbUri}, relay={relay}). Now claim your name.");
        }
        catch (Exception ex) { Log("Start failed: " + ex.Message); }
    }

    private void StopDaemon()
    {
        try { if (_daemon is { HasExited: false }) { _daemon.Kill(true); _daemon.WaitForExit(2000); } } catch { }
        _daemon = null; _nameClaimed = false; _hosting = false; _daemonReady = false; _autoClaimDone = false;
    }

    private bool Connected => _daemon is { HasExited: false };

    private void SetState()
    {
        bool c = Connected;
        _start.Text = c ? "Stop daemon" : "Start daemon";
        _status.Text = c ? "connected" : "disconnected";
        _status.ForeColor = c ? Color.ForestGreen : Color.Firebrick;
        _name.Enabled = !c; _server.Enabled = !c; _advanced.Enabled = !c; _relay.Enabled = !c;
        _starfield.Enabled = !c; _browse.Enabled = !c; _autoSave.Enabled = !c;
        _claim.Enabled = c && !_nameClaimed;
        _join.Enabled = c && _nameClaimed && !_hosting;
        _joinCode.Enabled = _codeBox.Enabled = _codePw.Enabled = c && _nameClaimed && !_hosting;
        _host.Enabled = c && _nameClaimed;
        _host.Text = _hosting ? "Close lobby" : "Host lobby";
        if (_nameClaimed) { _nameStatus.Text = "you are '" + _name.Text.Trim() + "'"; _nameStatus.ForeColor = Color.ForestGreen; }
        else { _nameStatus.Text = c ? "claim your name" : "not claimed"; _nameStatus.ForeColor = Color.Gray; }
    }

    // ── actions ───────────────────────────────────────────────────────────────
    private void DoClaim()
    {
        if (!Connected) return;
        var name = _name.Text.Trim();
        if (name.Length < 1 || name.Length > 32) { MessageBox.Show(this, "Name must be 1..32 characters.", "PULSAR Lobby"); return; }
        var json = "{\"action\":\"register\",\"cmdId\":" + Esc(NewId(out var id)) + ",\"name\":" + Esc(name) + "}";
        Send("register", json, id, false, $"Claiming '{name}' ...");
    }

    private void DoHostToggle()
    {
        if (_hosting)
        {
            var cj = "{\"action\":\"close\",\"cmdId\":" + Esc(NewId(out var cid)) + "}";
            Send("close", cj, cid, false, "Closing lobby ...");
            return;
        }
        if (!Connected || !_nameClaimed) return;
        var ln = _hostName.Text.Trim();
        if (ln.Length == 0) { MessageBox.Show(this, "Enter a lobby name.", "PULSAR Lobby"); return; }
        var json = "{\"action\":\"host\",\"cmdId\":" + Esc(NewId(out var id)) + ",\"name\":" + Esc(ln) + ",\"max\":" + (int)_max.Value + ",\"passwordHash\":" + Esc(HashPw(_hostPw.Text)) + "}";
        Send("host", json, id, _launch.Checked, $"Hosting '{ln}' ...");
    }

    private void DoJoin()
    {
        if (!Connected || !_nameClaimed) return;
        if (_lobbies.SelectedItems.Count == 0 || _lobbies.SelectedItems[0].Tag is not LobbyRow row)
        { MessageBox.Show(this, "Select a lobby first.", "PULSAR Lobby"); return; }
        string pw = "";
        if (row.HasPw) { var p = PromptPassword(this); if (p is null) return; pw = p; }
        var json = "{\"action\":\"join\",\"cmdId\":" + Esc(NewId(out var id)) + ",\"lobbyId\":" + row.Id + ",\"passwordHash\":" + Esc(HashPw(pw)) + "}";
        Send("join", json, id, _launch.Checked, $"Joining lobby {row.Id} ...");
    }

    private void DoJoinByCode()
    {
        if (!Connected || !_nameClaimed) return;
        var code = _codeBox.Text.Trim();
        if (code.Length < 4) { MessageBox.Show(this, "Enter a lobby code.", "PULSAR Lobby"); return; }
        var json = "{\"action\":\"joinCode\",\"cmdId\":" + Esc(NewId(out var id))
                 + ",\"code\":" + Esc(code) + ",\"passwordHash\":" + Esc(HashPw(_codePw.Text)) + "}";
        Send("joinCode", json, id, _launch.Checked, $"Joining by code {code} ...");
    }

    private void Send(string kind, string json, string cmdId, bool launchAfter, string logMsg)
    {
        try { _pendingKind = kind; _pendingCmdId = cmdId; _launchAfterPending = launchAfter; File.WriteAllText(_cmdPath, json); Log(logMsg); }
        catch (Exception ex) { Log("command write failed: " + ex.Message); _pendingCmdId = ""; }
    }

    // ── poll ──────────────────────────────────────────────────────────────────
    private void Poll()
    {
        // Auto-claim once the daemon is connected - your name is already yours
        // (bound to your identity), so no manual claim is needed on each restart.
        if (_daemonReady && !_nameClaimed && !_autoClaimDone && _pendingKind != "register"
            && Connected && _name.Text.Trim().Length is >= 1 and <= 32)
        { _autoClaimDone = true; DoClaim(); }
        RefreshLobbies(); CheckAck();
    }

    private void RefreshLobbies()
    {
        string json;
        try { json = File.Exists(_lobbiesPath) ? File.ReadAllText(_lobbiesPath) : "[]"; } catch { return; }
        if (json == _lastLobbiesJson) return;
        _lastLobbiesJson = json;
        ulong sel = _lobbies.SelectedItems.Count > 0 && _lobbies.SelectedItems[0].Tag is LobbyRow s ? s.Id : 0;
        _lobbies.BeginUpdate(); _lobbies.Items.Clear();
        try
        {
            using var doc = JsonDocument.Parse(json);
            foreach (var e in doc.RootElement.EnumerateArray())
            {
                ulong id = e.TryGetProperty("lobbyId", out var v) ? v.GetUInt64() : 0;
                string nm = Str(e, "name"), st = Str(e, "status");
                long mc = e.TryGetProperty("memberCount", out var m) ? m.GetInt64() : 0;
                long mx = e.TryGetProperty("maxPlayers", out var x) ? x.GetInt64() : 0;
                bool hasPw = e.TryGetProperty("hasPassword", out var hp) && hp.GetBoolean();
                var it = new ListViewItem(nm);
                it.SubItems.Add($"{mc}/{mx}");
                it.SubItems.Add(st);
                it.SubItems.Add(hasPw ? "\U0001F512" : "");
                it.Tag = new LobbyRow(id, hasPw);
                if (id == sel) it.Selected = true;
                _lobbies.Items.Add(it);
            }
            if (_lobbies.Items.Count == 0) { var it = new ListViewItem("No open lobbies - host one!"); it.ForeColor = Color.Gray; _lobbies.Items.Add(it); }
        }
        catch (Exception ex) { Log("lobbies parse error: " + ex.Message); }
        _lobbies.EndUpdate();
    }

    private void CheckAck()
    {
        if (!File.Exists(_ackPath)) return;
        string text; try { text = File.ReadAllText(_ackPath); } catch { return; }
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            string cmdId = Str(root, "cmdId");
            if (cmdId.Length == 0 || cmdId == _lastAckCmdId) return;
            _lastAckCmdId = cmdId;
            if (cmdId != _pendingCmdId) return;
            bool ok = root.TryGetProperty("ok", out var o) && o.GetBoolean();
            string err = Str(root, "error");
            string kind = _pendingKind; _pendingCmdId = ""; _pendingKind = "";
            if (kind == "register")
            {
                _nameClaimed = ok;
                if (ok) Log("Name claimed.");
                else { Log("Claim rejected: " + err); MessageBox.Show(this, "Name taken - choose another.", "PULSAR Lobby"); }
                SetState();
            }
            else if (kind == "close")
            {
                _hosting = false;
                _hostCodeLbl.Visible = _hostCode.Visible = _copyCode.Visible = false;
                _hostCode.Text = "";
                SetState(); Log(ok ? "Lobby closed." : "Close failed: " + err);
            }
            else if (ok)
            {
                if (kind == "host")
                {
                    _hosting = true; SetState();
                    string code = Str(root, "code");
                    if (code.Length > 0)
                    {
                        _hostCode.Text = code;
                        _hostCodeLbl.Visible = _hostCode.Visible = _copyCode.Visible = true;
                        Log("Lobby hosted. Invite code: " + code);
                    }
                    else Log("Lobby hosted.");
                }
                else Log(kind == "joinCode" || kind == "join" ? "Joined." : "Done.");
                if (_launchAfterPending) LaunchGame();
            }
            else { Log(kind + " failed: " + err); if (kind == "join" || kind == "joinCode") MessageBox.Show(this, "Wrong password or cannot join.", "PULSAR Lobby"); }
            _launchAfterPending = false;
        }
        catch { /* partial write; retry */ }
    }

    private void LaunchGame()
    {
        var exe = _starfield.Text.Trim();
        if (!File.Exists(exe)) { Log("Starfield not found: " + exe); return; }
        try { Process.Start(new ProcessStartInfo { FileName = exe, UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe) ?? "" }); Log("Starfield launched."); }
        catch (Exception ex) { Log("Launch failed: " + ex.Message); }
    }

    // ── helpers ───────────────────────────────────────────────────────────────
    private static string HostOnly(string s)
    {
        var h = s; int i = h.IndexOf("://"); if (i >= 0) h = h.Substring(i + 3);
        int slash = h.IndexOf('/'); if (slash >= 0) h = h.Substring(0, slash);
        int colon = h.IndexOf(':'); if (colon >= 0) h = h.Substring(0, colon);
        return h;
    }
    private void Log(string m) { if (InvokeRequired) { BeginInvoke(() => Log(m)); return; } _log.AppendText($"[{DateTime.Now:HH:mm:ss}] {m}{Environment.NewLine}"); }
    private static string Str(JsonElement e, string p) => e.TryGetProperty(p, out var v) && v.ValueKind == JsonValueKind.String ? (v.GetString() ?? "") : "";
    private static string Esc(string s) => "\"" + (s ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    private static string NewId(out string id) { id = Guid.NewGuid().ToString("N"); return id; }

    // Hash the password here so the plaintext never reaches the daemon / cmd file.
    // Salt must match across all clients (same build) so host + joiner hashes align.
    private const string PwSalt = "PULSAR-lobby-v1:";
    private static string HashPw(string pw)
    {
        pw = (pw ?? "").Trim();
        if (pw.Length == 0) return "";
        using var sha = System.Security.Cryptography.SHA256.Create();
        return Convert.ToHexString(sha.ComputeHash(System.Text.Encoding.UTF8.GetBytes(PwSalt + pw)));
    }

    private static string? PromptPassword(IWin32Window owner)
    {
        using var f = new Form { Text = "Lobby password", FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterParent, ClientSize = new Size(300, 96), MinimizeBox = false, MaximizeBox = false };
        var lbl = new Label { Text = "Enter password:", Location = new Point(12, 8), AutoSize = true };
        var tb = new TextBox { Location = new Point(12, 28), Width = 276, UseSystemPasswordChar = true };
        var ok = new Button { Text = "OK", DialogResult = DialogResult.OK, Location = new Point(132, 60), Size = new Size(70, 26) };
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Location = new Point(214, 60), Size = new Size(74, 26) };
        f.Controls.AddRange(new Control[] { lbl, tb, ok, cancel }); f.AcceptButton = ok; f.CancelButton = cancel;
        return f.ShowDialog(owner) == DialogResult.OK ? tb.Text : null;
    }

    private static string GuessCompanionExe()
    {
        var baseDir = AppContext.BaseDirectory;
        foreach (var rel in new[] { "PulsarCompanion.exe", Path.Combine("companion", "PulsarCompanion.exe") })
        { var c = Path.Combine(baseDir, rel); if (File.Exists(c)) return c; }
        var dir = baseDir;
        for (int i = 0; i < 8 && dir != null; i++)
        {
            foreach (var cfg in new[] { "Release", "Debug" })
            { var c = Path.Combine(dir, "companion", "bin", cfg, "net8.0", "PulsarCompanion.exe"); if (File.Exists(c)) return c; }
            dir = Directory.GetParent(dir)?.FullName;
        }
        return "PulsarCompanion.exe";
    }

    private static string GuessStarfieldExe()
    {
        foreach (var p in new[]
        {
            @"<Starfield>\sfse_loader.exe",
            @"C:\Program Files (x86)\Steam\steamapps\common\Starfield\sfse_loader.exe",
            @"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Starfield.exe",
        })
            if (File.Exists(p)) return p;
        return @"<Starfield>\sfse_loader.exe";
    }
}
