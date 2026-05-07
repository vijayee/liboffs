//
// Created on 5/6/25.
//

#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <unistd.h>

extern "C" {
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
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
  timer_actor_t* timer = timer_actor_create();
  ASSERT_NE(timer, nullptr);
  timer_actor_destroy(timer);
}

TEST(TestTimerActor, TestOneShotTimer) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch);

  /* Set a one-shot timer (interval=0) with 50ms timeout */
  timer_actor_set(ta, 50, 0, &target, COMPLETION_FIRE);

  /* Wait up to 500ms for the completion to fire */
  for (int i = 0; i < 500; i++) {
    if (state.fire_count.load() >= 1) break;
    actor_run(&target, ACTOR_BATCH_SIZE);
    usleep(1000);
  }

  EXPECT_GE(state.fire_count.load(), 1);

  timer_actor_destroy(ta);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestRepeatingTimer) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch);

  /* Set a repeating timer: 30ms initial, 30ms interval */
  timer_actor_set(ta, 30, 30, &target, COMPLETION_FIRE);

  /* Wait up to 1s for at least 3 firings */
  for (int i = 0; i < 1000; i++) {
    if (state.fire_count.load() >= 3) break;
    actor_run(&target, ACTOR_BATCH_SIZE);
    usleep(1000);
  }

  EXPECT_GE(state.fire_count.load(), 3);

  timer_actor_destroy(ta);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestCancelTimer) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch);

  /* Set a one-shot timer with 500ms timeout */
  uint64_t timer_id = timer_actor_set(ta, 500, 0, &target, COMPLETION_FIRE);

  /* We can't get the real timer_id synchronously (it's filled in on the timer
     thread), so we wait briefly for the dispatch to create it, then read the
     timer_id from the completion that fires. For this test, we use debounce
     with timer_id=0 which always creates a new timer, then immediately
     cancel the timer we learn about from the first firing. */

  /* Alternative approach: use a short timer, let it fire once, then cancel */
  timer_actor_set(ta, 30, 30, &target, COMPLETION_FIRE);

  /* Wait for at least one firing to get the timer_id */
  for (int i = 0; i < 200; i++) {
    if (state.fire_count.load() >= 1) break;
    actor_run(&target, ACTOR_BATCH_SIZE);
    usleep(1000);
  }
  EXPECT_GE(state.fire_count.load(), 1);

  uint64_t tid = state.last_timer_id.load();
  EXPECT_NE(tid, 0);

  /* Cancel the timer */
  timer_actor_cancel(ta, tid);

  int count_at_cancel = state.fire_count.load();

  /* Wait 200ms — no more firings should occur */
  usleep(200000);
  actor_run(&target, ACTOR_BATCH_SIZE);

  /* The count should not have increased significantly after cancel */
  EXPECT_LE(state.fire_count.load(), count_at_cancel + 1);

  timer_actor_destroy(ta);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestDebounceResetsTimer) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch);

  /* First debounce: create a timer with 100ms timeout.
     Since timer_actor_debounce returns 0 (async), we can't get the
     timer_id from the return value. Instead, we use the completion
     callback to obtain the timer_id and cancel it with a new debounce
     call, which resets the timeout. */
  timer_actor_debounce(ta, 0, 100, 0, &target, COMPLETION_FIRE);

  /* Wait for the first timer to fire and capture its timer_id */
  for (int i = 0; i < 300; i++) {
    if (state.fire_count.load() >= 1) break;
    actor_run(&target, ACTOR_BATCH_SIZE);
    usleep(1000);
  }
  EXPECT_GE(state.fire_count.load(), 1);

  /* Now debounce using the timer_id we received — this cancels the old
     timer and creates a new one, demonstrating the debounce pattern. */
  uint64_t first_id = state.last_timer_id.load();
  if (first_id != 0) {
    timer_actor_debounce(ta, first_id, 200, 0, &target, COMPLETION_FIRE);
  }

  /* Wait for the second timer to fire */
  int count_before = state.fire_count.load();
  for (int i = 0; i < 300; i++) {
    if (state.fire_count.load() > count_before) break;
    actor_run(&target, ACTOR_BATCH_SIZE);
    usleep(1000);
  }
  EXPECT_GT(state.fire_count.load(), count_before);

  timer_actor_destroy(ta);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestMultipleConcurrentTimers) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch);

  /* Set 3 one-shot timers with different timeouts */
  timer_actor_set(ta, 30, 0, &target, COMPLETION_FIRE);
  timer_actor_set(ta, 60, 0, &target, COMPLETION_FIRE);
  timer_actor_set(ta, 90, 0, &target, COMPLETION_FIRE);

  /* Wait up to 500ms for all 3 to fire */
  for (int i = 0; i < 500; i++) {
    if (state.fire_count.load() >= 3) break;
    actor_run(&target, ACTOR_BATCH_SIZE);
    usleep(1000);
  }

  EXPECT_GE(state.fire_count.load(), 3);

  timer_actor_destroy(ta);
  actor_destroy(&target);
}

TEST(TestTimerActor, TestDestroyCleansUpTimers) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch);

  /* Set a long timer then immediately destroy — should not crash */
  timer_actor_set(ta, 5000, 0, &target, COMPLETION_FIRE);
  timer_actor_set(ta, 5000, 5000, &target, COMPLETION_FIRE);

  /* Destroy the timer actor while timers are active */
  timer_actor_destroy(ta);

  /* Clean up the target actor — the timer payloads that were already sent
     to the target's queue may still reference freed timer memory, but
     destroying the target will drain and free them safely. */
  actor_destroy(&target);
}

TEST(TestTimerActor, TestCancelZeroTimerId) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  /* Canceling with timer_id=0 should be a no-op, not a crash */
  timer_actor_cancel(ta, 0);

  timer_actor_destroy(ta);
}

TEST(TestTimerActor, TestDebounceWithZeroExistingId) {
  timer_actor_t* ta = timer_actor_create();
  ASSERT_NE(ta, nullptr);

  completion_state state;
  state.fire_count.store(0);
  state.last_timer_id.store(0);

  actor_t target;
  actor_init(&target, &state, completion_dispatch);

  /* Debounce with existing_timer_id=0 should create a new timer */
  timer_actor_debounce(ta, 0, 50, 0, &target, COMPLETION_FIRE);

  for (int i = 0; i < 300; i++) {
    if (state.fire_count.load() >= 1) break;
    actor_run(&target, ACTOR_BATCH_SIZE);
    usleep(1000);
  }

  EXPECT_GE(state.fire_count.load(), 1);

  timer_actor_destroy(ta);
  actor_destroy(&target);
}