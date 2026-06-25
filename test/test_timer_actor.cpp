//
// Created on 5/6/25.
//

#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include "../src/Platform/platform_time.h"

extern "C" {
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
}

/* A custom completion type for test actors */
#define COMPLETION_FIRE 100

/* ---- helper: simple actor that records received completion messages ---- */

struct completion_state {
  std::atomic<int> fire_count;
  std::atomic<uint64_t> last_timer_id;
};

static void completion_dispatch(void* state, message_t* msg) {
  auto* cs = static_cast<completion_state*>(state);
  if (msg->type == COMPLETION_FIRE) {
    timer_completion_payload_t* payload = (timer_completion_payload_t*)msg->payload;
    cs->fire_count.fetch_add(1);
    cs->last_timer_id.store(payload->timer_id);
    /* We take ownership — free the payload ourselves */
    free(payload);
    msg->payload = NULL;
    msg->payload_destroy = NULL;
  }
}

/* ---- tests ---- */

TEST(TestTimerActor, TestCreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);
  timer_actor_destroy(timer);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

#ifdef _WIN32
#include <windows.h>

/* Isolate the per-restart OS-handle leak to the timer_actor (which owns a
   pd_loop IOCP + a loop thread). offs_node_restart destroys and re-creates the
   timer_actor every cycle, so each cycle must release every OS handle it
   allocates. The scheduler_pool is created once outside the loop (it is
   handle-balanced per TestPoolStartStopNoHandleLeak). */
static DWORD ta_handle_count() {
  DWORD count = 0;
  GetProcessHandleCount(GetCurrentProcess(), &count);
  return count;
}

TEST(TestTimerActor, TestCreateDestroyNoHandleLeak) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  /* Warm up: first cycle may cache runtime handles; discard it for a steady
     baseline. */
  for (int i = 0; i < 3; i++) {
    timer_actor_t* t = timer_actor_create(pool);
    ASSERT_NE(t, nullptr);
    timer_actor_destroy(t);
    scheduler_pool_wait_for_idle(pool);
  }

  DWORD before = ta_handle_count();
  const int N = 50;
  for (int i = 0; i < N; i++) {
    timer_actor_t* t = timer_actor_create(pool);
    ASSERT_NE(t, nullptr);
    timer_actor_destroy(t);
    scheduler_pool_wait_for_idle(pool);
  }
  DWORD after = ta_handle_count();

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);

  LONG delta = (LONG)after - (LONG)before;
  EXPECT_LE(delta, 2) << "handle leak: +" << delta << " over " << N
                      << " timer_actor cycles (before=" << before
                      << " after=" << after << ")";
}

/* Isolate the timer set/cancel handle path. A persistent timer_actor is
   reused across many timer_actor_set + timer_actor_cancel cycles (the pattern
   network_create/network_destroy uses, with recurring timers that never fire
   within the short cycle). Each cycle must release every OS handle the timer
   allocates (CreateEvent stop_event + CreateTimerQueueTimer queue_timer).
   Measures the handle count at several checkpoints to tell unbounded linear
   growth (a real leak) from sublinear growth that plateaus (e.g. Windows timer
   queue / thread-pool caching ramping up to a steady state). */
static void set_cancel_one(timer_actor_t* ta, actor_t* target,
                           scheduler_pool_t* pool) {
  ATOMIC(uint64_t) timer_id = 0;
  timer_actor_set(ta, 60000, 60000, target, COMPLETION_FIRE, &timer_id);
  /* Wait for the TIMER_SET dispatch to create the timer (fill timer_id). */
  for (int i = 0; i < 200; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (atomic_load(&timer_id) != 0) break;
    platform_sleep_ms(1);
  }
  ASSERT_NE(atomic_load(&timer_id), 0u);
  timer_actor_cancel(ta, atomic_load(&timer_id));
  scheduler_pool_wait_for_idle(pool);
}

