#include <gtest/gtest.h>
#include <future>
#include <chrono>
extern "C" {
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/Buffer/buffer.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/atomic_compat.h"
}

TEST(TupleCacheLRU, TestCreateDestroy) {
  tuple_cache_lru_t* lru = tuple_cache_lru_create(10);
  ASSERT_NE(lru, nullptr);
  EXPECT_EQ(tuple_cache_lru_size(lru), 0u);
  tuple_cache_lru_destroy(lru);
}

TEST(TupleCacheLRU, TestPutAndGet) {
  tuple_cache_lru_t* lru = tuple_cache_lru_create(10);

  uint8_t d1[] = {0x01};
  uint8_t d2[] = {0x02};
  buffer_t* h1 = buffer_create_from_pointer_copy(d1, 1);
  buffer_t* h2 = buffer_create_from_pointer_copy(d2, 1);

  tuple_t* key = tuple_create(2);
  tuple_push(key, h1);
  tuple_push(key, h2);

  uint8_t val_data[] = {0xAA, 0xBB};
  buffer_t* val = buffer_create_from_pointer_copy(val_data, 2);

  tuple_cache_lru_put(lru, key, val);
  EXPECT_EQ(tuple_cache_lru_size(lru), 1u);

  buffer_t* result = tuple_cache_lru_get(lru, key);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(memcmp(result->data, val_data, 2), 0);

  DESTROY(result, buffer);
  tuple_cache_lru_destroy(lru);
  DESTROY(val, buffer);
  tuple_destroy(key);
  DESTROY(h1, buffer);
  DESTROY(h2, buffer);
}

TEST(TupleCacheLRU, TestCacheMiss) {
  tuple_cache_lru_t* lru = tuple_cache_lru_create(10);

  uint8_t d1[] = {0x01};
  buffer_t* h1 = buffer_create_from_pointer_copy(d1, 1);
  tuple_t* key = tuple_create(1);
  tuple_push(key, h1);

  buffer_t* result = tuple_cache_lru_get(lru, key);
  EXPECT_EQ(result, nullptr);

  tuple_cache_lru_destroy(lru);
  tuple_destroy(key);
  DESTROY(h1, buffer);
}

TEST(TupleCacheLRU, TestEviction) {
  tuple_cache_lru_t* lru = tuple_cache_lru_create(2);

  uint8_t d1[] = {0x01};
  uint8_t d2[] = {0x02};
  uint8_t d3[] = {0x03};
  uint8_t vd[] = {0xFF};

  buffer_t* h1 = buffer_create_from_pointer_copy(d1, 1);
  buffer_t* h2 = buffer_create_from_pointer_copy(d2, 1);
  buffer_t* h3 = buffer_create_from_pointer_copy(d3, 1);

  tuple_t* key1 = tuple_create(1);
  tuple_push(key1, h1);
  tuple_t* key2 = tuple_create(1);
  tuple_push(key2, h2);
  tuple_t* key3 = tuple_create(1);
  tuple_push(key3, h3);

  buffer_t* val1 = buffer_create_from_pointer_copy(vd, 1);
  buffer_t* val2 = buffer_create_from_pointer_copy(vd, 1);
  buffer_t* val3 = buffer_create_from_pointer_copy(vd, 1);

  tuple_cache_lru_put(lru, key1, val1);
  tuple_cache_lru_put(lru, key2, val2);
  EXPECT_EQ(tuple_cache_lru_size(lru), 2u);

  tuple_cache_lru_put(lru, key3, val3);
  EXPECT_EQ(tuple_cache_lru_size(lru), 2u);

  buffer_t* miss = tuple_cache_lru_get(lru, key1);
  EXPECT_EQ(miss, nullptr);

  buffer_t* hit = tuple_cache_lru_get(lru, key3);
  ASSERT_NE(hit, nullptr);
  DESTROY(hit, buffer);

  tuple_cache_lru_destroy(lru);
  tuple_destroy(key1);
  tuple_destroy(key2);
  tuple_destroy(key3);
  DESTROY(h1, buffer);
  DESTROY(h2, buffer);
  DESTROY(h3, buffer);
  DESTROY(val1, buffer);
  DESTROY(val2, buffer);
  DESTROY(val3, buffer);
}

TEST(TupleCacheLRU, TestRemove) {
  tuple_cache_lru_t* lru = tuple_cache_lru_create(10);

  uint8_t d1[] = {0x01};
  buffer_t* h1 = buffer_create_from_pointer_copy(d1, 1);
  tuple_t* key = tuple_create(1);
  tuple_push(key, h1);

  uint8_t vd[] = {0xFF};
  buffer_t* val = buffer_create_from_pointer_copy(vd, 1);

  tuple_cache_lru_put(lru, key, val);
  EXPECT_EQ(tuple_cache_lru_size(lru), 1u);

  tuple_cache_lru_remove(lru, key);
  EXPECT_EQ(tuple_cache_lru_size(lru), 0u);

  buffer_t* result = tuple_cache_lru_get(lru, key);
  EXPECT_EQ(result, nullptr);

  tuple_cache_lru_destroy(lru);
  tuple_destroy(key);
  DESTROY(h1, buffer);
  DESTROY(val, buffer);
}

