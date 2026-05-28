# Runtime Config API Design

## Overview

Add a client API to read and update node configuration at runtime. Dynamic settings
apply immediately. Static settings are saved to a pending config file and take effect
on the next node restart — which the API can also trigger.

## Endpoints

| Method | Path              | Auth                          | Description                                |
|--------|-------------------|-------------------------------|--------------------------------------------|
| GET    | `/config`         | Transport-gated               | Return full current config as JSON         |
| PUT    | `/config`         | Transport-gated               | Update config; apply dynamic, stage static |
| POST   | `/config/restart` | Local transports only         | Trigger graceful node restart              |

## Auth Model — Transport-Gated

The auth middleware is refactored to always run, but is a no-op when
`api_key_hash == NULL`. Once a key is set, every request requires a valid Bearer
token **unless** the transport is locally bound.

| Transport | Binding              | Auth for `/config` | `/config/restart` |
|-----------|----------------------|--------------------|--------------------|
| Unix      | n/a                  | Never              | Allowed            |
| HTTP      | 127.0.0.1 / local IP | Never              | Allowed            |
| HTTP      | 0.0.0.0 / public     | Bearer token       | Blocked            |
| HTTPS     | 127.0.0.1 / local IP | Never              | Allowed            |
| HTTPS     | 0.0.0.0 / public     | Bearer token       | Blocked            |
| WS        | 127.0.0.1 / local IP | Never              | Allowed            |
| WS        | 0.0.0.0 / public     | Bearer token       | Blocked            |
| WT        | any                  | Bearer token       | Blocked            |
| TCP       | any                  | Bearer token       | Blocked            |

"Local IP" means the socket's bind address is a loopback or link-local address.
The decision is per-server — if an HTTP server is bound to `127.0.0.1:8080`, all
requests arriving on that server are trusted. If another HTTP server is bound to
`0.0.0.0:9090`, those requests require auth.

## PUT /config — Request

All fields optional; send only what you want to change.

```json
{
  "api_key_hash": "$2b$12$...",
  "rate_limits": {
    "FIND_BLOCK":       {"base_rate": 5.0, "max_rate": 50.0, "burst_size": 20.0, "cost": 1.0},
    "STORE_BLOCK":      {"base_rate": 0.5, "max_rate": 5.0,  "burst_size": 3.0,  "cost": 1.0},
    "SEEKING_BLOCKS":   {"base_rate": 1.0, "max_rate": 10.0, "burst_size": 5.0,  "cost": 1.0},
    "PING_CAPACITY":    {"base_rate": 10.0,"max_rate": 10.0, "burst_size": 10.0, "cost": 0.1},
    "PING":             {"base_rate": 10.0,"max_rate": 10.0, "burst_size": 10.0, "cost": 0.1}
  },
  "max_capacity_bytes": 1073741824,
  "cache_size": 100,
  "max_snapshots": 5,
  "max_wals": 5,
  "scheduler_thread_count": 8,
  "http_enabled": true,
  "http_port": 8080,
  "https_enabled": true,
  "https_port": 443,
  "https_cert_path": "/etc/offs/cert.pem",
  "https_key_path": "/etc/offs/key.pem",
  "unix_enabled": true,
  "tcp_enabled": true,
  "tcp_port": 9000,
  "ws_enabled": true,
  "ws_port": 9001,
  "wt_enabled": false,
  "wt_port": 9002
}
```

## PUT /config — Response

```json
{
  "applied": ["api_key_hash", "rate_limits.STORE_BLOCK.base_rate", "max_capacity_bytes"],
  "pending_restart": ["cache_size", "http_port", "https_enabled", "https_cert_path"],
  "rejected": [],
  "restart_required": true
}
```

- **applied**: Dynamic fields changed immediately.
- **pending_restart**: Static fields saved to disk; take effect on next node start.
- **rejected**: Fields that failed validation with reason strings.
- **restart_required**: `true` if any static field was changed.

## Field Classification

### Dynamic (applied immediately)

| Field              | How applied                                                       |
|--------------------|-------------------------------------------------------------------|
| `api_key_hash`     | Update `config_t.api_key_hash`. Auth middleware reads it per-request — takes effect immediately. NULL→value activates auth; value→NULL deactivates; value→value rotates the key. |
| `rate_limits.*`    | Store mutable config copy on `rate_limit_table_t`. Update the table's config fields. Next `rate_limit_check` call uses new values. |
| `max_capacity_bytes`| Update `block_cache->max_capacity_bytes`. Must be >= current bytes used (shrink only). If new value < current usage, reject. |

### Static (saved to pending config, restart required)

`cache_size`, `max_snapshots`, `max_wals`, `scheduler_thread_count`,
`http_enabled`, `http_port`, `https_enabled`, `https_port`, `https_cert_path`,
`https_key_path`, `unix_enabled`, `tcp_enabled`, `tcp_port`, `ws_enabled`,
`ws_port`, `wt_enabled`, `wt_port`

These are all bind-time or alloc-time decisions — changing them requires tearing
down and re-creating the affected subsystem.

## Validation Flow

