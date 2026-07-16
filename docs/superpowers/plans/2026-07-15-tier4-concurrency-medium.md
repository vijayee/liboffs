# Tier-4 Concurrency MEDIUM/LOW Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the remaining concurrency-pass findings F8-F11 — two MEDIUM (timer completion target dangling; backpressure mute-vs-destroy livelock), one MEDIUM-suspected (offs_node_stop phase 6 reads rings/hebbian while workers can mutate), one LOW-suspected (msquic_singleton lazy lock-init race). All are latent today but represent real races the type system doesn't prevent.

**Architecture:** Four small, independent fixes, each landing as its own commit. F8 adds a `timer_actor_cancel_target` API + a dispatch re-check. F9 is a one-line reorder in `offs_node_stop`. F10 adds a DESTROY check inside the `pressured_senders` CAS loop + a second `backpressure_release` sweep. F11 replaces a check-then-act with an `_Atomic` pointer + CAS.

**Tech Stack:** C11, `ATOMIC(...)` / `__atomic_*`, platform mutex, GoogleTest, CMake, valgrind (DWARF4 build at `cmake-build-vg/`).

**Scope (in):** `docs/concurrency-pass.md` findings F8, F9, F10, F11.

**Scope (out — deferred):** audit #8/#11 (identity/TLS), #15-17/#19/#20/#24 (CLI lies), #18 (NAT/relay), #19-29/#30-33 (MEDIUM/LOW audit findings).

**Build / test commands:**
```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='<filter>'
test -d cmake-build-vg || cmake -S . -B cmake-build-vg -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-gdwarf-4 -O0" -DCMAKE_CXX_FLAGS="-gdwarf-4 -O0"
cmake --build cmake-build-vg -j$(nproc) --target testliboffs
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='<filter>'
```

**Style reminder (from `docs/STYLE_GUIDE.md` + CLAUDE.md):** `_t` suffix, `type_action()`, no single-letter names, no TODO/FIXME, no `Co-Authored-By`, use de-wonk before marking done, check valgrind.

**Known valgrind noise (acceptable, pre-existing):** the 320-byte + 5120-byte msquic "possibly lost" TLS blocks; the 136-byte `block_handlers.c` definitely-lost (pre-existing). `definitely lost` must not grow.

---

## Task 1: F8 — Timer completion target safety

**Files:**
- Modify: `src/Timer/timer_actor.h` (declare `timer_actor_cancel_target`), `src/Timer/timer_actor.c` (implement it; re-check in the TIMER_COMPLETION dispatch)
- Test: `test/test_timer_actor.cpp` (append a test)

**Why:** `_timer_completion_callback` (`timer_actor.c:109-124`) fires on the pd-loop thread, copies the completion (with a raw `actor_t* target`), and `actor_send`s it to the timer_actor. The timer_actor's dispatch (line 128+) processes TIMER_COMPLETION and `actor_send`s to `payload->target`. If the target is freed before the completion is processed, `actor_send(payload->target, ...)` reads freed `flags` → UAF. Latent today (in-tree targets are long-lived, torn down after pool stop), but nothing prevents a short-lived target. **Fix:** add `timer_actor_cancel_target(ta, target)` that cancels all debounce entries for a target (under `loop_lock`); the dispatch re-checks that the target is still tracked before `actor_send`. The destroyer calls `cancel_target` before freeing; any in-flight completion is dropped (the target isn't in the map → no dereference).

### Step 1: Read the current code

Read `src/Timer/timer_actor.c`:
- `_timer_completion_callback` (line 109-124): the pd-loop callback that copies the completion + actor_sends to the timer_actor.
- `_timer_actor_dispatch` TIMER_COMPLETION case (find it after line 130): where the dispatch actor_sends to `payload->target`.
- `_timer_actor_find_debounce` (line 81-84): finds a debounce entry by (target, completion_type).
- The `debounce_map` (the array of `debounce_entry_t` storing `target`, `timer`, `completion_payload`).
- `timer_actor_cancel` (find it): the existing cancel-by-timer_id API. The new `cancel_target` is similar but cancels all entries for a target.
- `loop_lock` (the mutex protecting the debounce_map / pd_timer operations).

