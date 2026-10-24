using SpacetimeDB;

// PULSAR lobby/profile backend (Teil 2A). Stores accounts + opaque profile
// blobs (Teil-1 SerializeProfile bytes) and brokers lobbies. All mutations go
// through reducers; authorization is via ctx.Sender. The Blob is never
// interpreted here.
public static partial class Module
{
    private const int  MAX_BLOB_BYTES  = 262144; // 256 KB profile blob cap
    private const int  MAX_NAME_LEN    = 32;     // account display name
    private const int  MAX_LOBBY_NAME  = 48;     // lobby name
    private const int  MAX_ADDR_LEN    = 48;     // "host:port"
    private const uint MAX_PLAYERS_CAP = 8;      // co-op upper bound
    private const uint MAX_PW_FAILS    = 10;          // failed joins per lobby before cooldown
    private const long PW_WINDOW_US     = 30_000_000; // 30 s cooldown window (microseconds)

    // Dead-account pruning: an account with NO connection/activity for this long is
    // deleted together with its profile (character) by the scheduled PruneAccounts.
    // Tunable - change + republish. (Active players bump LastSeen on connect/save/join.)
    private const long ACCOUNT_TTL_US   = 90L * 24 * 60 * 60 * 1_000_000; // 90 days idle
    private const long ACCOUNT_PRUNE_US = 24L * 60 * 60 * 1_000_000;      // run the prune daily

    // Phase 2 - relay master-list caps + freshness.
    private const int  MAX_RELAY_NAME    = 32;          // relay display name
    private const int  MAX_RELAY_REGION  = 24;          // region label (e.g. "eu-central")
    private const int  MAX_RELAY_HOST    = 48;          // "host:port" (same shape as a lobby addr)
    private const long RELAY_STALE_US    = 60_000_000;  // 60 s - a relay older than this is dead
    private const long RELAY_PRUNE_US    = 30_000_000;  // 30 s - scheduled prune interval

    // Official relay API keys: a relay registering with one of these gets Verified=true.
    // The matching key is set as PULSAR_RELAY_APIKEY in the relay's .env. Anyone may
    // still self-register without a listed key - they just show as community/unverified.
    // Operator keys do NOT belong in source. If you run your own instance,
    // supply them through an owner-set DB table or an environment variable.
    private static readonly string[] OFFICIAL_RELAY_KEYS =
    {
        // Intentionally empty in the public tree.
        // Relays without a listed key self-register as community/unverified.
    };

    // Lazily delete every relay row whose LastSeen is older than RELAY_STALE_US.
    // Called from register/heartbeat AND from the scheduled prune, so the public
    // table never shows dead relays even if the scheduler is slow.
    private static void PruneStaleRelays(ReducerContext ctx)
    {
        long nowUs = ctx.Timestamp.MicrosecondsSinceUnixEpoch;
        foreach (var r in ctx.Db.Relay.Iter().ToList())
            if (nowUs - r.LastSeen.MicrosecondsSinceUnixEpoch > RELAY_STALE_US)
                ctx.Db.Relay.RelayId.Delete(r.RelayId);
    }

    // Close + clean up every lobby this identity hosts (members + private relay row
    // + throttle row), then DELETE the lobby row. A host owns at most one open lobby;
    // re-hosting replaces the old. We delete rather than mark "closed" so the public
    // Lobby table can't accumulate dead rows forever (memory DoS) and the browser /
    // GenLobbyCode scan only ever see live lobbies.
    private static void CloseHostedLobbies(ReducerContext ctx, Identity host)
    {
        foreach (var lobby in ctx.Db.Lobby.HostIdentity.Filter(host).ToList())
        {
            foreach (var m in ctx.Db.LobbyMember.LobbyId.Filter(lobby.LobbyId).ToList())
            {
                // Drop each member's released session grant (only if it points here).
                if (ctx.Db.RelayGrant.Member.Find(m.MemberIdentity) is RelayGrant g && g.LobbyId == lobby.LobbyId)
                    ctx.Db.RelayGrant.Member.Delete(m.MemberIdentity);
                ctx.Db.LobbyMember.Id.Delete(m.Id);
            }
            ctx.Db.LobbyRelay.LobbyId.Delete(lobby.LobbyId);
            ctx.Db.JoinAttempt.LobbyId.Delete(lobby.LobbyId);
            ctx.Db.Lobby.LobbyId.Delete(lobby.LobbyId);
        }
    }

