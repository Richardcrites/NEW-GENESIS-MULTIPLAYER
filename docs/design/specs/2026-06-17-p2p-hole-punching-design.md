# P2P-Primary Co-op via NAT Hole-Punching - Design

**Date:** 2026-06-17
**Status:** Approved (brainstorming)
**Context:** PULSAR co-op is going to a PUBLIC alpha. Today the VPS relay is a *transparent proxy carrying all game traffic for the whole session* (relay-always) - fine for 2 friends, but it does not scale (VPS bandwidth per session + latency hairpin) for many globally-distributed players. The relay was intended as a connection bootstrap, not the transport.

## Goal
Make **direct P2P the primary transport**: the relay becomes a **rendezvous + hole-punch coordinator + fallback**. In the normal case the host↔client ENet stream flows directly (zero game traffic over the VPS); the relay only carries traffic when a direct path can't be established (e.g. symmetric NAT, ~20-30%).

## Approach (decided)
**Direct-first, relay-fallback** (ICE-light). Per join: rendezvous via relay → hole-punch → direct ENet connect → if it fails within a timeout, connect via the relay (today's behaviour). No mid-session migration. A kill-switch (`PULSAR_FORCE_RELAY=1`) forces the proven relay-always path.

## Engine reality that shapes the design
- ENet owns its UDP socket and drains it in `enet_host_service`; a raw rendezvous datagram would be seen as a non-ENet packet and dropped. **Solution: ENet's `host->intercept` callback** - called for every received datagram *before* protocol processing. The callback inspects the datagram; if it's our control message it handles it and returns 1 (consume), else 0 (let ENet process). This catches `PEER`/punch packets on both host and client, integrated with the normal service loop, on the **same socket** ENet uses (so the punched NAT mapping is the one ENet sends/receives on). A separate rendezvous socket would punch the wrong mapping - explicitly rejected.
- The relay already learns both public endpoints from the source addresses of REGISTER (host) / JOIN (client). No STUN server needed.
- The host distinguishes a direct vs relayed connection by the connected peer's address: `== relay address` ⇒ relayed (keep keepalive); else ⇒ direct (stop keepalive, relay idle → TTL-expires).

## Components

### 1. Wire protocol - `include/Network/RelayProtocol.h` (+ mirror in `lobby/relay/Program.cs`)
- New control type `kTypePeer = 3`.
- `PEER` message (relay → each peer): `magic[8] "PULSARLY"` + `type(1)=3` + `session[16]` + **endpoint(6)** = 31 bytes, where endpoint = peer's public `IPv4(4, network order)` + `port(2, network order)`.
- Existing `REGISTER=1` / `JOIN=2` (25 bytes) unchanged.
- Helpers: `MakePeer(session, ip, port)`, `ParsePeer(buf,len) -> {ip,port}`, and a predicate `IsControl(buf,len)` (magic + known type) so the intercept callback can cheaply classify.

### 2. Relay - `lobby/relay/Program.cs`
- On a control packet that completes a pair (session now has both Host and Client endpoints), send a `PEER` to **each** peer with the **other's** endpoint. Re-send on each subsequent REGISTER/JOIN for the session (cheap, beats loss) until traffic is seen / TTL.
- Keep the existing data-forwarding path **unchanged** - it is the fallback. (Endpoint-pinning + session cap stay.)
- `--selftest` extended: assert a `PEER` is delivered to both simulated peers carrying the other's loopback endpoint.

### 3. Networking - `src/Network/NetworkHost.{h,cpp}`
- `PULSAR_FORCE_RELAY` (env, surfaced via handoff/companion) → skip P2P, use today's relay-connect verbatim.
- Set `m_host->intercept = &RelayInterceptThunk` after `enet_host_create`. The thunk (static, C-ABI) reads `host->receivedAddress` + `host->receivedData[receivedDataLength]`; if `IsControl`:
  - `PEER` → parse → store peer `ENetAddress` in an atomic + set `g_peerKnown`; return 1.
  - else (stray REGISTER/JOIN echoes) → return 1.
  - non-control → return 0.
- **Rendezvous + connect state machine** (replaces the current "relayMode ⇒ connect to relay" branch):
  - **Client:** send JOIN×3 → poll for `g_peerKnown` (deadline `kPeerWaitMs≈2000`). On peer known: **punch** = send `kPunchCount≈5` raw 1-byte datagrams to the peer endpoint via `enet_socket_send(m_host->socket,…)` over ~500 ms; then `enet_host_connect(peerEndpoint)`. Await `OnConnect` (deadline `kDirectMs≈3000`). On connect ⇒ **direct**. On either deadline miss ⇒ **fallback**: `enet_host_connect(relayAddress)` (current path).
  - **Host:** REGISTER + keepalive as today; on `g_peerKnown` ⇒ punch toward the client endpoint (keep `enet_host_service` running so the client's direct connect is accepted). On `OnConnect`: if peer addr `==` relay ⇒ keep keepalive (relayed); else ⇒ stop keepalive (direct, relay no longer needed).
- A `[PULSAR][Relay]` log line for: peer learned, punch sent, direct-connected vs relay-fallback - so the in-game (real-NAT) test is verifiable from the log.

## Data flow
```
host: REGISTER ─┐                          ┌─ JOIN: client
                ├─► relay (pairs session) ──┤
relay ─ PEER(client ep) ─► host             relay ─ PEER(host ep) ─► client   (via intercept)
host: punch ─► client ep                    client: punch ─► host ep
                                            client: enet_connect(host ep) ──► direct ENet  ✓
   (if no direct within kDirectMs) client: enet_connect(relay) ──► relayed (fallback)
direct established ⇒ host stops REGISTER keepalive ⇒ relay session idles out (no VPS game traffic)
```

## Error handling / safety
- No `PEER` within `kPeerWaitMs` (relay down/unreachable) ⇒ relay-fallback connect (so behaviour ≥ today even if rendezvous fails).
- Punch fails (symmetric NAT) ⇒ direct connect times out ⇒ relay-fallback. Expected for a minority; logged.
- `PULSAR_FORCE_RELAY=1` ⇒ guaranteed today's verified relay-always (public-alpha safety valve).
- Intercept thunk does only parse + atomic store (runs on ENet's service thread) - no allocation, no locks beyond an atomic.
- Relay session must stay alive until direct is confirmed (fallback needs it) - host keepalive continues until `OnConnect` proves direct.

## Testing
- **Relay self-test (local):** `PEER` delivered both ways with correct endpoints; data still forwards (fallback intact).
- **C++ local (no NAT):** `PULSAR_FORCE_RELAY=1` = byte-for-byte today's path (regression). With P2P on, loopback: peers are same host ⇒ direct connect to the loopback endpoint should succeed (exercises the punch+direct+intercept code path even without a real NAT). Verify the log states "direct".
- **In-game, real NATs (USER, host + client on different networks):** confirm log shows `direct-connected` (not fallback), co-op plays, then kill the relay container mid-session → session survives (proves no game traffic on the VPS). Also test a forced fallback (`PULSAR_FORCE_RELAY=1` on one side or block direct) ⇒ still connects via relay.
- **Honest limit:** the real direct path can only be validated with two distributed NATs; local proves protocol + fallback + the loopback-direct path, not true cross-NAT punch.

## Deploy
- Relay change ⇒ rebuild + redeploy the VPS relay container. Protocol is **backward-additive** (PEER is a new type; old clients ignore it and just relay) but the C++ side must match for P2P; ship relay + DLL together.
- DLL rebuild + deploy (local + NAS).

## Out of scope
- Mid-session relay→direct upgrade (Model 2). Dedicated STUN/TURN infra. IPv6. Multiple simultaneous peers (co-op is 2-player; ENet `MAX_CLIENTS=1`). UPnP/port-mapping.
