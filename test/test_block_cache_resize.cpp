#include <gtest/gtest.h>
#include <cstdio>
#include <string>
extern "C" {
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Platform/platform_time.h"
#include "../src/Util/allocator.h"
}

/* Mirrors test_block_cache.cpp's OFFS_BC_TMP isolation: parallel test
   processes must not collide on one on-disk cache directory. */
static const char* bc_tmp_base() {
  const char* t = getenv("OFFS_BC_TMP");
  return (t != NULL && t[0] != '\0') ? t : "/tmp";
}

typedef struct {
  ATOMIC(uint8_t) done;
  size_t max_capacity_bytes;
  size_t current_bytes;
} resize_completion_t;

static void resize_completion_dispatch(void* state, message_t* msg) {
  resize_completion_t* cs = (resize_completion_t*)state;
  if (msg->type == CACHE_RESIZE_RESULT) {
    cache_resize_result_payload_t* r = (cache_resize_result_payload_t*)msg->payload;
    cs->max_capacity_bytes = r->max_capacity_bytes;
    cs->current_bytes = r->current_bytes;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Resize and wait for the CACHE_RESIZE_RESULT reply. Mirrors
   test_block_cache.cpp's bc_put_sync wait pattern: the completion actor
   is scheduled by actor_send when bc->actor delivers the result, and the
   pool must be idle before actor_destroy. */
static void resize_sync(block_cache_t* bc, size_t max_capacity_bytes,
                        scheduler_pool_t* pool, size_t* out_max,
                        size_t* out_current) {
  resize_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, resize_completion_dispatch, pool);

  block_cache_resize(bc, max_capacity_bytes, &comp);

  while (!ATOMIC_LOAD(&cs.done)) {
    platform_sleep_ms(1);
  }
  scheduler_pool_wait_for_idle(pool);

  actor_destroy(&comp);
  *out_max = cs.max_capacity_bytes;
  *out_current = cs.current_bytes;
}

class BlockCacheResize : public ::testing::Test {
protected:
  void SetUp() override {
    location_ = path_join(bc_tmp_base(), "BlockCacheResizeTest");
    rm_rf(location_);
    mkdir_p(location_);

    pool_ = scheduler_pool_create(2);
    ASSERT_NE(pool_, nullptr);
    scheduler_pool_start(pool_);
    timer_actor_ = timer_actor_create(pool_);
    ASSERT_NE(timer_actor_, nullptr);

    config_t config = config_default();
    config.index_wait = 60000;
    config.index_max_wait = 60000;
    bc_ = block_cache_create(config, location_, mini, timer_actor_, pool_,
                             NULL, 0);
    ASSERT_NE(bc_, nullptr);
    scheduler_pool_wait_for_idle(pool_);
  }

  void TearDown() override {
    block_cache_destroy(bc_);
    scheduler_pool_wait_for_idle(pool_);
    timer_actor_destroy(timer_actor_);
    scheduler_pool_wait_for_idle(pool_);
    scheduler_pool_stop(pool_);
    scheduler_pool_destroy(pool_);
    rm_rf(location_);
    free(location_);
  }

  scheduler_pool_t* pool_ = nullptr;
  timer_actor_t* timer_actor_ = nullptr;
  block_cache_t* bc_ = nullptr;
  char* location_ = nullptr;
};

/* Growing updates the capacity the live CACHE_PUT gate reads, and the
   reply reports the applied capacity. The empty cache's recomputed
   footprint is 0. */
TEST_F(BlockCacheResize, GrowUpdatesCapacityLive) {
  size_t out_max = 0, out_current = 0;
  resize_sync(bc_, 2000000, pool_, &out_max, &out_current);
  EXPECT_EQ((size_t)2000000, out_max);
  EXPECT_EQ((size_t)0, out_current);

  EXPECT_EQ(CACHE_FIT_OK, block_cache_can_fit(bc_, 1500000));
  EXPECT_EQ(CACHE_FIT_FULL, block_cache_can_fit(bc_, 2500000));
}

/* A shrink recomputes current_bytes from the index (0 here — no blocks)
   and re-arms the capacity gate at the smaller ceiling. The shed of any
   pre-existing entries rides the async respiration exhale, so the reply
   reports the recomputation, not the post-shed footprint. */
TEST_F(BlockCacheResize, ShrinkRecomputesAndRearmsGate) {
  size_t out_max = 0, out_current = 0;
  resize_sync(bc_, 8000000, pool_, &out_max, &out_current);
  ASSERT_EQ((size_t)8000000, out_max);

  resize_sync(bc_, 100000, pool_, &out_max, &out_current);
  EXPECT_EQ((size_t)100000, out_max);
  EXPECT_EQ((size_t)0, out_current);
  EXPECT_EQ(CACHE_FIT_FULL, block_cache_can_fit(bc_, 500000));
}

/* max_capacity_bytes == 0 means unlimited at the primitive level — the
   wire handler is what rejects a user-facing 0. */
TEST_F(BlockCacheResize, ZeroMeansUnlimited) {
  size_t out_max = 0, out_current = 0;
  resize_sync(bc_, 500000, pool_, &out_max, &out_current);
  ASSERT_EQ((size_t)500000, out_max);

  resize_sync(bc_, 0, pool_, &out_max, &out_current);
  EXPECT_EQ((size_t)0, out_max);
  EXPECT_EQ(CACHE_FIT_OK, block_cache_can_fit(bc_, 999999999ULL));
}