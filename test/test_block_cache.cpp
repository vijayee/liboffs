//
// Created by victor on 9/11/25.
//
#include <gtest/gtest.h>
#include <functional>
#include <gmock/gmock.h>
#include <future>
#include <atomic>
extern "C" {
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Configuration/config.h"
#include "../src/Workers/priority.h"
#include <cbor.h>
#include "../src/Util/allocator.h"
}


using ::testing::_;
using ::testing::MockFunction;
using ::testing::AtLeast;

class TestBlockLRU : public testing::Test {
public:
  size_t size = 5;
  size_t overage = 3;
  block_t* blocks[25];
  block_size_e block_type = mini;
  void SetUp() override {
    for (size_t i = 0; i < 25; i++) {
      blocks[i] = block_create_random_block_by_type(block_type);
    }
  }
  void TearDown() override {
    for (size_t i = 0; i < 25; i++) {
      block_destroy(blocks[i]);
    }
  }

};

TEST_F(TestBlockLRU, TestBlockLRUOperations) {
  block_lru_cache_t* lru = block_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    block_lru_cache_put(lru, blocks[i]);
  }
  for (size_t i = 0; i < overage; i++) {
    block_t* block = REFERENCE(block_lru_cache_get(lru, blocks[i]->hash), block_t);
    EXPECT_EQ(block == NULL, true);
    if (block != NULL) {
      DESTROY(block, block);
    }
  }
  for (size_t i = overage; i < (size + overage); i++) {
    block_t* block = REFERENCE(block_lru_cache_get(lru, blocks[i]->hash), block_t);
    EXPECT_NE(block == NULL, true);
    if (block != NULL) {
      DESTROY(block, block);
    }
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(block_lru_cache_contains(lru, blocks[i]->hash), true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    block_lru_cache_delete(lru, blocks[i]->hash);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(block_lru_cache_contains(lru, blocks[i]->hash), false);
  }

  block_lru_cache_destroy(lru);
}

#define BLOCK_COUNT 25
class TestBlockCache : public testing::Test {
public:
  block_size_e type = standard;
  char* location;
  work_pool_t* pool;
  hierarchical_timing_wheel_t* wheel;
  block_cache_t* block_cache;
  block_t* blocks[BLOCK_COUNT];
  std::promise<void> put_promise[BLOCK_COUNT];
  std::promise<void> re_put_promise[BLOCK_COUNT];
  std::promise<block_t*> get_promise[BLOCK_COUNT];
  std::promise<void> remove_promise[BLOCK_COUNT];
  std::promise<block_t*> re_get_promise[BLOCK_COUNT];
  config_t config;
  MockFunction<void((void*, void*))> mock_put_callback;
  MockFunction<void((void*, async_error_t*))> mock_put_err_callback;
  MockFunction<void((void*, void*))> mock_re_put_callback;
  MockFunction<void((void*, async_error_t*))> mock_re_put_err_callback;
  MockFunction<void((void*, block_t*))> mock_get_callback;
  MockFunction<void((void*, async_error_t*))> mock_get_err_callback;
  MockFunction<void((void*, void*))> mock_remove_callback;
  MockFunction<void((void*, async_error_t*))> mock_remove_err_callback;
  MockFunction<void((void*, async_error_t*))> mock_err_callback;
  MockFunction<void((void*, block_t*))> mock_re_get_callback;
  MockFunction<void((void*, async_error_t*))> mock_re_get_err_callback;
  void SetUp() override {
    location = path_join(".", "BlockCacheTest");
    rm_rf(location);
    pool = work_pool_create(platform_core_count());
    work_pool_launch(pool);
    wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    mkdir_p(location);
    config = config_default();
    for (size_t i = 0; i < BLOCK_COUNT; i++) {
      blocks[i] = block_create_random_block_by_type(type);
    }
  }
  void TearDown() override {
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    free(location);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
    block_cache_destroy(block_cache);
    for (size_t i = 0; i < BLOCK_COUNT; i++) {
      block_destroy(blocks[i]);
    }
  }
};
typedef struct {
  size_t i;
  TestBlockCache* test;
} tbc_ctx;

