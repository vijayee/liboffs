#include <gtest/gtest.h>
#include <future>
#include <chrono>
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
#include "../src/Util/atomic_compat.h"
#include <cbor.h>
#include "../src/Util/allocator.h"
}

/* ---- Completion actor for async block_cache tests ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  int put_result;
  block_t* get_block;
  buffer_t* get_hash;
  int remove_result;
} bc_completion_t;

static void bc_completion_dispatch(void* state, message_t* msg) {
  bc_completion_t* cs = (bc_completion_t*)state;
  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* r = (cache_put_result_payload_t*)msg->payload;
      cs->put_result = r->result;
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* r = (cache_get_result_payload_t*)msg->payload;
      cs->get_block = r->block;
      cs->get_hash = r->hash;
      r->block = NULL;
      r->hash = NULL;
      break;
    }
    case CACHE_REMOVE_RESULT: {
      cache_remove_result_payload_t* r = (cache_remove_result_payload_t*)msg->payload;
      cs->remove_result = r->result;
      break;
    }
    default:
      break;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Helper: put a block and wait for result */
static int bc_put_sync(block_cache_t* bc, block_t* block, scheduler_pool_t* pool) {
  bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, bc_completion_dispatch, pool);

  block_t* ref_block = (block_t*)refcounter_reference((refcounter_t*)block);
  refcounter_yield((refcounter_t*)ref_block);
  block_cache_put(bc, ref_block, 0, &comp);

  if (pool) {
    scheduler_inject(pool, &comp);
    while (!ATOMIC_LOAD(&cs.done)) { usleep(1000); }
  } else {
    while (!ATOMIC_LOAD(&cs.done)) {
      actor_run(&bc->actor, ACTOR_BATCH_SIZE);
      actor_run(&comp, ACTOR_BATCH_SIZE);
    }
  }

  actor_destroy(&comp);
  return cs.put_result;
}

/* Helper: get a block and wait for result */
static block_t* bc_get_sync(block_cache_t* bc, buffer_t* hash, scheduler_pool_t* pool) {
  bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, bc_completion_dispatch, pool);

  block_cache_get(bc, hash, &comp);

  if (pool) {
    scheduler_inject(pool, &comp);
    while (!ATOMIC_LOAD(&cs.done)) { usleep(1000); }
  } else {
    while (!ATOMIC_LOAD(&cs.done)) {
      actor_run(&bc->actor, ACTOR_BATCH_SIZE);
      actor_run(&comp, ACTOR_BATCH_SIZE);
    }
  }

  if (cs.get_hash) {
    DESTROY(cs.get_hash, buffer);
  }

  actor_destroy(&comp);
  return cs.get_block;
}

/* Helper: remove a block and wait for result */
static int bc_remove_sync(block_cache_t* bc, buffer_t* hash, scheduler_pool_t* pool) {
  bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, bc_completion_dispatch, pool);

  block_cache_remove(bc, hash, &comp);

  if (pool) {
    scheduler_inject(pool, &comp);
    while (!ATOMIC_LOAD(&cs.done)) { usleep(1000); }
  } else {
    while (!ATOMIC_LOAD(&cs.done)) {
      actor_run(&bc->actor, ACTOR_BATCH_SIZE);
      actor_run(&comp, ACTOR_BATCH_SIZE);
    }
  }

  actor_destroy(&comp);
  return cs.remove_result;
}

class TestBlockLRU : public testing::Test {
public:
  size_t size = 5;
  size_t overage = 3;
  block_t* blocks[25];
  index_entry_t* entries[25];
  block_size_e block_type = mini;
  void SetUp() override {
    for (size_t i = 0; i < 25; i++) {
      blocks[i] = block_create_random_block_by_type(block_type);
      entries[i] = index_entry_create(blocks[i]->hash);
    }
  }
  void TearDown() override {
    for (size_t i = 0; i < 25; i++) {
      index_entry_destroy(entries[i]);
      block_destroy(blocks[i]);
    }
  }
};

