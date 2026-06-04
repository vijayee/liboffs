# Timer Actor Redesign

Redesign the timer_actor to follow the same architecture as network connection
actors (http_server, quic_listener, relay_client/server), eliminating the
use-after-free on scheduler pool locks and establishing proper timer lifecycle
ownership.

## Problem

The timer_actor runs its own I/O thread with `pool=NULL` and calls
`actor_run` directly on that thread. When a timer fires, the completion
callback calls `actor_send(target_actor)` which accesses `pool->inject.lock`.
If the pool has been destroyed before the timer_actor, the lock memory is
freed — causing the "weird memory corruption on locks" crash
(`pthread_mutex_lock` with `__kind=-1`).

Additionally, no actors cancel their registered timers on destroy. The index
actor registers `INDEX_SAVE` debounce timers targeting `&index->actor` but
never calls `timer_actor_debounce_flush` or `timer_actor_cancel` in
`index_destroy`. After the index is freed (memory poisoned with `0xDD`),
timer callbacks access freed actor memory.

The index actor also has `pool=NULL`, meaning `INDEX_SAVE` messages enqueued
via `actor_send` are never dispatched by the scheduler — they sit in the
queue indefinitely.

## Design

### 1. Timer Actor on the Scheduler

The timer_actor follows the network connection pattern: actor on the
scheduler pool, I/O thread as thin notification mechanism.

**Before:**
```
I/O thread: actor_run(timer_actor) + pd_loop_run_once
timer_actor has pool=NULL
timer callback → actor_send(target) → scheduler_inject(pool) → UAF
```

**After:**
```
I/O thread: destroy_stack_drain + pd_loop_run_once
timer_actor has pool=scheduler_pool
timer callback → actor_send(timer_actor) → scheduler → timer_actor dispatch → actor_send(target) → scheduler
```

The `timer_actor_create` function accepts a `scheduler_pool_t*` parameter
and passes it to `actor_init`. The I/O thread no longer calls `actor_run`.

### 2. I/O Thread (Thin, Like http_server)

The I/O thread becomes identical to `_server_thread` in http_server:

```c
while (atomic_load(&ta->running)) {
    _destroy_stack_drain(ta);
    pd_loop_run_once(ta->loop, 100);
}
```

- Remove `actor_run(&timer_actor->actor, ACTOR_BATCH_SIZE)` from the loop
- Add destroy stack for deferred `pd_timer_destroy`
- External API calls `pd_loop_async_send(ta->loop, NULL)` to wake the I/O
  thread for destroy stack drain (replaces current usage that wakes for
  `actor_run`)

### 3. Timer Completion Path (Two-Hop, Pony-style)

Timer completions go through the timer_actor, not directly to targets.
This matches Pony's pattern: ASIO → Timers actor → target actor.

**New callback:**
```c
static void _timer_completion_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                        pd_event_t events, void* user_data) {
    timer_completion_payload_t* completion = user_data;
    timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
    *copy = *completion;
    message_t msg = {0};
    msg.type = TIMER_COMPLETION;
    msg.payload = copy;
    msg.payload_destroy = free;
    actor_send(&completion->timer_actor->actor, &msg);
}
```

The completion payload gains a `timer_actor` back-reference so the callback
knows where to send the message:

```c
typedef struct {
    uint64_t timer_id;
    actor_t* target;
    uint32_t completion_type;
    timer_actor_t* timer_actor;  // NEW: back-reference for two-hop dispatch
} timer_completion_payload_t;
```

**New dispatch case in timer_actor:**
```c
case TIMER_COMPLETION: {
    timer_completion_payload_t* completion = msg->payload;
    if (atomic_load(&completion->target->flags) & ACTOR_FLAG_DESTROY) {
        // Safety net: target is destroyed, drop completion
        break;
    }
    message_t target_msg = {0};
    target_msg.type = completion->completion_type;
    target_msg.payload = completion;
    target_msg.payload_destroy = free;
    actor_send(completion->target, &target_msg);
    break;
}
```

### 4. Destroy Stack for pd_timer_destroy

`pd_timer_destroy` must happen on the I/O thread (same constraint as
`pd_watcher_destroy` in network code). Add a destroy stack to the
timer_actor identical to the one in http_server/quic_listener:

```c
typedef struct timer_destroy_node_t {
    struct timer_destroy_node_t* next;
    pd_timer_t* timer;
    void* user_data;  // the completion payload to free
} timer_destroy_node_t;
```

When TIMER_CANCEL or TIMER_DEBOUNCE dispatches on a scheduler worker thread:
1. Call `pd_timer_stop(timer)` (thread-safe, calls epoll_ctl)
2. Push timer to destroy stack
3. `pd_loop_async_send` to wake I/O thread

The I/O thread drains the destroy stack at the top of each loop iteration,
calling `pd_timer_destroy` and freeing the completion payload.

### 5. Actors Must Cancel Timers on Destroy (Semantic Contract)

Every actor that registers timers must cancel them in its destroy function.
This is the ownership contract: if you registered it, you cancel it.

