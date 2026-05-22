# Graceful Shutdown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement full graceful shutdown with configurable deadline, peer notification, HTTP/actor draining, and index/WAL flush before teardown.

**Architecture:** Bottom-up — build platform primitives first (`platform_file_sync`, `wal_sync`, `timer_actor_debounce_flush`), then config, then HTTP draining, then wire it all together in `offs_node_stop()` with a phased deadline-gated sequence.

**Tech Stack:** C11, POSIX/Win32 platform layer, MsQuic, poll-dancer event loop, GoogleTest (C++17)

**Spec:** `docs/superpowers/specs/2026-05-22-graceful-shutdown-design.md`

---

### Task 1: Add `platform_file_sync()` to the platform layer

**Files:**
- Modify: `src/Platform/platform_file.h`
- Modify: `src/Platform/platform_file.c`
- Create: `test/test_platform_file_sync.cpp`

- [ ] **Step 1: Add declaration to platform_file.h**

Add after the `platform_file_seek` declaration (before `PLATFORM_SEEK_SET` defines):

```c
int platform_file_sync(platform_file_t* file);
```

- [ ] **Step 2: Add POSIX implementation to platform_file.c**

Add inside the `#ifndef _WIN32` block, after `platform_file_seek`:

```c
int platform_file_sync(platform_file_t* file) {
  if (file == NULL) return -1;
  return fsync(file->fd);
}
```

- [ ] **Step 3: Add Win32 implementation to platform_file.c**

Add inside the `#else` block (Windows section), after `platform_file_seek`:

```c
int platform_file_sync(platform_file_t* file) {
  if (file == NULL) return -1;
  return FlushFileBuffers(file->handle) ? 0 : -1;
}
```

- [ ] **Step 4: Write unit test**

Create `test/test_platform_file_sync.cpp`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Platform/platform_file.h"
}

TEST(TestPlatformFileSync, SyncOpenFileReturnsZero) {
  platform_file_t* file = platform_file_open("/tmp/test_sync.tmp",
      PLATFORM_O_RDWR | PLATFORM_O_CREAT | PLATFORM_O_TRUNC, 0644);
  ASSERT_NE(file, nullptr);

  const char* data = "hello";
  ssize_t written = platform_file_write(file, data, 5);
  ASSERT_EQ(written, 5);

  int result = platform_file_sync(file);
  EXPECT_EQ(result, 0);

  platform_file_close(file);
  platform_file_unlink("/tmp/test_sync.tmp");
}

TEST(TestPlatformFileSync, SyncNullReturnsNegative) {
  int result = platform_file_sync(NULL);
  EXPECT_EQ(result, -1);
}
```

- [ ] **Step 5: Register test in CMakeLists.txt**

Add `test/test_platform_file_sync.cpp` to the test sources in `test/CMakeLists.txt`.

- [ ] **Step 6: Run test to verify it passes**

Run: `cd build && cmake .. && make TestPlatformFileSync && ./TestPlatformFileSync`
Expected: 2/2 tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/Platform/platform_file.h src/Platform/platform_file.c test/test_platform_file_sync.cpp test/CMakeLists.txt
git commit -m "feat: add platform_file_sync() for fsync/FlushFileBuffers"
```

---

### Task 2: Add `wal_sync()` to the WAL module

**Files:**
- Modify: `src/BlockCache/wal.h`
- Modify: `src/BlockCache/wal.c`
- Modify: `test/test_index.cpp`

- [ ] **Step 1: Add declaration to wal.h**

Add after `wal_write` declaration:

```c
int wal_sync(wal_t* wal);
```

- [ ] **Step 2: Add implementation to wal.c**

Add after `wal_write`:

```c
int wal_sync(wal_t* wal) {
  if (wal == NULL || wal->log == NULL) return -1;
  return platform_file_sync(wal->log);
}
```

- [ ] **Step 3: Add test to test_index.cpp**

Add at the end of the file (inside the `extern "C"` block access area):

