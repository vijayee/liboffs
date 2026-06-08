# Production Blockers

Audit of the `ClientAPI` branch, 2026-05-22.

## Summary

The core engine (P2P protocol, block cache, streaming, actor model) is well-built and
memory-clean. Gaps are concentrated in security and operational layers. The library is
suitable for trusted-LAN or research use; it is not ready for public-internet deployment.

## Critical (Production Blockers)

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| 1 | TLS certificate validation disabled globally | `quic_listener.c`, `relay_client.c`, `relay_server.c`, `wt_transport.c`, `offs_client.c` | MITM trivial on all QUIC connections |
| 2 | No authentication or authorization | Entire codebase | Any client can PUT/GET arbitrary blocks |
| 3 | Windows Platform layer incomplete | `platform_socket.c`, `platform_local.c`, `platform_file.c` | Cannot build on Windows |

### 1. TLS Certificate Validation Disabled

Every QUIC connection uses `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`
unconditionally. Affected files:
- `src/Network/quic_listener.c:578-581` — P2P listener
- `src/Network/relay_client.c:508-510` — relay client
- `src/Network/Relay/relay_server.c:615-617` — relay server
- `src/ClientAPI/WT/wt_transport.c:375` — WT transport server
- `src/ClientLibs/c/offs_client.c:872` — WT client (non-secure path only, but secure path
  has no CA configuration either)

TLS encrypts but does not authenticate peers.

### 2. No Authentication

The HTTP server has a middleware system (`http_middleware_t`) but no auth middleware
exists. The client API (`offs_client.h`) has no token, key, or credential parameters.
The P2P layer has no peer identity verification beyond the optional `public_key` field
in `authority_t` (used in salutation, but not enforced).

### 3. Windows Platform Layer Incomplete

Three modules have only stub implementations for `#ifdef _WIN32`:
- `platform_socket.c:216` — `/* Windows implementation deferred */`
- `platform_local.c:56` — `/* Windows implementation deferred */`
- `platform_file.c:76` — `/* Windows implementation deferred */`

Threading, time, process, and random have full Windows support.

## High (Significant Concerns)

| # | Issue | Impact |
|---|-------|--------|
| 4 | No monitoring, metrics, or health checks | Cannot observe runtime behavior |
| 5 | No graceful shutdown / connection draining | In-flight operations may be lost |
| 6 | No client-side retry or backoff | Transient failures become permanent |
| 7 | No input validation | Malformed data may cause undefined behavior |
| 8 | No wire protocol versioning | Protocol changes break all clients silently |

### 4. No Monitoring or Health Checks

Logging uses `rxi/log.c` (basic printf-style, no structured output, no log level
configuration per module). No Prometheus/StatsD metrics export. `topology_metrics_t`
is referenced in `authority.h` but has no implementation for health endpoints.

### 5. No Graceful Shutdown

`offs_node_stop()` in `node.c:66` sets `running=0` and tears down components in
sequence. No waiting for in-flight operations, no connection draining, no graceful
client disconnect. `scheduler_pool_stop()` is called immediately.

### 6. No Client Retry or Backoff

`offs_client.c` operations fail immediately on error. No exponential backoff, no
retry policy. Hardcoded timeouts: 100ms poll interval, 1s WS connect timeout (100
loops x 10ms), 2s WT connect timeout (200 loops x 10ms). No user-configurable
timeouts.

### 7. No Input Validation

ORI strings, content types, file names, and URLs are passed directly to internal
functions. The `client_api_wire.c` decoder trusts CBOR input without bounds
checking on decoded values.

### 8. No Wire Protocol Versioning

`client_api_wire.h` defines message type constants (1-11) but has no version field
in the protocol. A protocol change would require flag-day cutover or break all
existing clients.

## Medium (Should Fix Before Broad Deployment)

| # | Issue | Impact |
|---|-------|--------|
| 9 | No HTTP-level rate limiting | HTTP endpoints have no request throttling |
| 10 | No configuration validation | Invalid config values pass silently |
| 11 | Bootstrap relies on static peer lists | No DNS seed, DHT, or mDNS discovery |

### 9. No HTTP Rate Limiting

Rate limiting exists at the P2P network layer (`rate_limit.h` with token buckets
per RPC type). The HTTP server (`http_server.h`) has no per-connection or
per-endpoint rate limiting.

### 10. No Configuration Validation

`config_t` in `config.h` has no validation. Invalid values (zero bucket size,
max_tuple_size < min_tuple_size) pass through silently.

### 11. Static Peer Bootstrap Only

`authority_t` stores bootstrap peers as a static string array. No DNS seeding,
no DHT bootstrap, no mDNS local discovery. Nodes must be manually configured
with peer addresses.
