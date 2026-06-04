# Timer Actor Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redesign timer_actor to follow the network connection actor pattern — schedulable on the pool, two-hop completion dispatch, destroy stack for pd_timer_destroy, and actors cancel timers on destroy with ACTOR_FLAG_DESTROY safety net.

**Architecture:** The timer_actor becomes a schedulable actor on the scheduler pool like http_server/quic_listener. Its I/O thread becomes a thin notification loop (pd_loop_run_once + destroy_stack_drain). Timer completions go through the timer_actor (Pony-style two-hop: callback → timer_actor → target). Actors that register timers must cancel them on destroy. The timer_actor checks ACTOR_FLAG_DESTROY as a defense-in-depth safety net.

**Tech Stack:** C11, pthreads, poll-dancer (timerfd/epoll), liboffs actor/scheduler system

---

### Task 1: Add destroy stack and pool to timer_actor struct and header

**Files:**
- Modify: `src/Timer/timer_actor.h`

This task updates the header with the new struct fields, new message type, and changed API signature. No implementation yet — just the interface.

- [ ] **Step 1: Update timer_actor_t struct**

Add `destroy_lock`, `destroy_stack` fields to `timer_actor_t`. Add `timer_actor` back-reference to `timer_completion_payload_t`. Add `TIMER_COMPLETION` message type. Change `timer_actor_create` signature to accept `pool`.

```c
// Add after the debounce_map field in timer_actor_t:
  platform_mutex_t* destroy_lock;
  struct timer_destroy_node_t* destroy_stack;
```

```c
// Add the TIMER_COMPLETION message type (after existing TIMER_DEBOUNCE_FLUSH):
#define TIMER_COMPLETION 5
```

```c
// Update timer_completion_payload_t to add back-reference:
typedef struct {
  uint64_t timer_id;
  actor_t* target;
  uint32_t completion_type;
  timer_actor_t* timer_actor;  // back-reference for two-hop dispatch
} timer_completion_payload_t;
```

```c
// Add destroy stack node type:
typedef struct timer_destroy_node_t {
  struct timer_destroy_node_t* next;
  pd_timer_t* timer;
  void* user_data;
} timer_destroy_node_t;
```

```c
// Change timer_actor_create signature:
timer_actor_t* timer_actor_create(scheduler_pool_t* pool);
```

- [ ] **Step 2: Commit**

```bash
git add src/Timer/timer_actor.h
git commit -m "refactor: update timer_actor header for schedulable actor redesign"
```

---

### Task 2: Rewrite timer_actor.c — schedulable actor with two-hop completion and destroy stack

**Files:**
- Modify: `src/Timer/timer_actor.c`

This is the core implementation change. The timer_actor becomes schedulable on the pool, uses a destroy stack for pd_timer_destroy, implements two-hop completion dispatch, and adds the ACTOR_FLAG_DESTROY safety net.

- [ ] **Step 1: Add destroy stack functions**

Add `_destroy_stack_init`, `_destroy_stack_push`, `_destroy_stack_drain`, `_destroy_stack_destroy` following the identical pattern from `src/ClientAPI/HTTP/http_server.c` lines 23-43:

```c
static void _destroy_stack_init(timer_actor_t* ta) {
  ta->destroy_lock = platform_mutex_create();
  ta->destroy_stack = NULL;
}

static void _destroy_stack_push(timer_actor_t* ta, pd_timer_t* timer, void* user_data) {
  timer_destroy_node_t* node = get_clear_memory(sizeof(timer_destroy_node_t));
  node->timer = timer;
  node->user_data = user_data;
  platform_mutex_lock(ta->destroy_lock);
  node->next = ta->destroy_stack;
  ta->destroy_stack = node;
  platform_mutex_unlock(ta->destroy_lock);
  pd_loop_async_send(ta->loop, NULL);
}

static void _destroy_stack_drain(timer_actor_t* ta) {
  platform_mutex_lock(ta->destroy_lock);
  timer_destroy_node_t* node = ta->destroy_stack;
  ta->destroy_stack = NULL;
  platform_mutex_unlock(ta->destroy_lock);
  while (node != NULL) {
    timer_destroy_node_t* next = node->next;
    pd_timer_destroy(node->timer);
    free(node->user_data);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(timer_actor_t* ta) {
  _destroy_stack_drain(ta);
  platform_mutex_destroy(ta->destroy_lock);
}
```

