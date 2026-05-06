# Actor-Based Concurrency Architecture for liboffs

## Context

liboffs is a content-addressed, XOR-split, P2P-backed filesystem library being ported from Pony to C. The current C implementation uses mutex-protected shared state and a global-mutex thread pool, which creates lock contention, race conditions, and prevents efficient parallel I/O. The Pony version uses the actor model with Pony's work-stealing scheduler, which eliminates locks entirely and enables concurrent I/O through independent Section actors. This design brings the Pony actor model to the C implementation, using poll-dancer as the cross-platform event loop for timers and future network I/O.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    WORKER POOL                                │
│   Per-worker Chase-Lev deques with work stealing             │
├──────────┬──────────┬──────────┬──────────────────────────────┤
│ Worker 0 │ Worker 1 │ Worker 2 │ Worker N                      │
├──────────┴──────────┴──────────┴──────────────────────────────┤
│                                                               │
│  ACTOR LAYER (scheduled on workers, batch-processed)          │
│                                                               │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐      │
│  │  BlockCache  │   │  Section[0]  │   │  Section[1]  │      │
│  │   Actor      │──▶│    Actor     │   │    Actor     │ ...  │
│  │              │   │  (file I/O)  │   │  (file I/O)  │      │
│  │ - Index      │   └──────────────┘   └──────────────┘      │
│  │ - LRU Cache  │                                           │
│  │ - Checkout   │   ┌──────────────┐   ┌──────────────┐      │
│  └──────┬───────┘   │   Sections   │   │  TupleCache  │      │
│         │           │    Actor     │   │    Actor     │      │
│         │           │ (round-robin)│   │  (XOR LRU)   │      │
│         │           └──────────────┘   └──────────────┘      │
│         │                                                    │
│  ┌──────┴───────┐   ┌──────────────┐                        │
│  │   Timer      │   │  Stream      │                        │
│  │   Actor      │   │   Actors     │  ...                   │
│  │ (poll-dancer │   │ (per-stream  │                        │
│  │  event loop) │   │  message q)  │                        │
│  └──────────────┘   └──────────────┘                        │
│                                                              │
│  FUTURE: NetworkRecipe Actor (P2P block requests via        │
│          poll-dancer socket readiness)                      │
└─────────────────────────────────────────────────────────────┘
```

## Component Design

### 1. Actor Runtime (`src/Actor/`)

**`actor.h`** — Core actor type:
```c
typedef struct actor_t {
    message_queue_t queue;      // lock-free Michael-Scott queue
    _Atomic uint8_t flags;      // SCHEDULED bit, OVERLOADED bit
    void* state;               // module-specific state
    void (*dispatch)(void* state, message_t* msg);
} actor_t;
```

**`actor_send(actor_t* actor, message_t* msg) -> bool`:**
- Pushes message onto actor's lock-free queue
- Returns `true` if queue was empty (actor needs scheduling)
- If true, push actor to current worker's local deque (or inject queue from non-worker thread)

**`actor_run(actor_t* actor, size_t batch_size) -> bool`:**
- Pops up to `batch_size` messages, dispatches each
- Returns `true` if more messages remain (reschedule actor)

**`message.h`** — Lightweight pool-allocated messages:
```c
typedef struct message_t {
    uint32_t type;
    void* payload;
    void (*payload_destroy)(void*);
} message_t;
```

No refcounter on messages — single-use, freed by pool allocator after dispatch.

### 2. Lock-Free Message Queue (`src/Actor/message_queue.h`)

Michael-Scott queue (Pony pattern):
- `_Atomic(message_node_t*) head` with LSB as "empty" flag
- `message_node_t* tail` — single-consumer (no atomic)
- Push: atomic exchange on head. Returns whether queue was empty (scheduling decision).
- Pop: non-atomic tail read, advance tail.
- Single-producer fast path: non-atomic load+store when applicable.

### 3. Work-Stealing Scheduler (`src/Scheduler/`)

**`scheduler_t`** — per-worker state (contiguous array for cache locality):
```c
typedef struct scheduler_t {
    size_t index;
    PLATFORMTHREADTYPE thread;
    deque_t local_queue;          // Chase-Lev work-stealing deque
    _Atomic uint32_t last_victim; // round-robin steal pointer
    char _pad[CACHE_LINE_SIZE];   // prevent false sharing
    actor_t* current;             // currently executing actor
} scheduler_t;
```

**`scheduler_pool_t`:**
```c
typedef struct scheduler_pool_t {
    scheduler_t* workers;
    size_t worker_count;
    mpmcq_t inject;              // global inject queue for non-worker threads
    _Atomic uint32_t active_count;
    _Atomic uint8_t terminate;
} scheduler_pool_t;
```

**Chase-Lev deque:**
- Bottom: push/pop by owning worker (fast, mostly non-atomic)
- Top: steal by other workers (CAS-based)
- Push: amortized O(1), Pop: O(1), Steal: O(1) CAS

**Scheduling flow:**
1. Worker pops from local deque bottom (fast path)
2. If empty, try global `inject` queue
3. If empty, steal from another worker's deque top (round-robin via `last_victim`)
4. If nothing after threshold, suspend thread
5. `actor_send` from within actor dispatch → push to current worker's local deque
6. `actor_send` from non-worker thread → push to `inject` queue

**Thread-local context:**
```c
static __thread scheduler_t* current_scheduler;
```

### 4. Pool Allocator (`src/Actor/pool.h`)

Three-tier allocator (Pony pattern):
- Thread-local free lists: per-size-class, lock-free for same-thread alloc/free
- Global free lists: ABA-protected CAS for cross-thread transfers
- mmap fallback for large allocations

Size classes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes (8 classes, doubling).

Used for: message_t, message_node_t, actor scheduling, and any other hot-path allocations.

### 5. poll-dancer Extensions

**Timer support** — new API:
```c
pd_timer_t* pd_timer_create(pd_loop_t* loop, uint64_t timeout_ms,
                             uint64_t interval_ms, pd_callback_t cb, void* data);