    // Recompute a lobby's denormalized MemberCount from the ACTUAL member rows. Used by
    // leave/disconnect so the browser count can never drift (a missed decrement used to
    // inflate it permanently). No-op if the lobby is gone.
    private static void RecountMembers(ReducerContext ctx, ulong lobbyId)
    {
        if (ctx.Db.Lobby.LobbyId.Find(lobbyId) is Lobby lb)
        {
            uint c = 0;
            foreach (var _ in ctx.Db.LobbyMember.LobbyId.Filter(lobbyId)) c++;
            lb.MemberCount = c;
            ctx.Db.Lobby.LobbyId.Update(lb);
        }
    }

    // Per-user join outcome (read back via the MyRelay view). We signal results
    // this way instead of throwing, because a throw rolls back the transaction
    // (and with it the recorded brute-force fail count).
    private static void SetGrant(ReducerContext ctx, ulong lobbyId, string session, string status)
    {
        var g = new RelayGrant { Member = ctx.Sender, LobbyId = lobbyId, Session = session, Status = status };
        if (ctx.Db.RelayGrant.Member.Find(ctx.Sender) is not null) ctx.Db.RelayGrant.Member.Update(g);
        else ctx.Db.RelayGrant.Insert(g);
    }

    private const string CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0/O/1/I
    private const int    CODE_LEN      = 6;

    // Deterministic (ctx.Rng) join code, unique among OPEN lobbies. Closed lobbies
    // keep their old code (ignored - only open lobbies are joinable).
    private static string GenLobbyCode(ReducerContext ctx)
    {
        for (int attempt = 0; attempt < 6; attempt++)
        {
            var chars = new char[CODE_LEN];
            for (int i = 0; i < CODE_LEN; i++)
                chars[i] = CODE_ALPHABET[ctx.Rng.Next(0, CODE_ALPHABET.Length)];
            string code = new string(chars);
            bool taken = false;
            foreach (var l in ctx.Db.Lobby.Iter())
                if (l.Status == "open" && l.Code == code) { taken = true; break; }
            if (!taken) return code;
        }
        // Astronomically unlikely fallback: a longer code to guarantee uniqueness.
        var ch2 = new char[CODE_LEN + 2];
        for (int i = 0; i < ch2.Length; i++) ch2[i] = CODE_ALPHABET[ctx.Rng.Next(0, CODE_ALPHABET.Length)];
        return new string(ch2);
    }

    private static bool IsValidHostAddr(string addr)
    {
        if (string.IsNullOrEmpty(addr) || addr.Length > MAX_ADDR_LEN) return false;
        int colon = addr.LastIndexOf(':');
        if (colon <= 0 || colon == addr.Length - 1) return false;
        if (!uint.TryParse(addr.Substring(colon + 1), out var port)) return false;
        return port >= 1 && port <= 65535;
    }

    // Mark the caller's account as "seen now" (connection or activity). No-op if the
    // caller has no account yet (e.g. a relay/anon connection). Feeds the 90-day prune.
    private static void TouchAccount(ReducerContext ctx)
    {
        if (ctx.Db.Account.Identity.Find(ctx.Sender) is Account a)
        {
            a.LastSeen = ctx.Timestamp;
            ctx.Db.Account.Identity.Update(a);
        }
    }

    [SpacetimeDB.Table(Accessor = "Account")]   // PRIVATE - uniqueness is enforced server-side; clients never read others' accounts
    public partial struct Account
    {
        [PrimaryKey]
        public Identity  Identity;
        [Unique]
        public string    DisplayName;   // claimed; unique across identities
        public Timestamp CreatedAt;
        public Timestamp LastSeen;      // last connection/activity - the dead-account prune key
    }

    [SpacetimeDB.Table(Accessor = "Profile")]   // PRIVATE - clients read via MyProfile view only
    public partial struct Profile
    {
        [PrimaryKey]
        public Identity   Identity;
        public List<byte> Blob;        // opaque Teil-1 SerializeProfile() bytes
        public uint       Version;
        public Timestamp  UpdatedAt;
    }

    [SpacetimeDB.Table(Accessor = "Lobby", Public = true)]
    public partial struct Lobby
    {
        [PrimaryKey]
        [AutoInc]
        public ulong     LobbyId;
        [SpacetimeDB.Index.BTree]
        public Identity  HostIdentity;
        public string    HostAddr;     // "ip:port" (direct/LAN only; "" in relay mode)
        public string    RelayHost;    // relay "host:port" the joiner must use ("" = direct/LAN). Public is safe: a relay is a public endpoint, unlike the host IP.
        public bool      HasPassword;  // public flag → browser shows a lock
        public string    Name;
        public string    Status;       // "open" | "in_game" | "closed"
        public uint      MaxPlayers;
        public uint      MemberCount;   // denormalized joiner count for the browser
        public string    Code;          // short typable join code (browser shortcut)
        public Timestamp CreatedAt;
        public uint      ModCount;      // # of user mods active on the host (badge in browser). 0 = vanilla.
        public string    ModList;       // host's active mod filenames, '|'-joined (for the join parity check). NAMES ONLY, never mod files.
    }

