# OFFS Configurable Settings

The 24 fields that can be set via the `offs` CLI (`offs config set/add/remove`)
and `PUT /config`. All go through the same staging path: changes are written to
`{data_dir}/pending_config.json` and require a daemon restart to apply
(`POST /config/restart` or `offs config reload` for reloadable fields).

Registry: `src/Configuration/config_json.c:15-30`
CLI help: `OFFS/src/offs/commands/config.c:31-58`

## String (5)

| Field | Description |
|---|---|
| `api_key_hash` | bcrypt hash (`$2b$` prefix) for client auth; null = auth disabled |
| `https_cert_path` | HTTPS server certificate PEM |
| `https_key_path` | HTTPS server private key PEM |
| `tcp_tls_cert_path` | TCP transport TLS certificate PEM |
| `tcp_tls_key_path` | TCP transport TLS private key PEM |

## Bool (9) — accept `true`/`false` or `1`/`0`

| Field | Description |
|---|---|
| `http_enabled` | Enable the HTTP server |
| `https_enabled` | Enable the HTTPS server |
| `unix_enabled` | Enable the Unix socket transport |
| `tcp_enabled` | Enable the raw TCP transport |
| `ws_enabled` | Enable the WebSocket transport |
| `wt_enabled` | Enable the WebTransport (QUIC) transport |
| `tcp_tls_enabled` | Enable TLS on the TCP transport |
| `allow_secure` | Accept CA-validated node certs (secure/authority mode) |
| `fsync_data` | fsync data writes to disk |

## Number (10) — integers

| Field | Description |
|---|---|
| `cache_size` | Block sections held in the in-memory round-robin |
| `max_snapshots` | Max snapshot files retained by the persistent index |
| `max_wals` | Max WAL files retained by the persistent index |
| `max_capacity_bytes` | Max bytes the block cache holds (0 = disabled); PUT rejects oversized uploads |
| `scheduler_thread_count` | Scheduler worker threads (0 = auto) |
| `http_port` | HTTP port (0 = disabled) |
| `https_port` | HTTPS port (0 = disabled) |
| `tcp_port` | Raw TCP transport port (0 = disabled) |
| `ws_port` | WebSocket port (0 = disabled) |
| `wt_port` | WebTransport UDP port (0 = disabled) |

## Auth shortcuts

These map onto `api_key_hash` but add client-side handling:

- `offs config set-auth <hash>` — validates a 60-char `$2a$/$2b$/$2y$` bcrypt
  hash before staging it.
- `offs config generate-auth <key> [--cost N]` — bcrypt-hashes the key locally
  (default cost 12, range 4–31) so the plaintext never reaches the daemon;
  prints and stages the hash.

## Startup-only (4) — not in the mutable registry

These tune the ephemeral representation registry's persistent elastic bloom
filter. They are read once at daemon startup when the registry is created
(`src/BlockCache/ephemeral_registry.c:ephemeral_registry_create`) and cannot be
changed via the CLI or `PUT /config` — they are not in `config_json.c`'s
mutable field tables.

| Field | Description |
|---|---|
| `ephemeral_registry_size` | Initial elastic bloom filter capacity for the ephemeral representation registry (default 1024) |
| `ephemeral_registry_hash_count` | Hashes per element (default 4) |
| `ephemeral_registry_omega` | Load ratio that triggers filter expansion (default 0.85) |
| `ephemeral_registry_fp_bits` | Fingerprint bits per entry (default 8) |

## Notes

- `offs config remove <field>` stages JSON `null`, which reverts the field to
  its default.
- The CLI help text omits `allow_secure` and `fsync_data`, but both are
  settable — the daemon registry is the authority.
- Config mutations are refused (403) on non-loopback transports regardless of
  auth status.