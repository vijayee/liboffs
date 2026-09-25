# IPv6 Support — Phase 2 Design

Date: 2026-09-25
Status: Approved (brainstorm session)
Scope: Phase 2 of the IPv6 effort — v6 HOST-candidate enumeration (peer_info.c) and mDNSv6 (mdns.c). Builds on Phase 1 (spec 2026-09-24-ipv6-phase1-design.md, landed).

## Goal

Complete IPv6 LAN discovery: dual-stack hosts advertise v6 LAN addresses in
peer-info/QR payloads, and mDNS discovers v6 peers — including link-local
v6-only hosts.

## Decisions (from brainstorm)

1. **Both items in one phase** — candidate enumeration + mDNSv6 share the
   "v6 LAN address discovery" theme.
2. **Link-local included.** fe80::/10 addresses participate as candidates and
   mDNS records, carrying scope end-to-end (user decision; Phase 1 had
   deferred all link-local work).
3. **Scoped addresses end-to-end (Approach A).** Scope ids flow through the
   platform socket layer, endpoint parsing, peer_info, net_node, and mDNS
   AAAA handling (receiver applies the arriving-interface scope via
   IPV6_RECVPKTINFO). Not source-addr-learning-only (B) or v6-only mDNS (C).

## Section 1 — Scoped-address foundation

Platform layer (src/Platform/platform_socket.{h,c}, mirrored POSIX + Windows):

- `platform_address_t`'s `inet6` member gains `uint32_t scope_id` (0 = unscoped).
- `platform_address_parse`: after a successful INET6 parse, accept an optional
  `%zone` suffix — interface name (`%eth0`, resolved via `if_nametoindex`;
  unknown names → -1) or numeric index (`%5`; index 0 rejected). Scoped input
  with a v4 or unparsable address is rejected.
- `platform_address_to_string`: when `scope_id != 0`, append `%<name>`
  (via `if_indextoname`, falling back to the numeric index on failure).
- `platform_socket_bind`/`connect`: copy `scope_id` into `sin6_scope_id` when
  building the native sockaddr (both platform sections).

String flows need no change: `endpoint_parse` is string-level (brackets
stripped, host copied verbatim), so `fe80::1%eth0` passes through untouched;
`endpoint_host_header` keeps brackets around scoped literals. Scope resolution
happens only when a host string reaches `platform_address_parse`.

net_node gains `uint8_t scope_valid` + `uint32_t scope_id` next to `addr6`
(and the rendezvous pair), set by `net_node_set_platform_addr` /
`net_node_set_rendv_platform` from scoped INET6 input and emitted by
`net_node_addr_string` as the `%name` suffix — dial paths that rebuild
addresses from net_node get the scope for free.

## Section 2 — v6 HOST-candidate enumeration (peer_info.c)

Both platform paths gain a v6 loop after the existing v4 loop, appending
PEER_ADDR_HOST candidates alongside the v4 ones:

- POSIX (`getifaddrs`): also accept AF_INET6 addresses, skipping loopback
  (IN6_IS_ADDR_LOOPBACK) but not link-local. Scope from `sin6_scope_id`,
  emitted as `%<ifname>` via `platform_address_to_string`.
- Windows (`GetAdaptersAddresses`): call once with `AF_UNSPEC` instead of
  `AF_INET` (one walk returns both families); v6 scope from
  `sockaddr_in6.sin6_scope_id`.
- v6 filter: include link-local `fe80::/10`, global-unicast `2000::/3`, and
  ULA `fc00::/7`; exclude loopback, multicast, and v4-mapped (the v4 loop
  already covers those). `_peer_info_is_lan_ipv4` stays; a new
  `_peer_info_is_lan_v6` handles the v6 families.

`PEER_ADDR_HOST`'s type doc updates to mention v6 link-local/ULA/global-LAN.
The existing `include_lan` privacy gate applies to all HOST candidates
unchanged.

## Section 3 — mDNSv6 (mdns.c)