    // PRIVATE - the relay session (the real access secret) + password hash never
    // reach clients directly. Released to members/host only via the MyRelay view.
    [SpacetimeDB.Table(Accessor = "LobbyRelay")]
    public partial struct LobbyRelay
    {
        [PrimaryKey]
        public ulong  LobbyId;
        public string Session;   // relay session (32 hex) or "" for direct lobbies
        public string Hash;      // password hash (64 hex) or "" for open lobbies
    }

    // PRIVATE per-member: the relay session RELEASED to a verified joiner (written
    // by JoinLobby after the password check). Read back via the MyRelay view as a
    // point lookup on the caller's identity (works in a per-user view; .Iter does not).
    [SpacetimeDB.Table(Accessor = "RelayGrant")]
    public partial struct RelayGrant
    {
        [PrimaryKey]
        public Identity Member;
        public ulong    LobbyId;
        public string   Session;   // real relay session (32 hex) on success; "" otherwise
        public string   Status;    // "ok" | "denied" | "locked" | "full" - the join outcome
    }

    // PRIVATE - per-lobby failed-password throttle. DEPRECATED (2026-06-27): replaced by
    // the per-IDENTITY JoinFail below to stop a griefer from locking a lobby's gate for
    // everyone. Kept defined (not dropped) so republish stays an additive hot-swap; the
    // table is now unused (CloseHostedLobbies still clears any legacy rows, harmless).
    [SpacetimeDB.Table(Accessor = "JoinAttempt")]
    public partial struct JoinAttempt
    {
        [PrimaryKey]
        public ulong     LobbyId;
        public uint      Fails;
        public Timestamp WindowStart;
    }

    // PRIVATE - per-IDENTITY failed-join throttle (brute-force defence). Keyed by the
    // CALLER, not the lobby: a flood of wrong guesses can only lock the GUESSER out, and
    // can NEVER deny a legit joiner who has the right password (the old per-lobby table
    // let one griefer lock a public passworded lobby for everyone). MAX_PW_FAILS within a
    // PW_WINDOW_US window, across all lobbies. Identity-minting to get fresh windows is
    // bounded at the connection layer (reverse-proxy rate limit), not here.
    [SpacetimeDB.Table(Accessor = "JoinFail")]
    public partial struct JoinFail
    {
        [PrimaryKey]
        public Identity  Member;
        public uint      Fails;
        public Timestamp WindowStart;
    }

    // PUBLIC relay master-list. Relays self-register + heartbeat into this table;
    // the tool reads it live and pings each PublicHost client-side. One row per
    // operator identity (re-registering replaces the old row). Verified is set by
    // the module from the relay's API key (an official key → trusted badge).
    [SpacetimeDB.Table(Accessor = "Relay", Public = true)]
    public partial struct Relay
    {
        [PrimaryKey]
        [AutoInc]
        public uint      RelayId;
        [SpacetimeDB.Index.BTree]
        public Identity  Operator;     // the registering identity (one relay per operator)
        public string    Name;         // display name
        public string    Region;       // region label for sorting/filtering
        public string    PublicHost;   // "host:port" the tool connects + pings
        public bool      Verified;     // true if the API key is in OFFICIAL_RELAY_KEYS
        public uint      Sessions;     // live relayed sessions (self-reported via heartbeat)
        public uint      SoftCap;      // operator's comfortable session ceiling (0 = unset)
        public uint      LoadPct;      // 0..100 self-reported utilisation (sessions + bandwidth)
        public bool      Accepting;    // false once past the soft cap → the tool skips this relay
        public Timestamp LastSeen;     // heartbeat freshness; rows older than 60 s are pruned
    }

    // Scheduled-table driver for periodic relay pruning. STDB calls PruneRelays
    // every RELAY_PRUNE_US (the row re-arms itself on Init). Public so the schema
    // is visible; only the module ever writes it.
    [SpacetimeDB.Table(
        Accessor    = "RelayPruneTimer",
        Scheduled   = nameof(PruneRelays),
        ScheduledAt = nameof(ScheduledAt),
        Public      = true)]
    public partial struct RelayPruneTimer
    {
        [PrimaryKey]
        [AutoInc]
        public ulong      ScheduledId;
        public ScheduleAt ScheduledAt;
    }

