//
// Created on 5/22/26.
//

#include <gtest/gtest.h>
extern "C" {
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/message.h"
#include "../src/Actor/message_queue.h"
#include "../src/Util/allocator.h"
#include "../src/Platform/platform.h"
#include "../src/Scheduler/scheduler.h"
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
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);

  test_actor_t target;
  actor_init(&target.actor, &target, test_actor_dispatch, pool);
  target.last_msg_type = 0;
  target.msg_count = 0;

  /* Set up a debounce with a long timeout so it won't fire on its own. */
  timer_actor_debounce(timer, 60000, 0, &target.actor, 42);
  platform_sleep_ms(50);  /* let the timer thread process */

  /* Flush -- should immediately cancel the timer and dispatch msg type 42. */
  timer_actor_debounce_flush(timer, &target.actor, 42);
  platform_sleep_ms(50);  /* let the timer thread process the flush */

  /* Wait for the dispatched message via the scheduler pool. */
  for (int i = 0; i < 100; i++) {
    scheduler_pool_wait_for_idle(pool);
    if (target.msg_count >= 1) break;
    platform_sleep_ms(1);
  }

  EXPECT_EQ(target.msg_count, 1);
  EXPECT_EQ(target.last_msg_type, (uint32_t)42);

  /* Clean up. */
  timer_actor_destroy(timer);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  message_queue_destroy(&target.actor.queue);
}

TEST(TestTimerDebounceFlush, FlushNoExistingDebounceIsNoop) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);

  test_actor_t target;
  actor_init(&target.actor, &target, test_actor_dispatch, pool);
  target.last_msg_type = 0;
  target.msg_count = 0;

  /* Flush a key that was never debounced -- should not crash. */
  timer_actor_debounce_flush(timer, &target.actor, 99);
  platform_sleep_ms(50);

  /* Wait for any pending work. */
  scheduler_pool_wait_for_idle(pool);

  EXPECT_EQ(target.msg_count, 0);

  timer_actor_destroy(timer);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  message_queue_destroy(&target.actor.queue);
}
