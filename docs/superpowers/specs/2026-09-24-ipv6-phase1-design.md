# IPv6 Support — Phase 1 Design

Date: 2026-09-24
Status: Approved (brainstorm session)
Scope: Phase 1 of the two-phase IPv6 effort. Phase 2 (HOST-candidate enumeration in peer_info.c and mDNSv6) is a separate future cycle.

## Goal

IPv6 end-to-end for routing, wire transport, API listeners, and all client APIs/CLIs —
in both liboffs and the OFFS repo (offsd + offs CLI). Today only incoming QUIC is
dual-stack; the routing layer and wire format are IPv4-only and every API listener
hardcodes IPv4.

## Decisions (from brainstorm)

1. **Two phases.** Phase 1 = wire format + routing layer + listener binds + client
   APIs/CLIs. Phase 2 = v6 HOST-candidate enumeration + mDNSv6.
2. **Wire compat = append fields.** New optional trailing CBOR fields; old decoders
   already tolerate extra trailing fields (they check min-length only
   `cbor_array_size(item) < N`), so no version negotiation and mixed v4/v6 networks work.
3. **Dual-stack default.** All listeners bind AF_INET6 with `IPV6_V6ONLY=0`, matching
   the QUIC listener's existing `QUIC_ADDRESS_FAMILY_UNSPEC` behavior. C client drops
   its AF_INET getaddrinfo hint. No new config surface.

## Section 1 — Routing layer + wire format

### Routing layer (`net_node_t`)

- Add `uint8_t addr_family` (PLATFORM_AF_INET / PLATFORM_AF_INET6) and
  `uint8_t addr6[16]` to `net_node_t` (src/Network/net_node.h:22-24).
- The existing `uint32_t addr` stays: populated for IPv4 (and for v4-mapped v6),
  zero for pure-v6 — so existing consumers (SOPPSON/Hebbian keying, LRU, peer book)
  keep working unchanged.
- New helpers in net_node.c:
  - `net_node_set_addr()` — sockaddr → fields, v4-mapped handling.
  - `net_node_to_sockaddr()` — fields → sockaddr.
  - `net_node_addr_string()` — bracket-aware formatting for logs/config.
- Constructors (`net_node_create`, `_rendv`, `_unidentified`) gain family-aware
  variants; the u32-only versions remain as thin wrappers defaulting to AF_INET.
- network.c:951-955 stops extracting only the AF_INET portion of the peer sockaddr;
  it stores the full sockaddr via `net_node_set_addr()`, keeping `addr` populated as
  today for v4 and setting `addr6` for pure-v6.

### Wire format (append-only)

- One new CBOR encoding pair in wire.c: `_addr_encode`/`_addr_decode` — a 2-element
  array `[family: uint, addr: 16-byte bytestring (v4-mapped)]`.
- Appended as a trailing optional field to every message that carries a u32 address:
  the two reflexive-addr structs (src/Network/wire.h:339,354) and both
  rendezvous-addr structs (wire.h:373,382), plus any message embedding a
  `net_node_t` (find_node_response closest-nodes, salutation). Exact message list to
  be inventoried during implementation planning.
- Decoders: when the trailing field is present, fill the new family/addr6 fields;
  when absent, fall back to the u32 (IPv4) field. Old peers decode new messages
  (extra field ignored); new peers on old networks get clean v4 fallback.

## Section 2 — Listeners + client APIs/CLIs

### Dual-stack binds (liboffs)

- http_server.c:155,172; tcp_transport.c:179,199; ws_transport.c:180,200;
  webtransport_h3.c:1072 stop hardcoding `PLATFORM_AF_INET` and bind
  `PLATFORM_AF_INET6` with `IPV6_V6ONLY=0`.
- platform_socket gains one helper that sets `IPV6_V6ONLY=0` before bind (explicit
  for determinism even where the OS default is already 0). All four listeners call it.
- Fallback: if the INET6 bind fails (host without IPv6), fall back to the AF_INET
  bind with one logged warning. No silent downgrade to v6-only.

### C client (offs_client.c)

- Drop `hints.ai_family = AF_INET` at offs_client.c:1411 and 2820-2829 — use
  `AI_ADDRCONFIG` so the client prefers v6 when the host has v6 connectivity and
  never picks v6 on a v4-only host. `endpoint_parse()` (already bracket-aware)
  covers parsing on both sides.

### OFFS side (offsd + offs CLI)

- offsd listen-address values pass through liboffs listeners, so dual-stack comes for
  free. Remaining work: verify config plumbing accepts v6 literals / `[::1]:port` via
  endpoint_parse, and sweep both repos for `host:port` parsers that split on the
  first `:` (the pattern network.c:4506 used to have).
- offs CLI target-address arguments get v6-literal support via endpoint_parse;
  display code (logs, `peer info`) formats v6 with brackets via
  `net_node_addr_string()`.

### Explicitly out of scope (Phase 2)

HOST-candidate enumeration (peer_info.c:381,410) and mDNSv6 stay IPv4-only in
Phase 1. A v6 client can still connect via literal addresses and relays; local
auto-discovery of v6-only peers waits for Phase 2.

## Section 3 — Error handling + testing

### Error handling

- Wire decoders: malformed trailing addr field (wrong family, bad bytestring length)
  → reject the message exactly like a malformed required field. Absent field is the
  only fallback-to-u32 path. No partial-fill tolerance.
- `net_node_set_addr()` from an unsupported family → returns error; caller keeps the
  node addressless (addr=0, family=UNSPEC) — same posture as today's non-IPv4 peers
  at network.c:951-955.
- Bind fallback fires only on the v6-bind failure path and logs once. The u32 `addr`
  staying zero for pure-v6 nodes is handled by existing "addr 0" consumers (they
  treat it as unidentified today; nothing new to break).
- NAT detection (nat_detect.c) and topology report (topology_report.c:317-326) stay
  per-connection-family: a v6 connection reports v6 reflexive data when the peer
  supports it, otherwise v4-only as today.

### Testing

- Wire: round-trip tests for every message that gains the appended field (v4
  present, pure-v6 present, field absent → u32 fallback); old-format decode
  compatibility tests (encode without the new field, decode must succeed and fill
  u32).
- net_node: set/to_sockaddr/string helpers over v4, v4-mapped, pure-v6.
- Listeners: bind tests over `[::1]` plus a v4-mapped connection; fallback-to-v4
  test on a host without v6 (failure injection).
- C client: getaddrinfo resolution of a v6 literal and a hostname resolving to AAAA.
- OFFS e2e: offsd + offs CLI from ../OFFS against local liboffs master with node
  certs, exercising a v6 loopback connection end to end.
- Full valgrind pass on all touched suites (zero-leak baseline, `-gdwarf-4`).

## Wire-message inventory (to complete at planning time)

Every wire message carrying a u32 address field must be enumerated and each must
get: (a) appended `_addr_encode` field on encode, (b) optional decode with u32
fallback, (c) round-trip + old-format tests. Known starting points:
wire.h:339 (WIRE_RELAY_PUNCH reflexive_addr), wire.h:354, wire.h:373, wire.h:382
(rendezvous_addr), plus messages embedding net_node_t (FIND_NODE_RESPONSE,
SALUTATION) and the net_node serialization itself.