    // Scheduled-table driver for the periodic dead-account prune (armed by Init,
    // daily). Public so the schema is visible; only the module writes it.
    [SpacetimeDB.Table(
        Accessor    = "AccountPruneTimer",
        Scheduled   = nameof(PruneAccounts),
        ScheduledAt = nameof(ScheduledAt),
        Public      = true)]
    public partial struct AccountPruneTimer
    {
        [PrimaryKey]
        [AutoInc]
        public ulong      ScheduledId;
        public ScheduleAt ScheduledAt;
    }

    [SpacetimeDB.Table(Accessor = "LobbyMember")]   // PRIVATE - member identities never leave the server
    public partial struct LobbyMember
    {
        [PrimaryKey]
        [AutoInc]
        public ulong     Id;
        [SpacetimeDB.Index.BTree]
        public ulong     LobbyId;
        [SpacetimeDB.Index.BTree]
        public Identity  MemberIdentity;
        public Timestamp JoinedAt;
    }

    // Profile is a PRIVATE table (no Public=true): clients cannot subscribe to it
    // directly. A client reads only its OWN profile through this per-user view.
    // The database owner (module publisher) retains full access for operations.
    [SpacetimeDB.View(Accessor = "MyProfile", Public = true)]
    public static Profile? MyProfile(ViewContext ctx)
    {
        return ctx.Db.Profile.Identity.Find(ctx.Sender) as Profile?;
    }

    // Per-user: the host or a verified member of a lobby may read its relay
    // session. Non-members never see it → a password actually gates relay access.
    [SpacetimeDB.View(Accessor = "MyRelay", Public = true)]
    public static RelayGrant? MyRelay(ViewContext ctx)
    {
        return ctx.Db.RelayGrant.Member.Find(ctx.Sender) as RelayGrant?;
    }

    [SpacetimeDB.Reducer]
    public static void RegisterAccount(ReducerContext ctx, string displayName)
    {
        displayName = (displayName ?? "").Trim();
        if (displayName.Length < 1 || displayName.Length > MAX_NAME_LEN)
            throw new Exception("displayName length must be 1.." + MAX_NAME_LEN);

        // Unique: reject a name already held by a DIFFERENT identity (your own rename is fine).
        if (ctx.Db.Account.DisplayName.Find(displayName) is Account heldBy && heldBy.Identity != ctx.Sender)
            throw new Exception("name taken");

        if (ctx.Db.Account.Identity.Find(ctx.Sender) is Account acc)
        {
            acc.DisplayName = displayName;
            acc.LastSeen    = ctx.Timestamp;
            ctx.Db.Account.Identity.Update(acc);
        }
        else
        {
            ctx.Db.Account.Insert(new Account
            {
                Identity    = ctx.Sender,
                DisplayName = displayName,
                CreatedAt   = ctx.Timestamp,
                LastSeen    = ctx.Timestamp,
            });
        }
    }

    [SpacetimeDB.Reducer]
    public static void UpsertProfile(ReducerContext ctx, List<byte> blob, uint version)
    {
        // Null/empty guard FIRST: a null List<byte> would NRE on .Count (a trivially
        // client-triggerable reducer crash); an empty blob would store a useless row.
        if (blob is null || blob.Count == 0 || blob.Count > MAX_BLOB_BYTES)
            throw new Exception("profile blob missing/empty or exceeds " + MAX_BLOB_BYTES + " bytes");
        TouchAccount(ctx);   // a save = activity → keeps the account alive

        if (ctx.Db.Profile.Identity.Find(ctx.Sender) is Profile p)
        {
            if (version <= p.Version) return;   // last-writer-wins by version
            p.Blob      = blob;
            p.Version   = version;
            p.UpdatedAt = ctx.Timestamp;
            ctx.Db.Profile.Identity.Update(p);
        }
        else
        {
            ctx.Db.Profile.Insert(new Profile
            {
                Identity  = ctx.Sender,
                Blob      = blob,
                Version   = version,
                UpdatedAt = ctx.Timestamp,
            });
        }
    }

    // RECOVERY - wipe the caller's stored profile so the next SOLO join
    // re-bootstraps a clean DB profile from the player's own save. Use this when
    // a profile got poisoned (e.g. host items leaked into the client's DB via a
    // bootstrap-while-bridged upload). Only ever touches ctx.Sender's OWN row
    // (never another identity's) - the client's companion calls it with the
    // client's identity.
    [SpacetimeDB.Reducer]
    public static void ResetMyProfile(ReducerContext ctx)
    {
        if (ctx.Db.Profile.Identity.Find(ctx.Sender) is Profile p)
        {
            ctx.Db.Profile.Identity.Delete(ctx.Sender);
            Log.Info("ResetMyProfile: deleted profile (version " + p.Version +
                     ", " + p.Blob.Count + " bytes) for " + ctx.Sender);
        }
        else
        {
            Log.Info("ResetMyProfile: no profile to delete for " + ctx.Sender);
        }
    }

