# HTTP Hardening, Memory Safety, Relay/Gossip Gating Implementation Plan (Stage 4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden the HTTP API, close the memory-safety gaps in the actor/scheduler/buffer/timer core, and add mode-aware gating to the relay/gossip paths — closing the remaining code-level security and robustness findings from the production-readiness audit.

**Architecture:** HTTP: a per-connection idle/hard timeout via a poll-dancer timer; a finite `max_connections` default; bearer tokens required over TLS; the streamed PUT bounded against real bytes + `max_tuple_size`; the local-binding auth bypass made **optional but default-on** via a `config_local_binding_no_auth` flag (default false → bearer required for config mutations even on loopback; true → restore the loopback no-auth behavior for environments that need it). Memory-safety: `actor_detach_pool` on `timer_actor_create` early failure; `realloc` NULL-check in `buffer_ensure_capacity` (abort on OOM, consistent with the project's `get_memory` allocator); free the losing-race mutex in `_pool_global_init`; replace `abort()` with an error return in `scheduler_pool_wait_for_idle`; verify the timer-callback lifetime contract. Relay/gossip: key the relay rate-limiter on the relay endpoint id; cap per-source gossip ring insertion + Hebbian penalty for unreachable referrals; a `network_secure_mode()` predicate (`authority->allow_secure && ca_cert_data != NULL`) gates relay-admitted routing — secure mode requires `relay_verified=true`; default mode gates on Hebbian weight (reputation, not identity).

**Tech Stack:** C11, poll-dancer (`pd_timer_t`/`pd_watcher_t`), OpenSSL, GoogleTest, the existing Hebbian/rate-limit/config/authority APIs.

**Spec:** `docs/superpowers/specs/2026-08-22-production-readiness-fixes-design.md` (Section 7). **Refinement:** local-binding auth is optional (via `config_local_binding_no_auth`) but default-on — the spec's "restrict to loopback + bearer-required" is the default; the flag opts back into the no-auth loopback mode.

---

## File Structure

**Modify:**
- `src/ClientAPI/HTTP/http_connection.{h,c}` — per-connection idle/hard timeout timer.
- `src/ClientAPI/HTTP/http_server.c` — `max_connections` default 1024.
- `src/ClientAPI/HTTP/auth_middleware.{h,c}` — `_auth_handler` local-binding-aware (gated by the flag); remove the unused `api_key` copy.
- `src/ClientAPI/HTTP/config_routes.c` — `/config` PUT + `/config/restart` refused on non-loopback; bearer required on loopback unless the flag is set.
- `src/ClientAPI/HTTP/off_routes.c` — bound `stream-length` + `tuple-size`; remove `_off_post_handler` stub + its route registration.
- `src/Configuration/config.{h,c}` — `config_local_binding_no_auth` flag (default false) + validation (bearer-requires-TLS).
- `src/Timer/timer_actor.c` — `actor_detach_pool` on early failure; verify/harden the completion-callback lifetime.
- `src/Buffer/buffer.c` — `buffer_ensure_capacity` realloc NULL-check (abort on OOM).
- `src/Actor/pool.c` — free the losing-race mutex.
- `src/Scheduler/scheduler.c` — `scheduler_pool_wait_for_idle` returns `int` (abort→error).
- `src/Network/network.{h,c}` — `network_secure_mode()`; relay rate-limit keyed on endpoint; per-source gossip cap + referral penalty; mode-aware relay routing gate.
- `src/Network/connection_manager.c` — mode-aware drop (relay_verified gating in secure mode).
- `test/test_http_server.cpp`, `test/test_network.cpp`, `test/test_config_validate.cpp` + new `test/test_http_hardening.cpp`, `test/test_relay_gossip_gating.cpp`.
- `test/CMakeLists.txt` — register new test files.

**Investigation (time-boxed):** the NULL-buffer heap-corruption root cause in `http_connection.c` (the historical crash; the 7 pre-existing `TestStream*` failures are the symptom).

---

### Task 1: Local-binding auth optional + default-on

**Files:**
- Modify: `src/Configuration/config.h` (add `bool config_local_binding_no_auth` near `api_key_hash`, line ~74)
- Modify: `src/Configuration/config.c` (default false in `config_default` ~line 64; no deep-copy handling needed for bool)
- Modify: `src/ClientAPI/HTTP/auth_middleware.c` — `_auth_handler` skips the bearer check on local binding **only when `config_local_binding_no_auth` is true**; the auth_middleware needs access to the flag (pass it into `auth_middleware_create` or store it on the server).
- Modify: `src/ClientAPI/HTTP/config_routes.c` — `/config` PUT + `/config/restart` refused on non-loopback; bearer required on loopback unless the flag is set.
- Modify: `src/ClientAPI/HTTP/off_routes.c` — pass the flag to `auth_middleware_create`.
- Test: `test/test_http_server.cpp` + `test/test_config_validate.cpp`.

- [ ] **Step 1: Add the config flag**

In `config.h` after `api_key_hash` (line ~74):
```c
  bool config_local_binding_no_auth;  // when true, skip bearer auth for config-mutation endpoints on loopback (default false — bearer required even on loopback)
```
In `config.c` `config_default` near line 64: `config.config_local_binding_no_auth = false;`

- [ ] **Step 2: Make `_auth_handler` local-binding-aware**

`auth_middleware_t` (`auth_middleware.c:17-20`) currently holds `api_key` + `bcrypt_hash`. Add a `bool allow_local_no_auth` field. `auth_middleware_create` takes a new `bool allow_local_no_auth` arg. In `_auth_handler` (line ~49), at the top:
```c
  if (auth->allow_local_no_auth && http_server_is_local_binding(request->server)) {
    request->is_authenticated = 1;  // local binding + opt-out flag → skip bearer
    return 0;  // (match the existing _auth_handler's "next()" success contract)
  }
```
(Confirm `_auth_handler`'s signature + how it accesses the server — read `auth_middleware.c:49-92`. The `request->server` field or the handler's user_data carries the server. If the middleware doesn't have the server, pass it via `auth_middleware_create` or look it up from the request.)

- [ ] **Step 3: Tighten `/config` PUT + `/config/restart`**

In `config_routes.c`:
- `_config_put_handler` + `_config_restart_handler`: on a **non-loopback** binding, refuse outright (403) — these are management endpoints, not for remote exposure. On **loopback**: require the bearer (i.e. rely on the global middleware) UNLESS `config_local_binding_no_auth` is true (in which case the middleware already skipped via Step 2). The existing `_config_check_auth` local-binding shortcut (`if (http_server_is_local_binding(server)) return 0;`) becomes: `if (http_server_is_local_binding(server) && ctx->local_no_auth) return 0;` — and add the non-loopback refusal.
- `_config_get_handler`: stays allowed on loopback (read-only); on non-loopback, require bearer (existing behavior).

Pass the flag from `config_routes_register` (it receives `const config_t* config`) → store it on `config_routes_ctx_t`.

- [ ] **Step 4: Wire the flag through `off_routes_register`**

`off_routes.c:1067-1074` creates the auth_middleware. Pass `config->config_local_binding_no_auth` to `auth_middleware_create`.

- [ ] **Step 5: Tests**

`test/test_http_server.cpp`:
- Local-bound server + `api_key_hash` set + `config_local_binding_no_auth = false` → no-bearer request to `/config` PUT returns 401 (bearer required even on loopback).
- Local-bound server + `api_key_hash` set + `config_local_binding_no_auth = true` → no-bearer request to `/config` PUT returns 200 (opt-out).
- Non-local-bound server (`0.0.0.0`) + `/config` PUT → 403 (refused, not just 401).
- `/config` GET on loopback → 200 (read-only, no bearer).

`test/test_config_validate.cpp`: `config_local_binding_no_auth` defaults false; validate accepts true/false.

- [ ] **Step 6: Build + run + commit**

```bash
cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='*HttpServer*:*ConfigValidate*'
```
Commit: `feat(http): local-binding auth optional but default-on (config_local_binding_no_auth)`

---

### Task 2: `max_connections` default 1024

**Files:**
- Modify: `src/ClientAPI/HTTP/http_server.c:111` — default 0 → 1024.
- Test: `test/test_http_server.cpp`.

- [ ] **Step 1: Change the default**

`http_server.c:111`: `server->max_connections = 0;` → `server->max_connections = 1024;`

- [ ] **Step 2: Test**

`test/test_http_server.cpp`: set `http_server_set_max_connections(server, 2)`, open 3 sockets, assert the 3rd is refused/closed. (Use the `_connect_to_server` helper.)

- [ ] **Step 3: Build + run + commit**

Commit: `feat(http): default max_connections to 1024 (was unlimited)`

---

### Task 3: Slowloris / idle timeout

**Files:**
- Modify: `src/ClientAPI/HTTP/http_connection.{h,c}` — add a `pd_timer_t* idle_timer` to `http_connection_t`; arm it on accept; re-arm on each read; fire on idle (default 30s) / hard (default 60s) → close.
- Modify: `src/Configuration/config.{h,c}` — `uint32_t http_idle_timeout_ms` (default 30000) + `http_hard_timeout_ms` (default 60000) tunables.
- Test: `test/test_http_hardening.cpp` (new).

- [ ] **Step 1: Add the tunables**

`config.h`: `uint32_t http_idle_timeout_ms;` + `uint32_t http_hard_timeout_ms;`. `config.c` `config_default`: 30000 + 60000. `config_validate`: both > 0.

- [ ] **Step 2: Add the idle timer to http_connection_t**

`http_connection.h`: add `pd_timer_t* idle_timer;` + `uint64_t hard_deadline_ms;` to `http_connection_t`. In `http_connection_create`, after arming the read watcher (~line 999), arm the idle timer:
```c
  connection->idle_timer = pd_timer_create(server->loop, _connection_idle_timer_callback, connection);
  if (connection->idle_timer != NULL) {
    pd_timer_start(connection->idle_timer, server->idle_timeout_ms);
  }
  connection->hard_deadline_ms = platform_now_ms() + server->hard_timeout_ms;
```
(Confirm `pd_timer_create`/`pd_timer_start` signatures in `deps/poll-dancer/include/poll-dancer/poll-dancer.h:227-256`. The server needs `idle_timeout_ms`/`hard_timeout_ms` fields — add them to `http_server_t` + set from config in `http_server_create` or a new setter.)

`_connection_idle_timer_callback`: close the connection (`_connection_stop_watcher` + `_connection_close_fd` + destroy). On each `_connection_read_callback` / `_connection_do_reads`, re-arm: `pd_timer_stop(connection->idle_timer); pd_timer_start(connection->idle_timer, server->idle_timeout_ms);` and check the hard deadline: `if (platform_now_ms() > connection->hard_deadline_ms) { close; }`.

- [ ] **Step 3: Test**

`test/test_http_hardening.cpp` (new, register in `test/CMakeLists.txt`): start a server with `http_idle_timeout_ms = 100` (fast for the test), open a socket, send `"GET /hello HTTP/1.1\r\nHost: localhost\r\n"` (no `\r\n\r\n`), then `recv` in a loop — assert the connection is closed (recv returns 0) within ~200ms. A normal request (full `\r\n\r\n`) still gets a 200.

- [ ] **Step 4: Build + run + commit**

Commit: `feat(http): per-connection idle/hard timeout (slowloris defense)`

---

### Task 4: Bearer-requires-TLS validator

**Files:**
- Modify: `src/Configuration/config.c:~239-255` — extend the `api_key_hash` validation block.

- [ ] **Step 1: Add the validation**

In the `if (config->api_key_hash != NULL)` block (~line 239), add:
```c
    if (config->http_enabled && !config->https_enabled) {
      log_error("http_enabled (plaintext) cannot be used with api_key_hash (bearer over plaintext HTTP)");
      valid = false;
    }
    if (config->http_enabled && config->https_enabled && /* non-loopback http binding */) {
      // If http_enabled is on a non-loopback binding with api_key_hash, reject.
      // (The binding host isn't in config_t — it's runtime. Document this as a
      // runtime check in off_routes_register instead, OR reject http_enabled
      // entirely when api_key_hash is set and rely on https_enabled.)
    }
```
Keep it simple: reject `http_enabled && !https_enabled && api_key_hash != NULL` (plaintext HTTP with bearer). Loopback-only plaintext-with-auth is a separate runtime concern (the local-binding auth flag handles it).

- [ ] **Step 2: Test**

`test/test_config_validate.cpp`: `api_key_hash` set + `http_enabled` + `!https_enabled` → invalid.

- [ ] **Step 3: Build + run + commit**

Commit: `feat(config): reject bearer tokens over plaintext HTTP`

---

### Task 5: Bound the streamed PUT

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c:~929-961` — the streaming PUT pre-flight.

- [ ] **Step 1: Bound stream-length + tuple-size**

In the streaming PUT headers-complete handler (~line 929): the current `if (stream_length == 0)` check has no upper bound. Add: `if (stream_length > OFFS_MAX_STREAM_LENGTH)` (define a cap, e.g. `max_capacity_bytes` or a fixed `OFFS_MAX_STREAM_LENGTH = 1ULL << 32`). Enforce `tuple_size` against `max_tuple_size` — the `off_routes_context_t` needs a `max_tuple_size` field (plumb from config, or from `block_cache->...`). Read the context struct to see what's available; if no `max_tuple_size` is accessible, add it to `off_routes_context_t` + plumb from the config/block_cache at `off_routes_register`.

The pre-flight space check (~line 953) uses `stream_length` (client-declared). Add a hard cap on actual bytes received in the streaming data handler (`_put_on_request_data`) — track bytes received + reject if they exceed `stream_length` (or a cap).

- [ ] **Step 2: Test**

`test/test_http_server.cpp` or `test_off_routes.cpp`: a streaming PUT with `stream-length: <huge>` → 413/500; a PUT with `tuple-size: <huge>` → 400.

- [ ] **Step 3: Build + run + commit**

Commit: `feat(http): bound streamed PUT stream-length + tuple-size`

---

### Task 6: Remove unused `api_key` copy + `_off_post_handler` stub

**Files:**
- Modify: `src/ClientAPI/HTTP/auth_middleware.c` — remove the `api_key` field + its strdup/zero/free.
- Modify: `src/ClientAPI/HTTP/off_routes.c:~1030-1046, 1083-1084` — remove `_off_post_handler` + its `http_server_post_with_data` registration.

- [ ] **Step 1: Remove the unused api_key**

`auth_middleware_t` (line 17-20): drop `char* api_key;`. `auth_middleware_create`: drop the `api_key` param + the strdup. `auth_middleware_destroy`: drop the api_key zero/free. Update callers (`off_routes.c:1067-1074`) to not pass `api_key`. (Task 1 already changes `auth_middleware_create`'s signature for the `allow_local_no_auth` flag — fold this in.)

- [ ] **Step 2: Remove the POST stub**

`off_routes.c`: remove `_off_post_handler` (line ~1030) + its `http_server_post_with_data(server, OFF_GET_PATTERN, _off_post_handler, ...)` registration (line ~1083). POST to OFF URLs now returns 405 (no route).

- [ ] **Step 3: Test + commit**

Commit: `chore(http): remove unused api_key copy + _off_post_handler stub`

---

### Task 7: Memory-safety fixes

**Files:**
- Modify: `src/Timer/timer_actor.c:~321-328` — `actor_detach_pool` on early failure.
- Modify: `src/Buffer/buffer.c:~39` — `buffer_ensure_capacity` realloc NULL-check (abort on OOM).
- Modify: `src/Actor/pool.c:~42-48` — free the losing-race mutex.
- Modify: `src/Scheduler/scheduler.c:~420-428` — `scheduler_pool_wait_for_idle` returns `int` (abort→error).
- Modify: `src/Timer/timer_actor.c:~109-125` — verify/harden the completion-callback lifetime (acquire `loop_lock`, look up `completion` before deref).
- Test: `test/test_network.cpp` / `test_http_server.cpp` (targeted tests for the allocator-failure paths where feasible).

- [ ] **Step 1: `timer_actor_create` detach-on-failure**

`timer_actor.c:321-328`: before `free(timer_actor)` on `pd_loop_create` failure, call `actor_detach_pool(&timer_actor->actor);`. Also NULL-check `platform_mutex_create`/`platform_thread_create` (lines ~329-332) + clean up on failure (detach + free).

- [ ] **Step 2: `buffer_ensure_capacity` realloc**

`buffer.c:39`:
```c
  void* new_data = realloc(buf->data, new_capacity);
  if (new_data == NULL) {
    abort();  // OOM — consistent with get_memory/get_clear_memory which abort on OOM
  }
  buf->data = new_data;
  buf->capacity = new_capacity;
```
(Decision: abort on OOM, matching the project's `get_memory` allocator. A return-code change would touch every caller — out of scope. Document the choice.)

- [ ] **Step 3: `_pool_global_init` losing-race**

`pool.c:42-48`: in the `else` (CAS failed) branch, `platform_mutex_destroy(m);` (the loser's mutex). Fix the misleading comment.

- [ ] **Step 4: `scheduler_pool_wait_for_idle` abort→error**

`scheduler.c:388`: change `void scheduler_pool_wait_for_idle(scheduler_pool_t* pool)` → `int scheduler_pool_wait_for_idle(scheduler_pool_t* pool)` (return 0 on success, -1 on stuck). At line ~425, replace `abort();` with `return -1;` (+ the existing `log_error`). Update callers (`http_server.c:251`, `node.c` shutdown phases) to handle the return (log on -1; the shutdown continues — the pool is stopped next anyway).

- [ ] **Step 5: `_timer_completion_callback` lifetime**

`timer_actor.c:109-125`: acquire `loop_lock`, look up `completion` in `active_timers`/`debounce_map` before derefencing `completion->timer_actor->actor`; drop the firing if not found. (Mirror the F8 re-check pattern at `timer_actor.c:242-288`.) If the poll-dancer contract can't be verified, hold a per-`timer_actor` rwlock around the back-pointer deref.

- [ ] **Step 6: Build + run + valgrind + commit**

```bash
cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='*HttpServer*:*Network*:*Timer*:*Buffer*:*Scheduler*'
```
Valgrind on the subset. Commit: `fix(core): memory-safety fixes (timer detach, realloc, pool race, scheduler abort, callback lifetime)`

---

### Task 8: NULL-buffer heap-corruption root-cause investigation (time-boxed)

**Files:**
- Investigation in `src/ClientAPI/HTTP/http_connection.c` + `src/Streams/` (the 7 pre-existing `TestStream*` failures are the symptom).

- [ ] **Step 1: Reproduce under ASAN**

The 7 `TestStream*` tests fail (pre-existing). Run them under ASAN: `cd build-asan && ./test/testliboffs --gtest_filter='TestPushFileStream.*:TestPullFileStream.*:TestStreamActor.*' 2>&1 | head -50`. ASAN should point at the corruption site (heap-buffer-overflow / use-after-free / etc.).

- [ ] **Step 2: Fix the root cause OR restore the sentinel**

If ASAN identifies the root cause, fix it. If not found within a time box (the investigation is open-ended), restore an explicit `buffer->data == NULL` guard at the entry of `_on_body`, `_put_on_request_data`, and `http_response_pipe` (the historical defensive sentinel) + document the open question in `docs/OPERATIONS.md`. Per the spec, this is acceptable as a fallback.

- [ ] **Step 3: Re-run the 7 stream tests**

Confirm the 7 `TestStream*` tests pass (or at least the ASAN error is resolved). Commit: `fix(http): NULL-buffer heap-corruption root cause` (or `fix(http): restore NULL-buffer sentinel (root cause TBD)`).

---

### Task 9: Relay rate-limit keyed on endpoint

**Files:**
- Modify: `src/Network/network.c:~5398-5416` — key the relay rate-limit on the relay endpoint id, not the spoofable wire `sender_id`.

- [ ] **Step 1: Key on the endpoint**

At `network.c:5398-5416`, the rate-limit check uses `rl_sender` (the wire sender_id). Replace the key with `relay_payload->src_endpoint_id` (the relay connection endpoint — not spoofable). The `network_rate_limit_check` signature takes a `node_id_t*` key — the endpoint id is a `uint32_t`. Either: (a) change the rate-limit key to a generic `uint64_t` (wide change), or (b) keep the `node_id_t` key but derive a stable node_id from the endpoint (hash the endpoint id into a node_id_t). Simplest: add a `network_rate_limit_check_endpoint(network, endpoint_id, type, now_ms)` variant that keys an internal endpoint-keyed bucket, OR pass the endpoint as a separate key. Read `network_rate_limit_check`'s signature + the `rate_limit_table_t` to decide the cleanest approach. The audit's intent: a peer can't dodge the bucket by varying the wire `sender_id`; the endpoint is the stable identity on the relay path.

- [ ] **Step 2: Test**

`test/test_network.cpp`: build a minimal network with `rate_limit_table_init`, craft `wire_relay_received_t` payloads with varying spoofed sender_ids but the same endpoint, dispatch them in a loop, assert the rate limit fires (the bucket is shared by endpoint, not per-spoofed-id).

- [ ] **Step 3: Build + run + commit**

Commit: `fix(network): key relay rate-limit on endpoint id (not spoofable sender_id)`

---

### Task 10: Per-source gossip cap + referral penalty

**Files:**
- Modify: `src/Network/network.c:~1994-2001, 2048-2053` — cap per-source gossip insertions; apply a Hebbian penalty for unreachable referrals.

- [ ] **Step 1: Cap per-source insertions**

In `network_handle_gossip_received` + `network_handle_gossip_pull_received`, the `for (index = 0; index < gossip->target_count && index < RING_MAX_RINGS; index++)` loop inserts every target. Cap: `size_t inserted = 0; ... if (inserted >= GOSSIP_PER_SOURCE_CAP) break;` (define `GOSSIP_PER_SOURCE_CAP = 32` or `RING_MAX_RINGS / N`). Only count actually-inserted (new) entries toward the cap.

- [ ] **Step 2: Referral penalty**

When a gossip-advertised target is later found unreachable (a ping/find_block to it fails), apply `hebbian_frequency(&network->hebbian, &gossip->sender_id, -config->referral_penalty)`. This requires wiring the referral-failure signal — the simplest in-scope version: when `network_add_node_to_ring` adds a target with `addr=0, port=0` (no rendezvous address, unreachable), apply a small immediate penalty to the sender (the referrer advertised an unreachable peer). Read `network_add_node_to_ring` (line ~1127) to confirm the addr=0 case.

- [ ] **Step 3: Test**

`test/test_network.cpp`: a gossip message with `target_count = 64` + distinct targets → assert only `GOSSIP_PER_SOURCE_CAP` are inserted. A gossip with unreachable (addr=0) targets → the sender's Hebbian weight decreases.

- [ ] **Step 4: Build + run + commit**

Commit: `feat(network): per-source gossip cap + Hebbian referral penalty`

---

### Task 11: Mode-aware relay routing gate

**Files:**
- Modify: `src/Network/network.h` — declare `bool network_secure_mode(const network_t*)`.
- Modify: `src/Network/network.c` — implement `network_secure_mode`; gate relay-admitted routing.
- Modify: `src/Network/connection_manager.c` — mode-aware drop.
- Test: `test/test_relay_gossip_gating.cpp` (new) + `test/test_network.cpp`.

- [ ] **Step 1: `network_secure_mode`**

In `network.c`:
```c
bool network_secure_mode(const network_t* n) {
  return n != NULL && n->authority != NULL
      && n->authority->allow_secure
      && n->authority->ca_cert_data != NULL && n->authority->ca_cert_len > 0;
}
```

- [ ] **Step 2: Gate relay-admitted routing**

At the routing entry points (`find_block.c:301`, `closest_nodes.c:289`, and the `ring_set_insert` at `network.c:5378`): in **secure mode**, a relay-admitted peer (`relay_verified=false`) is NOT routed until `relay_verified=true` (the signed-nonce challenge sets it). In **default mode**, the peer is gated by Hebbian weight (it starts at `FIND_BLOCK_MIN_WEIGHT` — already low; the routing min-weight gate skips it until it earns weight by serving verified blocks).

The cleanest implementation: add a `network_peer_routable(network, peer)` helper that returns true if (secure mode AND `peer->relay_verified`) OR (default mode AND `hebbian_weight >= routing_min_weight`). Use it in the routing selection loops. For `ring_set_insert` at `network.c:5378` (relay-admit), in secure mode insert at `HEBBIAN_MIN_WEIGHT` (below the routing gate) instead of `FIND_BLOCK_MIN_WEIGHT` so the peer is unroutable until verified — consistent with the spec's Section 7.3.

- [ ] **Step 3: Mode-aware drop in connection_manager**

`connection_manager.c:267`: in secure mode, drop `relay_verified=false` peers faster (e.g. a lower `drop_threshold` for unverified peers, OR a separate sweep that evicts `relay_verified=false` peers whose signed-nonce challenge timed out). The simplest in-scope: the existing `drop_threshold` applies; the signed-nonce challenge's sweep (`network_relay_challenge_sweep`) already removes unanswered challenges — add a parallel eviction of `relay_verified=false` peers whose challenge was swept.

- [ ] **Step 4: Tests**

`test/test_relay_gossip_gating.cpp` (new, register in CMake):
- **Secure mode**: relay-admitted peer (`relay_verified=false`) NOT routed; after `relay_verified=true`, routed.
- **Default mode**: relay-admitted peer at `HEBBIAN_MIN_WEIGHT` not routed; after serving verified blocks (weight crosses the gate), routed.

- [ ] **Step 5: Build + run + commit**

Commit: `feat(network): mode-aware relay routing gate (secure=verified, default=reputation)`

---

### Task 12: De-wonk + ASAN + valgrind

**Files:** none (verification).

- [ ] **Step 1: De-wonk** — run the de-wonk skill on the Stage 4 changes. Fix any unimplemented/stubbed/disabled/broken/weird code. No TODOs.
- [ ] **Step 2: ASAN** — `cd build-asan && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='*Http*:*Network*:*Timer*:*Buffer*:*Scheduler*:*Config*'` — no ASAN errors. Confirm the 7 `TestStream*` tests now pass (Task 8) or are explicitly documented as open.
- [ ] **Step 3: Valgrind** — `cd build-gdwarf4 && valgrind --leak-check=full --error-exitcode=1 ./test/testliboffs --gtest_filter='*Http*:*Network*:*Relay*:*Gossip*'` — 0 leaks, 0 errors.
- [ ] **Step 4: Commit** any de-wonk fixes.

---

## Self-Review

**Spec coverage (Section 7):**
- 7.1 HTTP hardening → Tasks 1-6, 8. ✓ (local-binding auth optional-but-default-on per the user's refinement)
- 7.2 Memory-safety → Task 7. ✓
- 7.3 Relay/gossip → Tasks 9-11. ✓ (mode-aware gate using `network_secure_mode`)

**Key risks flagged inline:**
- Task 1's `_auth_handler` local-binding-aware change: confirm how `_auth_handler` accesses the server (the middleware's user_data vs the request's server field). The current bypass is incomplete (the global middleware 401s before the handler); the fix makes the middleware itself local-binding-aware.
- Task 3's `pd_timer` API: confirm `pd_timer_create`/`pd_timer_start`/`pd_timer_stop` signatures + the callback contract.
- Task 7's `scheduler_pool_wait_for_idle` signature change (`void`→`int`): update all callers (http_server.c, node.c) — grep `scheduler_pool_wait_for_idle`.
- Task 8 (NULL-buffer root cause): time-boxed; the fallback is the defensive sentinel. The 7 `TestStream*` failures are the success signal.
- Task 9's rate-limit key change: the endpoint is a `uint32_t`; the rate-limit table is keyed on `node_id_t`. Decide the cleanest approach (endpoint-keyed bucket vs hashed-into-node_id).
- Task 11's `network_secure_mode`: `allow_secure && ca_cert_data != NULL` is the predicate (per the Explore findings).