**index_destroy:**
```c
void index_destroy(index_t* index) {
    if (refcounter_dereference_is_zero((refcounter_t*)index)) {
        // Cancel pending INDEX_SAVE timer
        if (index->timer_actor != NULL) {
            timer_actor_debounce_flush(index->timer_actor,
                                       &index->actor, INDEX_SAVE);
        }
        // ... existing cleanup
    }
}
```

**block_cache_destroy** and any other actor that registers timers — same
pattern.

### 6. Safety Net: ACTOR_FLAG_DESTROY Check (Defense in Depth)

The timer_actor checks `ACTOR_FLAG_DESTROY` on the target before forwarding
completions. This catches the case where an actor fails to cancel its timers
— the exact bug that exists today.

This mirrors Pony's safety net: Pony uses a cycle detector to prevent
dispatching to collected actors. liboffs uses `ACTOR_FLAG_DESTROY` for the
same purpose.

### 7. Index Actor on the Scheduler

The index actor currently has `pool=NULL`, meaning messages enqueued via
`actor_send` are never dispatched. Fix this by passing the block_cache's
pool to `actor_init` when creating the index:

```c
actor_init(&index->actor, index, index_dispatch, pool);
```

This ensures `INDEX_SAVE` messages (dispatched from timer completions) are
properly scheduled on worker threads.

## API Changes

### timer_actor_t struct

Add destroy stack fields to `timer_actor_t`:

```c
typedef struct timer_actor_t {
    actor_t actor;                          // pool is set via actor_init
    pd_loop_t* loop;
    platform_thread_t* thread;
    ATOMIC(uint8_t) running;
    pd_timer_t** active_timers;
    size_t active_timer_count;
    size_t active_timer_capacity;
    debounce_entry_t debounce_map[MAX_DEBOUNCE_KEYS];
    // NEW: destroy stack for deferred pd_timer_destroy on I/O thread
    platform_mutex_t* destroy_lock;
    timer_destroy_node_t* destroy_stack;
} timer_actor_t;
```

### timer_actor_create

```c
// Before:
timer_actor_t* timer_actor_create(void);

// After:
timer_actor_t* timer_actor_create(scheduler_pool_t* pool);
```

The pool is passed to `actor_init` and stored in `timer_actor->actor.pool`.

### timer_actor_destroy

No signature change. Internally:
1. Stop all active timers
2. Set `running=0`, wake I/O thread, join thread
3. Drain destroy stack
4. Destroy pd_loop
5. `actor_destroy(&timer_actor->actor)`
6. Free timer_actor

### Public API (timer_actor_set, timer_actor_cancel, timer_actor_debounce, timer_actor_debounce_flush)

No signature changes. Internally, `pd_loop_async_send` is used only to wake
the I/O thread for destroy stack drain — `actor_send` now routes through the
scheduler instead of needing the I/O thread to call `actor_run`.

## Migration

All call sites of `timer_actor_create()` must be updated to pass the pool:

- `test/test_block_cache.cpp` — pass pool from test fixture
- `test/test_off_stream_integration.cpp` — pass pool
- `test/test_readable_off_stream.cpp` — pass pool
- `test/test_writeable_off_stream.cpp` — pass pool
- `src/Node/node.c` — pass pool from offs_node
- Any other callers

All actors that register timers must add cancellation in their destroy
functions:

- `index_destroy` — cancel INDEX_SAVE debounce
- `block_cache_destroy` — cancel any timers registered on behalf of the
  block_cache
- Any other actors with registered timers

Test teardown ordering: `timer_actor_destroy` must be called before
`scheduler_pool_destroy`, but the safety net means incorrect ordering is a
bug, not a crash.

## Files Changed

| File | Change |
|------|--------|
| `src/Timer/timer_actor.h` | Add `pool` param to `timer_actor_create`, add destroy stack |
| `src/Timer/timer_actor.c` | Full rewrite: schedulable actor, two-hop completion, destroy stack, safety net |
| `src/BlockCache/index.c` | Pass pool to `actor_init`, add timer cancellation in `index_destroy` |
| `src/BlockCache/block_cache.c` | Add timer cancellation in `block_cache_destroy` if needed |
| `src/Node/node.c` | Pass pool to `timer_actor_create` |
| All test files | Pass pool to `timer_actor_create`, fix teardown order |

## Secondary Fixes (Found During Investigation)

| Issue | File | Fix |
|-------|------|-----|
| `pending_fetches` leak in `readable_off_stream_destroy` | `readable_off_stream.c:352-366` | Clean up `pending_fetches` linked list in destroy |
| `is_deactivated` set after `stream_notify(close_event)` | `writeable_off_stream.c:52-56` | Set `is_deactivated` before `stream_notify` |
| tuple leak in `ReadableOffStreamDecodesBlock` test | `test_off_stream_integration.cpp:187` | Ensure tuple is destroyed after stream processes it |
| recipe leak in `WriteableOffStreamEncodesData` test | `test_off_stream_integration.cpp:84` | Verify recipe ownership after stream takes it |