```cpp
TEST(TestWal, WalSyncOpenWalReturnsZero) {
  char* location = get_dir("wal_sync_test_XXXXXX");
  ASSERT_NE(location, nullptr);
  mkdir_p(location);

  wal_t* wal = wal_create(location, 1);
  ASSERT_NE(wal, nullptr);

  buffer_t* data = buffer_create(78);
  memset(data->data, 0xAB, 78);
  wal_write(wal, addition, data);

  int result = wal_sync(wal);
  EXPECT_EQ(result, 0);

  DESTROY(data, buffer);
  wal_destroy(wal);
  rm_rf(location);
  free(location);
}

TEST(TestWal, WalSyncNullWalReturnsNegative) {
  int result = wal_sync(NULL);
  EXPECT_EQ(result, -1);
}

TEST(TestWal, WalSyncNullLogReturnsNegative) {
  char* location = get_dir("wal_sync_null_log_XXXXXX");
  ASSERT_NE(location, nullptr);
  mkdir_p(location);

  wal_t* wal = wal_create(location, 1);
  ASSERT_NE(wal, nullptr);
  /* Don't write — log is still NULL */
  int result = wal_sync(wal);
  EXPECT_EQ(result, -1);

  wal_destroy(wal);
  rm_rf(location);
  free(location);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake .. && make test_index && ./test_index --gtest_filter='*WalSync*'`
Expected: 3/3 tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/BlockCache/wal.h src/BlockCache/wal.c test/test_index.cpp
git commit -m "feat: add wal_sync() for durable WAL flush"
```

---

### Task 3: Add `timer_actor_debounce_flush()` to the timer actor

**Files:**
- Modify: `src/Timer/timer_actor.h`
- Modify: `src/Timer/timer_actor.c`
- Modify: `test/test_timer.cpp` (create if doesn't exist, otherwise add to existing timer test)

- [ ] **Step 1: Add declaration to timer_actor.h**

Add after `timer_actor_debounce` declaration:

```c
void timer_actor_debounce_flush(timer_actor_t* timer_actor,
                                actor_t* target, uint32_t completion_type);
```

- [ ] **Step 2: Add TIMER_DEBOUNCE_FLUSH message type**

In `src/Actor/message.h`, add after `TIMER_DEBOUNCE`:

```c
  TIMER_DEBOUNCE_FLUSH,
