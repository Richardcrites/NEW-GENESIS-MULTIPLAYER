# PULSAR Lobby Backend - VPS Deployment (Tier 1)

## Components
- SpacetimeDB standalone (`spacetime start`) - the module host.
- A TLS terminator in front of it (clients must connect over wss/https).

## TLS (required before going online)
SpacetimeDB speaks ws/http on `:3000`. Put a TLS reverse proxy in front so
clients connect over `wss`. Example (Caddy):

    lobby.example.com {
        reverse_proxy 127.0.0.1:3000
    }

Caddy auto-provisions a Let's Encrypt cert; it upgrades http→https and proxies
the WebSocket transparently. (nginx with `proxy_pass` + the `Upgrade` /
`Connection` headers and a manual cert is equivalent.) Do **not** expose
`:3000` directly to the internet.

## Publish to the VPS server
    spacetime server add pulsar-vps --url https://lobby.example.com
    spacetime publish pulsar-lobby -s pulsar-vps -p lobby/server/spacetimedb -y

Note: adding a column to a live table needs a default-value annotation or a
manual migration (seen locally for `member_count`). The first online publish is
to an empty DB, so this only matters for later schema changes - plan migrations
accordingly rather than relying on `--delete-data` in production.

## Companion configuration
Point the companion at the TLS endpoint via environment:

    PULSAR_STDB_URI = wss://lobby.example.com   (or https://… ; the SDK upgrades to wss)
    PULSAR_STDB_DB  = pulsar-lobby

Defaults (unset) are `http://127.0.0.1:3000` / `pulsar-lobby` for local dev.

## Security posture (Tier 1)
- `profile` table is **private**; clients read only their own row via the
  `my_profile` per-user view. The module-owner identity is the operator.
- `lobby_member` is **private**; the browser sees `member_count` only - member
  identities never leave the server.
- Reducer input caps: blob ≤ 256 KB, displayName ≤ 32, lobby name ≤ 48,
  maxPlayers 1..8, `host:port` validated, status whitelisted.
- Reducers authorize on `ctx.Sender` (no identity-by-argument spoofing).
- Companion identity token encrypted at rest (Windows DPAPI, CurrentUser).
- Identity is still anonymous / machine-bound. Real accounts (OIDC) = Tier 2 / Beta.

## Operator notes
- The publishing identity owns the DB and can read all tables - protect that
  machine's `cli.toml`.
- Back up the SpacetimeDB data dir (`~/.local/share/spacetime` / the VPS data
  dir) for profile durability.
- RLS (`ClientVisibilityFilter`) was intentionally **not** used: SpacetimeDB
  marks it `STDB_UNSTABLE` ("test purposes only"). The private-table + view
  approach is the documented-stable path and fails safe.