TEST_F(TestBlockLRU, TestBlockLRUOperations) {
  block_lru_cache_t* lru = block_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    index_entry_t* ejected = (index_entry_t*)refcounter_reference(
        (refcounter_t*)block_lru_cache_put(lru, blocks[i], entries[i]));
    if (i >= size) {
      EXPECT_NE(ejected, nullptr);
    }
    if (ejected) {
      index_entry_destroy(ejected);
    }
  }
  for (size_t i = 0; i < overage; i++) {
    block_t* block = block_lru_cache_get(lru, blocks[i]->hash);
    EXPECT_EQ(block, nullptr);
    if (block) {
      block_destroy(block);
    }
  }
  for (size_t i = overage; i < (size + overage); i++) {
    block_t* block = block_lru_cache_get(lru, blocks[i]->hash);
    EXPECT_NE(block, nullptr);
    if (block) {
      block_destroy(block);
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
  timer_actor_t* timer_actor;
  block_cache_t* block_cache;
  block_t* blocks[BLOCK_COUNT];
  config_t config;
  void SetUp() override {
    location = path_join("/tmp", "BlockCacheTest");
    rm_rf(location);
    timer_actor = timer_actor_create();
    mkdir_p(location);
    config = config_default();
    for (size_t i = 0; i < BLOCK_COUNT; i++) {
      blocks[i] = block_create_random_block_by_type(type);
    }
  }
  void TearDown() override {
    timer_actor_destroy(timer_actor);
    block_cache_destroy(block_cache);
    free(location);
    for (size_t i = 0; i < BLOCK_COUNT; i++) {
      block_destroy(blocks[i]);
    }
  }
};

TEST_F(TestBlockCache, TestBlockCache) {
  block_cache = block_cache_create(config, location, type, timer_actor, NULL);

  /* Put all blocks */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = bc_put_sync(block_cache, blocks[i], NULL);
    EXPECT_EQ(result, CACHE_PUT_NEW) << "Failed to store block " << i;
  }

  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);

  if (HasFailure()) {
    GTEST_SKIP();
  }

  /* Re-put same blocks (should return CACHE_PUT_EXISTS, no duplicate) */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = bc_put_sync(block_cache, blocks[i], NULL);
    EXPECT_EQ(result, CACHE_PUT_EXISTS) << "Re-put should return CACHE_PUT_EXISTS for block " << i;
  }

  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);

  if (HasFailure()) {
    GTEST_SKIP();
  }

  /* Get all blocks */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = bc_get_sync(block_cache, blocks[i]->hash, NULL);
    EXPECT_NE(block, nullptr) << "Failed to retrieve block " << i;
    if (block != NULL) {
      EXPECT_EQ(buffer_compare(block->hash, blocks[i]->hash), 0);
      EXPECT_EQ(buffer_compare(block->data, blocks[i]->data), 0);
      block_destroy(block);
    }
  }

  if (HasFailure()) {
    GTEST_SKIP();
  }

  /* Remove all blocks */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = bc_remove_sync(block_cache, blocks[i]->hash, NULL);
    EXPECT_EQ(result, 0) << "Failed to remove block " << i;
  }

  EXPECT_EQ(block_cache_count(block_cache), 0u);

  /* Get removed blocks (should return NULL) */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = bc_get_sync(block_cache, blocks[i]->hash, NULL);
    EXPECT_EQ(block, nullptr) << "Retrieved removed block " << i;
    if (block != NULL) {
      block_destroy(block);
    }
  }
}

TEST_F(TestBlockCache, TestBlockCachePutOnly) {
  block_cache = block_cache_create(config, location, type, timer_actor, NULL);
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = bc_put_sync(block_cache, blocks[i], NULL);
    EXPECT_EQ(result, CACHE_PUT_NEW);
  }
  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);
}

TEST_F(TestBlockCache, TestBlockCachePutGetOnly) {
  block_cache = block_cache_create(config, location, type, timer_actor, NULL);
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = bc_put_sync(block_cache, blocks[i], NULL);
    EXPECT_EQ(result, CACHE_PUT_NEW);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = bc_get_sync(block_cache, blocks[i]->hash, NULL);
    EXPECT_NE(block, nullptr);
    if (block) block_destroy(block);
  }
  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);
}

TEST_F(TestBlockCache, TestBlockCachePutRemoveOnly) {
  block_cache = block_cache_create(config, location, type, timer_actor, NULL);
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = bc_put_sync(block_cache, blocks[i], NULL);
    EXPECT_EQ(result, CACHE_PUT_NEW);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = bc_remove_sync(block_cache, blocks[i]->hash, NULL);
    EXPECT_EQ(result, 0);
  }
  EXPECT_EQ(block_cache_count(block_cache), 0u);
}

