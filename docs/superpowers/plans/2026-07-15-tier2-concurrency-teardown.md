# Tier-2 Concurrency Teardown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the remaining concurrency-pass findings — two CRITICAL (F2 `sections_dispatch` runs on two actor threads; F3 `network_shutdown_connections` races the network actor on `conn_mgr`) and two HIGH (F6 `relay_server` teardown; F7 `wt_transport` + sibling transports teardown) — so no shared mutable state is touched from two threads and no MsQuic callback can reach freed memory during teardown.

**Architecture:** Four focused fixes, each landing as its own commit. F2 is the smallest (serialize `sections_dispatch` with a mutex — the sync block_cache-worker path and the async sections-actor-worker path both touch `sections->lru`/`robin`). F3 routes the connection-shutdown loop through the network actor via a new `NETWORK_SHUTDOWN_CONNECTIONS` message + semaphore wait, so `ConnectionShutdown`/`connection_manager_remove`/`ConnectionClose` all run on the network actor's thread. F6 and F7 share a teardown pattern: stop the listener, shut down each connection, await `SHUTDOWN_COMPLETE` quiesce, then free. F7 also verifies the tcp/ws/unix sibling transports.

**Tech Stack:** C11, `__atomic_*` / `ATOMIC(...)`, MsQuic (gated on `HAS_MSQUIC`), platform mutex/semaphore/condvar (`src/Platform/platform.h`), GoogleTest, CMake, valgrind (DWARF4 build at `cmake-build-vg/`).

**Scope (in):** `docs/concurrency-pass.md` findings F2, F3, F6, F7.

**Scope (out — deferred to follow-on tiers):** audit #5/#6/#9/#10 (multi-hop RPC hangs), #8/#11 (identity/TLS), #15-17/#19/#20/#24 (CLI lies), #18 (NAT/relay feature), and concurrency F8-F11 (MEDIUM/LOW). Those belong to separate tiers per the audit's suggested fix order.

**Build / test commands (used throughout):**
```bash
# Build (cmake-build-verify exists, DWARF5)
cmake --build cmake-build-verify -j$(nproc) --target testliboffs

# Run one test filtered by name
cmake-build-verify/test/testliboffs --gtest_filter='TestBlockCache.*'

# Valgrind (DWARF4 build at cmake-build-vg; create if missing)
test -d cmake-build-vg || cmake -S . -B cmake-build-vg -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-gdwarf-4 -O0" -DCMAKE_CXX_FLAGS="-gdwarf-4 -O0"
cmake --build cmake-build-vg -j$(nproc) --target testliboffs
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='<filter>'
```

**Style reminder (from `docs/STYLE_GUIDE.md` + CLAUDE.md):** `_t` suffix, `type_action()`, `refcounter_t refcounter` first member on refcounted structs, no single-letter names, no TODO/FIXME in completed work, no `Co-Authored-By` in commits, use the de-wonk skill before marking done, check valgrind for leaks after.

**Known valgrind noise (acceptable, pre-existing):** a 320-byte "possibly lost" msquic TLS block (`MsQuicLibraryInitialize` / `CxPlatThreadCreate`); a 5120-byte "possibly lost" msquic worker pool block (`CxPlatWorkerPoolInitWorker`). These are third-party (`deps/msquic`), out of scope. `definitely lost` must be 0.

---

## File Structure