### Step 2: Write the failing test

A test that:
1. Creates a timer_actor + a target actor on a pool.
2. Sets a debounce timer for the target.
3. Calls `timer_actor_cancel_target(ta, target)`.
4. Frees the target.
5. Lets the timer_actor drain its mailbox (any in-flight completion is processed).
6. Asserts no crash / no valgrind invalid read on the freed target.

Pre-fix, the dispatch would `actor_send` to the freed target → valgrind invalid read. Post-fix, the dispatch drops the completion (target not in the map).

Read `test/test_timer_actor.cpp` for the existing fixture pattern. If a full timer_actor + target fixture is infeasible, fall back to a valgrind-only assertion on the existing `TestTimerActor.*` suite + code-review-verified correctness.

### Step 3: Implement `timer_actor_cancel_target`

In `src/Timer/timer_actor.h`, declare:

```c
/* Cancel all debounce timers for `target` and remove its entries from the
   debounce_map. Call this BEFORE freeing `target` — any in-flight completion
   already in the timer_actor's mailbox will be dropped by the dispatch (the
   target is no longer in the map, so the dispatch won't actor_send to it).
   See concurrency-pass.md F8. */
void timer_actor_cancel_target(timer_actor_t* ta, actor_t* target);
```

In `src/Timer/timer_actor.c`, implement:

```c
void timer_actor_cancel_target(timer_actor_t* ta, actor_t* target) {
  if (ta == NULL || target == NULL) return;
  platform_mutex_lock(ta->loop_lock);
  for (size_t index = 0; index < DEBOUNCE_MAP_SIZE; index++) {  /* adapt to the actual map size macro */
    debounce_entry_t* entry = &ta->debounce_map[index];
    if (entry->target == target) {
      if (entry->timer != NULL) {
        pd_timer_stop(entry->timer);
        void* old_user_data = entry->timer->user_data;
        _timer_actor_untrack(ta, entry->timer);
        pd_timer_destroy(entry->timer);
        free(old_user_data);
        entry->timer = NULL;
        entry->completion_payload = NULL;
      }
      entry->target = NULL;
      entry->completion_type = 0;
    }
  }
  platform_mutex_unlock(ta->loop_lock);
}
```

(Adapt to the actual `debounce_map` size macro and the `debounce_entry_t` field names — read the struct.)

### Step 4: Re-check in the TIMER_COMPLETION dispatch