- [ ] **Step 2: Rewrite _timer_completion_callback for two-hop dispatch**

The callback now sends to the timer_actor instead of the target. It uses the back-reference in the completion payload:

```c
static void _timer_completion_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                        pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  (void)events;
  timer_completion_payload_t* completion = (timer_completion_payload_t*)user_data;
  timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
  *copy = *completion;
  message_t msg = {0};
  msg.type = TIMER_COMPLETION;
  msg.payload = copy;
  msg.payload_destroy = free;
  actor_send(&completion->timer_actor->actor, &msg);
}
```

- [ ] **Step 3: Add TIMER_COMPLETION dispatch case with safety net**

In `_timer_actor_dispatch`, add a new case before `default:`:

```c
case TIMER_COMPLETION: {
  timer_completion_payload_t* completion = (timer_completion_payload_t*)msg->payload;
  if (atomic_load(&completion->target->flags) & ACTOR_FLAG_DESTROY) {
    break;
  }
  message_t target_msg = {0};
  target_msg.type = completion->completion_type;
  target_msg.payload = completion;
  target_msg.payload_destroy = free;
  actor_send(completion->target, &target_msg);
  msg->payload = NULL;
  msg->payload_destroy = NULL;
  break;
}
```

Note: `msg->payload = NULL` prevents `actor_run` from also freeing the payload — ownership transfers to the target_msg.

- [ ] **Step 4: Update TIMER_SET to set timer_actor back-reference**

In the `TIMER_SET` case, after creating the completion payload, set the back-reference:

```c
completion->timer_actor = timer_actor;
```

Do the same for `TIMER_DEBOUNCE` and `TIMER_DEBOUNCE_FLUSH` where completion payloads are created.

- [ ] **Step 5: Update TIMER_CANCEL to use destroy stack**

Replace the current direct `pd_timer_destroy(timer)` with destroy stack push:

```c
case TIMER_CANCEL: {
  timer_cancel_payload_t* payload = (timer_cancel_payload_t*)msg->payload;
  if (payload->timer_id != 0) {
    pd_timer_t* timer = (pd_timer_t*)(uintptr_t)payload->timer_id;
    void* user_data = timer->user_data;
    pd_timer_stop(timer);
    _timer_actor_untrack(timer_actor, timer);
    _destroy_stack_push(timer_actor, timer, user_data);
  }
  // ... existing payload cleanup
  break;
}
```

Do the same for `TIMER_DEBOUNCE` where old timers are cancelled (replace `pd_timer_destroy(entry->timer)` with `_destroy_stack_push`).

- [ ] **Step 6: Rewrite _timer_actor_thread to be thin like http_server**

```c
static void* _timer_actor_thread(void* arg) {
  platform_thread_setup_stack();
  timer_actor_t* timer_actor = (timer_actor_t*)arg;
  while (atomic_load(&timer_actor->running)) {
    _destroy_stack_drain(timer_actor);
    int result = pd_loop_run_once(timer_actor->loop, 100);
    if (result < 0) {
      break;
    }
  }
  return NULL;
}
```

Key change: remove `actor_run(&timer_actor->actor, ACTOR_BATCH_SIZE)` from the loop.

- [ ] **Step 7: Update timer_actor_create to accept pool**

```c
timer_actor_t* timer_actor_create(scheduler_pool_t* pool) {
  timer_actor_t* timer_actor = get_clear_memory(sizeof(timer_actor_t));
  actor_init(&timer_actor->actor, timer_actor, _timer_actor_dispatch, pool);
  timer_actor->loop = pd_loop_create(NULL);
  if (timer_actor->loop == NULL) {
    free(timer_actor);
    return NULL;
  }
  _destroy_stack_init(timer_actor);
  atomic_store(&timer_actor->running, 1);
  timer_actor->thread = platform_thread_create(_timer_actor_thread, timer_actor);
  return timer_actor;
}
```

