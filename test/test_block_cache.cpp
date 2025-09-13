//
// Created by victor on 9/11/25.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include <cbor.h>
#include "../src/Util/allocator.h"
}


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
    EXPECT_EQ(block_lru_cache_get(lru, blocks[i]->hash) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_NE(block_lru_cache_get(lru, blocks[i]->hash) == NULL, true);
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