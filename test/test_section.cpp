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
#include "../src/Util/threadding.h"
#include <time.h>
#include <cbor.h>
#include "../src/Time/wheel.h"
#include "../src/BlockCache/section.h"
}

TEST(TestSection, TestSectionFunction) {
  size_t block_count = 20;
  block_t* blocks[block_count];
  index_entry_t* entries[block_count];
  for (size_t i = 0; i < block_count; i++) {
    blocks[i] = block_create_random_block_by_type(mini);
  }

  char *location = path_join(".", "block_index");
  work_pool_t* pool= work_pool_create(platform_core_count());
  work_pool_launch(pool);
  hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
  hierarchical_timing_wheel_run(wheel);
  uint64_t wait = 200;
  uint64_t max_wait = 5000;
  index_t* index = index_create(25, location, wheel, wait, max_wait);
  char* section_location = path_join(".", "sections");
  char* meta_location = path_join(".", "meta");
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
      index_add(index, entry);
    }
  }

  for (size_t i = 0; i < block_count; i++) {
    index_entry_t* entry =  entries[i];
    buffer_t* buf = section_read(section, entry->section_index);
    EXPECT_NE(buf, (buffer_t*) NULL);
    EXPECT_EQ(buffer_compare(buf, blocks[i]->data), 0);
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
  hierarchical_timing_wheel_wait_for_idle_signal(wheel);
  hierarchical_timing_wheel_stop(wheel);
  work_pool_shutdown(pool);
  work_pool_join_all(pool);
  work_pool_destroy(pool);
  hierarchical_timing_wheel_destroy(wheel);

  for (size_t i = 0; i < block_count; i++) {
    index_entry_destroy(entries[i]);
    block_destroy(blocks[i]);
  }
  free(location);
  free(section_location);

}