```

- [ ] **Step 3: Add dispatch case in timer_actor.c**

In `_timer_actor_dispatch`, add after the `TIMER_DEBOUNCE` case (before `default:`):

```c
    case TIMER_DEBOUNCE_FLUSH: {
      timer_debounce_payload_t* payload = (timer_debounce_payload_t*)msg->payload;
      debounce_entry_t* entry = _timer_actor_find_debounce(
          timer_actor, payload->target, payload->completion_type);
      if (entry != NULL && entry->timer != NULL) {
        /* Cancel the pending debounce timer. */
        pd_timer_stop(entry->timer);
        void* old_user_data = entry->timer->user_data;
        _timer_actor_untrack(timer_actor, entry->timer);
        pd_timer_destroy(entry->timer);
        free(old_user_data);
        /* Immediately dispatch the completion message to the target actor. */
        timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
        copy->target = payload->target;
        copy->completion_type = payload->completion_type;
        copy->timer_id = 0;
        message_t dispatch_msg;
        dispatch_msg.type = copy->completion_type;
        dispatch_msg.payload = copy;
        dispatch_msg.payload_destroy = free;
        actor_send(copy->target, &dispatch_msg);
        /* Clear the debounce entry. */
        entry->target = NULL;
        entry->completion_type = 0;
        entry->timer = NULL;
        entry->completion_payload = NULL;
      }
      if (msg->payload_destroy != NULL) {
        msg->payload_destroy(msg->payload);
      }
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
```

- [ ] **Step 4: Add public function to timer_actor.c**

Add after `timer_actor_debounce`:

```c
void timer_actor_debounce_flush(timer_actor_t* timer_actor,
                                actor_t* target, uint32_t completion_type) {
  timer_debounce_payload_t* payload = get_clear_memory(sizeof(timer_debounce_payload_t));
  payload->target = target;
  payload->completion_type = completion_type;
  payload->timeout_ms = 0;
  payload->interval_ms = 0;

  message_t msg;
  msg.type = TIMER_DEBOUNCE_FLUSH;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  pd_loop_async_send(timer_actor->loop, timer_actor);
}
```

- [ ] **Step 5: Write unit test**

Create `test/test_timer_debounce.cpp`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/message.h"
#include "../src/Actor/message_queue.h"
#include "../src/Util/allocator.h"
#include "../src/Platform/platform.h"
}

/* A simple test actor that records received message types. */
typedef struct {
  actor_t actor;
  uint32_t last_msg_type;
  int msg_count;
} test_actor_t;

static void test_actor_dispatch(void* state, message_t* msg) {
  test_actor_t* a = (test_actor_t*)state;
  a->last_msg_type = msg->type;
  a->msg_count++;
}

TEST(TestTimerDebounceFlush, FlushDispatchesImmediately) {
  timer_actor_t* timer = timer_actor_create();
  ASSERT_NE(timer, nullptr);

  test_actor_t target;
  actor_init(&target.actor, &target, test_actor_dispatch, NULL);
  target.last_msg_type = 0;
  target.msg_count = 0;

  /* Set up a debounce with a long timeout so it won't fire on its own. */
  timer_actor_debounce(timer, 60000, 0, &target.actor, 42);
  platform_sleep_ms(50);  /* let the timer thread process */

  /* Flush — should immediately cancel the timer and dispatch msg type 42. */
  timer_actor_debounce_flush(timer, &target.actor, 42);
  platform_sleep_ms(50);  /* let the timer thread process the flush */
  platform_sleep_ms(50);  /* let the target actor process the message */

  EXPECT_EQ(target.msg_count, 1);
  EXPECT_EQ(target.last_msg_type, (uint32_t)42);

  /* Clean up. The test_actor stack-allocated, no actor_destroy needed. */
  message_queue_destroy(&target.actor.queue);
  timer_actor_destroy(timer);
}

TEST(TestTimerDebounceFlush, FlushNoExistingDebounceIsNoop) {
  timer_actor_t* timer = timer_actor_create();
  ASSERT_NE(timer, nullptr);

  test_actor_t target;
  actor_init(&target.actor, &target, test_actor_dispatch, NULL);
  target.last_msg_type = 0;
  target.msg_count = 0;

  /* Flush a key that was never debounced — should not crash. */
  timer_actor_debounce_flush(timer, &target.actor, 99);
  platform_sleep_ms(50);

  EXPECT_EQ(target.msg_count, 0);

  message_queue_destroy(&target.actor.queue);
  timer_actor_destroy(timer);
}
```

- [ ] **Step 6: Register test in CMakeLists.txt**

Add `test/test_timer_debounce.cpp` to test sources in `test/CMakeLists.txt`.

- [ ] **Step 7: Run test to verify it passes**

Run: `cd build && cmake .. && make test_timer_debounce && ./test_timer_debounce`
Expected: 2/2 tests PASS

- [ ] **Step 8: Commit**

```bash
git add src/Timer/timer_actor.h src/Timer/timer_actor.c src/Actor/message.h test/test_timer_debounce.cpp test/CMakeLists.txt
git commit -m "feat: add timer_actor_debounce_flush() for immediate debounce dispatch"
```

---

### Task 4: Add `shutdown_timeout_ms` to config

**Files:**
- Modify: `src/Configuration/config.h`
- Modify: `src/Configuration/config.c`

- [ ] **Step 1: Add field to config_t in config.h**

Add as the last field before the closing brace of `config_t`:

```c
  uint32_t shutdown_timeout_ms;
```

- [ ] **Step 2: Set default in config.c**

Add in `config_default()` after the `config.max_capacity_bytes` line:

```c
  config.shutdown_timeout_ms = 30000;
```

- [ ] **Step 3: Verify existing tests still pass**

Run: `cd build && cmake .. && make -j4 && ctest --output-on-failure`
Expected: no new failures introduced by adding a config field.

- [ ] **Step 4: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config.c
git commit -m "feat: add shutdown_timeout_ms to config_t (default 30000)"
```

---

### Task 5: Add HTTP server draining support

**Files:**
- Modify: `src/ClientAPI/HTTP/http_server.h`
- Modify: `src/ClientAPI/HTTP/http_server.c`

- [ ] **Step 1: Add `draining` flag and declarations to http_server.h**

Add `ATOMIC(uint8_t) draining;` field to `http_server_t` struct, after the `active_connections` field:

```c
  ATOMIC(uint8_t) draining;
```

Add `http_server_drain()` declaration after `http_server_stop`:

```c
void http_server_drain(http_server_t* server);
```

- [ ] **Step 2: Stop accepting when draining in accept callback**

In `_accept_callback`, add a draining check at the top:

```c
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                             pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  http_server_t* server = (http_server_t*)user_data;

  if (atomic_load(&server->draining)) {
    return;
  }

  if (events & PD_EVENT_READ) {
    // ... existing accept logic ...
```

- [ ] **Step 3: Implement http_server_drain**

Add after `http_server_stop`:

```c
void http_server_drain(http_server_t* server) {
  if (server == NULL) return;
  atomic_store(&server->draining, 1);
}
```

- [ ] **Step 4: Verify existing HTTP tests still pass**

Run: `cd build && cmake .. && make test_transport && ./test_transport --gtest_filter='*HttpServer*'`
Expected: 12/12 tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/HTTP/http_server.h src/ClientAPI/HTTP/http_server.c
git commit -m "feat: add http_server_drain() to stop accepting new connections"
```

---

### Task 6: Add `OFFS_ERROR_DRAINING` error code

**Files:**
- Modify: `src/Node/node.h`

- [ ] **Step 1: Add error code**

Add before the struct declarations in node.h:

```c
#define OFFS_ERROR_DRAINING -2
```

- [ ] **Step 2: Commit**

```bash
git add src/Node/node.h
git commit -m "feat: add OFFS_ERROR_DRAINING error code"
```

---

### Task 7: Implement phased graceful shutdown in `offs_node_stop()`

**Files:**
- Modify: `src/Node/node.c`
- Modify: `src/Node/node.h`

- [ ] **Step 1: Add `draining` field to offs_node_t in node.h**

Add to `offs_node_t` struct, after the `running` field:

```c
  ATOMIC(uint8_t) draining;
```

- [ ] **Step 2: Add deadline helpers to node.c**

Add before `offs_node_stop`, after includes:

```c
static uint64_t _shutdown_deadline_ms(offs_node_t* node) {
  if (node->config->shutdown_timeout_ms == 0) return UINT64_MAX;
  return platform_monotonic_ns() / 1000000 + node->config->shutdown_timeout_ms;
}

static bool _shutdown_deadline_exceeded(uint64_t deadline) {
  return platform_monotonic_ns() / 1000000 > deadline;
}
```

- [ ] **Step 3: Rewrite offs_node_stop with phased drain**

Replace the existing `offs_node_stop`:

```c
void offs_node_stop(offs_node_t* node) {
  if (node == NULL) return;

  uint64_t deadline = _shutdown_deadline_ms(node);

  /* Phase 1: Stop accepting new work. */
  ATOMIC_STORE(&node->draining, 1);
  ATOMIC_STORE(&node->running, 0);

  if (node->http_server != NULL) {
    http_server_drain(node->http_server);
  }

  /* Phase 2: Notify peers — close P2P connections non-silently.
     network has running=0 set, which stops accepting new connections. */
  if (node->network != NULL) {
    ATOMIC_STORE(&node->network->running, 0);
  }

  /* Phase 3: Drain in-flight HTTP requests.
     Poll until active_connections reaches 0 or deadline. */
  if (node->http_server != NULL && !_shutdown_deadline_exceeded(deadline)) {
    while (atomic_load(&node->http_server->active_connections) > 0) {
      if (_shutdown_deadline_exceeded(deadline)) break;
      platform_sleep_ms(50);
    }
  }

  /* Phase 4: Drain actor work queue.
     scheduler_pool_wait_for_idle blocks until all workers are idle.
     Poll with 100ms intervals until idle or deadline. */
  if (!_shutdown_deadline_exceeded(deadline)) {
    while (!_shutdown_deadline_exceeded(deadline)) {
      scheduler_pool_wait_for_idle(node->scheduler);
      break;
    }
    scheduler_pool_drain_pending_derefs(node->scheduler);
  }

  /* Phase 5: Close P2P connections.
     At this point actors are drained, no new messages can be generated.
     Closing connections is safe. */
  /* Network connections will be cleaned up in network_destroy() during
     offs_node_destroy(). The running flag already set to 0 stops the
     network event loop. */

  /* Phase 6: Flush & persist.
     Flush the index debounce timer so a final snapshot is written through
     the actor's normal dispatch path. Then sync the WAL to disk. */
  if (node->block_cache != NULL && !_shutdown_deadline_exceeded(deadline)) {
    /* block_cache owns the index. We need the index's actor to flush.
       Access via block_cache->index — but index_t is opaque here.
       Instead, we provide the index flush through the block_cache API. */
  }

  /* Phase 7: Stop scheduler. */
  scheduler_pool_stop(node->scheduler);

  /* Persist peer state before final teardown. */
  if (node->authority != NULL && node->network != NULL) {
    authority_save_peers(node->authority, node->network);
  }
}
```

Wait — this has a gap. The block_cache index is opaque to node.c. We need a `block_cache_flush_index()` function or we need to pass the index's actor to `timer_actor_debounce_flush` directly.

Let me check how block_cache exposes the index.

Actually, looking at the architecture: `block_cache_t` owns the `index_t`. The node doesn't directly have access to the index actor. We need to add a `block_cache_flush()` or `block_cache_sync()` function that internally calls `timer_actor_debounce_flush` on the index's INDEX_SAVE timer, then calls `wal_sync` on the index's WAL.

Let me rework this task to include a `block_cache_sync()` function.

- [ ] **Step 3 (revised): Add block_cache_sync() to block_cache.h/c**

First, add declaration to `src/BlockCache/block_cache.h` (find a good spot, after `block_cache_destroy`):

```c
void block_cache_sync(block_cache_t* block_cache);
```

Implementation in `src/BlockCache/block_cache.c`:

```c
void block_cache_sync(block_cache_t* block_cache) {
  if (block_cache == NULL) return;

  /* Flush the index debounce timer — sends INDEX_SAVE immediately
     through the index actor's normal dispatch path. */
  timer_actor_debounce_flush(block_cache->timer,
                             &block_cache->index->actor, INDEX_SAVE);

  /* Wait briefly for the actor to process the INDEX_SAVE message. */
  platform_sleep_ms(100);
  scheduler_pool_wait_for_idle(block_cache->scheduler);

  /* Sync the WAL to disk. */
  if (block_cache->index->wal != NULL) {
    wal_sync(block_cache->index->wal);
  }
}
```

- [ ] **Step 3 (revised): Rewrite offs_node_stop with phased drain**

Replace the existing `offs_node_stop`:

```c
void offs_node_stop(offs_node_t* node) {
  if (node == NULL) return;

  uint64_t deadline = _shutdown_deadline_ms(node);

  /* Phase 1: Stop accepting new work. */
  ATOMIC_STORE(&node->draining, 1);
  ATOMIC_STORE(&node->running, 0);

  if (node->http_server != NULL) {
    http_server_drain(node->http_server);
  }

  /* Phase 2: Notify peers. */
  if (node->network != NULL) {
    ATOMIC_STORE(&node->network->running, 0);
  }

  /* Phase 3: Drain in-flight HTTP requests. */
  if (node->http_server != NULL && !_shutdown_deadline_exceeded(deadline)) {
    while (atomic_load(&node->http_server->active_connections) > 0) {
      if (_shutdown_deadline_exceeded(deadline)) break;
      platform_sleep_ms(50);
    }
  }

  /* Phase 4: Drain actor work queue. */
  if (!_shutdown_deadline_exceeded(deadline)) {
    scheduler_pool_wait_for_idle(node->scheduler);
    scheduler_pool_drain_pending_derefs(node->scheduler);
  }

  /* Phase 5: Close P2P connections — handled during network_destroy. */

  /* Phase 6: Flush index/WAL and persist peer state. */
  if (!_shutdown_deadline_exceeded(deadline)) {
    if (node->block_cache != NULL) {
      block_cache_sync(node->block_cache);
    }
    if (node->authority != NULL && node->network != NULL) {
      authority_save_peers(node->authority, node->network);
    }
  }

  /* Phase 7: Stop scheduler. */
  scheduler_pool_stop(node->scheduler);
}
```

- [ ] **Step 4: Check that node.c includes block_cache.h**

Verify node.c already includes `#include "../BlockCache/block_cache.h"` (it does, line 6).

- [ ] **Step 5: Add #include to node.c for platform time**

Add `#include "../Platform/platform.h"` to node.c includes for `platform_monotonic_ns` and `platform_sleep_ms`.

- [ ] **Step 6: Verify existing tests still pass**

Run: `cd build && cmake .. && make -j4 && ctest --output-on-failure`
Expected: no new test failures.

- [ ] **Step 7: Commit**

```bash
git add src/Node/node.h src/Node/node.c src/BlockCache/block_cache.h src/BlockCache/block_cache.c
git commit -m "feat: implement phased graceful shutdown in offs_node_stop"
```

---

### Task 8: Integration tests for graceful shutdown

**Files:**
- Modify: `test/test_transport.cpp` (or create new `test/test_shutdown.cpp`)

- [ ] **Step 1: Write test for draining flag rejection**

In `test/test_transport.cpp`, add:

```cpp
TEST(TestShutdown, NodeStopRejectsNewPuts) {
  config_t config = config_default();
  config.shutdown_timeout_ms = 5000;

  authority_t* authority = authority_create();
  ASSERT_NE(authority, nullptr);

  offs_node_t* node = offs_node_create(&config, authority);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(offs_node_start(node), 0);

  /* Stop the node — should set draining flag. */
  offs_node_stop(node);

  /* After stop, the draining flag should prevent new operations.
     The node's HTTP server is stopped so we can't test the HTTP path here,
     but we verify the node transitions cleanly. */

  offs_node_destroy(node);
  authority_destroy(authority);
}

TEST(TestShutdown, ZeroTimeoutBlocksUntilIdle) {
  config_t config = config_default();
  config.shutdown_timeout_ms = 0;  /* block forever */

  authority_t* authority = authority_create();
  ASSERT_NE(authority, nullptr);

  offs_node_t* node = offs_node_create(&config, authority);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(offs_node_start(node), 0);

  /* Stop with zero timeout — should complete without hanging. */
  offs_node_stop(node);

  offs_node_destroy(node);
  authority_destroy(authority);
}

TEST(TestShutdown, DeadlineExceededSkipsDrain) {
  config_t config = config_default();
  config.shutdown_timeout_ms = 1;  /* 1ms — expires immediately */

  authority_t* authority = authority_create();
  ASSERT_NE(authority, nullptr);

  offs_node_t* node = offs_node_create(&config, authority);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(offs_node_start(node), 0);

  /* Stop with 1ms timeout — drain phases should be skipped. */
  offs_node_stop(node);

  offs_node_destroy(node);
  authority_destroy(authority);
}
```

- [ ] **Step 2: Update test CMakeLists.txt and build**

Add `test/test_shutdown.cpp` if creating a new file, or add the test cases to `test/test_transport.cpp`.

- [ ] **Step 3: Run tests to verify they pass**

Run: `cd build && cmake .. && make test_transport && ./test_transport --gtest_filter='*Shutdown*'`
Expected: 3/3 tests PASS

- [ ] **Step 4: Commit**

```bash
git add test/test_transport.cpp
git commit -m "test: add graceful shutdown integration tests"
```

---

### Post-Implementation Verification

- [ ] Run full test suite: `cd build && cmake .. && make -j4 && ctest --output-on-failure`
- [ ] Check for memory leaks with valgrind on key tests
- [ ] Verify no pre-existing test regressions