    // ---- Phase 2: relay master-list -------------------------------------------------

    // Arm the periodic prune on module init (and re-arm on every republish, which is
    // safe - the row is idempotent enough; a duplicate timer would just prune twice).
    [SpacetimeDB.Reducer(ReducerKind.Init)]
    public static void Init(ReducerContext ctx)
    {
        if (ctx.Db.RelayPruneTimer.Count == 0)
            ctx.Db.RelayPruneTimer.Insert(new RelayPruneTimer
            {
                ScheduledId = 0,
                ScheduledAt = new ScheduleAt.Interval(new TimeDuration(RELAY_PRUNE_US)),
            });

        if (ctx.Db.AccountPruneTimer.Count == 0)
            ctx.Db.AccountPruneTimer.Insert(new AccountPruneTimer
            {
                ScheduledId = 0,
                ScheduledAt = new ScheduleAt.Interval(new TimeDuration(ACCOUNT_PRUNE_US)),
            });

        // Migration safety: an in-place schema upgrade leaves existing accounts with
        // LastSeen=epoch(0). Backfill to "now" so the 90-day clock starts at publish,
        // not at 1970 (which would prune every existing account on the first run).
        foreach (var a in ctx.Db.Account.Iter().ToList())
            if (a.LastSeen.MicrosecondsSinceUnixEpoch == 0)
            {
                var u = a;
                u.LastSeen = ctx.Timestamp;
                ctx.Db.Account.Identity.Update(u);
            }
    }

    // A relay self-registers (and re-registers, e.g. on restart). One row per operator
    // identity: re-registering REPLACES the old row (delete + insert) so an operator
    // can never accumulate ghost relays. Verified is decided here from the API key -
    // clients can't fake it. apiKey must be non-empty (a relay with no key is rejected).
    [SpacetimeDB.Reducer]
    public static void RegisterRelay(ReducerContext ctx, string name, string region,
                                     string publicHost, string apiKey)
    {
        name       = (name ?? "").Trim();
        region     = (region ?? "").Trim();
        publicHost = (publicHost ?? "").Trim();
        apiKey     = (apiKey ?? "").Trim();

        if (name.Length < 1 || name.Length > MAX_RELAY_NAME)
            throw new Exception("relay name length must be 1.." + MAX_RELAY_NAME);
        if (region.Length < 1 || region.Length > MAX_RELAY_REGION)
            throw new Exception("relay region length must be 1.." + MAX_RELAY_REGION);
        if (!IsValidHostAddr(publicHost) || publicHost.Length > MAX_RELAY_HOST)
            throw new Exception("invalid publicHost (expected host:port)");
        if (apiKey.Length == 0)
            throw new Exception("apiKey required");

        bool verified = System.Array.IndexOf(OFFICIAL_RELAY_KEYS, apiKey) >= 0;

        PruneStaleRelays(ctx);   // opportunistic cleanup so the list stays fresh on write

        // One relay per operator: replace any existing row for this identity.
        foreach (var existing in ctx.Db.Relay.Operator.Filter(ctx.Sender).ToList())
            ctx.Db.Relay.RelayId.Delete(existing.RelayId);

        ctx.Db.Relay.Insert(new Relay
        {
            RelayId    = 0,            // AutoInc assigns the real id
            Operator   = ctx.Sender,
            Name       = name,
            Region     = region,
            PublicHost = publicHost,
            Verified   = verified,
            Sessions   = 0,
            SoftCap    = 0,
            LoadPct    = 0,
            Accepting  = true,         // a fresh relay accepts until a heartbeat says otherwise
            LastSeen   = ctx.Timestamp,
        });
    }

    // Keep the caller's relay row fresh. No-op if the caller has no relay row
    // (e.g. it was pruned after a long stall - the relay should RegisterRelay again).
    [SpacetimeDB.Reducer]
    public static void RelayHeartbeat(ReducerContext ctx, uint sessions, uint softCap,
                                      uint loadPct, bool accepting)
    {
        if (loadPct > 100) loadPct = 100;   // clamp defensively - clients can't fake a sane bar
        foreach (var r in ctx.Db.Relay.Operator.Filter(ctx.Sender).ToList())
        {
            var u = r;
            u.Sessions  = sessions;
            u.SoftCap   = softCap;
            u.LoadPct   = loadPct;
            u.Accepting = accepting;
            u.LastSeen  = ctx.Timestamp;
            ctx.Db.Relay.RelayId.Update(u);
        }
        PruneStaleRelays(ctx);   // piggyback cleanup on each heartbeat
    }