pd_error_t pd_timer_start(pd_timer_t* timer);
pd_error_t pd_timer_stop(pd_timer_t* timer);
pd_error_t pd_timer_destroy(pd_timer_t* timer);
```

Platform implementations:
- Linux: `timerfd_create`/`timerfd_settime`, watched via epoll for `EPOLLIN`
- macOS/BSD: `EVFILT_TIMER` kevent filter
- Windows: `CreateTimerQueueTimer`

**Fix `async_send`** — cross-thread wakeup:
- Linux: Create `eventfd` at loop creation, add to epoll set. `async_send` writes to eventfd.
- macOS/BSD: Register `EVFILT_USER` kevent filter. `async_send` triggers via `kevent` with `EV_TRIGGER`.
- Windows: Already works via `PostQueuedCompletionStatus`.

### 6. Timer Actor (`src/Timer/`)

Runs on a dedicated thread calling `pd_loop_run()`. Manages all timers for the system.

Messages:
- `TIMER_SET` — create and start a timer (returns timer_id)
- `TIMER_CANCEL` — stop and destroy a timer
- `TIMER_DEBOUNCE` — cancel existing timer, create new one (for debounced persistence)

When a timer fires, the poll-dancer callback sends a completion message back to the requesting actor via `actor_send`.

### 7. Section Actor (`src/BlockCache/section.h`, `section.c`)

**Bitmap replaces fragment list:**
```c
typedef struct section_t {
    refcounter_t refcounter;
    actor_t actor;
    int fd;
    size_t id;
    char* meta_path;
    char* path;
    uint32_t free_map;          // bitmap: bit set = slot free
    uint32_t* free_map_large;   // heap bitmap for size > 32
    size_t size;
    block_size_e block_size;
    uint8_t dirty;              // needs metadata save
} section_t;
```

Operations:
- Allocate: `ffs(free_map) - 1`, clear bit — O(1)
- Deallocate: `free_map |= (1 << index)` — O(1)
- Full check: `free_map == 0` — O(1)

**Debounced metadata save:** After any write/deallocation, set `dirty = 1` and send `TIMER_DEBOUNCE` to timer actor. Save fires when debounce completes, not on every operation.

**Section actor messages:**
- `SECTION_WRITE` — write block, return index
- `SECTION_READ` — read block at index
- `SECTION_DEALLOCATE` — free slot
- `SECTION_SAVE_META` — debounced metadata save
- `SECTION_CLOSE` — flush and close

**Bug fixes (done as part of this refactoring):**
- `int32_t` → `off_t`/`ssize_t` for file offsets
- `section_read`: `O_RDWR | O_CREAT` → `O_RDONLY`
- Check `write()` return value in `section_save_fragments`
- VLA `uint8_t buffer[size]` → heap allocation with size check

### 8. Sections Actor (`src/BlockCache/sections.h`, `sections.c`)

Round-robin allocator + section LRU cache, no locks.

Messages:
- `SECTIONS_WRITE` — pick section, forward `SECTION_WRITE`
- `SECTIONS_READ` — find section, forward `SECTION_READ`
- `SECTIONS_DEALLOCATE` — forward to section
- `SECTIONS_SECTION_FULL` — remove from round-robin, create new section actor
- `SECTIONS_WRITE_COMPLETE` — callback from Section, forward to BlockCache

### 9. BlockCache Actor (`src/BlockCache/block_cache.h`, `block_cache.c`)

Serializes all index/LRU operations. No mutexes.

Messages:
- `CACHE_GET` — LRU check → index lookup → delegate to Sections
- `CACHE_PUT` — index check → delegate to Sections
- `CACHE_REMOVE` — remove from index, LRU, deallocate
- `CACHE_BLOCK_LOADED` — callback from Sections: insert into LRU, resolve promise

Internal state (no locks):
- `index_t*` — trie (only BlockCache touches it)
- `block_lru_cache_t*` — hot block cache
- `actor_t* sections_actor` — Sections actor reference
- Checkout map — tracks in-flight entries

### 10. Stream Actor Refactoring (`src/Streams/`)

Each stream becomes an actor:
- Replace `message_queue_t` with lock-free queue from actor runtime
- Replace `_stream_message_worker` one-per-turn with batch processing
- Remove `is_working` flag + `worker_status.lock` (actor scheduling handles this)
- Remove `stream->pool` (scheduling through actor system)
- Remove `stream->lock` (actor serializes access)
- Add max handler count (32) to prevent VLA stack overflow in `stream_notify`

Stream dispatch function:
```c
void stream_dispatch(void* state, message_t* msg) {
    stream_t* stream = (stream_t*)state;
    switch (msg->type) {
        case READABLE_PUSH:   stream->on_push(stream); break;
        case WRITEABLE_WRITE: stream->on_write(stream, msg->payload); break;
        case CLOSE_STREAM:    stream->on_close(stream); break;
        case READABLE_PULL:   stream->on_pull(stream); break;
        case DEFERRED_DEREF:  stream->destructor(stream); break;
    }
}
```

### 11. Refcounter Fixes (`src/RefCounter/`)

1. **`error_destroy` use-after-free** (error.c:29): `refcounter_reference` after `free(error)` → replace with `refcounter_destroy_lock`
2. **Atomic `refcounter_dereference` drops pending derefs** (refcounter.c:66-71): Add `pending_deref` tracking in atomic mode, matching non-atomic behavior
3. **`debouncer_t` naming collision** (debouncer.h:21): `refcounter_t refcounter_t` → `refcounter_t refcounter`
4. **`writeable_stream_write` race** (streams.c:704-717): Move `_stream_start_message_worker` call to avoid race
5. **Actor-internal refcounts become non-atomic**: Once index_entry_t, index_t, etc. are only touched by one actor, switch to plain uint16_t

### 11b. Async Completion: Actor Messages, Not Promises

The current `promise_t` mechanism (resolve/reject callbacks) is replaced by **actor messages** for all async completion within the actor system. When BlockCache completes a `CACHE_GET`, it sends a `CACHE_BLOCK_LOADED` message back to the requesting actor. No promises, no cross-thread callbacks — just messages.

The key pattern is **"collect N async results"**, used heavily by ReadableOffStream:
- A tuple of N block hashes needs to be fetched from BlockCache
- The actor sends N `CACHE_GET` messages and tracks `pending_blocks` in its own state
- Each `CACHE_BLOCK_LOADED` message decrements the counter
- When `pending_blocks == 0`, the actor proceeds (XOR reconstruction, emit data)

```c
// OffStream actor state
uint8_t pending_blocks;
block_t** collected_blocks;

