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
  uint8_t full;
  for (size_t i = 0; i < block_count; i++) {
    blocks[i] = block_create_random_block_by_type(mini);
  }

  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 20, 4000, mini);
  for (size_t i = 0; i < block_count; i++) {
    size_t section_index = 0;
    int result = section_write(section, blocks[i]->data, &section_index, &full);
    EXPECT_EQ(result, 0);
    if (result == 0) {
      index_entry_t* entry = index_entry_create(blocks[i]->hash);
      entry->section_id = 4000;
      entry->section_index = section_index;
      entries[i] = entry;
    } else {

      section_destroy(section);
      free(meta_location);
      free(section_location);
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

  section_destroy(section);
  section = section_create(section_location, meta_location, 20, 4000, mini);

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
    int result = section_write(section, blocks[i]->data, &section_index, &full);
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

  result = section_write(section, blocks[12]->data, &entries[12]->section_index, &full);
  EXPECT_EQ(result, 0);


  platform_rw_lock_r(&section->lock);
  EXPECT_EQ(section->fragments->count, 2);
  platform_rw_unlock_r(&section->lock);

  EXPECT_EQ(entries[12]->section_index, entries[2]->section_index);

  result = section_write(section, blocks[18]->data, &entries[18]->section_index, &full);
  EXPECT_EQ(result, 0);

  EXPECT_EQ(entries[18]->section_index, entries[10]->section_index);


  section_destroy(section);
  free(meta_location);
  free(section_location);

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
  char* section_location;
  char* meta_location;
  void SetUp() override {
    section_location = path_join(".", "sections");
    meta_location = path_join(".", "meta");
    rm_rf(section_location);
    rm_rf(meta_location);
    mkdir_p(section_location);
    mkdir_p(meta_location);

    for (size_t i = 0; i < 8; i++) {
      sections[i] = section_create(section_location, meta_location, 20, id, mini);
      id++;
    }
  }
  void TearDown() override {
    free(meta_location);
    free(section_location);

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

class TestRoundRobin : public testing::Test {
public:
  char* robin_location;
  work_pool_t* pool;
  hierarchical_timing_wheel_t* wheel;
  round_robin_t* robin;
  void SetUp() override {
    robin_location = path_join(".", "robin");
    rm_rf(robin_location);
    pool = work_pool_create(platform_core_count());
    work_pool_launch(pool);
    wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    mkdir_p(robin_location);
  }
  void TearDown() override {
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    free(robin_location);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
    round_robin_destroy(robin);
  }
};

TEST_F(TestRoundRobin, TestRoundRobinFunctions) {
  robin = round_robin_create(path_join(robin_location,".robin"), wheel);
  size_t size = 6;
  for (size_t i = 0; i < size; i++) {
    round_robin_add(robin, i);
  }
  EXPECT_EQ(robin->size, size);
  for (size_t i= 0; i < (size * 10); i++) {
    EXPECT_EQ(round_robin_next(robin), i % size);
  }
  for (size_t i = 0; i < size; i++) {
    round_robin_remove(robin, i);
  }
  EXPECT_EQ(robin->size, 0);
  for (size_t i= 0; i < (size * 10); i++) {
    EXPECT_EQ(round_robin_next(robin), 0);
  }
}


class TestSections : public testing::Test {
public:
  block_size_e block_type = mini;
  block_t* blocks[25];
  index_entry_t* entries[25];
  size_t cache_size = 5;
  size_t size = 5;
  size_t max_tuple_size = 5;
  size_t id = 0;
  uint64_t wait = 5;
  uint64_t max_wait = 5000;
  char* path;
  work_pool_t* pool;
  hierarchical_timing_wheel_t* wheel;
  sections_t* sections = NULL;
  void SetUp() override {
    path = path_join(".", "sections");
    rm_rf(path);
    pool = work_pool_create(platform_core_count());
    work_pool_launch(pool);
    wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    mkdir_p(path);
    for (size_t i = 0; i < 25; i++) {
      blocks[i] = block_create_random_block_by_type(block_type);
      entries[i] = index_entry_create(blocks[i]->hash);
    }
    sections = sections_create(path, size, cache_size, max_tuple_size, block_type, wheel, wait, max_wait);
  }
  void TearDown() override {
    for (size_t i = 0; i < 25; i++) {
      block_destroy(blocks[i]);
      index_entry_destroy(entries[i]);
    }
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    free(path);
    sections_destroy(sections);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
  }
};

TEST_F(TestSections, SectionsFunctions) {
  size_t section_index;
  size_t section_id;

  for (size_t i = 0; i < 25; i++) {
    uint8_t result = sections_write(sections, blocks[i]->data, &section_id, &section_index);
    EXPECT_EQ(result, 0);
    if (result == 0) {
      entries[i]->section_id = section_id;
      entries[i]->section_index = section_index;
    } else {
      GTEST_FATAL_FAILURE_("Failed to write to sections");
    }
  }
  for (size_t i = 0; i < 25; i++) {
    buffer_t* data = sections_read(sections, entries[i]->section_id, entries[i]->section_index);
    EXPECT_EQ(data == NULL, false);
    if (data != NULL) {
      EXPECT_EQ(buffer_compare(data, blocks[i]->data) == 0, true);
      buffer_destroy(data);
    }
  }
  for (size_t i = 0; i < 25; i++) {
    int result = sections_deallocate(sections, entries[i]->section_id, entries[i]->section_index);
    EXPECT_EQ(result, 0);
  }
  for (size_t i = 0; i < 25; i++) {
    int result = sections_deallocate(sections, entries[i]->section_id, entries[i]->section_index);
    EXPECT_NE(result, 0);
  }
}