- [ ] **Step 8: Update timer_actor_destroy**

Add destroy stack cleanup and remove the `actor_run` reference:

```c
void timer_actor_destroy(timer_actor_t* timer_actor) {
  if (timer_actor == NULL) return;
  atomic_store(&timer_actor->running, 0);
  pd_loop_async_send(timer_actor->loop, NULL);
  pd_loop_stop(timer_actor->loop);
  platform_thread_join(timer_actor->thread);
  for (size_t i = 0; i < timer_actor->active_timer_count; i++) {
    pd_timer_t* timer = timer_actor->active_timers[i];
    if (timer != NULL) {
      void* user_data = timer->user_data;
      pd_timer_stop(timer);
      pd_timer_destroy(timer);
      free(user_data);
    }
  }
  free(timer_actor->active_timers);
  _destroy_stack_destroy(timer_actor);
  pd_loop_destroy(timer_actor->loop);
  actor_destroy(&timer_actor->actor);
  free(timer_actor);
}
```

Since `timer_actor_destroy` runs after the I/O thread is joined, `pd_timer_destroy` is safe to call directly here (we're on the I/O thread's former context, and the loop is stopped).

- [ ] **Step 9: Update public API functions**

`timer_actor_set`, `timer_actor_cancel`, `timer_actor_debounce`, `timer_actor_debounce_flush` all follow the same pattern: `actor_send` + `pd_loop_async_send`. The `pd_loop_async_send` is now only needed to wake the I/O thread for destroy stack drain. But since `actor_send` may schedule the timer_actor on the pool, the dispatch will happen on a scheduler worker thread — which is correct.

No signature changes needed for these functions.

- [ ] **Step 10: Build and verify compilation**

```bash
cd build_noasan && cmake .. && make -j$(nproc)
```