    // Scheduled prune (armed by Init, fires every RELAY_PRUNE_US). The timer row is
    // passed in (and persists, since it's a repeating interval). Deletes dead relays.
    [SpacetimeDB.Reducer]
    public static void PruneRelays(ReducerContext ctx, RelayPruneTimer timer)
    {
        PruneStaleRelays(ctx);
    }

    // Scheduled (Init-armed, daily): delete DEAD accounts - no connection/activity for
    // ACCOUNT_TTL_US (90 days) - together with their profile (character), so the name
    // frees up and storage is reclaimed. Disconnect already cleaned any lobbies/members,
    // so only Account + Profile remain. LastSeen==0 (un-backfilled migration rows) is
    // skipped as a safety net - Init backfills those to publish-time first.
    [SpacetimeDB.Reducer]
    public static void PruneAccounts(ReducerContext ctx, AccountPruneTimer timer)
    {
        long nowUs = ctx.Timestamp.MicrosecondsSinceUnixEpoch;
        foreach (var a in ctx.Db.Account.Iter().ToList())
        {
            long seenUs = a.LastSeen.MicrosecondsSinceUnixEpoch;
            if (seenUs == 0) continue;                        // migration safety net
            if (nowUs - seenUs <= ACCOUNT_TTL_US) continue;   // still alive
            ctx.Db.Profile.Identity.Delete(a.Identity);       // remove the character
            ctx.Db.Account.Identity.Delete(a.Identity);       // free the name
            Log.Info("PruneAccounts: removed dead account '" + a.DisplayName + "' (idle > 90d)");
        }

        // ORPHAN profiles: a profile row whose account no longer exists (e.g. a legacy
        // upload before account-linking, or abuse). These would otherwise NEVER be pruned
        // (the account loop above only touches account-linked profiles) = an unbounded
        // storage leak. Gate on UpdatedAt + the same 90-day TTL so a freshly-saved profile
        // is NEVER deleted even if its account is momentarily absent. (IP-rate-flooding of
        // fresh accounts is bounded at the reverse-proxy, not here - reducers see no IP.)
        foreach (var pr in ctx.Db.Profile.Iter().ToList())
            if (ctx.Db.Account.Identity.Find(pr.Identity) is null
                && nowUs - pr.UpdatedAt.MicrosecondsSinceUnixEpoch > ACCOUNT_TTL_US)
            {
                ctx.Db.Profile.Identity.Delete(pr.Identity);
                Log.Info("PruneAccounts: removed orphan profile (no account, idle > 90d) for " + pr.Identity);
            }

        // Stale per-identity join-throttle rows (window long past) - tiny, but don't let
        // them accumulate for one-shot guessers who never return.
        foreach (var jf in ctx.Db.JoinFail.Iter().ToList())
            if (nowUs - jf.WindowStart.MicrosecondsSinceUnixEpoch > ACCOUNT_PRUNE_US)
                ctx.Db.JoinFail.Member.Delete(jf.Member);
    }

    // Bound the published mod list (NAMES ONLY, '|'-joined) to keep the public Lobby row
    // small. Cut on a '|' boundary so the last entry is never a half name (which would
    // show as a bogus "missing mod" on the joiner).
    private const int MAX_MOD_LIST = 4096;
    private static string CapModList(string s)
    {
        s ??= "";
        if (s.Length <= MAX_MOD_LIST) return s;
        int cut = s.LastIndexOf('|', MAX_MOD_LIST - 1);
        return cut > 0 ? s.Substring(0, cut) : s.Substring(0, MAX_MOD_LIST);
    }