TEST(TestTimerActor, TestSetCancelNoHandleLeak) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);
  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Warm up: let the timer queue / thread pool reach a steady state and
     discard the handles cached there for a stable baseline. */
  for (int i = 0; i < 5; i++) {
    ASSERT_NO_FATAL_FAILURE(set_cancel_one(ta, &target, pool));
  }
  scheduler_pool_wait_for_idle(pool);

  const int N = 100;
  DWORD before = ta_handle_count();
  DWORD mid = 0;
  for (int i = 0; i < N; i++) {
    ASSERT_NO_FATAL_FAILURE(set_cancel_one(ta, &target, pool));
    if (i == N / 2) mid = ta_handle_count();
  }
  DWORD after = ta_handle_count();

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&target);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);

  long first_half = (long)mid - (long)before;
  long second_half = (long)after - (long)mid;
  long total = (long)after - (long)before;
  /* A real leak grows linearly: both halves ~equal. Timer-queue / thread-pool
     caching plateaus: the second half is much smaller than the first. Tolerate
     a small bounded total (the cache steady-state) but reject linear growth. */
  EXPECT_LE(total, 10) << "timer set/cancel handle growth: +" << total
                       << " over " << N << " cycles (before=" << before
                       << " mid=" << mid << " after=" << after
                       << "; first_half=+" << first_half
                       << " second_half=+" << second_half << ")";
  if (first_half > 5 && second_half > first_half / 2) {
    ADD_FAILURE() << "timer set/cancel handle growth is linear, not plateauing:"
                     " first_half=+"
                  << first_half << " second_half=+" << second_half
                  << " (before=" << before << " mid=" << mid
                  << " after=" << after << ")";
  }
}

/* Mirror network_create/network_destroy's EXACT timer pattern: five recurring
   timers (gossip 2s, eabf 60s, metrics 300s, ping 900s, friend 5s) set in a
   burst, then cancelled as a burst, on a PERSISTENT timer_actor. The 1-timer
   test above is handle-flat; this exercises the 5-concurrent-timer pattern at
   2 scheduler workers. With the cancel path now synchronous (timer_actor_cancel
   untracks + stops + destroys the pd_timer inline under loop_lock instead of
   posting a TIMER_CANCEL actor message), every cancel releases its OS handles
   immediately regardless of scheduler delivery — so active_timer_count returns
   to 0 after each cycle and no handles leak. The earlier async-cancel path
   stranded ~13% of cancel messages under 2+ workers (the scheduler's
   work-stealing mailbox dropped them), leaving the pd_timer tracked and its
   CreateEvent + CreateTimerQueueTimer handles leaked until timer_actor_destroy.
   This test runs at pool size 2 to keep that regression guarded. */
static void set_cancel_five(timer_actor_t* ta, actor_t* target,
                            scheduler_pool_t* pool,
                            ATOMIC(uint64_t) ids[5]) {
  for (int k = 0; k < 5; k++) atomic_store(&ids[k], 0);
  timer_actor_set(ta, 2000,   2000,   target, COMPLETION_FIRE, &ids[0]);
  timer_actor_set(ta, 60000,  60000,  target, COMPLETION_FIRE, &ids[1]);
  timer_actor_set(ta, 300000, 300000, target, COMPLETION_FIRE, &ids[2]);
  timer_actor_set(ta, 900000, 900000, target, COMPLETION_FIRE, &ids[3]);
  timer_actor_set(ta, 5000,   5000,   target, COMPLETION_FIRE, &ids[4]);
  /* network_create returns here; drain the pool so every TIMER_SET has
     dispatched and filled its id before network_destroy reads + cancels them.
     timer_actor_cancel is synchronous, so the cancels below complete inline
     and the trailing wait is only for parity with the network pattern. */
  scheduler_pool_wait_for_idle(pool);
  for (int k = 0; k < 5; k++) {
    if (atomic_load(&ids[k]) != 0) {
      timer_actor_cancel(ta, atomic_load(&ids[k]));
    }
  }
  scheduler_pool_wait_for_idle(pool);
}

