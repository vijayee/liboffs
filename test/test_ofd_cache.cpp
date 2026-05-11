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
        timer = timer_actor_create();
        bc = block_cache_create(config, (char*)"/tmp/test_ofd_cache_basic", standard, timer);
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

TEST_F(TestOfdCacheBasic, ResolveEmptyPathReturnsNull) {
    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)i;
    buffer_t* hash = buffer_create_from_pointer_copy(hash_data, 32);

    ori_t* result = ofd_cache_resolve(cache, hash, "");
    EXPECT_EQ(result, nullptr);

    buffer_destroy(hash);
}

TEST_F(TestOfdCacheBasic, ResolveNonexistentHashReturnsNull) {
    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)i;
    buffer_t* hash = buffer_create_from_pointer_copy(hash_data, 32);

    ori_t* result = ofd_cache_resolve(cache, hash, "somefile.txt");
    EXPECT_EQ(result, nullptr);

    buffer_destroy(hash);
}