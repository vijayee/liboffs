# Changelog

## [Unreleased] - 2026-08-24

### Security

- **Content integrity at receive boundary** — every peer-supplied block is re-hashed (BLAKE3) and compared against the requested address; mismatches are rejected and the supplier's Hebbian weight is penalized.
- **Peer reputation system** — Hebbian learning weights drive routing; bad blocks cost the supplier rate-limit tokens + Hebbian penalty.
- **Peer-state persistence** — Hebbian weights + connected-peer info survive restart via atomic write to `peer_state.cbor` (v3 format); TTL filtering drops stale peers on load.
- **Signed release manifests** — self-update verifies an ed25519 signed manifest against a compile-in release public key (`OFFS_RELEASE_PUBKEY`, fail-closed); per-file SHA256 from the verified manifest; in-process path-contained USTAR extraction; verified staged updater binary.
- **TLS certificate verification** — update check + download verify the server's TLS certificate against the system CA store.
- **Local-binding auth optional but default-on** — bearer tokens required for `/config` mutations even on loopback by default; `config_local_binding_no_auth` flag (default false) opts back in.
- **Bearer-requires-TLS** — validator rejects `http_enabled && !https_enabled && api_key_hash`.
- **Slowloris defense** — per-connection idle/hard timeout (default 30s/60s).
- **Relay rate-limit keyed on endpoint** — rate-limiter keyed on the stable relay connection endpoint id, not the spoofable wire `sender_id`.
- **Mode-aware relay routing gate** — `network_secure_mode()` predicate; secure mode requires `relay_verified`; default mode gates on Hebbian weight.

### Storage Durability

- **WAL replay on the happy path** — recovers writes between the last snapshot and a crash.
- **Stop-and-keep-prefix WAL recovery** — torn tail or CRC mismatch loses only the bad record, not the whole index.
- **fsync ordering** — `fsync_data` config flag (default true); section data + WAL + snapshot fsync in the correct order.
- **Atomic peer-state write** — `platform_file_atomic_write` (temp + fsync + rename).

### HTTP Hardening

- **`max_connections` default 1024** (was 0 = unlimited).
- **Streamed PUT bounded** — `stream-length` upper cap; `tuple-size` enforced; actual-bytes tracking.
- **Removed unused `api_key` copy** + `_off_post_handler` stub.

### Memory Safety

- `timer_actor_create` detaches on early failure.
- `buffer_ensure_capacity` checks `realloc` result.
- `_pool_global_init_once` frees the losing-race mutex.
- `scheduler_pool_wait_for_idle` returns `int` (abort→error).
- `_timer_completion_callback` lookup-before-deref under `loop_lock`.

### CI

- Added `.github/workflows/ci.yml` — build matrix (gcc + clang, Debug + Release) + test (842 tests).