Expected: clean compilation (test call sites will still have wrong `timer_actor_create()` args — that's Task 3).

- [ ] **Step 11: Commit**

```bash
git add src/Timer/timer_actor.c
git commit -m "feat: rewrite timer_actor as schedulable actor with two-hop completion and destroy stack"
```

---

### Task 3: Update all production call sites for timer_actor_create(pool)

**Files:**
- Modify: `src/Node/node.c`
- Modify: `src/BlockCache/block_cache.c`
- Modify: `src/BlockCache/block_cache.h`

Every call to `timer_actor_create()` must now pass a `scheduler_pool_t*`.

- [ ] **Step 1: Update node.c**

At `src/Node/node.c:31`, the offs_node_t creates the timer_actor. Change:
```c
timer_actor_t* timer = timer_actor_create();
```
to:
```c
timer_actor_t* timer = timer_actor_create(pool);
```

The pool is available from the `offs_node_start` function parameter. Check that `pool` is accessible at the call site. At line 237, the same change applies in `offs_node_restart`.

- [ ] **Step 2: Verify block_cache.h and block_cache.c don't create timer_actors**

`block_cache_create` receives a `timer_actor_t*` parameter — it doesn't create one. No changes needed in block_cache.c for creation.

- [ ] **Step 3: Commit**

```bash
git add src/Node/node.c
git commit -m "refactor: pass pool to timer_actor_create in node.c"
```

---

### Task 4: Put index and section actors on the scheduler pool

**Files:**
- Modify: `src/BlockCache/index.c`
- Modify: `src/BlockCache/section.c`

Both currently have `pool=NULL` in their `actor_init` calls. They need the pool so that timer completion messages (INDEX_SAVE, SECTION_SAVE_META) can be dispatched by the scheduler.

- [ ] **Step 1: Add pool parameter to _index_new_empty**

In `src/BlockCache/index.c`, `_index_new_empty` (line 170) currently doesn't take a pool parameter. Add one and pass it to `actor_init`:

Change signature:
```c
static index_t* _index_new_empty(size_t bucket_size, char* location,
    timer_actor_t* timer_actor, scheduler_pool_t* pool,
    uint64_t wait, uint64_t max_wait, uint64_t most_recent_id,
    size_t max_snapshots, size_t max_wals)
```

Change `actor_init` at line 190:
```c
actor_init(&index->actor, index, index_dispatch, pool);
```

- [ ] **Step 2: Add pool parameter to index_create and index_create_from**

`index_create` (line 197) and `index_create_from` (line 462) both call `_index_new_empty`. Add `scheduler_pool_t* pool` to their signatures (after `timer_actor_t*`), update `index.h` declarations, and pass pool through to `_index_new_empty`.

Update `cbor_to_index` (line 1021) similarly — add pool parameter.

- [ ] **Step 3: Update section.c**

At `src/BlockCache/section.c:261`, change:
```c
actor_init(&section->actor, section, section_dispatch, NULL);
```
Add `scheduler_pool_t* pool` parameter to `section_create` and pass it:
```c
actor_init(&section->actor, section, section_dispatch, pool);
```

Update `section.h` declaration accordingly.

- [ ] **Step 4: Update all callers of index_create, index_create_from, section_create**

These are called from `block_cache_create` and `sections_create`. Pass the pool through. Follow the call chain:
- `block_cache_create` already has a `pool` param → pass to `index_create` and `sections_create`
- `sections_create` already has a `pool` param → pass to `round_robin_create` and `section_create`
- `round_robin_create` → pass pool if it creates sections
- `cbor_to_index` → pass pool

- [ ] **Step 5: Build and verify compilation**

```bash
cd build_noasan && make -j$(nproc)
```

Expected: compilation errors only in test files (fixed in Task 5).

- [ ] **Step 6: Commit**

```bash
git add src/BlockCache/index.c src/BlockCache/index.h src/BlockCache/section.c src/BlockCache/section.h src/BlockCache/block_cache.c src/BlockCache/block_cache.h src/BlockCache/sections.c src/BlockCache/sections.h
git commit -m "refactor: put index and section actors on scheduler pool"
```

---

### Task 5: Update all test call sites for timer_actor_create(pool) and new signatures

**Files:**
- Modify: All 18 test files that call `timer_actor_create()`

Every test that calls `timer_actor_create()` must pass a pool. Every test that calls `index_create`, `index_create_from`, or `section_create` must pass a pool.

- [ ] **Step 1: Update test_timer_actor.cpp**

This file has the most timer_actor usage. Each `timer_actor_create()` call needs a pool. Some tests in this file currently don't create a scheduler pool — they rely on the timer_actor's own thread. Those tests need a pool added.

For each test fixture or standalone test:
- Add `scheduler_pool_t* pool = scheduler_pool_create(2); scheduler_pool_start(pool);` in SetUp
- Change `timer_actor_create()` to `timer_actor_create(pool)`
- Add `scheduler_pool_wait_for_idle(pool);` before teardown
- Ensure teardown order: `timer_actor_destroy` before `scheduler_pool_destroy`
- Add `scheduler_pool_stop(pool); scheduler_pool_destroy(pool);` in TearDown

Apply to all 9 `timer_actor_create()` calls in this file (lines 42, 48, 75, 102, 152, 193, 222, 246, 256).

- [ ] **Step 2: Update test_timer_debounce.cpp**

Same pattern: add pool, pass to timer_actor_create, fix teardown. Lines 28, 60.

- [ ] **Step 3: Update test_block_cache.cpp**

Two fixtures:
- `TestBlockCache` (line 204): Add pool, change `timer_actor_create()`, fix teardown order
- `TestBlockCacheIntegration` (line 333): Already has pool. Change `timer_actor_create()` to `timer_actor_create(pool)`. Fix teardown order (timer_actor_destroy before scheduler_pool_destroy).

- [ ] **Step 4: Update test_off_stream_integration.cpp**

Lines 78 and 138: Already have pool. Change `timer_actor_create()` to `timer_actor_create(pool)`.

- [ ] **Step 5: Update test_index.cpp**

Line 119: Add pool or pass existing pool. Update `index_create` calls for new pool parameter.

- [ ] **Step 6: Update remaining test files**

Apply the same pattern to each of these files (add pool if missing, change timer_actor_create call, fix teardown order):

- `test/test_offs_client.cpp` (lines 153, 519, 693)
- `test/test_off_routes.cpp` (line 111)
- `test/test_ws_transport.cpp` (line 225)
- `test/test_tcp_transport.cpp` (line 118)
- `test/test_quic_integration.cpp` (lines 616, 663)
- `test/test_health_handler.cpp` (line 130)
- `test/test_health_http.cpp` (lines 81, 197)
- `test/test_section.cpp` (lines 530, 581)
- `test/test_block_recipe.cpp` (lines 71, 123, 193, 337)
- `test/test_ofd_cache.cpp` (line 87)
- `test/test_unix_transport.cpp` (line 109)
- `test/test_stream_network.cpp` (lines 121, 372)
- `test/test_node_main.c` (line 1784)

- [ ] **Step 7: Build and run all tests**

```bash
cd build_noasan && make -j$(nproc) && ./test/testliboffs
```

Expected: All tests pass. Some tests that relied on timer_actor's inline dispatch may need adjustment (timer messages now go through the scheduler, which is async — tests may need `scheduler_pool_wait_for_idle` before assertions).

- [ ] **Step 8: Commit**

```bash
git add test/
git commit -m "refactor: update all test call sites for timer_actor_create(pool)"
```

---

### Task 6: Add timer cancellation in index_destroy and block_cache_destroy

**Files:**
- Modify: `src/BlockCache/index.c`
- Modify: `src/BlockCache/block_cache.c`
- Modify: `src/BlockCache/sections.c`

Actors that register timers must cancel them on destroy.

- [ ] **Step 1: Add debounce flush in index_destroy**

In `src/BlockCache/index.c`, at `index_destroy` (line 940), before `index_debounce(index)`, add:

```c
if (index->timer_actor != NULL) {
  timer_actor_debounce_flush(index->timer_actor, &index->actor, INDEX_SAVE);
}
```

- [ ] **Step 2: Add debounce flush for sections in sections_destroy / round_robin_destroy**

In `src/BlockCache/sections.c`, find where `sections_t` and `round_robin_t` are destroyed. Add debounce flush for `SECTION_SAVE_META` timers:

```c
if (robin->timer_actor != NULL) {
  timer_actor_debounce_flush(robin->timer_actor, robin->save_target, SECTION_SAVE_META);
}
```

- [ ] **Step 3: Add timer cancellation in block_cache_destroy**

In `src/BlockCache/block_cache.c`, at `block_cache_destroy` (line 621), before `index_destroy`, add:

```c
if (block_cache->index != NULL && block_cache->index->timer_actor != NULL) {
  timer_actor_debounce_flush(block_cache->index->timer_actor,
                              &block_cache->index->actor, INDEX_SAVE);
}
```

Note: this is in addition to the flush inside `index_destroy` itself — defense in depth.

- [ ] **Step 4: Build and run all tests**

```bash
cd build_noasan && make -j$(nproc) && ./test/testliboffs
```

Expected: All tests pass. Timer UAF should no longer be reproducible.

- [ ] **Step 5: Commit**

```bash
git add src/BlockCache/index.c src/BlockCache/block_cache.c src/BlockCache/sections.c
git commit -m "fix: add timer cancellation in index_destroy, block_cache_destroy, and sections_destroy"
```

---

### Task 7: Fix secondary leaks and ordering issues found during investigation

**Files:**
- Modify: `src/OFFStreams/readable_off_stream.c`
- Modify: `src/OFFStreams/writeable_off_stream.c`

These are bugs found during the memory corruption investigation.

- [ ] **Step 1: Fix pending_fetches leak in readable_off_stream_destroy**

In `src/OFFStreams/readable_off_stream.c`, at `readable_off_stream_destroy` (line 352), add cleanup for `pending_fetches` before `stream_deinit`:

```c
void readable_off_stream_destroy(readable_off_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*)stream)) {
    pending_tuple_t* queued = stream->tuple_queue;
    while (queued != NULL) {
      pending_tuple_t* next = queued->next;
      DESTROY(queued->tuple, tuple);
      free(queued);
      queued = next;
    }
    if (stream->pending_tuple != NULL) {
      DESTROY(stream->pending_tuple, tuple);
    }
    pending_block_fetch_t* fetch = stream->pending_fetches;
    while (fetch != NULL) {
      pending_block_fetch_t* next = fetch->next;
      DESTROY(fetch->hash, buffer);
      free(fetch);
      fetch = next;
    }
    stream_deinit((stream_t*)stream);
    free(stream);
  }
}
```

- [ ] **Step 2: Fix is_deactivated ordering in writeable_off_stream**

In `src/OFFStreams/writeable_off_stream.c`, at `_maybe_finalize` (line 52), move `is_deactivated` flag set BEFORE `stream_notify(close_event)`:

```c
stream->stream.is_deactivated = 1;
stream_notify((stream_t*)stream, close_event, NULL, NULL);
```

- [ ] **Step 3: Build and run all tests**

```bash
cd build_noasan && make -j$(nproc) && ./test/testliboffs
```

Expected: All tests pass. Valgrind should show 0 definitely-lost bytes for readable_off_stream tests.

- [ ] **Step 4: Commit**

```bash
git add src/OFFStreams/readable_off_stream.c src/OFFStreams/writeable_off_stream.c
git commit -m "fix: clean up pending_fetches in readable_off_stream_destroy, fix is_deactivated ordering in writeable_off_stream"
```

---

### Task 8: Run valgrind to verify zero UAF and zero leaks

**Files:**
- No file changes — verification only

- [ ] **Step 1: Run valgrind on timer actor tests**

```bash
valgrind --tool=memcheck --leak-check=full --run-libc-freeres=no --num-callers=20 build_noasan/test/testliboffs --gtest_filter='*Timer*' 2>&1 | tail -20
```

Expected: 0 definitely lost, 0 invalid reads/writes.

- [ ] **Step 2: Run valgrind on block cache tests**

```bash
valgrind --tool=memcheck --leak-check=full --run-libc-freeres=no --num-callers=20 build_noasan/test/testliboffs --gtest_filter='*BlockCache*' 2>&1 | tail -20
```

Expected: 0 definitely lost, 0 invalid reads/writes. No "Invalid read of size 4" at pthread_mutex_lock.

- [ ] **Step 3: Run valgrind on off-stream tests**

```bash
valgrind --tool=memcheck --leak-check=full --run-libc-freeres=no --num-callers=20 build_noasan/test/testliboffs --gtest_filter='*OffStream*' 2>&1 | tail -20
```

Expected: 0 definitely lost (pending_fetches leak should be gone).

- [ ] **Step 4: Run valgrind on full test suite**

```bash
valgrind --tool=memcheck --leak-check=full --run-libc-freeres=no --num-callers=20 build_noasan/test/testliboffs 2>&1 | grep -E 'Invalid|definitely lost|ERROR SUMMARY'
```

Expected: 0 invalid reads/writes, 0 definitely lost bytes. Possibly-lost bytes from pthread thread stacks are acceptable (valgrind false positive).

---

### Task 9: Final commit and cleanup

- [ ] **Step 1: Run ASAN build to confirm no sanitizer errors**

If an ASAN build exists, run the full test suite under it. Otherwise, build with ASAN:

```bash
mkdir -p build-asan && cd build-asan && cmake -DCMAKE_C_FLAGS="-fsanitize=address -fsanitize=undefined -gdwarf-4" -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=undefined -gdwarf-4" .. && make -j$(nproc) && ./test/testliboffs
```

Expected: All tests pass with no ASAN/UBSAN errors.

- [ ] **Step 2: Commit any remaining fixes**

If the ASAN run found issues, fix them and commit.

- [ ] **Step 3: Update architecture doc**

Update `docs/ARCHITECTURE.md` "Current Development Status" section to remove "Weird memory corruption on locks" and "Memory leak closing streams" since they are now fixed.