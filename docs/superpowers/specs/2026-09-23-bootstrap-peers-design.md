# Bootstrap Peers — Design

Date: 2026-09-23
Status: Approved (brainstorm cycle)

## Problem

The runtime half of bootstrap peers exists — `network_start_connections`
(src/Network/network.c:4498) connects every entry in
`authority->bootstrap_peers` fire-and-forget at startup — but nothing ever
populates that list. There is no config key, no offsd flag, no CLI command, no
client-API op, no HTTP route, and no peer-store field. The startup path is dead
code, so a node cannot use bootstrap peers to enter the network at all.

## Conceptual model (user-validated)

- **Friends** = explicitly associated connections that must be maintained and
  never dropped (existing feature: persisted in `peer_store.cbor`, reconnect
  at startup + 5s reconnect timer).
- **Bootstrap peers** = how the node gains access to the greater network at
  startup. They follow **normal peer semantics** after connecting (regular
  churn, gossip, droppable). Orthogonal to friends — a peer may appear in both
  lists; the connect loop's "already connected" check makes overlap harmless.

## Design decisions (settled during brainstorm)

1. **Scope:** separate spec/plan cycles for bootstrap peers (this spec) and
   IPv6 support (follow-up). Only the bracket-aware endpoint parser (IPv6
   change item 4) is pulled forward here, because the bootstrap add path needs
   it.
2. **List source:** both a declarative config seed **and** a persisted,
   operator-managed runtime list.
3. **Coexistence:** two separate lists. Config-seeded entries are static at
   runtime (they cannot be removed and they reappear every restart).
   Operator-added entries live in the managed list and are persisted.
4. **Engagement:** startup **and** partition heal — when the node has zero
   active peers, bootstrap peers are re-engaged with backoff.
5. **Surfaces:** CLI (`offs bootstrap add|remove|list`), HTTP
   (`POST/DELETE/GET /bootstrap`), and the client API wire protocol over **all
   four transports** (Unix, TCP, WS, WT).
6. **Friend parity:** while touching the four transport dispatchers, the
   existing `CLIENT_API_FRIEND_*` ops are wired into TCP/WS/WT as well
   (friends are currently dispatched on the Unix socket only).
7. **Client bindings:** `offs` CLI commands, C client library functions, JS
   client methods + dist rebuild.
8. **Friend-save fix included:** friend changes via the Unix-socket/CLI path
   are not saved immediately (only HTTP handlers call `authority_save_peers`;
   the Unix path relies on the debounced dirty flag or shutdown save). The
   friend and bootstrap handlers mark peer state dirty so the debounced save
   covers the Unix path — closing the crash-lossy gap.

## 1. Data model & persistence

- `authority_t` keeps the existing `bootstrap_peers` (`char**` + count,
  config-seeded, read-only at runtime) and gains
  `managed_bootstrap_peers` (`char**` + count) — operator-added entries stored
  as `host:port` strings.
- `peer_store.cbor` gains an **index 6**: managed bootstrap entries as a CBOR
  string array, written by `authority_save_peers`, loaded by
  `authority_load_peers`. Missing index 6 on load = empty managed list
  (forward/backward compatible with existing stores).
- `offs bootstrap list` shows both lists with a source marker (`config` /
  `managed`). Add/remove operate only on the managed list; config entries
  cannot be removed at runtime.
- `authority_destroy` frees the managed list.

## 2. Config seeding

- New config fields: `bootstrap_peers_json` (comma-separated `host:port`
  list) in `config_t`, parsed by `config_json.c`; plus an offsd command-line
  flag in `OFFS/src/offsd/main.c`.
- At startup the config entries seed `authority->bootstrap_peers` exactly as
  the current field expects. They are never written into the managed list or
  the peer store.

## 3. Engagement

- **Startup:** `network_start_connections` connects config list + managed list
  via the existing fire-and-forget `network_connect_peer` path. The current
  first-`:` split (network.c:4506) is replaced by the new endpoint parser.
- **Partition heal:** the existing reconnect timer tick (5s) checks the active
  peer count; if zero, it re-runs the same connect loop over both lists with
  exponential backoff so a dead bootstrap peer cannot cause a hot connect
  loop. Backoff resets on any successful peer connection.

## 4. Client API wire ops — all four transports

- New op codes in `client_api_wire.h`: `CLIENT_API_BOOTSTRAP_ADD`,
  `CLIENT_API_BOOTSTRAP_REMOVE`, `CLIENT_API_BOOTSTRAP_LIST`,
  `CLIENT_API_BOOTSTRAP_LIST_RESPONSE`, numbered after the friend block.
- CBOR encode/decode in `client_api_wire.c`.
- New handlers in `peer_handlers.c` (`peer_handle_bootstrap_add/remove/list`)
  mirroring the friend handlers; all require authentication.
- Dispatched from **all four** transport connection loops: Unix
  (unix_connection.c), TCP (tcp_connection.c), WS (ws_connection.c), WT
  (wt_connection.c). The existing `CLIENT_API_FRIEND_*` ops are added to the
  TCP/WS/WT dispatchers in the same change.
- List response entries carry `host`, `port`, and `source`
  (`config` / `managed`).

## 5. HTTP routes

- `POST /bootstrap` (add), `DELETE /bootstrap` (remove), `GET /bootstrap`
  (list — both sources, source-marked) in `src/ClientAPI/HTTP/peer_routes.c`,
  mirroring the friend routes.
- The friend-save fix lands with this: friend and bootstrap mutation handlers
  mark peer state dirty so the debounced save covers the Unix path.

## 6. Client bindings

- **C client:** `offs_client.c` gains bootstrap add/remove/list functions.
- **CLI:** `offs bootstrap add|remove|list` in
  `../OFFS/src/offs/commands/` (mirrors `cmd_friend`, registered in
  `cli_util.c`).
- **JS client:** `src/ClientLibs/js/offs-client/src/` (wire.js, index.js,
  http-transport.js) gains bootstrap + friend methods; the `dist/` bundles are
  rebuilt (convention from commit cfb6af0).

## 7. Endpoint parsing

One shared helper, e.g.

```c
int parse_endpoint(const char* input, char* host_out, size_t host_len,
                   uint16_t* port_out);
```

accepting `host:port` (IPv4 or hostname) and `[v6-literal]:port`. Used by:
bootstrap add (CLI/C/JS/HTTP input validation), `network_start_connections`,
and config parsing. Bare (unbracketed) IPv6 literals remain unsupported —
that is part of the IPv6 cycle.

## 8. Error handling

- **add:** invalid endpoint → bad request; duplicate in either list →
  conflict.
- **remove:** unknown entry → not found; config-source entry → conflict
  (config entries are immutable at runtime).
- All responses reuse the friend-op status codes
  (`CLIENT_API_STATUS_*`).
- Partition-heal connect failures stay silent (fire-and-forget, logged at
  debug); backoff resets on any successful peer connection.

## 9. Testing

- **Unit:** endpoint parser (bracket cases, malformed input), peer_store
  round-trip with index 6, backoff state machine.
- **Integration:** add/remove/list over each transport (Unix, TCP, WS, WT) and
  HTTP; partition heal (connect node, lose all peers, verify rejoin via
  bootstrap); config-seed + managed-list coexistence; friend ops now reachable
  over TCP/WS/WT.
- **Memory:** valgrind clean on all touched suites (project convention).

## Out of scope

- Full IPv6 support (routing/wire layer, listeners, mDNS, candidate
  enumeration) — separate spec cycle.
- Bare unbracketed IPv6 literals in endpoint parsing.
- Node-to-node (peer-to-peer) bootstrap management — lists are local-only,
  like friends.