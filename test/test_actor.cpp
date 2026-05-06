//
// Created by victor on 5/6/25.
//

#include <gtest/gtest.h>
#include <vector>
extern "C" {
#include "../src/Actor/actor.h"
#include "../src/Actor/message_queue.h"
}

static void test_dispatch(void* state, message_t* msg) {
  auto* count = static_cast<int*>(state);
  (*count)++;
  (void)msg;
}

static void record_dispatch(void* state, message_t* msg) {
  auto* vec = static_cast<std::vector<uint32_t>*>(state);
  vec->push_back(msg->type);
}

static int payload_destroy_count;
static void test_payload_destroy(void* payload) {
  payload_destroy_count++;
  free(payload);
}

TEST(TestActor, TestInitDestroy) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch);
  EXPECT_TRUE(message_queue_isempty(&actor.queue));
  actor_destroy(&actor);
}

TEST(TestActor, TestSendReturnsEmpty) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch);

  message_t msg;
  msg.type = 1;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  bool was_empty = actor_send(&actor, &msg);
  EXPECT_TRUE(was_empty);

  actor_destroy(&actor);
}

TEST(TestActor, TestSendReturnsNotEmpty) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch);

  message_t msg1;
  msg1.type = 1;
  msg1.payload = NULL;
  msg1.payload_destroy = NULL;

  bool was_empty = actor_send(&actor, &msg1);
  EXPECT_TRUE(was_empty);

  message_t msg2;
  msg2.type = 2;
  msg2.payload = NULL;
  msg2.payload_destroy = NULL;

  was_empty = actor_send(&actor, &msg2);
  EXPECT_FALSE(was_empty);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunDispatchesMessages) {
  std::vector<uint32_t> received;
  actor_t actor;
  actor_init(&actor, &received, record_dispatch);

  message_t msg1;
  msg1.type = 10;
  msg1.payload = NULL;
  msg1.payload_destroy = NULL;

  message_t msg2;
  msg2.type = 20;
  msg2.payload = NULL;
  msg2.payload_destroy = NULL;

  message_t msg3;
  msg3.type = 30;
  msg3.payload = NULL;
  msg3.payload_destroy = NULL;

  actor_send(&actor, &msg1);
  actor_send(&actor, &msg2);
  actor_send(&actor, &msg3);

  actor_run(&actor, 32);

  ASSERT_EQ(received.size(), 3u);
  EXPECT_EQ(received[0], 10u);
  EXPECT_EQ(received[1], 20u);
  EXPECT_EQ(received[2], 30u);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunBatchSize) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch);

  for (int i = 0; i < 5; i++) {
    message_t msg;
    msg.type = i;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&actor, &msg);
  }

  actor_run(&actor, 2);
  EXPECT_EQ(count, 2);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunReturnsTrueWhenMoreRemain) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch);

  for (int i = 0; i < 5; i++) {
    message_t msg;
    msg.type = i;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&actor, &msg);
  }

  bool has_more = actor_run(&actor, 2);
  EXPECT_TRUE(has_more);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunReturnsFalseWhenEmpty) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch);

  message_t msg;
  msg.type = 1;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&actor, &msg);

  bool has_more = actor_run(&actor, 32);
  EXPECT_FALSE(has_more);
  EXPECT_EQ(count, 1);

  actor_destroy(&actor);
}

TEST(TestActor, TestPayloadDestroy) {
  payload_destroy_count = 0;
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch);

  message_t msg;
  msg.type = 1;
  msg.payload = malloc(16);
  msg.payload_destroy = test_payload_destroy;

  actor_send(&actor, &msg);
  actor_run(&actor, 32);

  EXPECT_EQ(payload_destroy_count, 1);

  actor_destroy(&actor);
}