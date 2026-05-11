//
// Created by victor on 5/6/25.
//

#include <gtest/gtest.h>
extern "C" {
#include "../src/Actor/message_queue.h"
#include "../src/Util/allocator.h"
}

static message_node_t* make_node(uint32_t type, void* payload) {
  message_node_t* node = (message_node_t*)get_clear_memory(sizeof(message_node_t));
  node->msg.type = type;
  node->msg.payload = payload;
  node->msg.payload_destroy = NULL;
  ATOMIC_STORE(&node->next, (message_node_t*)NULL);
  return node;
}

TEST(TestMessageQueue, TestInitDestroy) {
  message_queue_t queue;
  message_queue_init(&queue);
  EXPECT_TRUE(message_queue_isempty(&queue));
  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestPushPop) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* node = make_node(42, NULL);
  message_queue_push(&queue, node, node);

  message_node_t* popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 42u);

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestPushReturnsEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* node1 = make_node(1, NULL);
  bool was_empty = message_queue_push(&queue, node1, node1);
  EXPECT_TRUE(was_empty);

  message_node_t* node2 = make_node(2, NULL);
  was_empty = message_queue_push(&queue, node2, node2);
  EXPECT_FALSE(was_empty);

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestPopEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* popped = message_queue_pop(&queue);
  EXPECT_EQ(popped, nullptr);

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestMarkEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  EXPECT_TRUE(message_queue_markempty(&queue));

  message_node_t* node = make_node(1, NULL);
  message_queue_push(&queue, node, node);

  EXPECT_FALSE(message_queue_markempty(&queue));

  message_node_t* popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);

  EXPECT_TRUE(message_queue_markempty(&queue));

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestIsEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  EXPECT_TRUE(message_queue_isempty(&queue));

  message_node_t* node = make_node(1, NULL);
  message_queue_push(&queue, node, node);

  EXPECT_FALSE(message_queue_isempty(&queue));

  message_node_t* popped = message_queue_pop(&queue);
  (void)popped;

  EXPECT_TRUE(message_queue_isempty(&queue));

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestPushSingle) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* node1 = make_node(1, NULL);
  bool was_empty = message_queue_push_single(&queue, node1, node1);
  EXPECT_TRUE(was_empty);

  message_node_t* node2 = make_node(2, NULL);
  was_empty = message_queue_push_single(&queue, node2, node2);
  EXPECT_FALSE(was_empty);

  message_node_t* popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 1u);

  popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 2u);

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestBatchPush) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* first = make_node(1, NULL);
  message_node_t* second = make_node(2, NULL);
  message_node_t* third = make_node(3, NULL);

  ATOMIC_STORE(&first->next, second);
  ATOMIC_STORE(&second->next, third);
  ATOMIC_STORE(&third->next, (message_node_t*)NULL);

  bool was_empty = message_queue_push(&queue, first, third);
  EXPECT_TRUE(was_empty);

  message_node_t* popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 1u);

  popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 2u);

  popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 3u);

  message_queue_destroy(&queue);
}