    [SpacetimeDB.Reducer]
    public static void CreateLobby(ReducerContext ctx, string name, string hostAddr,
                                   uint maxPlayers, string relaySession, string passwordHash,
                                   string relayHost, uint modCount, string modList)
    {
        name = (name ?? "").Trim();
        if (name.Length < 1 || name.Length > MAX_LOBBY_NAME)
            throw new Exception("lobby name length must be 1.." + MAX_LOBBY_NAME);
        if (maxPlayers < 1 || maxPlayers > MAX_PLAYERS_CAP)
            throw new Exception("maxPlayers must be 1.." + MAX_PLAYERS_CAP);
        relaySession = (relaySession ?? "").Trim();
        // relaySession may carry an appended per-lobby E2E auth secret as "relayPart|secret"
        // (relay session + an anti-MITM secret the relay must never see). Validate the relay
        // part; the secret is OPAQUE here and just passed through to both members over the
        // trusted DB channel (the relay never sees DB traffic). Bound it so a client can't
        // inflate the row. Stored + delivered combined; the companion splits it on read.
        int barIdx = relaySession.IndexOf('|');
        string relayPart = barIdx >= 0 ? relaySession.Substring(0, barIdx) : relaySession;
        string authPart  = barIdx >= 0 ? relaySession.Substring(barIdx + 1) : "";
        if (relayPart.Length != 0 && relayPart.Length != 32)
            throw new Exception("relaySession must be empty or 32 hex chars");
        if (authPart.Length > 80)
            throw new Exception("relay auth secret too long");
        passwordHash = (passwordHash ?? "").Trim();
        if (passwordHash.Length != 0 && passwordHash.Length != 64)
            throw new Exception("passwordHash must be empty or 64 hex chars");
        // The joiner connects to THIS relay (host-authoritative). Public + safe.
        relayHost = (relayHost ?? "").Trim();
        if (relayHost.Length != 0 && (relayHost.Length > MAX_RELAY_HOST || !IsValidHostAddr(relayHost)))
            throw new Exception("invalid relayHost (expected host:port)");
        // Relay mode → never store a public host IP (DDoS-safe). Direct/LAN mode needs one.
        // Use the RELAY part (the auth secret does not change relay-vs-direct mode).
        if (relayPart.Length == 0)
        {
            if (!IsValidHostAddr(hostAddr))
                throw new Exception("invalid hostAddr (expected host:port)");
        }
        else
        {
            hostAddr = "";
        }

        CloseHostedLobbies(ctx, ctx.Sender);   // one open lobby per host - replace any old one

        var row = ctx.Db.Lobby.Insert(new Lobby
        {
            LobbyId      = 0,            // AutoInc assigns the real id
            HostIdentity = ctx.Sender,
            HostAddr     = hostAddr,
            RelayHost    = relayHost,
            HasPassword  = passwordHash.Length > 0,
            Name         = name,
            Status       = "open",
            MaxPlayers   = maxPlayers,
            MemberCount  = 1,   // the host counts as member 1 (browser shows 1/X)
            Code         = GenLobbyCode(ctx),
            CreatedAt    = ctx.Timestamp,
            ModCount     = modCount,
            ModList      = CapModList(modList),
        });
        // The host is the first member: the browser shows 1/X immediately, and the
        // MaxPlayers cap includes the host (so a 2-player lobby = host + 1 joiner).
        ctx.Db.LobbyMember.Insert(new LobbyMember
        {
            Id             = 0,
            LobbyId        = row.LobbyId,
            MemberIdentity = ctx.Sender,
            JoinedAt       = ctx.Timestamp,
        });
        // Session + hash live in the PRIVATE table; never in the public lobby row.
        if (relaySession.Length > 0 || passwordHash.Length > 0)
            ctx.Db.LobbyRelay.Insert(new LobbyRelay { LobbyId = row.LobbyId, Session = relaySession, Hash = passwordHash });
    }

    // Explicit close from the host (GUI "Close lobby").
    [SpacetimeDB.Reducer]
    public static void CloseLobby(ReducerContext ctx)
    {
        CloseHostedLobbies(ctx, ctx.Sender);
    }

    [SpacetimeDB.Reducer]
    public static void UpdateLobbyStatus(ReducerContext ctx, ulong lobbyId, string status)
    {
        var lobby = ctx.Db.Lobby.LobbyId.Find(lobbyId)
            ?? throw new Exception("lobby not found");
        if (lobby.HostIdentity != ctx.Sender)
            throw new Exception("only the host may change lobby status");
        if (status != "open" && status != "in_game" && status != "closed")
            throw new Exception("invalid status");
        // Closing must clean up + delete (members, relay, throttle) - not leave a
        // dead row. Route it through the same path as CloseLobby/disconnect.
        if (status == "closed") { CloseHostedLobbies(ctx, ctx.Sender); return; }
        lobby.Status = status;
        ctx.Db.Lobby.LobbyId.Update(lobby);
    }

