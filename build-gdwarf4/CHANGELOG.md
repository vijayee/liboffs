# Changelog

## [Unreleased] - 2026-08-24

### Security

- **Content integrity at receive boundary** — every peer-supplied block is re-hashed (BLAKE3) and compared against the requested address; mismatches are rejected and the supplier's Hebbian weight is penalized (`failure_penalty * bad_block_multiplier`).
- **Peer reputation system** — Hebbian learning weights (the project's "SOPPSON numbers") drive routing; bad blocks cost the supplier `bad_block_rate_cost` rate-limit tokens + `failure_penalty * bad_block_multiplier` Hebbian penalty; `rate_limit_penalty` applied on any rate-limit rejection.
- **Peer-state persistence** — Hebbian weights + connected-peer info survive restart via atomic write to `peer_state.cbor` (v3 format with `relay_verified`, `nat_type`, `last_seen_ms`, `bad_blocks_received`); TTL filtering drops stale peers on load.
- **Signed release manifests** — self-update path verifies an ed25519 signed manifest against a compile-in release public key (`OFFS_RELEASE_PUBKEY`, fail-closed if unset); per-file SHA256 from the verified manifest; in-process path-contained USTAR extraction (rejects absolute paths, `..` components, symlinks, hardlinks, oversized archives); verified staged updater binary (absolute path, not `PATH` lookup); `offs-release-sign` tool for the release operator.
- **TLS certificate verification** — update check + download verify the server's TLS certificate against the system CA store (`SSL_VERIFY_PEER` + SNI + `SSL_get_verify_result`).
- **Local-binding auth optional but default-on** — bearer tokens required for `/config` mutations even on loopback by default; `config_local_binding_no_auth` flag (default false) opts back into the no-auth loopback mode; non-loopback `/config` PUT + `/config/restart` refused (403).
- **Bearer-requires-TLS** — validator rejects `http_enabled && !https_enabled && api_key_hash` (bearer over plaintext HTTP).
- **Slowloris defense** — per-connection idle/hard timeout (default 30s idle / 60s hard); connection closed on idle timeout via I/O→worker close deferral.
- **Relay rate-limit keyed on endpoint** — relay path rate-limiter keyed on the relay connection endpoint id (stable identity), not the spoofable wire `sender_id`.
- **Mode-aware relay routing gate** — `network_secure_mode()` predicate (`allow_secure && ca_cert_data != NULL`); secure mode requires `relay_verified=true` (signed-nonce challenge); default mode gates on Hebbian weight (reputation, not identity).

### Storage Durability

- **WAL replay on the happy path** — the live WAL is replayed when the newest snapshot is valid, recovering writes between the last snapshot and a crash.
- **Stop-and-keep-prefix WAL recovery** — a torn tail or CRC mismatch loses only the bad record, not the whole index; `cbor_to_index_entry` shape validation; unknown-type `default` in `wal_read` (no UB).
- **fsync ordering** — `fsync_data` config flag (default true); section data fsync after pwrite; section meta fsync; snapshot fsync in `index_debounce` (write-return checked); old-WAL fsync before destroy.
- **Atomic peer-state write** — `platform_file_atomic_write` (temp + fsync + rename + parent-dir fsync).

### HTTP Hardening

- **`max_connections` default 1024** (was 0 = unlimited).
- **Streamed PUT bounded** — `stream-length` upper cap (`OFFS_MAX_STREAM_LENGTH`); `tuple-size` enforced against `max_tuple_size`; actual-bytes tracking in the streaming data handler.
- **Removed unused `api_key` copy** in `auth_middleware`; removed the `_off_post_handler` stub.

### Memory Safety

- `timer_actor_create` detaches from the pool registry on early failure (no dangling pointer in the scheduler recovery scan).
- `buffer_ensure_capacity` checks `realloc` result (abort on OOM, consistent with `get_memory`/`get_clear_memory`).
- `_pool_global_init_once` frees the losing-race mutex (was leaked).
- `scheduler_pool_wait_for_idle` returns `int` (abort→error return; callers compile-clean ignoring the return).
- `_timer_completion_callback` lookup-before-deref under `loop_lock` (prevents UAF on concurrent teardown).

### CI

- Added `.github/workflows/ci.yml` — build matrix (gcc + clang, Debug + Release) + test (`testliboffs`, 842 tests).