/* ---- Integration tests ---- */

#define LRU_COUNT 35

class TestBlockCacheIntegration : public testing::Test {
public:
  block_size_e type = standard;
  char* location;
  timer_actor_t* timer_actor;
  block_cache_t* block_cache;
  block_t* blocks[LRU_COUNT];
  config_t config;
  scheduler_pool_t* pool;
  void SetUp() override {
    location = path_join("/tmp", "BlockCacheIntegrationTest");
    rm_rf(location);
    timer_actor = timer_actor_create();
    mkdir_p(location);
    config = config_default();
    config.lru_size = 10;
    for (size_t i = 0; i < LRU_COUNT; i++) {
      blocks[i] = block_create_random_block_by_type(type);
    }
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
  }
  void TearDown() override {
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    timer_actor_destroy(timer_actor);
    block_cache_destroy(block_cache);
    free(location);
    for (size_t i = 0; i < LRU_COUNT; i++) {
      block_destroy(blocks[i]);
    }
  }
};

TEST_F(TestBlockCacheIntegration, TestLRUEjectionAndReload) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool);

  /* Put more blocks than LRU can hold */
  for (size_t i = 0; i < LRU_COUNT; i++) {
    int result = bc_put_sync(block_cache, blocks[i], pool);
    EXPECT_EQ(result, CACHE_PUT_NEW) << "Failed to put block " << i;
  }

  /* All blocks should still be in the index (including ejected ones) */
  EXPECT_EQ(block_cache_count(block_cache), (size_t)LRU_COUNT);

  if (HasFailure()) {
    GTEST_SKIP();
  }

  /* Get all blocks — ejected ones should reload from sections */
  for (size_t i = 0; i < LRU_COUNT; i++) {
    block_t* block = bc_get_sync(block_cache, blocks[i]->hash, pool);
    EXPECT_NE(block, nullptr) << "Failed to retrieve block " << i;
    if (block != NULL) {
      EXPECT_EQ(buffer_compare(block->data, blocks[i]->data), 0)
          << "Data mismatch for block " << i;
      block_destroy(block);
    }
  }
}

TEST_F(TestBlockCacheIntegration, TestGetNonExistent) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool);

  block_t* result = bc_get_sync(block_cache, blocks[0]->hash, pool);
  EXPECT_EQ(result, nullptr);

  /* Removing non-existent block should succeed (idempotent) */
  int rc = bc_remove_sync(block_cache, blocks[0]->hash, pool);
  EXPECT_EQ(rc, 0);
}

TEST_F(TestBlockCacheIntegration, TestPutDuplicate) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool);

  /* Put same block twice — should not create duplicate index entry */
  int result = bc_put_sync(block_cache, blocks[0], pool);
  EXPECT_EQ(result, CACHE_PUT_NEW);

  result = bc_put_sync(block_cache, blocks[0], pool);
  EXPECT_EQ(result, CACHE_PUT_EXISTS);

  EXPECT_EQ(block_cache_count(block_cache), 1u);
}

TEST_F(TestBlockCacheIntegration, TestRemoveIdempotent) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool);

  int result = bc_put_sync(block_cache, blocks[0], pool);
  EXPECT_EQ(result, CACHE_PUT_NEW);
  EXPECT_EQ(block_cache_count(block_cache), 1u);

  /* Remove once */
  result = bc_remove_sync(block_cache, blocks[0]->hash, pool);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 0u);

  /* Remove again — should be idempotent */
  result = bc_remove_sync(block_cache, blocks[0]->hash, pool);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 0u);
}

TEST_F(TestBlockCacheIntegration, TestMiniBlocks) {
  block_cache = block_cache_create(config, location, mini, timer_actor, pool);

  for (size_t i = 0; i < 10; i++) {
    block_t* mini_block = block_create_random_block_by_type(mini);
    int result = bc_put_sync(block_cache, mini_block, pool);
    EXPECT_EQ(result, CACHE_PUT_NEW);
    block_t* retrieved = bc_get_sync(block_cache, mini_block->hash, pool);
    EXPECT_NE(retrieved, nullptr);
    if (retrieved) {
      EXPECT_EQ(buffer_compare(retrieved->data, mini_block->data), 0);
      block_destroy(retrieved);
    }
    block_destroy(mini_block);
  }
}