```
PUT /config
  → cJSON_Parse(body)
  → For each key in request:
      ├─ Unknown key?       → rejected: "unknown field: <key>"
      ├─ Wrong type?        → rejected: "expected <type> for <key>, got <actual>"
      ├─ Fails range check? → rejected: "<key>: <reason>"
      ├─ Dynamic field?     → apply immediately → applied
      └─ Static field?      → stage in pending_config → pending_restart
  → If any static fields staged: write pending config to disk
  → If api_key_hash changed: update auth middleware state
  → Return combined result
```

Validation rules are the same as `config_validate()` — numeric ranges,
cross-field constraints (e.g., `lru_size <= cache_size`), cert/key file
existence for TLS settings.

## Pending Config File

**Path**: `<data_dir>/pending_config.json` (same directory that contains `sections/`).

On startup, `config_default()` checks for this file. If present and valid, its
values override the compiled defaults. After successful startup, the file is
renamed to `pending_config.applied` so it isn't re-applied on subsequent starts.

If the file exists but startup fails (config invalid), the file is preserved so
the operator can fix it.

## POST /config/restart

Available only on local transports. Sequence:

1. Verify a pending config file exists (return 409 if not)
2. Validate the pending config (return 400 if invalid)
3. Return `202 Accepted`
4. Begin graceful restart on a background thread:
   a. `offs_node_stop()` — drain HTTP connections, stop network, flush, persist peers
   b. `offs_node_destroy()` — free all subsystems
   c. Load pending config, merge into new `config_t`
   d. `offs_node_create()` + `offs_node_start()` — bring up with new config
   e. Re-create and re-bind HTTP server(s) with the same transport settings
   f. Rename `pending_config.json` → `pending_config.applied`
   g. Re-register all routes (off, block, peer, config, health)
   h. Send response to any waiting client (or client polls GET /config to confirm)

The `202 Accepted` response is sent before the restart begins because the HTTP
server will be torn down during the restart.

## cJSON Integration

We add [cJSON](https://github.com/DaveGamble/cJSON) (MIT, single `.c`/`.h`) as a
vendored dependency. It handles both parsing (cJSON_Parse) and generation
(cJSON_CreateObject, cJSON_Print).

### New usage

- `config_routes.c` — parse PUT body, generate GET/PUT/POST responses
- `config_pending.c` — serialize/deserialize pending config file

### Retrofit existing manual JSON construction

All existing manual `snprintf`/`asprintf` JSON construction is replaced with cJSON:

| File                  | Current approach                  | Replace with                          |
|-----------------------|-----------------------------------|---------------------------------------|
| `health_handler.c`    | 30+ APPEND macro calls            | cJSON object builder                  |
| `peer_routes.c`       | snprintf into 64KB buffer + asprintf | cJSON object/array builder          |
| `ws_connection.c`     | stack buffer + health_data_to_json | cJSON → cJSON_Print                   |
| `tcp_connection.c`    | stack buffer + health_data_to_json | cJSON → cJSON_Print                   |
| `wt_connection.c`     | stack buffer + health_data_to_json | cJSON → cJSON_Print                   |
| `unix_connection.c`   | stack buffer + health_data_to_json | cJSON → cJSON_Print                   |

`health_data_to_json` is refactored to return a `cJSON*` tree instead of writing
into a pre-allocated buffer. Callers use `cJSON_Print()` to serialize. This
eliminates buffer overflow risk from fixed-size stack/heap buffers (current code
uses 8192-byte stack buffers and 65536-byte heap buffers with manual offset
tracking).

## Implementation Pieces

1. **Vendor cJSON** — add `cJSON.c`/`cJSON.h` to `src/Util/` or a new `src/JSON/`
2. **`config_routes.c`/`.h`** — new route module: GET, PUT, POST /config/restart
3. **`config_pending.c`/`.h`** — save/load/apply pending config file
4. **`rate_limit_config_update()`** — `rate_limit.c` — mutate rate limit config at runtime
5. **`block_cache_set_max_capacity()`** — `block_cache.c` — update max capacity
6. **`offs_node_restart()`** — `node.c` — graceful in-process restart
7. **Auth middleware refactor** — always register; no-op when `api_key_hash == NULL`
8. **Transport local-binding check** — `http_server_t` exposes whether it is locally bound
9. **Wire `rate_limit_check` into RPC dispatch** — prerequisite; currently only called in tests
10. **Retrofit existing JSON users** — health_handler, peer_routes, transport connections

## Dependencies

- `rate_limit_check` must be wired into `network.c` RPC dispatch before
  rate limit config updates are meaningful. Without this, rate limit config
  changes are stored but have no effect on actual traffic.

## Out of Scope

- Config update via non-HTTP transports (WS, WT, TCP) — can be added later using
  the same internal functions
- Rollback of failed config changes (dynamic fields are applied immediately with
  no undo)
- Config profiles or presets
- `index_bucket_size`, `section_size`, `max_tuple_size`, `min_tuple_size`,
  `descriptor_pad`, `section_cache_count` — these remain static-only
  (startup config file), not exposed via the runtime API