**Two sockets, one reader loop.** The existing AF_INET socket (224.0.0.251,
per-interface IP_ADD_MEMBERSHIP) is joined by a sibling AF_INET6 UDP socket
bound `[::]:5353` with `IPV6_V6ONLY=1`, joining `ff02::fb` (IPV6_JOIN_GROUP)
on every IPv6-capable interface. The poll loop reads both fds; each announce
tick sends on both (v4 packet to the v4 group, v6 packet to ff02::fb).
Windows stays stubbed (pre-existing rationale in mdns.h).

**Announce packet:** when a usable v6 LAN address exists, the v6 announce is
the same DNS response layout with the A record replaced by an AAAA record
(type 28) — 16-byte rdata, network byte order — plus the same SRV record.
Address selection mirrors `_find_lan_ipv4`: first global-unicast `2000::/3`,
then ULA `fc00::/7`, then link-local `fe80::/10` (recorded with its
interface). A v4-only node announces A-only; a v6-only node announces
AAAA-only; dual-stack nodes announce each family on its own socket, so each
receiver hears what its stack can use.

**Receiver scope resolution:** the v6 socket sets `IPV6_RECVPKTINFO` and uses
`recvmsg` to obtain the arriving interface index (IPV6_PKTINFO cmsg). When
parsing an announce: a link-local AAAA (or link-local source address) gets
`sin6_scope_id = <arriving interface index>` and is converted to the
`%<ifname>` string form; global/ULA AAAA records are taken as-is. Discovered
candidates flow into the existing mDNS → peer-connect path (`endpoint_parse`
passes `%scope` through).

**Interface enumeration** (`_find_lan_ipv4` → `_find_lan_addrs`): one
`getifaddrs` walk collecting v4 and v6 LAN addresses (skip loopback; v6
entries keep interface-name → scope info), reused by both announce paths.
Per-family rule: announce v6 only when a v6 LAN address exists, announce v4
only when a v4 one exists.

## Section 4 — Error handling + testing

### Error handling

- Scope parsing: unknown interface name → -1 (existing parse-failure paths);
  numeric index 0 rejected; `if_indextoname` failure on output → numeric
  `%<index>` fallback, never a truncated address.
- mDNS: v6 socket create/join failure is not fatal — log once, continue
  v4-only. Per-interface join failures are logged and skipped. No v6 LAN
  address → no v6 announce; v4 path unaffected.
- `IPV6_RECVPKTINFO` unavailable → v6 socket still parses unscoped records;
  link-local AAAAs from such packets are dropped (unrouteable without scope)
  with a debug log.
- peer_info enumeration: per-family failures are independent — a failed v6
  walk leaves v4 candidates intact and vice versa.

### Testing

- platform: parse/string round trips for scoped literals (`fe80::1%eth0`,
  `%5`, unknown-name rejection); scope present in native bind/connect
  addresses.
- endpoint: `endpoint_parse("[fe80::1%eth0]:port")` end to end.
- peer_info: v6 enumeration tests with fixture interfaces where injectable;
  Phase 1 SRFLX tests stay green.
- mdns: announce-builder tests for the AAAA variant (layout, rdata bytes,
  A+SRV and AAAA+SRV combinations); parser scope assignment (link-local AAAA
  + arriving interface → `%ifname`; global → bare).
- e2e: two offsd on `[::1]`; mDNS discovery between two nodes on the same
  LAN segment (loopback-only fallback: single-node announce/parse round trip
  over the multicast socket).
- Full valgrind pass on touched suites (strip the prebuilt msquic .so's
  DWARF5 first — see valgrind memory notes).

## Out of scope

- mDNS on Windows (stubbed pre-existing).
- mDNS query/response mode (announcements only, as today).
- Bare unbracketed v6 literals in endpoint strings (unsupported by design,
  unchanged from Phase 1).
- Windows CI smoke coverage for Phase 1/2 dual-stack paths (tracked separately).