#include <gtest/gtest.h>
extern "C" {
#include "../src/OFFStreams/readable_descriptor.h"
#include "../src/OFFStreams/ori.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Scheduler/scheduler.h"
}

TEST(ReadableDescriptor, TestCreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  block_cache_t* bc = block_cache_create(
      (config_t){.lru_size = 10, .section_size = 128000, .cache_size = 10,
                 .max_tuple_size = 30, .index_bucket_size = 10,
                 .section_wait = 0, .section_max_wait = 0,
                 .index_wait = 0, .index_max_wait = 0},
      "/tmp/test_desc_bc", standard, NULL);

  ori_t* ori = ori_create(256000);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->tuple_size = 3;
  ori->descriptor_offset = 0;
  ori->descriptor_hash = NULL;

  readable_descriptor_t* desc = readable_descriptor_create(pool, bc, ori, 32);
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->block_size, 128000u);
  EXPECT_EQ(desc->descriptor_pad, 32u);
  EXPECT_EQ(desc->cut_point, 128000u);
  EXPECT_EQ(desc->tuple_count, 2u);
  EXPECT_EQ(desc->offset_tuple, 0u);
  EXPECT_EQ(desc->tuple_counter, 0u);
  EXPECT_EQ(desc->offset_remainder, nullptr);
  EXPECT_EQ(desc->is_readable, 1);

  readable_descriptor_destroy(desc);
  ori_destroy(ori);
  block_cache_destroy(bc);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(ReadableDescriptor, TestOffsetCalculation) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  block_cache_t* bc = block_cache_create(
      (config_t){.lru_size = 10, .section_size = 128000, .cache_size = 10,
                 .max_tuple_size = 30, .index_bucket_size = 10,
                 .section_wait = 0, .section_max_wait = 0,
                 .index_wait = 0, .index_max_wait = 0},
      "/tmp/test_desc_offset_bc", standard, NULL);

  ori_t* ori = ori_create(256000);
  ori->block_type = standard;
  ori->file_offset = 128000;
  ori->tuple_size = 3;
  ori->descriptor_offset = 0;
  ori->descriptor_hash = NULL;

  readable_descriptor_t* desc = readable_descriptor_create(pool, bc, ori, 32);
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->offset_tuple, 1u);
  EXPECT_EQ(desc->tuple_count, 2u);

  readable_descriptor_destroy(desc);
  ori_destroy(ori);
  block_cache_destroy(bc);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}