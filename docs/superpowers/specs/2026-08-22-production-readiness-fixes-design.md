# Production Readiness Fixes — Design

**Date:** 2026-08-22
**Status:** Approved (brainstormed with user, 2026-08-22)
**Scope:** liboffs + the sibling OFFS daemon repo (`/home/victor/Workspace/src/github.com/vijayee/OFFS`)
**Supersedes/augments:** `docs/PRODUCTION_BLOCKERS.md` (2026-05-22)

## 1. Threat Model and Trust Architecture

This fix pass supports **two coexisting trust modes**, selected by operator configuration:

**Default (insecure) mode — `allow_secure=false`, no authority configured.**
> TLS is for encryption, not identity. Peers remain anonymous. Trust is established by **content verification at the receive boundary** plus **peer reputation** — not by certificate-verified identity.

- `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION` stays in place. Connections are encrypted, never authenticated. This is the default and is acceptable for trusted LANs.
- Content integrity (audit CRITICAL #1) is the load-bearing trust mechanism: every block received from a peer is re-hashed; mismatches are rejected and the supplier's reputation is penalized.
- Relay-admitted peers (no identity proof available in this mode) are gated by **reputation**: they start at low Hebbian weight and must earn routing weight by serving verified blocks (Section 7.3).

**Secure mode — `allow_secure=true` with an authority (CA + node key) configured.**
- TLS peer certificates are validated against the CA on the direct-QUIC path (already implemented).
- The **signed-nonce relay challenge** (`WIRE_RELAY_CHALLENGE` / `WIRE_RELAY_CHALLENGE_RESPONSE`, fully implemented at `network.c:1237/1445/1539`) provides relay-path identity: the challenger sends a nonce, the responder signs it with its private key, the challenger verifies `BLAKE3(public_key)==responder_id` and the signature, then sets `peer->relay_verified=true`. This flow is **kept and completed** — what is missing is the *gating* (routing currently proceeds before verification completes). Section 7.3 adds mode-aware gating: in secure mode, relay-admitted peers are not routed until `relay_verified=true`.
- Content integrity still applies in secure mode as defense-in-depth.

**Shared consequences (both modes):**
- The audit's CRITICAL #2 ("TLS peer auth off by default") is addressed by **supporting and documenting secure mode** and recommending it for public-internet exposure, while leaving the default as encryption-only so existing trusted-LAN deployments are not broken. The README disclosure is retained and updated with the secure-mode guidance.
- The audit's HIGH #3 ("relay-admitted peers join routing without identity proof") is addressed by **mode-aware gating** in Section 7.3 — not by dropping the signed-nonce flow. The signed-nonce flow is vital in secure mode and stays.
- The Hebbian weight table (`src/Network/hebbian.{c,h}`) **is** the reputation system — these are the project's "SOPPSON numbers". Per-peer directed weights with frequency/feedback/symmetry learning rules, decay, and an eviction cap. Routing gates (`find_block.c:301`, `closest_nodes.c:289`) and the connection manager drop (`connection_manager.c:267`) already consume the weight via `drop_threshold`. The work is wiring, not new machinery.

## 2. Findings Addressed

Every CRITICAL, HIGH, MEDIUM, and LOW finding from the 2026-08-21 production-readiness audit is addressed. CRITICAL #2 is addressed by supporting and documenting secure mode (and recommending it for public exposure) rather than by forcing it as the default. The findings map to the five implementation stages in Sections 4-8.

## 3. Non-Goals

- Forcing `allow_secure=true` as the default. The default stays encryption-only for backward compatibility; secure mode is opt-in and documented.
- A separate key server for update signing keys. The release public key is compiled into the binary (fail-closed if unset).
- ABI/soname versioning. The version bump to `v0.2.0` is a tag/changelog concern; symbol versioning is a future task.

## 4. Stage 1 — Content Integrity + Peer Reputation

### 4.1 Content verification

Add to `src/BlockCache/block.{c,h}`:

```c
bool block_verify_hash(const buffer_t* data, const buffer_t* hash);
```

Computes BLAKE3 over `data->data` / `data->size` (reuse `hash_data`), constant-time-compares against `hash->data` / `hash->size`. Returns `true` on match.

Call sites:

- `src/Network/network.c:~2857` (FindBlockResponse handler), `~3080` (StoreBlock handler), `~3786` (RecallAccept handler) — verify the recomputed hash against the **requested** hash (the address the local node asked for), not the peer-supplied one.
- `src/BlockCache/block_cache.c:~466, ~552` (cache read path) — verify the recomputed hash against the stored `entry->hash`.

On mismatch at a network receive site: do not construct the `block_t`, do not cache, increment `bad_blocks_received` in the metrics registry, treat the supplier as a bad-block source (Section 4.2). On mismatch at the cache read path: return a hash-mismatch error to the caller (audit finding #3); do not return corrupt data as valid.

### 4.2 Reputation wiring

On a **verified** block received from a peer:

- Apply the existing `hebbian_apply_success(&network->hebbian, response->path, response->path_len, latency_ms, HEBBIAN_FIND_BLOCK_MULTIPLIER)` (already present at `network.c:2795`).
- **New:** also apply `hebbian_frequency(&network->hebbian, &supplier, config->base_reward * rpc_multiplier)` — wiring the currently-unused `base_reward` config field.
- Call `network_sync_hebbian_to_rings(network)` (already present).

On a **bad** block (hash mismatch) from a peer:

- Apply `hebbian_frequency(&network->hebbian, &supplier, -config->failure_penalty * config->bad_block_multiplier)`. The `failure_penalty` field already exists (default 0.2) but is currently unused; this wires it.
- Charge the peer's `RPC_TYPE_FIND_BLOCK` token bucket an extra `config->bad_block_rate_cost` tokens via a new helper:
  ```c
  void rate_limit_charge(rate_limit_table_t* table, const node_id_t* peer_id,
                          rpc_type_e type, float cost);
  ```
  This consumes tokens (drives the peer toward rate-limit exhaustion) without granting any request.
- Record a new message-log outcome code `4 = bad_block` in `message_log_record` (`src/Network/message_log.c`).
- The existing `connection_manager.c:267` check drops peers whose weight falls below `drop_threshold` — **no new gating code** is needed for ejection.

### 4.3 Bootstrap and node_id rotation

New peers enter the Hebbian table at `initial_weight = 0.1`. Routing min-weight gates (`FIND_BLOCK_MIN_WEIGHT`, `CLOSEST_NODES_MIN_WEIGHT`) already skip peers below that floor. Therefore a peer that rotates `node_id` to shed a bad reputation restarts at 0.1 and is **unroutable** until it serves verified blocks. The "slow to earn" property falls out of the existing constants; no new persistence or bootstrap machinery is required.

### 4.4 Config surface

Add to `hebbian_config_t` (`src/Network/hebbian_config.h`) and the `Configuration` module's validated tunables:

| Field | Default | Purpose |
|-------|---------|---------|
| `bad_block_multiplier` | 5.0 | Bad block is far costlier than a generic RPC failure |
| `bad_block_rate_cost` | 10.0 | Extra tokens charged to the supplier's FIND_BLOCK bucket |

Wire the two existing-but-dead fields (`failure_penalty`, `rate_limit_penalty`) into actual call sites — `failure_penalty` per 4.2, `rate_limit_penalty` applied on any rate-limit rejection event.

### 4.5 Peer state persistence

Reputation and peer knowledge are only useful if they survive restart. Persist the Hebbian weight table and connected-peer information to disk and reload on startup.

**What is persisted** (per peer, in `peer_state_t`):

| Field | Source | Purpose on reload |
|-------|--------|-------------------|
| `node_id` | `peer_connection_t.remote_node_id` | Primary key |
| `addr`, `port` | `peer_connection_t.peer_addr` | Reconnect target |
| `hebbian_weight` | `hebbian_table_t` entry | Reputation survives restart |
| `relay_verified` | `peer_connection_t.relay_verified` | Secure-mode routing gate survives restart |
| `nat_type` | `peer_connection_t.nat_type` | Reconnect strategy (direct vs relay-only) |
| `last_seen_ms` | monotonic clock at last interaction | Drop stale peers on reload |
| `bad_blocks_received` | new metric (Section 4.2) | Bad-block history survives restart |

Runtime-only fields (`quic_connection`, `quic_stream`, mailbox state, token-bucket levels) are **not** persisted — they are re-established on reconnect.

**Format and location.** CBOR array of `peer_state_t`, written to `{data_dir}/peer_state.cbor`. CBOR is already a dependency and used by the BlockCache index/WAL.

**When to save.**
- Debounced: reuse the existing `hebbian_decay` timer tick (default 60s) — after each decay pass, write the file if the table has changed since the last save. Cheap, bounded.
- On graceful shutdown: `src/Node/node.c` phased drain writes a final copy before the network tears down.
- Not on every weight change (would be too frequent).

**Atomic write.** Write to `peer_state.cbor.tmp`, `platform_file_sync`, then `rename` over `peer_state.cbor`. A crash mid-write leaves the previous file intact. Check the write return (propagate ENOSPC/I/O errors to the log; do not silently drop).

**Load on startup.** Before the network accepts connections, read `peer_state.cbor`. Treat the file as hostile input: validate CBOR shape per entry (same discipline as Section 5.3), skip malformed entries. Populate the Hebbian table and a "known peers" reconnection list. Drop entries whose `last_seen_ms` is older than a configurable `peer_state_ttl` (default 7 days). Re-establish connections to the surviving peers via the existing peer-discovery/reconnect path; do not block startup on unreachable peers.

**Decay continuity.** Because weights are persisted with their current value and decay runs on the timer, decay continues naturally across restart — no catch-up computation needed.

**New config tunables** (added to `Configuration`):

| Field | Default | Purpose |
|-------|---------|---------|
| `peer_state_ttl_ms` | 604800000 (7 days) | Drop peers not seen for this long on reload |
| `peer_state_save_interval_ms` | 60000 | Debounced save cadence |

### 4.6 Tests

- `test_block.cpp`: round-trip `block_verify_hash` (good data → true); tampered data (single bit flipped → false); wrong-size hash → false; constant-time compare sanity.
- `test_network.cpp`: mock peer serves a wrong block → assert block rejected, supplier weight decreased by `failure_penalty * bad_block_multiplier`, rate-limit bucket charged `bad_block_rate_cost`, and after N bad blocks the peer drops below `drop_threshold` and is not routed to.
- `test_health_handler.cpp` / `test_health_http.cpp`: assert the new `bad_blocks_received` metric is exposed in `/health`.
- `test_peer_state.cpp` (new): write weights + peer info, reload, assert restored; a bad peer remains below `drop_threshold` after reload and is not routed; atomic write (kill mid-write → previous file intact, no corruption); malformed entry skipped, rest loaded; `peer_state_ttl` drops stale entries; graceful-shutdown save survives a simulated restart.

## 5. Stage 2 — Storage Durability

### 5.1 WAL replay on the happy path

`src/BlockCache/index.c:436-444` (the success branch of `index_create`): after a valid latest snapshot, replay the **current** WAL (paired with that snapshot id) into the in-memory index before returning, reusing the same replay loop already used for the invalid-snapshot branch. Re-enable `DISABLED_TestWalCrashRecovery` at `test/test_index.cpp:290` as the regression gate, with a variant that writes blocks after the snapshot debounce, kills the process, and asserts the post-snapshot writes survive.

### 5.2 fsync ordering

The durability contract is **data → WAL → snapshot**.

- `SECTION_WRITE` (`src/BlockCache/section.c:419-472`): after `platform_file_pwrite` of block bytes, call `platform_file_sync(section->file)` before marking the entry durable in the WAL. Add a `fsync_data` config flag (default on) so ASAN/valgrind test builds can disable it for speed (approved by user).
- `block_cache_sync` already fsyncs the WAL; ensure it is called **after** the section data fsync, not before.
- `index_debounce` (`src/BlockCache/index.c:1131-1141`): `platform_file_sync` the snapshot file after writing, **check the `platform_file_write` return** (propagate short-write/ENOSPC as an error and leave the old WAL intact rather than destroying it), then destroy the old WAL.
- `section_save_meta` (`src/BlockCache/section.c:853-868`): fsync the free-map write; a stale free-map on crash either overwrites live data or leaks slots.

### 5.3 Recovery-path input validation

Treat on-disk state as hostile.

- `wal_read` (`src/BlockCache/wal.c:111-124`): add a `default:` branch that returns an error instead of using uninitialized `size`; reject unknown type bytes.
- `cbor_to_index_entry` (`src/BlockCache/index.c:96-114`): validate the CBOR array has exactly 5 elements and each has the expected type before reading; return an error (skip the record) on any mismatch. Apply the same shape-validation discipline to every other recovery-side deserializer that currently calls `cbor_array_get` unconditionally.
- `round_robin_save` (`sections.c:699-706`): propagate `fwrite`/`fflush` errors; add `fsync`.

### 5.4 Tests

- Re-enabled `TestWalCrashRecovery` and the post-snapshot variant.
- Torn data write: mock a short `pwrite` → assert cache read returns a hash-mismatch error, not corrupt data.
- ENOSPC on snapshot write: assert old WAL preserved and in-memory state not lost.
- Malformed WAL type byte: assert recovery skips the record and continues.
- Malformed CBOR index entry: assert recovery skips and continues.
- Defrag mid-write crash: assert no slot double-allocation.
- Run the full BlockCache suite under valgrind (DWARF-4 build); confirm zero leaks.

## 6. Stage 3 — Self-Update Supply Chain

### 6.1 TLS verification

In `src/Update/update_check.c:166` and `update_download.c:185`, after `SSL_CTX_new`:

```c
SSL_CTX_set_default_verify_paths(ctx);
SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
SSL_CTX_set_verify_depth(ctx, 4);
```

Set `SSL_set_tlsext_host_name(ssl, hostname)` so SNI + hostname verification fires. Treat `SSL_get_verify_result() != X509_V_OK` as a hard failure. No pinning — system CA store is sufficient once signature verification (6.2) is in place.

### 6.2 ed25519 signed manifest

New tool `offs-release-sign` (in `OFFS/src/`) and a verify routine in liboffs `src/Update/update_verify.{c,h}`.

**Manifest format** (CBOR):

```
{
  version: 1,
  release_tag: "...",
  files: [{ path: "...", sha256: <32 bytes>, size: <uint> }, ...],
  signing_key_id: "..."
}
signature: ed25519_sig  // over the canonical CBOR of {version, release_tag, files}
```

**Sign:** release operator runs `offs-release-sign --key <ed25519_privkey> --release <tag> --files dist/*` → writes `manifest.cbor` alongside the release. Private key held offline by the release operator; never bundled.

**Verify:** a single ed25519 public key compiled into the binary via `update_verify_load_pubkey()` — the key path is configured at build time with `-DOFFS_RELEASE_PUBKEY=<path>` and embedded as a constant. **Fail-closed:** if the key is unset at build time, the updater refuses to run (compile-time error or a runtime guard that returns "no release key configured"). The updater:

1. Fetches `manifest.cbor` over the now-verified TLS.
2. Parses it; verifies the ed25519 signature over the canonical CBOR of `{version, release_tag, files}` against the embedded public key. Mismatch → abort, staging left empty.
3. Fetches each listed file; re-hashes it; compares to the manifest's `sha256`. Mismatch → abort.
4. Only then proceeds to extraction.

Crypto: OpenSSL EVP ed25519 (already a dependency). `offs-ca` already does ed25519 keygen, so the signing key pair is generated with existing tooling and the public key extracted for embedding.

### 6.3 Path-contained extraction

Replace `execlp("tar", ...)` at `src/Update/update_download.c:345` with an in-process extractor `src/Update/update_extract.c` that:

- Walks the tar entries.
- Resolves each member path against `staging_dir` via a realpath containment check: reject any entry whose resolved path escapes `staging_dir`, any absolute path, any symlink whose target resolves outside `staging_dir`, and any `..` components.
- Caps total extracted size against a configured limit (`max_extract_bytes`, default 1 GiB).

Removes the `tar` external dependency and the traversal hole.

### 6.4 Verified updater binary

`src/Update/update_actor.c:160`: replace `execlp("offs-updater", ...)` with:

- An absolute path computed at install time (CMake-configured `OFFS_UPDATER_PATH`).
- The updater binary is itself one of the manifest's listed files: its SHA256 is verified against the manifest **before exec**.
- `fork` failure (`pid < 0`) handled; the parent waits briefly for the child's exec-success signal via a pipe rather than `exit(0)`-and-pray.

### 6.5 SHA256 mandatory

Drop the `if (info->sha256[0] != '\0')` opt-out at `update_download.c:393`. The hash is **always** required and **always** sourced from the verified manifest — never from the release-notes markdown. Remove the release-notes `"sha256:"` parsing path entirely.

### 6.6 Tests

`test_update_verify.cpp` and `test_update_extract.cpp`:

- Good manifest passes.
- Tampered file → fails.
- Tampered signature → fails.
- Wrong pubkey → fails.
- Truncated manifest → fails.
- Traversal tarball (`../../etc/passwd` entry) → rejected.
- Symlink-escape tarball → rejected.
- Oversized tarball → rejected.
- Missing pubkey at build → fail-closed.
- Full fetch-over-mock-HTTPS integration test in `test/`.

## 7. Stage 4 — HTTP Hardening, Memory Safety, Relay/Gossip

### 7.1 HTTP hardening (`src/ClientAPI/HTTP/`)

- **Slowloris/idle timeout.** `http_connection.c`: arm a per-connection timer on accept; reset on each read; fire after a configurable idle timeout (default 30s) and a hard request timeout (default 60s) → close the connection. Reuse `timer_actor` or a cheap deadline field checked on the watcher tick.
- **Finite default `max_connections`.** `http_server.c:111`: default 1024 (configurable). Example server sets it explicitly.
- **Bearer tokens require TLS.** `config.c:222-237`: extend the validator to reject `http_enabled && !http_tls_enabled && api_key_hash != NULL` (bearer over plaintext HTTP) and `http_enabled && api_key_hash != NULL` on non-loopback bindings. Loopback-only plaintext-with-auth stays allowed.
- **Bound streamed PUT.** `off_routes.c:956-961`: pre-flight capacity check uses actual bytes received so far (or a hard cap), not the client-declared `stream-length`. Enforce `max_tuple_size` at `off_routes.c:773-776` and `:941-944` (the comments admit it's unenforced — implement the bound).
- **Local-binding auth.** `config_routes.c:29`: keep the local-binding bypass but restrict `/config` PUT + `/config/restart` to loopback **only** — refuse on any non-loopback binding even if `is_local_binding` was set. Read-only `/config` GET stays allowed locally.
- **Remove unused `api_key` copy** in `auth_middleware.c:27`.
- **Remove** the `_off_post_handler` stub at `off_routes.c:1041` and its route registration. The POST semantics for temporary-block management are not defined in this pass; re-add the route when concrete semantics are designed. No half-implemented handlers, no TODOs (per CLAUDE.md).
- **Re-investigate the NULL-buffer heap-corruption root cause.** Time-boxed debug pass: ASAN + a stress test that reproduces the historical crash. If the root cause is found, fix it. If not found in this cycle, restore an explicit guarded sentinel (a `buffer->data == NULL` check at the entry of `_on_body`, `_put_on_request_data`, and `http_response_pipe`) and document the open question in `docs/OPERATIONS.md`.

### 7.2 Memory-safety fixes

- `timer_actor_create` (`src/Timer/timer_actor.c:321-328`): on `pd_loop_create` failure, call `actor_detach_pool` before `free`. Check `platform_mutex_create` / `platform_thread_create` returns; clean up on failure.
- `buffer_ensure_capacity` (`src/Buffer/buffer.c:39`): check `realloc` result; keep the old pointer and return an error on failure (do not overwrite with NULL).
- `_pool_global_init` (`src/Actor/pool.c:42-48`): free the losing-race mutex.
- `scheduler_pool_wait_for_idle` (`src/Scheduler/scheduler.c:420-428`): replace `abort()` with an error return + log; caller decides.
- `_timer_completion_callback` (`src/Timer/timer_actor.c:109-125`): document the poll-dancer stop-implies-drain contract in-tree; if the contract cannot be verified, hold a per-`timer_actor` rwlock around the back-pointer deref.
- `_deque_grow` (`src/Scheduler/deque.c:24-31`): the "retired arrays never freed" comment is acknowledged; leave as-is for this pass but file a follow-up ticket. (Low severity, bounded per worker.)

### 7.3 Relay/gossip (`src/Network/`)

- **Relay rate-limit keying.** `network.c:5230-5241`: key the relay-path rate limiter on the authenticated relay endpoint id (the relay connection), not the spoofable wire `sender_id`. Frame-attack on a victim's `node_id` becomes impossible.
- **Per-source gossip cap.** Bound insertions from a single gossip source per message (cap at 32 or `RING_MAX_RINGS / N`, whichever is smaller). Apply a small Hebbian penalty to peers that advertise unreachable nodes — the referral penalty wires into the existing reputation system (bad referrals cost the referrer).
- **Mode-aware relay routing gate** (completes the signed-nonce flow). The signed-nonce challenge mechanism (`WIRE_RELAY_CHALLENGE` / `WIRE_RELAY_CHALLENGE_RESPONSE`, `network.c:1237/1445/1539`) is **already implemented end-to-end** — what is missing is the gating that refuses to route until verification completes. Add mode-aware gating at `network.c:5164-5207` and the routing entry points (`find_block.c:301`, `closest_nodes.c:289`):
  - **Secure mode** (authority configured, `allow_secure=true`): relay-admitted peers (`relay_verified=false`) are **not routed** until `relay_verified=true` is set by the signed-nonce response handler. The challenge is sent on admission (already the case); unanswered challenges are swept (already the case). This is the identity-gated path and is vital when an authority is configured.
  - **Default/insecure mode** (no authority): identity verification is unavailable, so relay-admitted peers are gated by **reputation** instead — they start at `weight=HEBBIAN_MIN_WEIGHT` (0.01), below `FIND_BLOCK_MIN_WEIGHT`, and are not routed until their Hebbian weight crosses the routing min-weight gate by serving verified blocks. Consistent with the bootstrap in Section 4.3.
  - The gate reads the mode from `network->authority` (or equivalent): if an authority/private key is loaded, use the `relay_verified` gate; otherwise use the Hebbian-weight gate. Both gates apply only to relay-admitted peers; direct-QUIC peers (already `relay_verified=true` via the salutation path, `network.c:855`) route normally.
  - Do **not** drop the signed-nonce flow. Do **not** remove the deferred-gating comment until the gating is implemented and the comment becomes obsolete.

### 7.4 Tests

- `test_http_server.cpp`: slowloris connection closed after idle timeout; `max_connections` refusal; bearer-over-plaintext rejected by validator; `/config` PUT refused on non-loopback.
- `test_network.cpp`: relay rate-limit keyed on endpoint (varying `sender_id` does not dodge the bucket); per-source gossip cap enforced; **secure mode** — relay-admitted peer not routed until `relay_verified=true` (signed-nonce response received and verified); **insecure mode** — relay-admitted peer not routed until Hebbian weight crosses the gate.
- Memory-safety fixes: inject `realloc` failure via a test allocator hook → assert no crash, no leak.

## 8. Stage 5 — Engineering, CI, Packaging, Daemon (liboffs + OFFS)

### 8.1 CI

Add `.github/workflows/ci.yml` to liboffs and OFFS:

- Build matrix: Ubuntu (gcc + clang), Windows (MSVC via existing `scripts/`), macOS (clang). Release + Debug.
- ASAN + UBSan build of `testliboffs` on Linux; valgrind run on the `build-gdwarf4` tree (DWARF-4 per the memory note).
- Run `ctest` / `testliboffs`; fail on any test failure or leak.
- `clang-format` check (add `.clang-format` matching 2-space Egyptian style) + `clang-tidy` on `src/` with a curated checks list (baseline-gated; no full clean required first pass).
- CMake configure with `-DOFFS_RELEASE_PUBKEY=` unset on a "no-auto-update" build to confirm fail-closed behavior compiles.

### 8.2 systemd unit fix (`OFFS/packaging/linux/.../offs-daemon.service`)

Add `--foreground` to `ExecStart`; keep `Type=simple` (matches systemd best practice for daemons with signal-handled main loops). Remove the in-process double-fork when `--foreground` is set (already skipped). Keep `Restart=on-failure`, `RestartSec=10`, journal for stdout/stderr. No `PIDFile=` needed under `Type=simple`.

### 8.3 Logging to file

`OFFS/src/offsd/main.c` and `src/Util/log.{c,h}`: add `--log-file <path>` (and a `log_file` config key) that calls `log_add_fp`. When daemonized without `--foreground` and no `--log-file`, refuse to start with an explicit error — do not silently drop logs to `/dev/null`. Default to journal when `--foreground` under systemd.

### 8.4 Packaging/release hygiene

- Commit the `.gitignore` changes; remove the committed 1.4 MB `test.pdf` / `test2.pdf` at repo root. Move test fixtures into `test/fixtures/` if any test references them (verify none do first).
- Track `offs.pc.in`, `test/cross-region-test.sh`, `test/cross-region-vm-test.sh`.
- Review the 40+ modified working-tree files against the audit; commit or discard deliberately, not silently.
- Tag `v0.2.0` (breaking config changes warrant a minor bump); write `CHANGELOG.md` covering the audit fixes. Add `CONTRIBUTING.md` pointing at `STYLE_GUIDE.md` + CI gates.

### 8.5 Test coverage gaps

Add unit tests for the three untested modules: `test_bloom.cpp`, `test_metrics.cpp`, `test_update.cpp` (the update-verify + extract tests from Section 6 land here). Add a fuzz target for the CBOR/wire decode path (libFuzzer harness around `wire_decode_dispatch`, gated behind `OFFS_ENABLE_FUZZING`).

### 8.6 Documentation

- Write `docs/OPERATIONS.md`: deployment runbook (CA provisioning with `offs-ca`, config for internet vs LAN exposure, log management, restart behavior, the new `bad_blocks_received` metric and alert guidance, update/rollback).
- Update `README.md`: secure defaults, the signed-manifest update flow, the reputation system.
- Add `docs/RELEASE.md`: release-signing process (key custody, `offs-release-sign` usage, manifest publishing).

### 8.7 No TODOs

Per CLAUDE.md, this pass resolves every `TODO`/`FIXME`/`HACK`/`XXX` in the files it touches — including the admitted-stub comments in `off_routes.c` (tuple-size bound, POST handler). The deferred-gating comment near `network.c:5172-5178` is resolved by **implementing** the mode-aware gate in Section 7.3 (the comment then becomes obsolete and is removed or replaced with a description of the gate). The `_deque_grow` "retired arrays never freed" comment is filed as a follow-up ticket rather than left as an in-tree TODO.

### 8.8 De-wonk + leak check

Per CLAUDE.md, run the de-wonk skill on completion and run the full test suite under valgrind to confirm zero leaks. The memory note says all known leaks are fixed; this pass must not regress that.

## 9. Open Questions for Implementation

None. All design decisions are resolved:

- **Scope:** everything, all severities.
- **Update signing:** ed25519 signed manifest, embedded pubkey via CMake, fail-closed.
- **TLS default:** stays `false` — encryption, not identity in default mode; secure mode opt-in via authority (closed by threat model).
- **Peer rating:** wired into the existing Hebbian + rate-limit infrastructure; no new reputation subsystem.
- **Peer state persistence:** Hebbian weights + connected-peer info persisted to `peer_state.cbor`, debounced save + graceful-shutdown save, atomic write, hostile-input-validated load with `peer_state_ttl`.
- **systemd unit:** `--foreground` + `Type=simple`.
- **Version bump:** `v0.2.0`.
- **fsync in tests:** disabled via flag for speed (approved).
- **NULL-buffer root cause:** time-boxed debug pass, restore sentinel if not found.

## 10. Implementation Order

Stages 1-5 are ordered by dependency and risk:

1. **Stage 1** (content integrity + reputation, Sections 4.1-4.4) — unblocks nothing else; highest value; small blast radius.
2. **Stage 1 persistence** (Section 4.5) — follows Stage 1 so the persisted weights are meaningful; independent of other stages.
3. **Stage 2** (storage durability) — independent of Stage 1; can land in parallel.
4. **Stage 4.2** (memory-safety fixes) — small, independent; land early to de-risk the rest.
5. **Stage 3** (self-update) — independent; larger; needs the release-signing tooling.
6. **Stage 4.1, 4.3** (HTTP + relay/gossip) — depends on Stage 1's reputation wiring for the gossip-referral penalty and the mode-aware relay gate.
7. **Stage 5** (CI/packaging/daemon) — land last so CI gates the final tree; the `v0.2.0` tag is the closing act.

The writing-plans skill will break each stage into concrete, testable tasks.