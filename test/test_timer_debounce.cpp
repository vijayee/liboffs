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

  /* Flush -- should immediately cancel the timer and dispatch msg type 42. */
  timer_actor_debounce_flush(timer, &target.actor, 42);
  platform_sleep_ms(50);  /* let the timer thread process the flush */

  /* Drain the target actor's queue to process the dispatched message. */
  for (int i = 0; i < 100; i++) {
    if (target.msg_count >= 1) break;
    actor_run(&target.actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
  }

  EXPECT_EQ(target.msg_count, 1);
  EXPECT_EQ(target.last_msg_type, (uint32_t)42);

  /* Clean up. The test_actor is stack-allocated, no actor_destroy needed. */
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

  /* Flush a key that was never debounced -- should not crash. */
  timer_actor_debounce_flush(timer, &target.actor, 99);
  platform_sleep_ms(50);

  /* Drain the target actor's queue. */
  actor_run(&target.actor, ACTOR_BATCH_SIZE);

  EXPECT_EQ(target.msg_count, 0);

  message_queue_destroy(&target.actor.queue);
  timer_actor_destroy(timer);
}