In `src/Timer/timer_actor.c`, `_timer_actor_dispatch`, the TIMER_COMPLETION case (find it). Before `actor_send(payload->target, ...)`, re-check that `payload->target` is still tracked in the debounce_map. If not (canceled), drop the completion (don't actor_send). Use the map lookup under `loop_lock`:

```c
    case TIMER_COMPLETION: {
      timer_completion_payload_t* payload = (timer_completion_payload_t*)msg->payload;
      /* Re-check that the target is still tracked. timer_actor_cancel_target
         removes the target from the debounce_map before the target is freed;
         if the target isn't tracked, this completion is stale (the target was
         canceled/freed) — drop it without dereferencing payload->target.
         The map lookup compares pointer VALUES (not dereferencing), so a
         freed target's stale pointer value is safe to compare. See F8. */
      platform_mutex_lock(timer_actor->loop_lock);
      bool target_tracked = false;
      for (size_t index = 0; index < DEBOUNCE_MAP_SIZE; index++) {
        if (timer_actor->debounce_map[index].target == payload->target) {
          target_tracked = true;
          break;
        }
      }
      platform_mutex_unlock(timer_actor->loop_lock);
      if (target_tracked) {
        /* ... existing actor_send to payload->target ... */
      }
      /* else: drop — the target was canceled. */
      break;
    }
```

(Read the existing TIMER_COMPLETION case to see what it does after the actor_send — preserve that logic inside the `if (target_tracked)` block.)

### Step 5: Run the test to verify it passes + valgrind

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='TestTimerActor.*:TimerCancelTarget.*'
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestTimerActor.*:TimerCancelTarget.*'
```

### Step 6: Commit

```bash
git add src/Timer/timer_actor.h src/Timer/timer_actor.c test/test_timer_actor.cpp
git commit -m "fix(timer_actor): cancel_target + dispatch re-check so freed targets aren't dereferenced

Timer completions carried a raw actor_t* target; if the target was freed
before the completion was processed, the dispatch's actor_send read freed
flags -> UAF (F8). Latent today (in-tree targets are long-lived, torn down
after pool stop) but the type system didn't prevent a short-lived target.
Add timer_actor_cancel_target(ta, target) that cancels all debounce entries
for a target and removes it from the debounce_map; the dispatch re-checks
the map before actor_send and drops stale completions. The destroyer calls
cancel_target before freeing; the map lookup compares pointer values (no
dereference), so a freed target's stale pointer is safe to compare.
See concurrency-pass.md F8."
```

---

## Task 2: F9 — Move `authority_save_peers` after pool stop

**Files:**
- Modify: `src/Node/node.c` (`offs_node_stop` — move the `authority_save_peers` call from phase 6 to after phase 7)
- Test: `test/test_shutdown.cpp` (existing — verify under valgrind)

**Why:** `offs_node_stop` phase 6 (`node.c:148-155`) calls `authority_save_peers` which reads `network->rings`/`hebbian`, while workers (joined only at phase 7, line 158) can still run a late RELAY_RECEIVED dispatch that mutates rings (`network.c:3614-3620`). **Fix:** move `authority_save_peers` after `scheduler_pool_stop` so no worker can mutate rings during the read.

### Step 1: Read the current code

Read `src/Node/node.c` `offs_node_stop` (lines 138-159). Phase 5: `network_shutdown_connections`. Phase 6: `block_cache_sync` + `authority_save_peers`. Phase 7: `scheduler_pool_stop`. The `authority_save_peers` call is at line 153.

### Step 2: Write the failing test (or rely on valgrind)

The race is narrow and non-deterministic. A unit test would need a real `offs_node_t` with active relay traffic during shutdown — heavy. Rely on the existing `TestShutdown.*` suite under valgrind (pre-fix: intermittent invalid read on `rings`/`hebbian` during phase 6; post-fix: clean). Document the approach.

### Step 3: Move `authority_save_peers` after pool stop

In `src/Node/node.c`, `offs_node_stop`:

```c
  /* Phase 5: Close P2P connections. */
  if (node->network != NULL) {
    network_shutdown_connections(node->network);
  }

  /* Phase 6: Flush index/WAL. (authority_save_peers moved to after pool stop
     — it reads network->rings/hebbian, which workers could mutate via a late
     RELAY_RECEIVED dispatch until the pool is stopped. See F9.) */
  if (!_shutdown_deadline_exceeded(deadline)) {
    if (node->block_cache != NULL) {
      block_cache_sync(node->block_cache);
    }
  }

  /* Phase 7: Stop scheduler — join all worker threads. */
  scheduler_pool_stop(node->scheduler);

  /* Phase 8: Persist peer state. Now that workers are joined, no late
     RELAY_RECEIVED can mutate rings/hebbian during the read. See F9. */
  if (!_shutdown_deadline_exceeded(deadline)) {
    if (node->authority != NULL && node->network != NULL) {
      authority_save_peers(node->authority, node->network);
    }
  }
```

**Important:** `block_cache_sync` stays in phase 6 (it may need the pool running — it sends a message to the block_cache actor). Only `authority_save_peers` moves. Read `block_cache_sync` to confirm it needs the pool (if it's synchronous file I/O that doesn't need actors, it could move too — but keep it in phase 6 to be safe; the audit only flagged `authority_save_peers`).

### Step 4: Run the shutdown tests + valgrind

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='TestShutdown.*'
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestShutdown.*'
```
Run 3× to confirm the non-deterministic race is gone.

### Step 5: Commit

```bash
git add src/Node/node.c
git commit -m "fix(node): move authority_save_peers after scheduler_pool_stop

offs_node_stop phase 6 called authority_save_peers (reads network->rings/
hebbian) while workers were still live (joined only at phase 7) — a late
RELAY_RECEIVED dispatch could mutate rings during the read (F9). Move
authority_save_peers to a new phase 8 after scheduler_pool_stop, so no
worker can mutate the rings during the read. block_cache_sync stays in
phase 6 (it needs the pool for the block_cache actor). See concurrency-pass.md F9."
```

---

## Task 3: F10 — Close backpressure mute-vs-destroy window

**Files:**
- Modify: `src/Actor/actor.c` (DESTROY check inside the `pressured_senders` CAS loop; second `backpressure_release` after detach in `actor_destroy`)
- Test: `test/test_actor.cpp` (append a stress test)

**Why:** `actor_send`'s DESTROY re-check (`actor.c:116`) + CAS append (`:119-121`) vs `actor_destroy`'s single `backpressure_release` (`:60-62`): a sender that passes the line 116 check (DESTROY not set) and enters the CAS loop can append to `pressured_senders` AFTER `backpressure_release` drains it. The appended node is never drained → the sender keeps `ACTOR_FLAG_MUTED`, is endlessly re-queued but never run (`scheduler.c:125-129`) → livelock. **Fix:** add a DESTROY check INSIDE the CAS loop (abort the append if DESTROY is set mid-loop) + a second `backpressure_release` sweep after `actor_detach_pool` (catch any append that raced the first drain before the DESTROY check inside the loop was reached).

### Step 1: Read the current code

Read `src/Actor/actor.c`:
- `actor_destroy` (line 55-87): sets DESTROY (58), `backpressure_release` if PRESSURED (60-62), `actor_detach_pool` (68), the queue_state wait, `message_queue_destroy` (85).
- `actor_send` (line 120+): the DESTROY check (89), the push (98), the PRESSURED check (110), the DESTROY re-check before mute (116), the CAS append loop (119-121).
- `backpressure_release` (line 249+): `atomic_exchange(&pressured_senders, NULL)` + walk + unmute each.

### Step 2: Write the failing test

A stress test that reproduces the livelock: many senders to a PRESSURED target, destroy the target mid-send, assert no sender is stuck MUTED forever. This is non-deterministic; the test is a regression guard + valgrind/ASan for the orphaned node. Read `test/test_actor.cpp` for the existing `TestBackpressure*` fixture.

```cpp
TEST(ActorBackpressure, DestroyDuringSendNoOrphanedMute) {
  // Stress: 4 senders on a pool, target is PRESSURED, destroy the target
  // while senders are mid-send. Post-fix, no sender stays MUTED forever
  // (the DESTROY check inside the CAS loop aborts the append, and the
  // second backpressure_release drains any that raced). Assert no hang
  // (the senders complete) + valgrind no leak (no orphaned msn node).
  ... adapt to the existing TestBackpressure fixture ...
}
```

If the stress test is hard to make deterministic, rely on the existing `TestActor.*` suite + valgrind + code-review-verified correctness.

### Step 3: Add the DESTROY check inside the CAS loop

In `src/Actor/actor.c`, `actor_send`, the CAS append loop (line 119-121):

```c
        if (!(atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY)) {
          muted_sender_node_t* msn = get_clear_memory(sizeof(muted_sender_node_t));
          msn->sender = sender;
          do {
            /* Re-check DESTROY inside the loop: actor_destroy may have set
               DESTROY + drained pressured_senders after our line-116 check.
               If DESTROY is now set, abort the append (free msn) so it's
               never orphaned in a drained list -> the sender isn't muted
               forever. See concurrency-pass.md F10. */
            if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
              free(msn);
              goto skip_mute;  /* or break out of the loop without appending */
            }
            msn->next = atomic_load(&actor->pressured_senders);
          } while (!atomic_compare_exchange_strong(&actor->pressured_senders, &msn->next, msn));
          atomic_fetch_or(&sender->flags, ACTOR_FLAG_MUTED);
        }
```

(Use a clean control flow — `goto skip_mute` after the loop, or restructure. The key: if DESTROY is set inside the loop, free `msn` and don't append / don't set MUTED. Read the surrounding code to fit the control flow.)

### Step 4: Add a second `backpressure_release` after detach in `actor_destroy`

In `src/Actor/actor.c`, `actor_destroy`, after `actor_detach_pool(actor)` (line 68) and before the queue_state wait:

```c
  actor_detach_pool(actor);
  /* Second backpressure_release sweep: a sender that passed the line-116
     DESTROY check and completed the CAS append AFTER the first
     backpressure_release (line 60-62) but BEFORE the DESTROY check inside
     the CAS loop (Task 3 Step 3) would have appended to a drained list.
     Drain again to catch that residual. With the DESTROY check inside the
     CAS loop, no NEW appends can happen (DESTROY is set since line 58), so
     this second drain is the final one. See concurrency-pass.md F10. */
  if (atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED) {
    backpressure_release(actor);
  }
```

### Step 5: Run the tests + valgrind

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='TestActor.*:TestBackpressure.*:TestMessageQueue.*:TestScheduler.*'
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestActor.*:TestBackpressure.*:TestScheduler.*'
```
Run the backpressure stress test 10× to confirm no hang / no orphaned node.

### Step 6: Commit

```bash
git add src/Actor/actor.c test/test_actor.cpp
git commit -m "fix(actor): close backpressure mute-vs-destroy window

A sender that passed the DESTROY re-check (actor.c:116) and entered the
pressured_senders CAS loop could append AFTER actor_destroy's
backpressure_release drained the list -> the orphaned node left the sender
MUTED forever, endlessly re-queued but never run -> livelock (F10). Add a
DESTROY check INSIDE the CAS loop (abort the append + free the node if
DESTROY is set mid-loop) and a second backpressure_release after
actor_detach_pool (catch any append that raced the first drain before the
in-loop check was reached). With both, no new appends can happen after
DESTROY is set, so the second drain is final. See concurrency-pass.md F10."
```

---

## Task 4: F11 — msquic_singleton atomic lock init

**Files:**
- Modify: `src/Network/msquic_singleton.c` (replace the check-then-act lazy lock init with an `_Atomic` pointer + CAS)
- Test: `test/test_network.cpp` or valgrind-only

**Why:** `_ensure_msquic_lock_initialized` (`msquic_singleton.c:18-22`) is check-then-act on `g_msquic_lock`: two first-callers can both see NULL, both create a mutex, and both enter the critical section → double `MsQuicOpen2`/refcount corruption. First open is single-threaded in practice. **Fix:** `_Atomic` pointer + CAS (as `pool.c:42-49` does), so only one caller creates the mutex.

### Step 1: Read the current code

Read `src/Network/msquic_singleton.c` (lines 15-40). `g_msquic_lock` is a `static platform_mutex_t*`. `_ensure_msquic_lock_initialized` does `if (g_msquic_lock == NULL) g_msquic_lock = platform_mutex_create();`. `offs_msquic_open` calls it then locks. Read `src/Util/atomic_compat.h` for the `ATOMIC(...)` macro and `src/Scheduler/pool.c:42-49` for the CAS-on-lazy-init pattern.

### Step 2: Write the failing test (or rely on code-review)

The race requires two threads calling `offs_msquic_open` simultaneously as the first callers. Hard to test deterministically. Rely on code-review-verified correctness + the existing `TestShutdown.*`/`QuicIntegration.*` suite under valgrind (the fix is a standard `_Atomic`-pointer + CAS pattern). Document the approach.

### Step 3: Replace the lazy init with `_Atomic` pointer + CAS

In `src/Network/msquic_singleton.c`:

```c
static uint32_t g_msquic_refcount = 0;
static ATOMIC(platform_mutex_t*) g_msquic_lock = NULL;  /* _Atomic pointer; CAS-init */

static void _ensure_msquic_lock_initialized(void) {
  platform_mutex_t* existing = atomic_load_explicit(&g_msquic_lock, memory_order_acquire);
  if (existing != NULL) return;
  platform_mutex_t* created = platform_mutex_create();
  if (created == NULL) return;  /* OOM — offs_msquic_open will fail on the lock */
  /* CAS: only one caller wins the NULL -> created swap; losers free their
     created mutex and use the winner's. See concurrency-pass.md F11. */
  platform_mutex_t* expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(&g_msquic_lock, &expected, created,
                                               memory_order_acq_rel, memory_order_acquire)) {
    /* Another thread won the race; use theirs, free ours. */
    platform_mutex_destroy(created);
  }
}
```

(Use `atomic_load_explicit` / `atomic_compare_exchange_strong_explicit` with the `ATOMIC(...)` macro, matching `pool.c:42-49`. Read `pool.c:42-49` for the exact pattern and adapt.)

**Note:** `ATOMIC(platform_mutex_t*)` expands to `std::atomic<platform_mutex_t*>` in C++ / `_Atomic(platform_mutex_t*)` in C. The `g_msquic_lock` is a file-static in a .c file (not a header), so C-only — `_Atomic(platform_mutex_t*)` is fine. But `msquic_singleton.c` might be included from C++ tests indirectly — confirm it's a .c file (it is). Use the `ATOMIC(...)` macro for consistency.

### Step 4: Run the tests + valgrind

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='QuicIntegration.*:TestShutdown.*:TestNetwork.*'
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='QuicIntegration.*:TestShutdown.*'
```

