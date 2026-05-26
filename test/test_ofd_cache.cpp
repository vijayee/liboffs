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
#include "../src/Platform/platform_time.h"
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
    result->ori = NULL;
    /* payload_destroy cleans up hash, path, and ori */
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Completion actor state for async ofd_cache_get */
typedef struct {
  ATOMIC(uint8_t) done;
  ofd_t* ofd;
} ofd_get_completion_t;

static void ofd_get_completion_dispatch(void* state, message_t* msg) {
  ofd_get_completion_t* cs = (ofd_get_completion_t*)state;
  if (msg->type == OFD_CACHE_GET_RESULT) {
    ofd_cache_get_result_payload_t* result = (ofd_cache_get_result_payload_t*)msg->payload;
    cs->ofd = result->ofd;
    result->ofd = NULL;
    /* payload_destroy cleans up hash and ofd */
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Poll for async completion with timeout (ms). Returns true if completed. */
static int wait_for_completion(ATOMIC(uint8_t)* done, scheduler_pool_t* pool,
                                int timeout_ms) {
  uint64_t deadline = platform_monotonic_ns() / UINT64_C(1000000) + (uint64_t)timeout_ms;
  while (!ATOMIC_LOAD(done)) {
    scheduler_pool_wait_for_idle(pool);
    if ((platform_monotonic_ns() / UINT64_C(1000000)) >= deadline) {
      return 0;
    }
  }
  return 1;
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
        bc = block_cache_create(config, (char*)"/tmp/test_ofd_cache_basic", standard, timer, pool, NULL, 0);
        cache = ofd_cache_create(pool, bc, 300000);
    }

    void TearDown() override {
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        ofd_cache_destroy(cache);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
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

    /* Poll until put is processed */
    actor_t completion_actor;
    ofd_get_completion_t completion_state = {{0}, NULL};
    actor_init(&completion_actor, &completion_state, ofd_get_completion_dispatch, pool);

    ofd_cache_get(cache, hash, &completion_actor);
    int completed = wait_for_completion(&completion_state.done, pool, 5000);

    EXPECT_NE(completion_state.ofd, nullptr);
    if (completion_state.ofd) {
        DESTROY(completion_state.ofd, ofd);
    }
    actor_destroy(&completion_actor);
    buffer_destroy(hash);
    ASSERT_TRUE(completed);
}

TEST_F(TestOfdCacheBasic, GetMissingReturnsNull) {
    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)i;
    buffer_t* hash = buffer_create_from_pointer_copy(hash_data, 32);

    actor_t completion_actor;
    ofd_get_completion_t completion_state = {{0}, NULL};
    actor_init(&completion_actor, &completion_state, ofd_get_completion_dispatch, pool);

    ofd_cache_get(cache, hash, &completion_actor);
    int completed = wait_for_completion(&completion_state.done, pool, 5000);

    EXPECT_EQ(completion_state.ofd, nullptr);
    actor_destroy(&completion_actor);
    buffer_destroy(hash);
    ASSERT_TRUE(completed);
}

/* Resolve a file path within a cached OFD — exercises the synchronous cached path */
TEST_F(TestOfdCacheBasic, ResolveFromCache) {
    /* Create an OFD with a file entry */
    ofd_t* root_ofd = ofd_create();
    ASSERT_NE(root_ofd, nullptr);

    ori_t* file_ori = ori_create(100);
    uint8_t hash_bytes[32];
    for (int i = 0; i < 32; i++) hash_bytes[i] = (uint8_t)(i + 0x10);
    file_ori->file_hash = buffer_create_from_pointer_copy(hash_bytes, 32);
    file_ori->file_name = strdup("test.txt");
    ofd_add_file(root_ofd, "test.txt", file_ori);
    DESTROY(file_ori, ori);

    /* Use a fixed hash key for the OFD */
    uint8_t root_hash[32];
    for (int i = 0; i < 32; i++) root_hash[i] = (uint8_t)(i + 0xAA);
    buffer_t* root_hash_buf = buffer_create_from_pointer_copy(root_hash, 32);

    /* Put the OFD in the cache */
    ofd_cache_put(cache, root_hash_buf, root_ofd);

    /* Resolve "test.txt" — should be synchronous (from cache) */
    actor_t completion_actor;
    ofd_resolve_completion_t completion_state = {{0}, NULL};
    actor_init(&completion_actor, &completion_state, ofd_resolve_completion_dispatch, pool);

    ofd_cache_resolve(cache, root_hash_buf, "test.txt", &completion_actor);
    int completed = wait_for_completion(&completion_state.done, pool, 5000);

    EXPECT_NE(completion_state.ori, nullptr);
    if (completion_state.ori) {
        EXPECT_STREQ(completion_state.ori->file_name, "test.txt");
        DESTROY(completion_state.ori, ori);
    }
    actor_destroy(&completion_actor);
    buffer_destroy(root_hash_buf);
    ASSERT_TRUE(completed);
}

/* Resolve a path that doesn't exist in the OFD */
TEST_F(TestOfdCacheBasic, ResolveNonexistentPath) {
    /* Create an OFD with a single file entry */
    ofd_t* root_ofd = ofd_create();
    ASSERT_NE(root_ofd, nullptr);

    ori_t* file_ori = ori_create(100);
    uint8_t hash_bytes[32];
    for (int i = 0; i < 32; i++) hash_bytes[i] = (uint8_t)(i + 0x30);
    file_ori->file_hash = buffer_create_from_pointer_copy(hash_bytes, 32);
    file_ori->file_name = strdup("real_file.txt");
    ofd_add_file(root_ofd, "real_file.txt", file_ori);
    DESTROY(file_ori, ori);

    uint8_t root_hash[32];
    for (int i = 0; i < 32; i++) root_hash[i] = (uint8_t)(i + 0x40);
    buffer_t* root_hash_buf = buffer_create_from_pointer_copy(root_hash, 32);

    ofd_cache_put(cache, root_hash_buf, root_ofd);

    actor_t completion_actor;
    ofd_resolve_completion_t completion_state = {{0}, NULL};
    actor_init(&completion_actor, &completion_state, ofd_resolve_completion_dispatch, pool);

    ofd_cache_resolve(cache, root_hash_buf, "nonexistent.txt", &completion_actor);
    int completed = wait_for_completion(&completion_state.done, pool, 5000);

    EXPECT_EQ(completion_state.ori, nullptr);
    actor_destroy(&completion_actor);
    buffer_destroy(root_hash_buf);
    ASSERT_TRUE(completed);
}