void put_callback_wrapper(void* ctx, void* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_put_callback.Call(ctx, payload);
  tbc->test->put_promise[tbc->i].set_value();
  free(ctx);
}

void put_callback_err_wrapper(void* ctx, async_error_t* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_put_err_callback.Call(ctx, payload);
  try {
    throw std::runtime_error((const char*)payload->message);
  } catch(...) {
    tbc->test->put_promise[tbc->i].set_exception(std::current_exception());
  }
  error_destroy(payload);
  free(ctx);
}

void re_put_callback_wrapper(void* ctx, void* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_re_put_callback.Call(ctx, payload);
  tbc->test->re_put_promise[tbc->i].set_value();
  free(ctx);
}

void re_put_callback_err_wrapper(void* ctx, async_error_t* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_re_put_err_callback.Call(ctx, payload);
  try {
    throw std::runtime_error((const char*)payload->message);
  } catch(...) {
    tbc->test->re_put_promise[tbc->i].set_exception(std::current_exception());
  }
  error_destroy(payload);
  free(ctx);
}

void get_callback_wrapper(void* ctx, void* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_get_callback.Call(ctx, (block_t*)payload);
  tbc->test->get_promise[tbc->i].set_value((block_t*) payload);
  free(ctx);
}


void get_callback_err_wrapper(void* ctx, async_error_t* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_get_err_callback.Call(ctx, payload);
  try {
    throw std::runtime_error((const char*)payload->message);
  } catch(...) {
    tbc->test->get_promise[tbc->i].set_exception(std::current_exception());
  }
  error_destroy(payload);
  free(ctx);
}


void remove_callback_wrapper(void* ctx, void* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_remove_callback.Call(ctx, payload);
  tbc->test->remove_promise[tbc->i].set_value();
  free(ctx);
}

void remove_callback_err_wrapper(void* ctx, async_error_t* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_remove_err_callback.Call(ctx, payload);
  try {
    throw std::runtime_error((const char*)payload->message);
  } catch(...) {
    tbc->test->get_promise[tbc->i].set_exception(std::current_exception());
  }
  error_destroy(payload);
  free(ctx);
}

void re_get_callback_wrapper(void* ctx, void* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_re_get_callback.Call(ctx, (block_t*)payload);
  tbc->test->re_get_promise[tbc->i].set_value((block_t*) payload);
  free(ctx);
}


void re_get_callback_err_wrapper(void* ctx, async_error_t* payload) {
  auto tbc = static_cast<tbc_ctx*>(ctx);
  tbc->test->mock_re_get_err_callback.Call(ctx, payload);
  try {
    throw std::runtime_error((const char*)payload->message);
  } catch(...) {
    tbc->test->re_get_promise[tbc->i].set_exception(std::current_exception());
  }
  error_destroy(payload);
  free(ctx);
}