TEST(TestTimerActor, TestSetCancelFiveNetworkPatternNoHandleLeak) {
  /* pool size 2: the async-cancel strand only manifested under 2+ workers
     (1 worker = 0% strand). The synchronous cancel must keep the path flat
     here too. */
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);
  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  ATOMIC(uint64_t) ids[5];
  for (int k = 0; k < 5; k++) atomic_store(&ids[k], 0);

  /* Warm up: let the timer queue / thread pool reach a steady state and
     discard the handles cached there for a stable baseline. */
  for (int i = 0; i < 5; i++) {
    ASSERT_NO_FATAL_FAILURE(set_cancel_five(ta, &target, pool, ids));
  }
  scheduler_pool_wait_for_idle(pool);

  const int N = 100;
  DWORD before = ta_handle_count();
  DWORD mid = 0;
  for (int i = 0; i < N; i++) {
    ASSERT_NO_FATAL_FAILURE(set_cancel_five(ta, &target, pool, ids));
    if (i == N / 2) mid = ta_handle_count();
  }
  DWORD after = ta_handle_count();

  /* Synchronous cancel untracks every timer inline, so no pd_timer is left
     stranded in the tracked set. The async-cancel strand left this > 0. */
  EXPECT_EQ(ta->active_timer_count, (size_t)0)
      << "stranded tracked timers after " << N << " cycles";

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&target);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);

  long first_half = (long)mid - (long)before;
  long second_half = (long)after - (long)mid;
  long total = (long)after - (long)before;
  /* A real leak grows linearly: both halves ~equal. Timer-queue / thread-pool
     caching plateaus: the second half is much smaller than the first. Tolerate
     a small bounded total (the cache steady-state) but reject linear growth. */
  EXPECT_LE(total, 10) << "5-timer set/cancel handle growth: +" << total
                       << " over " << N << " cycles (before=" << before
                       << " mid=" << mid << " after=" << after
                       << "; first_half=+" << first_half
                       << " second_half=+" << second_half << ")";
  if (first_half > 5 && second_half > first_half / 2) {
    ADD_FAILURE() << "5-timer set/cancel handle growth is linear, not plateauing:"
                     " first_half=+"
                  << first_half << " second_half=+" << second_half
                  << " (before=" << before << " mid=" << mid
                  << " after=" << after << ")";
  }
}
#endif