### Step 5: Commit

```bash
git add src/Network/msquic_singleton.c
git commit -m "fix(msquic_singleton): atomic CAS-init of the lazy lock

_ensure_msquic_lock_initialized was check-then-act on g_msquic_lock: two
first-callers could both see NULL, both create a mutex, and both enter the
critical section -> double MsQuicOpen2 / refcount corruption (F11). First
open is single-threaded in practice. Make g_msquic_lock an _Atomic pointer
and init via CAS (NULL -> created) — only one caller wins the swap; losers
free their mutex and use the winner's. Pattern matches pool.c:42-49.
See concurrency-pass.md F11."
```

---

## Task 5: Whole-tier verification

**Files:** none (verification only)

- [ ] Step 1: Build. `cmake --build cmake-build-verify -j$(nproc) --target testliboffs` — expect clean, no warnings on tier-4 files.
- [ ] Step 2: Full test suite. `cmake-build-verify/test/testliboffs` — expect all pass (modulo pre-existing SSL cert failures). No NEW failures.
- [ ] Step 3: Valgrind sweep. `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestTimerActor.*:TimerCancelTarget.*:TestShutdown.*:TestActor.*:TestBackpressure.*:TestScheduler.*:TestMessageQueue.*:QuicIntegration.*:TestNetwork.*:TestBlockCache.*'` — expect `definitely lost: 0 bytes`, 0 invalid reads. Run 2×.
- [ ] Step 4: De-wonk audit on tier-4 files.
- [ ] Step 5: TODO check. `git log master..HEAD --pickaxe-regex -S'TODO\|FIXME\|HACK\|XXX' -- <tier-4 files>` — expect empty.
- [ ] Step 6: Leak check (covered by step 3).
- [ ] Step 7: Final commit if needed.