    [SpacetimeDB.Reducer]
    public static void JoinLobby(ReducerContext ctx, ulong lobbyId, string passwordHash)
    {
        TouchAccount(ctx);   // joining = activity → keeps the account alive
        var lobby = ctx.Db.Lobby.LobbyId.Find(lobbyId)
            ?? throw new Exception("lobby not found");
        if (lobby.Status != "open")
            throw new Exception("lobby not open");
        passwordHash = (passwordHash ?? "").Trim();

        // Password check + brute-force throttle. We must NOT throw on a wrong/locked
        // attempt - STDB rolls back the transaction on throw, discarding the recorded
        // fail count. Instead we COMMIT and signal the outcome via RelayGrant.Status.
        if (lobby.HasPassword)
        {
            long nowUs = ctx.Timestamp.MicrosecondsSinceUnixEpoch;
            // Per-IDENTITY throttle (anti-griefing): the counter is keyed on the caller,
            // so wrong guesses only ever lock the GUESSER, never a legit joiner.
            JoinFail? prior = ctx.Db.JoinFail.Member.Find(ctx.Sender);
            // The window is LIVE only if the recorded start is within PW_WINDOW_US.
            bool windowLive = prior is JoinFail pa &&
                              nowUs - pa.WindowStart.MicrosecondsSinceUnixEpoch <= PW_WINDOW_US;
            uint fails = windowLive ? prior!.Value.Fails : 0;

            if (fails >= MAX_PW_FAILS) { SetGrant(ctx, lobbyId, "", "locked"); return; }

            bool good = ctx.Db.LobbyRelay.LobbyId.Find(lobbyId) is LobbyRelay lrk && lrk.Hash == passwordHash;
            if (!good)
            {
                // FIXED window: keep the original WindowStart while the window is live;
                // only stamp a new one when starting a fresh window. Stamping it on every
                // failure would slide the window forward, so a steady drip of guesses
                // would never expire (and never let MAX_PW_FAILS reset for legit users).
                var winStart = windowLive ? prior!.Value.WindowStart : ctx.Timestamp;
                var na = new JoinFail { Member = ctx.Sender, Fails = fails + 1, WindowStart = winStart };
                if (prior is not null) ctx.Db.JoinFail.Member.Update(na);
                else ctx.Db.JoinFail.Insert(na);
                SetGrant(ctx, lobbyId, "", "denied");
                return;
            }
            if (prior is not null) ctx.Db.JoinFail.Member.Delete(ctx.Sender);   // correct password → reset the throttle
        }

        uint count = 0;
        foreach (var m in ctx.Db.LobbyMember.LobbyId.Filter(lobbyId))
        {
            if (m.MemberIdentity == ctx.Sender)   // already a member - re-affirm the grant, idempotent
            {
                string sess0 = ctx.Db.LobbyRelay.LobbyId.Find(lobbyId) is LobbyRelay r0 ? r0.Session : "";
                SetGrant(ctx, lobbyId, sess0, "ok");
                return;
            }
            count++;
        }
        if (count >= lobby.MaxPlayers) { SetGrant(ctx, lobbyId, "", "full"); return; }

        ctx.Db.LobbyMember.Insert(new LobbyMember
        {
            Id             = 0,
            LobbyId        = lobbyId,
            MemberIdentity = ctx.Sender,
            JoinedAt       = ctx.Timestamp,
        });
        lobby.MemberCount = count + 1;
        ctx.Db.Lobby.LobbyId.Update(lobby);

        // Release the relay session to this verified member (private per-user).
        string sess = ctx.Db.LobbyRelay.LobbyId.Find(lobbyId) is LobbyRelay rel ? rel.Session : "";
        SetGrant(ctx, lobbyId, sess, "ok");
    }

    [SpacetimeDB.Reducer]
    public static void LeaveLobby(ReducerContext ctx, ulong lobbyId)
    {
        bool removed = false;
        foreach (var m in ctx.Db.LobbyMember.LobbyId.Filter(lobbyId).ToList())
        {
            if (m.MemberIdentity == ctx.Sender)
            {
                ctx.Db.LobbyMember.Id.Delete(m.Id);
                removed = true;
            }
        }
        if (removed)
        {
            ctx.Db.RelayGrant.Member.Delete(ctx.Sender);
            RecountMembers(ctx, lobbyId);   // recompute from rows - no denormalized drift
        }
    }

    // Mark a connecting player "seen now" so the 90-day dead-account prune only ever
    // removes accounts with NO connection for the full window. No-op for relay/anon
    // connections (no account row).
    [SpacetimeDB.Reducer(ReducerKind.ClientConnected)]
    public static void HandleConnect(ReducerContext ctx)
    {
        TouchAccount(ctx);
    }

    // Close every lobby this identity hosts (and drop its members) when the
    // host disconnects - keeps the server browser free of stale lobbies.
    [SpacetimeDB.Reducer(ReducerKind.ClientDisconnected)]
    public static void HandleDisconnect(ReducerContext ctx)
    {
        // Lobbies this identity HOSTS: close + clean up (members + relay row).
        CloseHostedLobbies(ctx, ctx.Sender);

        // This identity's memberships in OTHER lobbies: remove + decrement each,
        // so member counts don't inflate with stale rows.
        foreach (var m in ctx.Db.LobbyMember.MemberIdentity.Filter(ctx.Sender).ToList())
        {
            ulong lid = m.LobbyId;
            ctx.Db.LobbyMember.Id.Delete(m.Id);
            RecountMembers(ctx, lid);   // recompute from rows - no denormalized drift
        }
        ctx.Db.RelayGrant.Member.Delete(ctx.Sender);   // drop this member's released session
    }
}