TEST(TupleCacheLRU, TestContains) {
  tuple_cache_lru_t* lru = tuple_cache_lru_create(10);

  uint8_t d1[] = {0x01};
  uint8_t d2[] = {0x02};
  buffer_t* h1 = buffer_create_from_pointer_copy(d1, 1);
  buffer_t* h2 = buffer_create_from_pointer_copy(d2, 1);

  tuple_t* key1 = tuple_create(1);
  tuple_push(key1, h1);
  tuple_t* key2 = tuple_create(1);
  tuple_push(key2, h2);

  uint8_t vd[] = {0xFF};
  buffer_t* val = buffer_create_from_pointer_copy(vd, 1);

  EXPECT_FALSE(tuple_cache_lru_contains(lru, key1));

  tuple_cache_lru_put(lru, key1, val);
  EXPECT_TRUE(tuple_cache_lru_contains(lru, key1));
  EXPECT_FALSE(tuple_cache_lru_contains(lru, key2));

  tuple_cache_lru_destroy(lru);
  tuple_destroy(key1);
  tuple_destroy(key2);
  DESTROY(h1, buffer);
  DESTROY(h2, buffer);
  DESTROY(val, buffer);
}

/* Completion state for async tuple_cache actor tests */
typedef struct {
  ATOMIC(uint8_t) done;
  tuple_t* key;
  buffer_t* value;
} tuple_cache_completion_t;

static void tuple_cache_completion_dispatch(void* state, message_t* msg) {
  tuple_cache_completion_t* cs = (tuple_cache_completion_t*)state;
  if (msg->type == TUPLE_CACHE_GET_RESULT) {
    tuple_cache_get_result_payload_t* r = (tuple_cache_get_result_payload_t*)msg->payload;
    cs->key = r->key;
    cs->value = r->value;
    r->value = NULL;  /* transfer ownership to cs */
  }
  ATOMIC_STORE(&cs->done, 1);
}

TEST(TupleCacheActor, TestCreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  tuple_cache_t* tc = tuple_cache_create(10, pool);
  ASSERT_NE(tc, nullptr);
  tuple_cache_destroy(tc);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(TupleCacheActor, TestApplyAndUpdate) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  tuple_cache_t* tc = tuple_cache_create(10, pool);

  uint8_t d1[] = {0x01};
  buffer_t* h1 = buffer_create_from_pointer_copy(d1, 1);
  tuple_t* key = tuple_create(1);
  tuple_push(key, h1);

  uint8_t val_data[] = {0xAA, 0xBB};
  buffer_t* val = buffer_create_from_pointer_copy(val_data, 2);

  /* Put value via async API (fire-and-forget) */
  tuple_cache_put(tc, key, val);

  /* Wait for put to complete */
  while (!tuple_cache_lru_contains(tc->lru, key)) {
    usleep(1000);
  }

  /* Get value via async API with completion actor */
  tuple_cache_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, tuple_cache_completion_dispatch, pool);

  tuple_cache_get(tc, key, &comp);

  auto future = std::async(std::launch::async, [&cs]() {
    while (!ATOMIC_LOAD(&cs.done)) { usleep(1000); }
  });
  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  ASSERT_NE(cs.value, nullptr);
  EXPECT_EQ(memcmp(cs.value->data, val_data, 2), 0);
  DESTROY(cs.value, buffer);
  DESTROY(cs.key, tuple);

  tuple_cache_destroy(tc);
  actor_destroy(&comp);
  tuple_destroy(key);
  DESTROY(h1, buffer);
  DESTROY(val, buffer);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(TupleCacheActor, TestRemove) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  tuple_cache_t* tc = tuple_cache_create(10, pool);

  uint8_t d1[] = {0x01};
  buffer_t* h1 = buffer_create_from_pointer_copy(d1, 1);
  tuple_t* key = tuple_create(1);
  tuple_push(key, h1);

  uint8_t vd[] = {0xFF};
  buffer_t* val = buffer_create_from_pointer_copy(vd, 1);

  /* Put then remove via LRU (direct ops, no actor needed) */
  tuple_cache_lru_put(tc->lru, key, val);
  EXPECT_EQ(tuple_cache_lru_size(tc->lru), 1u);

  tuple_cache_lru_remove(tc->lru, key);
  EXPECT_EQ(tuple_cache_lru_size(tc->lru), 0u);

  /* Verify get returns null */
  buffer_t* result = tuple_cache_lru_get(tc->lru, key);
  EXPECT_EQ(result, nullptr);

  tuple_cache_destroy(tc);
  tuple_destroy(key);
  DESTROY(h1, buffer);
  DESTROY(val, buffer);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}