---

## Self-Review

**1. Spec coverage.** Tier-4 scope = F8, F9, F10, F11.
- F8 → Task 1 ✓
- F9 → Task 2 ✓
- F10 → Task 3 ✓
- F11 → Task 4 ✓
- Verification → Task 5 ✓

**2. Placeholder scan.** Every code step shows the actual code or the exact change. Where a test is infeasible (F11's two-first-caller race, F9's narrow phase-6 race), the step allows a valgrind + code-review fallback.

**3. Interaction check.**
- F8's `timer_actor_cancel_target` + the dispatch re-check: the map lookup compares pointer values (no dereference), so a freed target's stale pointer is safe. The cancel and the dispatch both touch the map under `loop_lock` — serialized.
- F9's reorder: `authority_save_peers` moves after pool stop; `block_cache_sync` stays in phase 6. No interaction with the other tasks.
- F10's in-loop DESTROY check + second drain: the DESTROY check inside the CAS loop prevents new appends after DESTROY is set; the second drain catches any append that raced the first drain. Together they close the window.
- F11's `_Atomic` pointer + CAS: standard lazy-init pattern, matches `pool.c`. No interaction with the other tasks.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-15-tier4-concurrency-medium.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, two-stage review between tasks. The 4 tasks are independent and small.
2. **Inline Execution** — execute in this session with checkpoints.

Which approach?