TEST(TestTimerActor, TestOneShotTimer) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Set a one-shot timer (interval=0) with 50ms timeout */
  timer_actor_set(ta, 50, 0, &target, COMPLETION_FIRE, NULL);

  /* Wait up to 500ms for the completion to fire */
  for (int i = 0; i < 500; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (state.fire_count.load() >= 1) break;
    platform_sleep_ms(1);
  }

  EXPECT_GE(state.fire_count.load(), 1);

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestRepeatingTimer) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Set a repeating timer: 30ms initial, 30ms interval */
  timer_actor_set(ta, 30, 30, &target, COMPLETION_FIRE, NULL);

  /* Wait up to 1s for at least 3 firings */
  for (int i = 0; i < 1000; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (state.fire_count.load() >= 3) break;
    platform_sleep_ms(1);
  }

  EXPECT_GE(state.fire_count.load(), 3);

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestCancelTimer) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Set a one-shot timer with 500ms timeout */
  (void)timer_actor_set(ta, 500, 0, &target, COMPLETION_FIRE, NULL);

  /* We can't get the real timer_id synchronously (it's filled in on the timer
     thread), so we wait briefly for the dispatch to create it, then read the
     timer_id from the completion that fires. For this test, we use debounce
     with timer_id=0 which always creates a new timer, then immediately
     cancel the timer we learn about from the first firing. */

  /* Alternative approach: use a short timer, let it fire once, then cancel */
  timer_actor_set(ta, 30, 30, &target, COMPLETION_FIRE, NULL);

  /* Wait for at least one firing to get the timer_id */
  for (int i = 0; i < 200; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (state.fire_count.load() >= 1) break;
    platform_sleep_ms(1);
  }
  EXPECT_GE(state.fire_count.load(), 1);

  uint64_t tid = state.last_timer_id.load();
  EXPECT_NE(tid, 0);

  /* Cancel the timer */
  timer_actor_cancel(ta, tid);

  int count_at_cancel = state.fire_count.load();

  /* Wait 200ms -- no more firings should occur */
  platform_sleep_ms(200);
  scheduler_pool_wait_for_idle(pool);

  /* The count should not have increased significantly after cancel */
  EXPECT_LE(state.fire_count.load(), count_at_cancel + 1);

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestDebounceResetsTimer) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* First debounce: create a timer with 100ms timeout.
     timer_actor_debounce now tracks timers internally by (target, completion_type)
     so calling it again with the same target/type will cancel the old timer. */
  timer_actor_debounce(ta, 100, 0, &target, COMPLETION_FIRE);

  /* Wait for the first timer to fire and capture its timer_id */
  for (int i = 0; i < 300; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (state.fire_count.load() >= 1) break;
    platform_sleep_ms(1);
  }
  EXPECT_GE(state.fire_count.load(), 1);

  /* Now debounce again -- this cancels the old timer and creates a new one,
     demonstrating the internal debounce pattern. */
  timer_actor_debounce(ta, 200, 0, &target, COMPLETION_FIRE);

  /* Wait for the second timer to fire */
  int count_before = state.fire_count.load();
  for (int i = 0; i < 300; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (state.fire_count.load() > count_before) break;
    platform_sleep_ms(1);
  }
  EXPECT_GT(state.fire_count.load(), count_before);

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestMultipleConcurrentTimers) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Set 3 one-shot timers with different timeouts */
  timer_actor_set(ta, 30, 0, &target, COMPLETION_FIRE, NULL);
  timer_actor_set(ta, 60, 0, &target, COMPLETION_FIRE, NULL);
  timer_actor_set(ta, 90, 0, &target, COMPLETION_FIRE, NULL);

  /* Wait up to 500ms for all 3 to fire */
  for (int i = 0; i < 500; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (state.fire_count.load() >= 3) break;
    platform_sleep_ms(1);
  }

  EXPECT_GE(state.fire_count.load(), 3);

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestDestroyCleansUpTimers) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Set a long timer then immediately destroy -- should not crash */
  timer_actor_set(ta, 5000, 0, &target, COMPLETION_FIRE, NULL);
  timer_actor_set(ta, 5000, 5000, &target, COMPLETION_FIRE, NULL);

  /* Destroy the timer actor while timers are active */
  timer_actor_destroy(ta);

  scheduler_pool_wait_for_idle(pool);

  /* Clean up the target actor -- the timer payloads that were already sent
     to the target's queue may still reference freed timer memory, but
     destroying the target will drain and free them safely. */
  actor_destroy(&target);

  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(TestTimerActor, TestCancelZeroTimerId) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  /* Canceling with timer_id=0 should be a no-op, not a crash */
  timer_actor_cancel(ta, 0);

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(TestTimerActor, TestDebounceWithZeroExistingId) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Debounce should create a new timer */
  timer_actor_debounce(ta, 50, 0, &target, COMPLETION_FIRE);

  for (int i = 0; i < 300; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (state.fire_count.load() >= 1) break;
    platform_sleep_ms(1);
  }

  EXPECT_GE(state.fire_count.load(), 1);

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestOutTimerId) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* ta = timer_actor_create(pool);
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch, pool);

  /* Set a timer and retrieve its id via out_timer_id */
  ATOMIC(uint64_t) timer_id = 0;
  timer_actor_set(ta, 50, 0, &target, COMPLETION_FIRE, &timer_id);

  /* Wait for dispatch to create the timer */
  for (int i = 0; i < 100; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (atomic_load(&timer_id) != 0) break;
    platform_sleep_ms(1);
  }

  /* The timer_id should be >0 after dispatch runs */
  EXPECT_NE(atomic_load(&timer_id), 0u);

  /* Cancel the timer using the retrieved id */
  timer_actor_cancel(ta, atomic_load(&timer_id));

  timer_actor_destroy(ta);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  actor_destroy(&target);
}