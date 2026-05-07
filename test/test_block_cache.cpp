#include <gtest/gtest.h>
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
#include <cbor.h>
#include "../src/Util/allocator.h"
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
    location = path_join(".", "BlockCacheTest");
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
  block_cache = block_cache_create(config, location, type, timer_actor);

  /* Put all blocks */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[i]);
    refcounter_yield((refcounter_t*)block);
    int result = block_cache_put(block_cache, block);
    EXPECT_EQ(result, 0) << "Failed to store block " << i;
  }

  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);

  if (HasFailure()) {
    GTEST_SKIP();
  }

  /* Re-put same blocks (should succeed, no duplicate) */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[i]);
    refcounter_yield((refcounter_t*)block);
    int result = block_cache_put(block_cache, block);
    EXPECT_EQ(result, 0) << "Failed to re-store block " << i;
  }

  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);

  if (HasFailure()) {
    GTEST_SKIP();
  }

  /* Get all blocks */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = block_cache_get(block_cache, blocks[i]->hash);
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
    int result = block_cache_remove(block_cache, blocks[i]->hash);
    EXPECT_EQ(result, 0) << "Failed to remove block " << i;
  }

  EXPECT_EQ(block_cache_count(block_cache), 0u);

  /* Get removed blocks (should return NULL) */
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = block_cache_get(block_cache, blocks[i]->hash);
    EXPECT_EQ(block, nullptr) << "Retrieved removed block " << i;
    if (block != NULL) {
      block_destroy(block);
    }
  }
}

TEST_F(TestBlockCache, TestBlockCachePutOnly) {
  block_cache = block_cache_create(config, location, type, timer_actor);
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[i]);
    refcounter_yield((refcounter_t*)block);
    int result = block_cache_put(block_cache, block);
    EXPECT_EQ(result, 0);
  }
  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);
}

TEST_F(TestBlockCache, TestBlockCachePutGetOnly) {
  block_cache = block_cache_create(config, location, type, timer_actor);
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[i]);
    refcounter_yield((refcounter_t*)block);
    int result = block_cache_put(block_cache, block);
    EXPECT_EQ(result, 0);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = block_cache_get(block_cache, blocks[i]->hash);
    EXPECT_NE(block, nullptr);
    if (block) block_destroy(block);
  }
  EXPECT_EQ(block_cache_count(block_cache), BLOCK_COUNT);
}

TEST_F(TestBlockCache, TestBlockCachePutRemoveOnly) {
  block_cache = block_cache_create(config, location, type, timer_actor);
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[i]);
    refcounter_yield((refcounter_t*)block);
    int result = block_cache_put(block_cache, block);
    EXPECT_EQ(result, 0);
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    int result = block_cache_remove(block_cache, blocks[i]->hash);
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
  void SetUp() override {
    location = path_join(".", "BlockCacheIntegrationTest");
    rm_rf(location);
    timer_actor = timer_actor_create();
    mkdir_p(location);
    config = config_default();
    config.lru_size = 10;
    for (size_t i = 0; i < LRU_COUNT; i++) {
      blocks[i] = block_create_random_block_by_type(type);
    }
  }
  void TearDown() override {
    timer_actor_destroy(timer_actor);
    block_cache_destroy(block_cache);
    free(location);
    for (size_t i = 0; i < LRU_COUNT; i++) {
      block_destroy(blocks[i]);
    }
  }
};

TEST_F(TestBlockCacheIntegration, TestLRUEjectionAndReload) {
  block_cache = block_cache_create(config, location, type, timer_actor);

  /* Put more blocks than LRU can hold */
  for (size_t i = 0; i < LRU_COUNT; i++) {
    block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[i]);
    refcounter_yield((refcounter_t*)block);
    int result = block_cache_put(block_cache, block);
    EXPECT_EQ(result, 0) << "Failed to put block " << i;
  }

  /* All blocks should still be in the index (including ejected ones) */
  EXPECT_EQ(block_cache_count(block_cache), (size_t)LRU_COUNT);

  if (HasFailure()) {
    GTEST_SKIP();
  }

  /* Get all blocks — ejected ones should reload from sections */
  for (size_t i = 0; i < LRU_COUNT; i++) {
    block_t* block = block_cache_get(block_cache, blocks[i]->hash);
    EXPECT_NE(block, nullptr) << "Failed to retrieve block " << i;
    if (block != NULL) {
      EXPECT_EQ(buffer_compare(block->data, blocks[i]->data), 0)
          << "Data mismatch for block " << i;
      block_destroy(block);
    }
  }
}

TEST_F(TestBlockCacheIntegration, TestGetNonExistent) {
  block_cache = block_cache_create(config, location, type, timer_actor);

  block_t* result = block_cache_get(block_cache, blocks[0]->hash);
  EXPECT_EQ(result, nullptr);

  /* Removing non-existent block should succeed (idempotent) */
  int rc = block_cache_remove(block_cache, blocks[0]->hash);
  EXPECT_EQ(rc, 0);
}

TEST_F(TestBlockCacheIntegration, TestPutDuplicate) {
  block_cache = block_cache_create(config, location, type, timer_actor);

  /* Put same block twice — should not create duplicate index entry */
  block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[0]);
  refcounter_yield((refcounter_t*)block);
  int result = block_cache_put(block_cache, block);
  EXPECT_EQ(result, 0);

  block = (block_t*)refcounter_reference((refcounter_t*)blocks[0]);
  refcounter_yield((refcounter_t*)block);
  result = block_cache_put(block_cache, block);
  EXPECT_EQ(result, 0);

  EXPECT_EQ(block_cache_count(block_cache), 1u);
}

TEST_F(TestBlockCacheIntegration, TestRemoveIdempotent) {
  block_cache = block_cache_create(config, location, type, timer_actor);

  block_t* block = (block_t*)refcounter_reference((refcounter_t*)blocks[0]);
  refcounter_yield((refcounter_t*)block);
  int result = block_cache_put(block_cache, block);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 1u);

  /* Remove once */
  result = block_cache_remove(block_cache, blocks[0]->hash);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 0u);

  /* Remove again — should be idempotent */
  result = block_cache_remove(block_cache, blocks[0]->hash);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 0u);
}

TEST_F(TestBlockCacheIntegration, TestMiniBlocks) {
  block_cache = block_cache_create(config, location, mini, timer_actor);

  for (size_t i = 0; i < 10; i++) {
    block_t* mini_block = block_create_random_block_by_type(mini);
    block_t* put_block = (block_t*)refcounter_reference((refcounter_t*)mini_block);
    refcounter_yield((refcounter_t*)put_block);
    int result = block_cache_put(block_cache, put_block);
    EXPECT_EQ(result, 0);
    block_t* retrieved = block_cache_get(block_cache, mini_block->hash);
    EXPECT_NE(retrieved, nullptr);
    if (retrieved) {
      EXPECT_EQ(buffer_compare(retrieved->data, mini_block->data), 0);
      block_destroy(retrieved);
    }
    block_destroy(mini_block);
  }
}

