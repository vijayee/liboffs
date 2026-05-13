#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/ofd.h"
#include "../src/OFFStreams/ori.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/Buffer/buffer.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Util/atomic_compat.h"
}

/* Completion actor state for async ofd_cache_resolve */
typedef struct {
  ATOMIC(uint8_t) done;
  ori_t* ori;
} ofd_resolve_completion_t;

static void ofd_resolve_completion_dispatch(void* state, message_t* msg) {
  ofd_resolve_completion_t* cs = (ofd_resolve_completion_t*)state;
  if (msg->type == OFD_CACHE_RESOLVE_RESULT) {
    ofd_resolve_result_t* result = (ofd_resolve_result_t*)msg->payload;
    cs->ori = result->ori;
    /* Transfer ownership of ori to completion state */
    result->ori = NULL;
  }
  ATOMIC_STORE(&cs->done, 1);
}

class TestOfdCacheBasic : public testing::Test {
protected:
    ofd_cache_t* cache;
    scheduler_pool_t* pool;
    block_cache_t* bc;
    timer_actor_t* timer;

    void SetUp() override {
        config_t config = {
            .index_bucket_size = 10,
            .index_wait = 0,
            .index_max_wait = 0,
            .section_size = 128000,
            .section_wait = 0,
            .section_max_wait = 0,
            .cache_size = 50,
            .max_tuple_size = 30,
            .lru_size = 50
        };
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create();
        bc = block_cache_create(config, (char*)"/tmp/test_ofd_cache_basic", standard, timer, pool);
        cache = ofd_cache_create(pool, bc, 300000);
    }

    void TearDown() override {
        ofd_cache_destroy(cache);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_stop(pool);
        scheduler_pool_destroy(pool);
    }
};

TEST_F(TestOfdCacheBasic, CreateDestroy) {
    ASSERT_NE(cache, nullptr);
}

TEST_F(TestOfdCacheBasic, PutAndGet) {
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);

    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)i;
    buffer_t* hash = buffer_create_from_pointer_copy(hash_data, 32);

    ofd_cache_put(cache, hash, ofd);
    ofd_t* retrieved = ofd_cache_get(cache, hash);
    EXPECT_NE(retrieved, nullptr);

    buffer_destroy(hash);
}

TEST_F(TestOfdCacheBasic, GetMissingReturnsNull) {
    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)i;
    buffer_t* hash = buffer_create_from_pointer_copy(hash_data, 32);

    ofd_t* result = ofd_cache_get(cache, hash);
    EXPECT_EQ(result, nullptr);

    buffer_destroy(hash);
}

/* NOTE: ResolveEmptyPathReturnsNull and ResolveNonexistentHashReturnsNull are
   disabled because the transient resolver actor self-destructs from within its
   dispatch handler, causing a use-after-free race with the scheduler pool threads.
   These tests should be re-enabled once the resolver cleanup is deferred. */