TEST_F(TestBlockCache, TestBlockCache) {
  block_cache = block_cache_create(config, location, type, pool, wheel);
  priority_t priority = priority_get_next();

  EXPECT_CALL(mock_put_callback, Call(_,_)).Times(BLOCK_COUNT);
  EXPECT_CALL(mock_put_err_callback, Call(_,_)).Times(0);
  promise_t* put_promises_c[BLOCK_COUNT];
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    tbc_ctx* ctx = (tbc_ctx*)get_memory(sizeof(tbc_ctx));
    ctx->i = i;
    ctx->test = this;
    promise_t* promise = promise_create(put_callback_wrapper,put_callback_err_wrapper,ctx);
    put_promises_c[i] = promise;
    block_cache_put(block_cache, priority, blocks[i], promise);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    std::future<void> put_future = put_promise[i].get_future();
    try {
      put_future.get();
    } catch (...) {
      GTEST_FATAL_FAILURE_("Failed to store block " + i);
    }
  }

  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);

  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    promise_destroy(put_promises_c[i]);
  }

  if (HasFailure()) {
    GTEST_SKIP();
  }

  EXPECT_CALL(mock_re_put_callback, Call(_,_)).Times(BLOCK_COUNT);
  EXPECT_CALL(mock_re_put_err_callback, Call(_,_)).Times(0);
  promise_t* re_put_promises_c[BLOCK_COUNT];
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    tbc_ctx* ctx = (tbc_ctx*)get_memory(sizeof(tbc_ctx));
    ctx->i = i;
    ctx->test = this;
    promise_t* promise = promise_create(re_put_callback_wrapper,re_put_callback_err_wrapper,ctx);
    re_put_promises_c[i] = promise;
    block_cache_put(block_cache, priority, blocks[i], promise);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    std::future<void> re_put_future = re_put_promise[i].get_future();
    try {
      re_put_future.get();
    } catch (...) {
      GTEST_FATAL_FAILURE_("Failed to stores block " + i);
    }
  }

  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);

  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    promise_destroy(re_put_promises_c[i]);
  }

  if (HasFailure()) {
    GTEST_SKIP();
  }

  EXPECT_CALL(mock_get_callback, Call(_,_)).Times(BLOCK_COUNT);
  EXPECT_CALL(mock_get_err_callback, Call(_,_)).Times(0);

  promise_t* get_promises_c[BLOCK_COUNT];
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    tbc_ctx* ctx= (tbc_ctx*)get_memory(sizeof(tbc_ctx));
    ctx->i = i;
    ctx->test = this;
    promise_t* promise = promise_create(get_callback_wrapper,get_callback_err_wrapper,ctx);
    get_promises_c[i] = promise;
    block_cache_get(block_cache, priority, blocks[i]->hash, promise);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    std::future<block_t*> get_future = get_promise[i].get_future();
    try {
      block_t* block = get_future.get();
      EXPECT_FALSE(block == NULL);
      if (block != NULL) {
        REFERENCE(block, block_t);
        EXPECT_EQ(buffer_compare(block->hash, blocks[i]->hash), 0);
        EXPECT_EQ(buffer_compare(block->data, blocks[i]->data), 0);
        block_destroy(block);
      }
    } catch (...) {
      GTEST_FATAL_FAILURE_("Failed to retrieve block " + i);
    }
  }

  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    promise_destroy(get_promises_c[i]);
  }

  if (HasFailure()) {
    GTEST_SKIP();
  }


  EXPECT_CALL(mock_remove_callback, Call(_,_)).Times(BLOCK_COUNT);
  EXPECT_CALL(mock_remove_err_callback, Call(_,_)).Times(0);

  promise_t* remove_promises_c[BLOCK_COUNT];
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    tbc_ctx* ctx= (tbc_ctx*)get_memory(sizeof(tbc_ctx));
    ctx->i = i;
    ctx->test = this;
    promise_t* promise = promise_create(remove_callback_wrapper,remove_callback_err_wrapper,ctx);
    remove_promises_c[i] = promise;
    block_cache_remove(block_cache, priority, blocks[i]->hash, promise);
  }

  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    std::future<void> remove_future = remove_promise[i].get_future();
    try {
      remove_future.get();
    } catch (...) {
      GTEST_FATAL_FAILURE_("Failed to remove block " + i);
    }
  }

  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    promise_destroy(remove_promises_c[i]);
  }

  if (HasFailure()) {
    GTEST_SKIP();
  }

  EXPECT_CALL(mock_re_get_callback, Call(_,_)).Times(BLOCK_COUNT);
  EXPECT_CALL(mock_re_get_err_callback, Call(_,_)).Times(0);

  promise_t* re_get_promises_c[BLOCK_COUNT];
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    tbc_ctx* ctx = (tbc_ctx*)get_memory(sizeof(tbc_ctx));
    ctx->i = i;
    ctx->test = this;
    promise_t* promise = promise_create(re_get_callback_wrapper,re_get_callback_err_wrapper,ctx);
    re_get_promises_c[i] = promise;
    block_cache_get(block_cache, priority, blocks[i]->hash, promise);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    std::future<block_t*> get_future = re_get_promise[i].get_future();
    try {
      block_t* block = get_future.get();
      EXPECT_TRUE(block == NULL);
      if (block != NULL) {
        block_destroy(block);
      }
    } catch (...) {
      GTEST_FATAL_FAILURE_("Failed to retrieve block " + i);
    }
  }

  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    promise_destroy(re_get_promises_c[i]);
  }
}