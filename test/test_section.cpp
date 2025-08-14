//
// Created by victor on 7/28/25.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/fibonacci.h"
#include "../src/BlockCache/index.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Util/threadding.h"
#include <time.h>
#include <cbor.h>
#include "../src/Time/wheel.h"
#include "../src/BlockCache/section.h"
#include "../src/BlockCache/sections.h"
#include "../src/Util/allocator.h"
}

class TestSection : public testing::Test {
public:
  char* section_location;
  char* meta_location;
  void SetUp() override {
    section_location = path_join(".", "sections");
    meta_location = path_join(".", "meta");
    rm_rf(section_location);
    rm_rf(meta_location);
  }
};


TEST_F(TestSection, TestSectionFunction) {
  size_t block_count = 20;
  block_t* blocks[block_count];
  index_entry_t* entries[block_count];
  for (size_t i = 0; i < block_count; i++) {
    blocks[i] = block_create_random_block_by_type(mini);
  }

  work_pool_t* pool= work_pool_create(platform_core_count());
  work_pool_launch(pool);
  hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
  hierarchical_timing_wheel_run(wheel);
  uint64_t wait = 5;
  uint64_t max_wait = 5000;
  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 20, 4000, wheel, wait, max_wait, mini);
  for (size_t i = 0; i < block_count; i++) {
    size_t section_index = 0;
    int result = section_write(section, blocks[i], &section_index);
    EXPECT_EQ(result, 0);
    if (result == 0) {
      index_entry_t* entry = index_entry_create(blocks[i]->hash);
      entry->section_id = 4000;
      entry->section_index = section_index;
      entries[i] = entry;
    } else {
      hierarchical_timing_wheel_wait_for_idle_signal(wheel);
      hierarchical_timing_wheel_stop(wheel);
      work_pool_shutdown(pool);
      work_pool_join_all(pool);

      section_destroy(section);
      free(meta_location);
      free(section_location);
      work_pool_destroy(pool);
      hierarchical_timing_wheel_destroy(wheel);
      for (size_t i = 0; i < block_count; i++) {
        block_destroy(blocks[i]);
      }
      GTEST_SKIP();
    }
  }

  for (size_t i = 0; i < block_count; i++) {
    index_entry_t* entry =  entries[i];
    buffer_t* buf = section_read(section, entry->section_index);
    EXPECT_NE(buf, (buffer_t*) NULL);
    EXPECT_EQ(buffer_compare(buf, blocks[i]->data), 0);
    refcounter_yield((refcounter_t*) buf);
    block_t* block = block_create_existing_data(buf);
    EXPECT_EQ(buffer_compare(block->hash, blocks[i]->hash), 0);
    EXPECT_EQ(buffer_compare(block->hash, entry->hash), 0);
    block_destroy(block);
  }

  for (size_t i = 0; i < block_count; i++) {
    index_entry_t* entry =  entries[i];
    int result = section_deallocate(section, entry->section_index);
    EXPECT_EQ(result, 0);
  }

  for (size_t i = 0; i < block_count; i++) {
    size_t section_index;
    int result = section_write(section, blocks[i], &section_index);
    EXPECT_EQ(result, 0);
  }
  int result = section_deallocate(section, entries[10]->section_index);
  EXPECT_EQ(result, 0);
  result = section_deallocate(section, entries[11]->section_index);
  EXPECT_EQ(result, 0);
  result = section_deallocate(section, entries[12]->section_index);
  EXPECT_EQ(result, 0);

  result = section_deallocate(section, entries[2]->section_index);
  EXPECT_EQ(result, 0);

  platform_rw_lock_r(&section->lock);
  EXPECT_EQ(section->fragments->count, 2);
  platform_rw_unlock_r(&section->lock);

  result = section_deallocate(section, entries[18]->section_index);
  EXPECT_EQ(result, 0);

  platform_rw_lock_r(&section->lock);
  EXPECT_EQ(section->fragments->count, 3);
  platform_rw_unlock_r(&section->lock);

  result = section_deallocate(section, entries[19]->section_index);
  EXPECT_EQ(result, 0);

  platform_rw_lock_r(&section->lock);
  EXPECT_EQ(section->fragments->count, 3);
  platform_rw_unlock_r(&section->lock);

  result = section_write(section, blocks[12], &entries[12]->section_index);
  EXPECT_EQ(result, 0);


  platform_rw_lock_r(&section->lock);
  EXPECT_EQ(section->fragments->count, 2);
  platform_rw_unlock_r(&section->lock);

  EXPECT_EQ(entries[12]->section_index, entries[2]->section_index);

  result = section_write(section, blocks[18], &entries[18]->section_index);
  EXPECT_EQ(result, 0);

  EXPECT_EQ(entries[18]->section_index, entries[10]->section_index);

  hierarchical_timing_wheel_wait_for_idle_signal(wheel);
  hierarchical_timing_wheel_stop(wheel);
  work_pool_shutdown(pool);
  work_pool_join_all(pool);
  section_destroy(section);
  free(meta_location);
  free(section_location);
  work_pool_destroy(pool);
  hierarchical_timing_wheel_destroy(wheel);

  for (size_t i = 0; i < block_count; i++) {
    index_entry_destroy(entries[i]);
    block_destroy(blocks[i]);
  }
}

class TestSectionsLRU : public testing::Test {
public:
  size_t size = 5;
  size_t overage = 3;
  section_t* sections[8];
  size_t id = 0;
  uint64_t wait = 5;
  uint64_t max_wait = 5000;
  char* section_location;
  char* meta_location;
  work_pool_t* pool;
  hierarchical_timing_wheel_t* wheel;
  void SetUp() override {
    section_location = path_join(".", "sections");
    meta_location = path_join(".", "meta");
    rm_rf(section_location);
    rm_rf(meta_location);
    pool = work_pool_create(platform_core_count());
    work_pool_launch(pool);
    wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    mkdir_p(section_location);
    mkdir_p(meta_location);

    for (size_t i = 0; i < 8; i++) {
      sections[i] = section_create(section_location, meta_location, 20, ++id, wheel, wait, max_wait, mini);
    }
  }
  void TearDown() override {
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    free(meta_location);
    free(section_location);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);

    for (size_t i = 0; i < 8; i++) {
      section_destroy(sections[i]);
    }
  }
};

TEST_F(TestSectionsLRU, TestSectionLRUInsertionDeletion) {
  sections_lru_cache_t* lru = sections_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    sections_lru_cache_put(lru, sections[i]);
  }
  for (size_t i = 0; i <  overage; i++) {
    EXPECT_EQ(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_NE(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    sections_lru_cache_delete(lru, sections[i]->id);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), false);
  }

  sections_lru_cache_destroy(lru);
}

TEST_F(TestSectionsLRU, TestSectionLRUSize1) {
  size = 1;
  sections_lru_cache_t* lru = sections_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    sections_lru_cache_put(lru, sections[i]);
  }
  for (size_t i = 0; i < overage; i++) {
    EXPECT_EQ(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_NE(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    sections_lru_cache_delete(lru, sections[i]->id);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), false);
  }

  sections_lru_cache_destroy(lru);
}

TEST_F(TestSectionsLRU, TestSectionLRUSize0) {
  size = 0;
  sections_lru_cache_t* lru = sections_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    sections_lru_cache_put(lru, sections[i]);
  }
  for (size_t i = 0; i < overage; i++) {
    EXPECT_EQ(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_NE(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    sections_lru_cache_delete(lru, sections[i]->id);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), false);
  }

  sections_lru_cache_destroy(lru);
}