// Handler for CACHE_BLOCK_LOADED messages
void off_stream_on_block_loaded(void* state, message_t* msg) {
    off_stream_t* stream = state;
    block_t* block = msg->payload;
    stream->collected_blocks[stream->current_index++] = block;
    stream->pending_blocks--;
    if (stream->pending_blocks == 0) {
        render_origin_data(stream);  // XOR and emit
    }
}
```

This matches the Pony pattern exactly — each `_bc.get()` callback sends a message back to the actor, and the actor collects them. No promises needed within the actor system.

**External API boundary:** For callers outside the actor system (e.g., the main thread), a simple callback in the initial message payload is sufficient. The actor sends a completion message to a designated handler. This replaces `promise_t` for most use cases.

### 12. Deleted Code

After all refactoring, these files/modules are removed:
- `src/Workers/work.h`, `work.c` — replaced by actor messages
- `src/Workers/queue.h`, `queue.c` — replaced by Chase-Lev deque + lock-free queue
- `src/Workers/priority.h`, `priority.c` — no longer needed (actor scheduling replaces priority queue)
- `src/Workers/pool.h`, `pool.c` — replaced by work-stealing scheduler
- `src/Time/` hierarchy (wheel.h, wheel.c, debouncer.h, debouncer.c) — replaced by timer actor + poll-dancer

## Implementation Phases

### Phase 1: Foundation (actor runtime + pool allocator + refcounter fixes)
- Fix refcounter bugs
- Build `src/Actor/` with actor_t, message_queue_t, message_t, pool allocator
- Unit tests for lock-free queue and pool allocator

### Phase 2: Scheduler (work-stealing pool)
- Build `src/Scheduler/` with Chase-Lev deque, worker threads, steal logic
- Integration with actor runtime
- Unit tests for scheduling, stealing, batch processing

### Phase 3: poll-dancer Extensions
- Add timer support (timerfd/EVFILT_TIMER/CreateTimerQueueTimer)
- Fix async_send (eventfd/EVFILT_USER)
- Unit tests per platform

### Phase 4: Timer Actor
- Build `src/Timer/` timer actor using poll-dancer
- Replace current timing wheel + debouncer
- Integration tests

### Phase 5: Section Refactoring (bitmap + actor + bug fixes)
- Replace fragment list with bitmap
- Fix Section bugs (int32_t, O_CREAT on read, write() check, VLA)
- Convert Section to actor
- Debounced metadata save
- Integration tests

### Phase 6: Sections and BlockCache as Actors
- Convert Sections to actor
- Convert BlockCache to actor
- Remove all BlockCache mutexes
- Make actor-internal refcounts non-atomic
- Integration tests

### Phase 7: Stream Refactoring
- Convert streams to use actor runtime
- Batch processing, remove is_working/worker_status lock
- Remove stream->pool, stream->lock
- Fix stream_notify VLA
- Integration tests

### Phase 8: Cleanup
- Delete work.h/c, queue.h/c, priority.h/c, pool.h/c
- Delete timing wheel and debouncer files
- Update CMakeLists.txt
- Full integration test suite

## Verification

After each phase:
1. Run existing Google Test suite
2. Run valgrind for memory leaks (per CLAUDE.md requirement)
3. Test file stream round-trip (read PDF → pipe → write → compare)
4. Test block cache put/get/remove cycle
5. Test concurrent access (multiple threads sending to same actor)
6. Test work stealing (all workers busy, overflow to stealing)
7. Test timer accuracy and debounce behavior
8. Test Section bitmap allocation/deallocation correctness
9. Test metadata save after dirty flag + debounce

End-to-end verification after Phase 8:
1. Full test suite passes
2. No valgrind errors
3. Concurrent file stream test (4+ threads)
4. Block cache stress test (1000+ concurrent operations)
5. Timer accuracy within 10ms of target