| File | Responsibility | Touched by task |
| --- | --- | --- |
| `src/BlockCache/sections.h`, `sections.c` | Add `platform_mutex_t dispatch_lock` to `sections_t`; lock it in `sections_dispatch`. | Task 1 (F2) |
| `test/test_block_cache.cpp` (or a new `test_sections.cpp`) | Stress test: concurrent CACHE_PUT + async sections_read → no corruption. | Task 1 (F2) |
| `src/Network/network.h`, `network.c` | New `NETWORK_SHUTDOWN_CONNECTIONS` message type + payload; `network_shutdown_connections` sends it and waits on a semaphore; network actor handler does the ConnectionShutdown loop. | Task 2 (F3) |
| `src/Network/quic_listener.c` | (If needed) adjust the SHUTDOWN_COMPLETE → NETWORK_QUIC_DISCONNECTED flow to be compatible with the new shutdown sequence. | Task 2 (F3) |
| `src/Node/node.c` | No change expected — `offs_node_stop` phase 5 already calls `network_shutdown_connections`, which now blocks until the network actor finishes. | Task 2 (F3) |
| `test/test_network.cpp` (or `test_shutdown.cpp`) | Test: `network_shutdown_connections` during active connections doesn't UAF the peers array. | Task 2 (F3) |
| `src/Network/Relay/relay_server.c` | `relay_server_stop` shuts down all client connections (StreamClose + ConnectionShutdown under `clients_lock`), awaits `num_clients == 0` (condvar); `relay_server_destroy` then frees safely. | Task 3 (F6) |
| `test/test_quic_integration.cpp` (or a relay-specific test) | Test: relay teardown with active clients doesn't UAF `client->framer`. | Task 3 (F6) |
| `src/ClientAPI/WT/wt_transport.c`, `wt_connection.c` | `wt_transport_stop` does ListenerStop + ConnectionShutdown each + await quiesce; `wt_transport_destroy` then frees + ListenerClose + ConfigurationClose + RegistrationClose. | Task 4 (F7) |
| `src/ClientAPI/TCP/tcp_transport.c`, `src/ClientAPI/WS/ws_transport.c`, `src/ClientAPI/Unix/unix_transport.c` | Verify the same teardown pattern; fix if the race exists (the audit said "identical `destroy_lock` pattern by grep" but didn't trace each). | Task 4 (F7) |
| `test/test_ws_transport.cpp`, `test_tcp_transport.cpp`, `test_unix_transport.cpp` | Tests: transport teardown with active connections doesn't UAF. | Task 4 (F7) |

---

## Task 1: F2 — Serialize `sections_dispatch` with a mutex

**Files:**
- Modify: `src/BlockCache/sections.h` (add `dispatch_lock` field), `src/BlockCache/sections.c` (init/destroy/lock)
- Test: `test/test_block_cache.cpp` or a new `test/test_sections_concurrency.cpp`

**Why:** `sections_dispatch` runs on two actor threads concurrently. Site A: the block_cache actor calls `sections_dispatch` synchronously (`block_cache.c:367` CACHE_PUT, `:463` sync CACHE_GET, `:524` CACHE_DEALLOCATE, `:599`). Site B: `sections_read`/`sections_write`/`sections_deallocate` (`sections.c:460-503`) `actor_send` to the sections actor, whose worker calls `sections_dispatch` (`sections.c:189`). Both mutate `sections->lru` (`sections_lru_cache_get`/`put` at lines 201, 208, 251, 258) and `sections->robin` (`round_robin_next` at 198). Interleaving → list/hashmap corruption, double `section_create`, concurrent `section_dispatch` on one section → on-disk corruption. No re-entrancy: `section_dispatch` does file I/O (no callback into `sections_dispatch`), `sections_full` mutates `robin` directly (no dispatch). A plain mutex is safe.

### Step 1: Write the failing test

The race is non-deterministic. The test stresses both paths concurrently and asserts no corruption (no double `section_create`, no crash, no valgrind invalid read). Add to `test/test_block_cache.cpp` (read it first to find the existing fixture/helpers) OR create `test/test_sections_concurrency.cpp` (add it to `test/CMakeLists.txt` in the `testliboffs` sources list).

The test: create a `sections_t` with a temp data path, spawn N threads that call `sections_dispatch` directly (sync path, simulating the block_cache worker) AND M threads that call `sections_read`/`sections_write` (async path, via the sections actor on a real scheduler pool). Run for a few seconds. Assert: no crash, no valgrind errors, the `sections->lru` and `sections->robin` are consistent (e.g., `round_robin_count` + `lru_size` are within expected bounds).

```cpp
#include <atomic>
#include <thread>
#include <vector>
extern "C" {
#include "../src/BlockCache/sections.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/allocator.h"
}

TEST(SectionsConcurrency, SyncAndAsyncDispatchDoNotCorrupt) {
  // Create a sections_t with a temp path, a real scheduler pool for the async path.
  // Read test_block_cache.cpp for the existing sections_create fixture pattern.
  sections_t* sections = test_sections_create("/tmp/sections_concurrent_XXXXXX");
  ASSERT_NE(sections, nullptr);
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  // Wire sections->actor to the pool (read sections_create/sections_actor_init).

  std::atomic<bool> stop{false};
  std::atomic<int> sync_ops{0};
  std::atomic<int> async_ops{0};

  // Sync-path threads: call sections_dispatch directly with SECTIONS_WRITE.
  std::vector<std::thread> sync_threads;
  for (int thread_index = 0; thread_index < 2; thread_index++) {
    sync_threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_acquire)) {
        sections_write_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        buffer_t* data = test_buffer_create(128);  // small block
        payload.data = data;
        payload.reply_to = NULL;
        message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = SECTIONS_WRITE;
        msg.payload = &payload;
        msg.payload_destroy = NULL;
        sections_dispatch(sections, &msg);
        buffer_destroy(data);
        sync_ops.fetch_add(1);
      }
    });
  }

  // Async-path thread: call sections_write (actor_send to sections->actor).
  std::thread async_thread([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      buffer_t* data = test_buffer_create(128);
      sections_write(sections, data, NULL);  // async; result dropped (reply_to NULL)
      async_ops.fetch_add(1);
    }
  });

  // Let it run for 500ms.
  platform_sleep_ms(500);
  stop.store(true, std::memory_order_release);

  for (auto& thread : sync_threads) thread.join();
  async_thread.join();

  // Assert no crash (reaching here is the assert). Valgrind at step 6 asserts
  // no invalid reads/writes on sections->lru/robin.
  EXPECT_GT(sync_ops.load(), 0);
  EXPECT_GT(async_ops.load(), 0);

  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  test_sections_destroy(sections);
  SUCCEED();
}
```

Adapt the helpers (`test_sections_create`, `test_buffer_create`, `test_sections_destroy`) to the existing test infrastructure — read `test_block_cache.cpp` for the pattern. If `sections_write` with `reply_to=NULL` doesn't work (the async path may require a reply_to to avoid leaking the result message), use a real reply_to actor that discards results.

### Step 2: Run to verify it fails (or valgrind catches corruption)

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='SectionsConcurrency.*'
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='SectionsConcurrency.*'
```
Expected pre-fix: intermittent crashes, valgrind invalid reads/writes on `sections->lru`/`robin`, or corrupted `round_robin`/`lru` state. The race is non-deterministic — the test may pass on some runs. The real assertion is valgrind + the post-fix reliability.

### Step 3: Add the mutex to `sections_t`

In `src/BlockCache/sections.h`, read the current `sections_t` struct. Add:

```c
  platform_mutex_t* dispatch_lock;  /* serializes sections_dispatch across the
                                       sync (block_cache worker) and async
                                       (sections actor worker) paths. See
                                       docs/concurrency-pass.md F2. */
```

In `src/BlockCache/sections.c`, in `sections_create` (or wherever the struct is initialized — read the file), after the struct is allocated:

```c
  sections->dispatch_lock = platform_mutex_create();
```

In `sections_destroy` (or the cleanup function), before freeing the struct:

```c
  platform_mutex_destroy(sections->dispatch_lock);
  sections->dispatch_lock = NULL;
```

### Step 4: Lock the mutex in `sections_dispatch`

In `src/BlockCache/sections.c`, `sections_dispatch` (line 189):

```c
void sections_dispatch(void* state, message_t* msg) {
  sections_t* sections = (sections_t*)state;
  platform_mutex_lock(sections->dispatch_lock);
  switch (msg->type) {
    /* ... existing body unchanged ... */
  }
  platform_mutex_unlock(sections->dispatch_lock);
}
```

Wrap the ENTIRE switch in the lock/unlock. Confirm no `return` inside the switch bypasses the unlock (read the switch — the existing cases `break`, so the unlock at the end is reached). If any case does an early `return`, change it to `break` or add an unlock-before-return.

### Step 5: Run the test to verify it passes

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='SectionsConcurrency.*'
```
Run the test 10× to confirm reliability:
```bash
for run in $(seq 1 10); do
  cmake-build-verify/test/testliboffs --gtest_filter='SectionsConcurrency.SyncAndAsyncDispatchDoNotCorrupt' 2>&1 | tail -1
done
```
Expected: PASS 10/10.

### Step 6: Run valgrind on the sections + block_cache suites

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='SectionsConcurrency.*:TestBlockCache.*:TestSection.*:TestBlock.*'
```
Expected: `definitely lost: 0 bytes`, 0 invalid reads/writes (only the known msquic "possibly lost" noise if the test uses msquic — the sections test shouldn't).

### Step 7: Commit

```bash
git add src/BlockCache/sections.h src/BlockCache/sections.c test/test_block_cache.cpp
git commit -m "fix(sections): serialize sections_dispatch across sync and async paths

sections_dispatch ran on two actor threads at once: the block_cache actor
called it synchronously (CACHE_PUT, sync CACHE_GET, CACHE_DEALLOCATE) while
the sections actor's worker ran it for async sections_read/write/deallocate.
Both mutated sections->lru and sections->robin -> list/hashmap corruption,
double section_create, concurrent section_dispatch on one section -> on-disk
corruption. Add a dispatch_lock mutex to sections_t and hold it across the
whole sections_dispatch switch. No re-entrancy (section_dispatch and
sections_full don't call back into sections_dispatch). See concurrency-pass.md F2."
```

---

## Task 2: F3 — Route network shutdown through the network actor

**Files:**
- Modify: `src/Network/network.h` (new message type + payload), `src/Network/network.c` (send + wait + handler)
- Test: `test/test_shutdown.cpp` or `test/test_network.cpp`

**Why:** `network_shutdown_connections` (`network.c:221-243`, main thread, `offs_node_stop` phase 5) iterates `conn_mgr.peers[i]`, nulls `peer->quic_connection`/`quic_stream`, calls `ConnectionShutdown`. That shutdown → `SHUTDOWN_COMPLETE` (MsQuic thread) → `actor_send(NETWORK_QUIC_DISCONNECTED)` → the network actor runs `connection_manager_remove` (`network.c:3421`) → frees the peer + `memmove` + `peer_count--` (`connection_manager.c:123-135`), then `ConnectionClose` (`network.c:3434`). The main thread reads `peers[i]` while a worker frees that peer and compacts the array → UAF + skipped/duplicated elements. `peer_connection_destroy` does NOT close the HQUIC (`peer_connection.c:77`); the network actor does `ConnectionClose` after remove — so the HQUIC lifecycle is also owned by the network actor. A mutex on `conn_mgr` alone doesn't fix the HQUIC race (the main thread's `ConnectionShutdown` vs the network actor's `ConnectionClose` on the same HQUIC). The clean fix: the entire shutdown loop runs on the network actor.

### Step 1: Write the failing test

The test: create a `network_t` with a real scheduler pool + a few fake peers in `conn_mgr`, call `network_shutdown_connections` from the main thread while the network actor's worker is processing a `NETWORK_QUIC_DISCONNECTED` for one of the peers. Assert no crash / no valgrind invalid read on `conn_mgr.peers`.

This requires a real `network_t` with msquic OR a mock. Read `test/test_shutdown.cpp` and `test/test_network.cpp` to find the existing network/shutdown fixture. If a real `network_t` is too heavy, the test can be valgrind-only on the existing `TestShutdown.*` suite (which exercises `offs_node_stop` with real connections).

A focused unit test (if feasible): construct a `network_t` with a 1-worker pool, manually add 3 fake peers to `conn_mgr` with fake HQUIC handles, start the network actor, call `network_shutdown_connections`, assert it returns (doesn't hang) and `conn_mgr.peer_count == 0` after. Run under valgrind to catch the UAF.

### Step 2: Run to verify it fails (or valgrind catches the UAF)

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestShutdown.*:TestNetwork.*'
```
Expected pre-fix: intermittent valgrind "Invalid read of size 8" on `conn_mgr.peers[i]` during shutdown, or a crash. The race is non-deterministic.

### Step 3: Add the message type + payload

In `src/Network/network.h` (or the network message-types header — find where `NETWORK_QUIC_CONNECTED` etc. are defined), add:

```c
  NETWORK_SHUTDOWN_CONNECTIONS,
```

Add a payload struct (in `network.h` or a new `network_internal.h`):

```c
typedef struct {
  platform_semaphore_t* done;  /* posted by the network actor when the shutdown loop completes */
} network_shutdown_payload_t;
```

### Step 4: Rewrite `network_shutdown_connections` to send + wait

In `src/Network/network.c`, replace the body of `network_shutdown_connections` (lines 221-243):

```c
void network_shutdown_connections(network_t* network) {
  if (network == NULL) return;

  /* Route the shutdown loop through the network actor so ConnectionShutdown,
     connection_manager_remove, and ConnectionClose all run on the same thread.
     The old code iterated conn_mgr.peers on the main thread while the network
     actor's worker ran connection_manager_remove (freed peers + memmove) on
     SHUTDOWN_COMPLETE -> UAF on the peers array. See concurrency-pass.md F3. */
#ifdef HAS_MSQUIC
  if (network->msquic != NULL) {
    platform_semaphore_t* done = platform_semaphore_create(0);
    if (done != NULL) {
      network_shutdown_payload_t* payload = get_clear_memory(sizeof(network_shutdown_payload_t));
      if (payload != NULL) {
        payload->done = done;
        message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = NETWORK_SHUTDOWN_CONNECTIONS;
        msg.payload = payload;
        msg.payload_destroy = free;
        actor_send(&network->actor, &msg);
        platform_semaphore_wait(done);  /* block until the network actor finishes the loop */
      } else {
        platform_semaphore_destroy(done);
        done = NULL;
      }
      if (done != NULL) platform_semaphore_destroy(done);
    }
  }
#endif

  if (network->relay != NULL) {
    relay_client_disconnect(network->relay);
  }
}
```

(Confirm `platform_semaphore_create` / `platform_semaphore_wait` / `platform_semaphore_destroy` exist in `src/Platform/platform.h`. If the project uses a different primitive, adapt — e.g., a `platform_condvar_t` + `platform_mutex_t` pair.)

### Step 5: Add the `NETWORK_SHUTDOWN_CONNECTIONS` handler in the network actor

In `src/Network/network.c`, in the network actor's dispatch function (find the `switch (msg->type)` that handles `NETWORK_QUIC_DATA`, `NETWORK_QUIC_CONNECTED`, `NETWORK_QUIC_DISCONNECTED`), add a case:

```c
    case NETWORK_SHUTDOWN_CONNECTIONS: {
      network_shutdown_payload_t* payload = (network_shutdown_payload_t*)msg->payload;
#ifdef HAS_MSQUIC
      if (network->msquic != NULL) {
        /* Snapshot the HQUIC handles under no lock (we're on the network actor,
           the sole owner now). ConnectionShutdown is safe here because
           connection_manager_remove + ConnectionClose also run on this actor. */
        for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
          peer_connection_t* peer = network->conn_mgr.peers[index];
          if (peer != NULL && peer->quic_connection != NULL) {
            network->msquic->ConnectionShutdown(
                peer->quic_connection,
                QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                0);
            peer->quic_connection = NULL;
            peer->quic_stream = NULL;
          }
        }
      }
#endif
      if (payload != NULL && payload->done != NULL) {
        platform_semaphore_post(payload->done);
      }
      break;
    }
```

The SHUTDOWN_COMPLETE callbacks will fire on MsQuic threads and `actor_send` `NETWORK_QUIC_DISCONNECTED` to the network actor's mailbox. The network actor processes them (existing handler at `network.c:3409` does `connection_manager_remove` + `ConnectionClose`) as its worker drains the mailbox. The main thread, after `platform_semaphore_wait`, proceeds to phase 6; the network actor continues processing disconnects. By phase 7 (`scheduler_pool_stop`), the worker drains any remaining disconnects before joining.

### Step 6: Run the test to verify it passes

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='TestShutdown.*:TestNetwork.*'
```
Expected: PASS, no hang. Run the shutdown test 10× to confirm reliability:
```bash
for run in $(seq 1 10); do
  cmake-build-verify/test/testliboffs --gtest_filter='TestShutdown.*' 2>&1 | tail -1
done
```

### Step 7: Run valgrind on the shutdown + network suites

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestShutdown.*:TestNetwork.*:QuicIntegration.*'
```
Expected: `definitely lost: 0 bytes`, 0 invalid reads on `conn_mgr.peers`. The known msquic "possibly lost" TLS noise is acceptable.

### Step 8: Commit

```bash
git add src/Network/network.h src/Network/network.c test/test_shutdown.cpp
git commit -m "fix(network): route connection shutdown through the network actor

network_shutdown_connections iterated conn_mgr.peers on the main thread and
called ConnectionShutdown, while the network actor's worker ran
connection_manager_remove (freed peers + memmove) + ConnectionClose on
SHUTDOWN_COMPLETE -> UAF on the peers array and a race on the HQUIC
lifecycle. Send a new NETWORK_SHUTDOWN_CONNECTIONS message to the network
actor and block on a semaphore; the network actor does the ConnectionShutdown
loop on its own thread, and the subsequent SHUTDOWN_COMPLETE -> remove ->
ConnectionClose sequence stays on the same actor. See concurrency-pass.md F3."
```

---

## Task 3: F6 — relay_server teardown ordering

**Files:**
- Modify: `src/Network/Relay/relay_server.c` (`relay_server_stop`, `relay_server_destroy`)
- Test: `test/test_quic_integration.cpp` or a relay-specific test

**Why:** `relay_server_destroy` (`relay_server.c:548-587`) iterates `server->clients`, `StreamClose`s and `_relay_remove_client` (frees `client->framer`, `:86-97`) WITHOUT `clients_lock`, while `_relay_stream_callback` RECEIVE (`:308-321`, reads `client->framer` via `stream_framer_feed` at 318) and `_relay_connection_callback` SHUTDOWN_COMPLETE (`:447-465`, takes `clients_lock` + `_relay_remove_client` + `ConnectionClose`) still fire on MsQuic threads. `relay_server_stop` (`:709-717`) never shuts down/awaits existing client connections. Destroy frees `client->framer` while a callback runs `stream_framer_feed` → UAF; or double `StreamClose` (`:558` vs `:454`).

### Step 1: Write the failing test

The test: start a relay server, connect a few clients (real or fake), trigger `relay_server_stop` + `relay_server_destroy` while a client is mid-receive. Assert no crash / no valgrind invalid read on `client->framer`. This requires msquic (the relay is msquic-gated). Read `test/test_quic_integration.cpp` for the existing relay test pattern. If a real relay test is too heavy, fall back to valgrind on the existing relay/shutdown tests.

### Step 2: Run to verify it fails (or valgrind catches the UAF)

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='RelayIntegration.*:TestShutdown.*'
```
Expected pre-fix: intermittent valgrind "Invalid read" on `client->framer` during teardown, or double-free on `StreamClose`.

### Step 3: Fix `relay_server_stop` to shut down clients + await quiesce

In `src/Network/Relay/relay_server.c`, `relay_server_stop` (`:709-717`), after `ListenerStop` (so no new connections arrive), shut down all client connections under `clients_lock` and await quiesce:

```c
void relay_server_stop(relay_server_t* server) {
  if (server == NULL) return;
  ATOMIC_STORE(&server->running, 0);
  pd_loop_async_send(server->loop, NULL);
  platform_thread_join(server->thread);
  if (server->listener != NULL) {
    server->msquic->ListenerStop(server->listener);
  }

  /* Shut down all client connections and await SHUTDOWN_COMPLETE quiesce
     before destroy frees the client table. Without this, MsQuic callbacks
     (_relay_stream_callback RECEIVE, _relay_connection_callback SHUTDOWN_COMPLETE)
     fire on MsQuic threads during destroy and touch freed client->framer.
     See concurrency-pass.md F6. */
  platform_mutex_lock(server->clients_lock);
  for (size_t index = 0; index < RELAY_MAX_CLIENTS; index++) {
    if (server->clients[index].active) {
      if (server->clients[index].connection != NULL && server->msquic != NULL) {
        server->msquic->ConnectionShutdown(
            (HQUIC)server->clients[index].connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
            0);
      }
    }
  }
  size_t remaining = 0;
  for (size_t index = 0; index < RELAY_MAX_CLIENTS; index++) {
    if (server->clients[index].active) remaining++;
  }
  platform_mutex_unlock(server->clients_lock);

  /* Await quiesce: poll num_clients until 0 or a timeout. The
     _relay_connection_callback SHUTDOWN_COMPLETE handler decrements
     num_clients and calls _relay_remove_client + ConnectionClose. */
  for (int wait_ms = 0; wait_ms < 5000 && remaining > 0; wait_ms += 10) {
    platform_sleep_ms(10);
    platform_mutex_lock(server->clients_lock);
    remaining = server->num_clients;
    platform_mutex_unlock(server->clients_lock);
  }
  if (remaining > 0) {
    log_error("relay: shutdown timed out with %zu clients still active", remaining);
  }
}
```

(If the project has a `platform_condvar_t` + `platform_mutex_t` pattern, use a condvar wait instead of the poll loop — read `scheduler.c`'s `wait_for_idle` for the pattern. The poll loop is a simpler fallback.)

### Step 4: Fix `relay_server_destroy` to take `clients_lock` during the cleanup

In `src/Network/Relay/relay_server.c`, `relay_server_destroy` (`:548-587`), the client-cleanup loop (`:555-562`) already runs AFTER `relay_server_stop` (which now awaits quiesce), so no callbacks should fire. But take `clients_lock` defensively during the loop in case a late callback races:

```c
  platform_mutex_lock(server->clients_lock);
  for (size_t index = 0; index < RELAY_MAX_CLIENTS; index++) {
    if (server->clients[index].active) {
      if (server->clients[index].stream != NULL && server->msquic != NULL) {
        server->msquic->StreamClose((HQUIC)server->clients[index].stream);
      }
      _relay_remove_client(server, &server->clients[index]);
    }
  }
  platform_mutex_unlock(server->clients_lock);
```

### Step 5: Run the test to verify it passes

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='RelayIntegration.*:TestShutdown.*'
```
Expected: PASS, no hang (the 5 s timeout is generous; real quiesce is fast).

### Step 6: Run valgrind on the relay + shutdown suites

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='RelayIntegration.*:TestShutdown.*:QuicIntegration.*'
```
Expected: `definitely lost: 0 bytes`, 0 invalid reads on `client->framer`. The known msquic "possibly lost" noise is acceptable.

### Step 7: Commit

```bash
git add src/Network/Relay/relay_server.c test/test_quic_integration.cpp
git commit -m "fix(relay_server): shut down clients + await quiesce before freeing

relay_server_destroy freed client->framer without clients_lock while
_relay_stream_callback RECEIVE (stream_framer_feed) and
_relay_connection_callback SHUTDOWN_COMPLETE still fired on MsQuic threads
-> UAF on framer and double StreamClose. relay_server_stop never shut down
existing client connections. Stop now does ListenerStop + ConnectionShutdown
each under clients_lock + awaits num_clients == 0 (poll) before destroy
runs. Destroy takes clients_lock defensively for the residual cleanup.
See concurrency-pass.md F6."
```

---

## Task 4: F7 — wt_transport + sibling teardown ordering

**Files:**
- Modify: `src/ClientAPI/WT/wt_transport.c` (`wt_transport_stop`, `wt_transport_destroy`), `src/ClientAPI/WT/wt_connection.c` (if `vec_remove` locking needs adjustment)
- Verify + fix if needed: `src/ClientAPI/TCP/tcp_transport.c`, `src/ClientAPI/WS/ws_transport.c`, `src/ClientAPI/Unix/unix_transport.c`
- Test: `test/test_ws_transport.cpp`, `test/test_tcp_transport.cpp`, `test/test_unix_transport.cpp`

**Why:** `wt_transport_stop` (`wt_transport.c:651-655`) only stops the pd loop. `wt_transport_destroy` frees every `wt_connection_t` (`:335-370`, no `conn_lock`) and only then stops the listener (`:373-376`); client connections are never shut down before `RegistrationClose` (`:380-382`). MsQuic callbacks for still-open connections deref freed structs → UAF. `wt_connection_destroy`'s `vec_remove` under `conn_lock` (`wt_connection.c:753-755`) races the unlocked iteration. The tcp/ws/unix transports share this teardown shape (identical `destroy_lock` pattern by grep) but were not individually traced.

### Step 1: Write the failing test

Read `test/test_ws_transport.cpp` / `test/test_tcp_transport.cpp` / `test/test_unix_transport.cpp` to find the existing transport-teardown test pattern. The test: start a transport, connect a few clients, trigger `wt_transport_stop` + `wt_transport_destroy` while a client connection is mid-callback. Assert no crash / no valgrind invalid read on the connection struct. For WT, this needs msquic; for tcp/ws/unix, it's pure socket I/O.

### Step 2: Run to verify it fails (or valgrind catches the UAF)

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestWsTransport.*:TestTcpTransport.*:TestUnixTransport.*:TestWtTransport.*'
```
Expected pre-fix: intermittent valgrind "Invalid read" on `wt_connection_t` during teardown.

### Step 3: Fix `wt_transport_stop` to shut down connections + await quiesce

In `src/ClientAPI/WT/wt_transport.c`, `wt_transport_stop` (`:651-655`), after stopping the pd loop, do `ListenerStop` + `ConnectionShutdown` each + await quiesce. Read the current `wt_transport_stop` and `wt_transport_destroy` first. The fix pattern mirrors F6 (relay):

```c
void wt_transport_stop(wt_transport_t* transport) {
  if (transport == NULL) return;
  atomic_store(&transport->running, 0);
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);

  /* Stop the listener (no new connections), then shut down each existing
     connection and await SHUTDOWN_COMPLETE quiesce before destroy frees the
     connection objects. Without this, MsQuic callbacks for still-open
     connections deref freed wt_connection_t during destroy. See
     concurrency-pass.md F7. */
  if (transport->listener != NULL) {
    transport->msquic->ListenerStop(transport->listener);
  }
  platform_mutex_lock(transport->conn_lock);  /* confirm conn_lock exists; if not, add it */
  for (int index = 0; index < transport->connections.length; index++) {
    wt_connection_t* conn = transport->connections.data[index];
    if (conn != NULL && conn->quic_connection != NULL) {
      transport->msquic->ConnectionShutdown(
          conn->quic_connection,
          QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
          0);
    }
  }
  platform_mutex_unlock(transport->conn_lock);

  /* Await quiesce: poll active_connections until 0 or a timeout. The
     SHUTDOWN_COMPLETE handler decrements active_connections. */
  for (int wait_ms = 0; wait_ms < 5000 && atomic_load(&transport->active_connections) > 0; wait_ms += 10) {
    platform_sleep_ms(10);
  }
  if (atomic_load(&transport->active_connections) > 0) {
    log_error("wt_transport: shutdown timed out with %d connections active",
              atomic_load(&transport->active_connections));
  }
}
```

(Confirm `transport->conn_lock` and `transport->active_connections` exist — read `wt_transport.h`. If `conn_lock` doesn't exist, add it. If the quiesce should use a condvar, adapt.)

### Step 4: Fix `wt_transport_destroy` to free AFTER quiesce + take `conn_lock`

In `src/ClientAPI/WT/wt_transport.c`, `wt_transport_destroy` (the free loop at `:335-370`), the connections are now quiesced (callbacks done) because `wt_transport_stop` awaited. Take `conn_lock` during the iteration/free defensively:

```c
  platform_mutex_lock(transport->conn_lock);
  for (int index = transport->connections.length - 1; index >= 0; index--) {
    wt_connection_t* conn = transport->connections.data[index];
    actor_detach_pool(&conn->actor);
    message_queue_destroy(&conn->actor.queue);
    /* ... existing free body ... */
    free(conn);
  }
  vec_deinit(&transport->connections);
  platform_mutex_unlock(transport->conn_lock);
```

Then `ListenerClose` + `ConfigurationClose` + `RegistrationClose` (already at `:373-382`, after the free loop — correct order now since stop already did `ListenerStop` + connection shutdown).

### Step 5: Verify + fix the sibling transports (tcp/ws/unix)

Read `src/ClientAPI/TCP/tcp_transport.c`, `src/ClientAPI/WS/ws_transport.c`, `src/ClientAPI/Unix/unix_transport.c`. Find their `*_transport_stop` and `*_transport_destroy` functions. The audit said they share the "identical `destroy_lock` pattern by grep" — confirm whether they have the same race (free connection objects before stopping the listener / while callbacks fire).

- For socket-based transports (tcp/ws/unix), the callbacks run on the pd-loop thread (not MsQuic threads). The race is: `destroy` frees connection objects while the pd-loop thread is inside a connection callback. The fix: `stop` joins the pd-loop thread (so no callbacks fire) BEFORE `destroy` frees. If `stop` already joins the pd-loop, the race may not exist for these transports — verify and document.
- If a sibling transport DOES have the race (e.g., `stop` doesn't join the loop, or `destroy` frees before `stop` joins), apply the same fix: `stop` joins the loop → `destroy` frees.

For each sibling, either fix it (same pattern) or add a comment documenting that the race doesn't apply (e.g., "stop joins the pd-loop thread before destroy frees, so no callback can fire during free"). Be explicit in the commit message about which siblings were fixed vs verified-clean.

### Step 6: Run the tests to verify they pass

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='TestWsTransport.*:TestTcpTransport.*:TestUnixTransport.*:TestWtTransport.*'
```
Expected: PASS, no hangs.

### Step 7: Run valgrind on the transport suites

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestWsTransport.*:TestTcpTransport.*:TestUnixTransport.*:TestWtTransport.*'
```
Expected: `definitely lost: 0 bytes`, 0 invalid reads on connection structs.

### Step 8: Commit

```bash
git add src/ClientAPI/WT/wt_transport.c src/ClientAPI/WT/wt_connection.c src/ClientAPI/TCP/tcp_transport.c src/ClientAPI/WS/ws_transport.c src/ClientAPI/Unix/unix_transport.c test/test_ws_transport.cpp test/test_tcp_transport.cpp test/test_unix_transport.cpp
git commit -m "fix(transports): shut down connections + await quiesce before freeing

wt_transport_destroy freed wt_connection_t objects before ListenerStop and
while MsQuic callbacks could still fire on them -> UAF; client connections
were never shut down before RegistrationClose. wt_transport_stop now does
ListenerStop + ConnectionShutdown each + awaits active_connections == 0
before destroy runs; destroy takes conn_lock defensively for the residual
free. [Document the sibling outcome: tcp/ws/unix verified-clean / fixed.]
See concurrency-pass.md F7."
```

---

## Task 5: Whole-tier verification

**Files:** none (verification only)

- [ ] **Step 1: Build.** `cmake --build cmake-build-verify -j$(nproc) --target testliboffs` — expect clean, no warnings on tier-2 touched files.
- [ ] **Step 2: Full test suite.** `cmake-build-verify/test/testliboffs` — expect all pass (modulo the pre-existing SSL cert failures if certs aren't present). Compare against the tier-1-merged baseline; no NEW failures.
- [ ] **Step 3: Valgrind sweep.** `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='SectionsConcurrency.*:TestBlockCache.*:TestSection.*:TestBlock.*:TestShutdown.*:TestNetwork.*:QuicIntegration.*:RelayIntegration.*:TestWsTransport.*:TestTcpTransport.*:TestUnixTransport.*:TestWtTransport.*:TestActor.*:TestScheduler.*:TestMessageQueue.*'` — expect `definitely lost: 0 bytes`, 0 invalid reads (only the known msquic "possibly lost" noise).
- [ ] **Step 4: De-wonk.** Invoke the de-wonk skill on the tier-2 touched files. No stubbed/disabled/broken code in the tier-2 changes.
- [ ] **Step 5: TODO check.** `git log master..HEAD --pickaxe-regex -S'TODO\|FIXME\|HACK\|XXX' -- <tier-2 files>` — expect no tier-2 commit introduced a TODO.
- [ ] **Step 6: Leak check.** Covered by step 3.
- [ ] **Step 7: Final commit** if the de-wonk/leak check found fixes.

---

## Self-Review

**1. Spec coverage.** Tier-2 scope = F2, F3, F6, F7.
- F2 → Task 1 ✓
- F3 → Task 2 ✓
- F6 → Task 3 ✓
- F7 → Task 4 ✓
- Whole-tier verification → Task 5 ✓

**2. Placeholder scan.** Every code step shows the actual code or the exact change. Where a test helper is referenced (`test_sections_create`, `test_buffer_create`), the step says to adapt to existing infrastructure. The sibling-transport step (Task 4 Step 5) is intentionally verify-and-fix-or-document because the audit didn't trace each — the step is explicit about this.

**3. Type / signature consistency.**
- `network_shutdown_payload_t` (Task 2) is used in both `network_shutdown_connections` (send) and the network actor handler (receive). The semaphore primitives (`platform_semaphore_create`/`wait`/`post`/`destroy`) must be confirmed to exist in `platform.h`; if not, adapt to condvar+mutex.
- The mutex additions (`sections->dispatch_lock`, `transport->conn_lock` if added) use `platform_mutex_create`/`destroy`/`lock`/`unlock` consistent with the existing `quic_listener.c` `conn_lock` pattern.
- The `NETWORK_SHUTDOWN_CONNECTIONS` enum value is added to the same enum as `NETWORK_QUIC_CONNECTED` etc. — confirm the enum location.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-15-tier2-concurrency-teardown.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, two-stage review between tasks. Best for this tier because the 4 tasks span 4 subsystems (block_cache/sections, network, relay, transports) and a fresh context per task keeps each fix tight.
2. **Inline Execution** — execute in this session with checkpoints.

Which approach?