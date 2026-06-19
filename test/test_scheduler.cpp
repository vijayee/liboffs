//
// Created on 5/6/25.
//

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include "../src/Platform/platform_time.h"

extern "C" {
#include "../src/Scheduler/scheduler.h"
#include "../src/Actor/actor.h"
}

static void test_dispatch(void* state, message_t* msg) {
  auto* counter = static_cast<std::atomic<int>*>(state);
  counter->fetch_add(1);
  (void)msg;
}

static void send_message(actor_t* actor, uint32_t type) {
  message_t msg;
  msg.type = type;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(actor, &msg);
}

TEST(TestScheduler, TestPoolCreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_destroy(pool);
}

TEST(TestScheduler, TestPoolStartStop) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);

  scheduler_pool_start(pool);
  platform_sleep_ms(50); // 50ms — let workers settle
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);

  scheduler_pool_destroy(pool);
}

TEST(TestScheduler, TestActorScheduling) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);

  std::atomic<int> counter{0};
  actor_t actor;
  actor_init(&actor, &counter, test_dispatch, NULL);

  send_message(&actor, 1);

  scheduler_pool_start(pool);
  scheduler_inject(pool, &actor);

  // Poll until the actor is dispatched or timeout
  for (int i = 0; i < 200; i++) {
    if (counter.load() >= 1) break;
    platform_sleep_ms(1);
  }
  EXPECT_GE(counter.load(), 1);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  actor_destroy(&actor);
  scheduler_pool_destroy(pool);
}

TEST(TestScheduler, TestActorBatchProcessing) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);

  std::atomic<int> counter{0};
  actor_t actor;
  actor_init(&actor, &counter, test_dispatch, NULL);

  // Send multiple messages
  for (int i = 0; i < 5; i++) {
    send_message(&actor, (uint32_t)i);
  }

  scheduler_pool_start(pool);
  scheduler_inject(pool, &actor);

  // Wait for all messages to be dispatched
  for (int i = 0; i < 200; i++) {
    if (counter.load() >= 5) break;
    platform_sleep_ms(1);
  }
  EXPECT_GE(counter.load(), 5);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  actor_destroy(&actor);
  scheduler_pool_destroy(pool);
}

TEST(TestScheduler, TestActorRescheduling) {
  // Verify that actor_run processes all messages across multiple batches
  std::atomic<int> counter{0};
  actor_t actor;
  actor_init(&actor, &counter, test_dispatch, NULL);

  // Send more messages than ACTOR_BATCH_SIZE
  int total_messages = ACTOR_BATCH_SIZE + 10;
  for (int i = 0; i < total_messages; i++) {
    send_message(&actor, (uint32_t)i);
  }

  // Run first batch
  bool has_more = actor_run(&actor, ACTOR_BATCH_SIZE);
  EXPECT_TRUE(has_more);
  EXPECT_EQ(counter.load(), ACTOR_BATCH_SIZE);

  // Run second batch to drain remaining messages
  has_more = actor_run(&actor, ACTOR_BATCH_SIZE);
  EXPECT_FALSE(has_more);
  EXPECT_EQ(counter.load(), total_messages);

  actor_destroy(&actor);
}

TEST(TestScheduler, TestActorReschedulingViaPool) {
  // Test rescheduling through the scheduler pool
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);

  std::atomic<int> counter{0};
  actor_t actor;
  actor_init(&actor, &counter, test_dispatch, NULL);

  // Send more messages than ACTOR_BATCH_SIZE to force rescheduling
  int total_messages = ACTOR_BATCH_SIZE + 10;
  for (int i = 0; i < total_messages; i++) {
    send_message(&actor, (uint32_t)i);
  }

  scheduler_pool_start(pool);
  scheduler_inject(pool, &actor);

  // Wait for all messages to be dispatched
  for (int i = 0; i < 5000; i++) {
    if (counter.load() >= total_messages) break;
    platform_sleep_ms(1);
  }

  EXPECT_GE(counter.load(), total_messages) << "Expected at least " << total_messages
                                            << " dispatches, got " << counter.load();

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  actor_destroy(&actor);
  scheduler_pool_destroy(pool);
}

TEST(TestScheduler, TestMultipleActors) {
  scheduler_pool_t* pool = scheduler_pool_create(4);
  ASSERT_NE(pool, nullptr);

  const int NUM_ACTORS = 4;
  std::atomic<int> counters[NUM_ACTORS];
  actor_t actors[NUM_ACTORS];

  for (int i = 0; i < NUM_ACTORS; i++) {
    counters[i].store(0);
    actor_init(&actors[i], &counters[i], test_dispatch, NULL);
  }

  scheduler_pool_start(pool);

  // Inject all actors
  for (int i = 0; i < NUM_ACTORS; i++) {
    send_message(&actors[i], (uint32_t)i);
    scheduler_inject(pool, &actors[i]);
  }

  // Wait for all actors to be dispatched
  for (int i = 0; i < 200; i++) {
    bool all_done = true;
    for (int j = 0; j < NUM_ACTORS; j++) {
      if (counters[j].load() < 1) {
        all_done = false;
        break;
      }
    }
    if (all_done) break;
    platform_sleep_ms(1);
  }

  for (int i = 0; i < NUM_ACTORS; i++) {
    EXPECT_GE(counters[i].load(), 1);
  }

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  for (int i = 0; i < NUM_ACTORS; i++) {
    actor_destroy(&actors[i]);
  }
  scheduler_pool_destroy(pool);
}

TEST(TestScheduler, TestWorkStealing) {
  // Create a pool with multiple workers so work stealing can occur
  scheduler_pool_t* pool = scheduler_pool_create(4);
  ASSERT_NE(pool, nullptr);

  std::atomic<int> counter{0};
  actor_t actor;
  actor_init(&actor, &counter, test_dispatch, NULL);

  // Send many messages to create work for stealing
  int total_messages = ACTOR_BATCH_SIZE * 3;
  for (int i = 0; i < total_messages; i++) {
    send_message(&actor, (uint32_t)i);
  }

  scheduler_pool_start(pool);
  scheduler_inject(pool, &actor);

  // Wait for all messages to be processed
  for (int i = 0; i < 3000; i++) {
    if (counter.load() >= total_messages) break;
    platform_sleep_ms(1);
  }
  EXPECT_GE(counter.load(), total_messages);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  actor_destroy(&actor);
  scheduler_pool_destroy(pool);
}

TEST(TestScheduler, TestInjectFromExternalThread) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);

  std::atomic<int> counter{0};
  actor_t actor;
  actor_init(&actor, &counter, test_dispatch, NULL);

  send_message(&actor, 1);

  scheduler_pool_start(pool);

  // Inject the actor from a non-worker (external) thread
  std::thread external([&pool, &actor]() {
    scheduler_inject(pool, &actor);
  });
  external.join();

  // Wait for the actor to be dispatched
  for (int i = 0; i < 200; i++) {
    if (counter.load() >= 1) break;
    platform_sleep_ms(1);
  }
  EXPECT_GE(counter.load(), 1);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  actor_destroy(&actor);
  scheduler_pool_destroy(pool);
}