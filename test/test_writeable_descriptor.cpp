#include <gtest/gtest.h>
extern "C" {
#include "../src/OFFStreams/writeable_descriptor.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/ori.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Scheduler/scheduler.h"
}

TEST(WriteableDescriptor, TestCreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  block_cache_t* bc = block_cache_create(
      (config_t){.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0, .section_size = 128000, .section_wait = 0, .section_max_wait = 0, .cache_size = 10, .max_tuple_size = 30, .lru_size = 10},
      (char*)"/tmp/test_wdesc_bc", standard, NULL, NULL, NULL, 0);

  writeable_descriptor_t* desc = writeable_descriptor_create(
      pool, bc, standard, 32, 3, 256000, NULL);
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->block_size, 128000u);
  EXPECT_EQ(desc->tuple_size, 3u);
  EXPECT_EQ(desc->descriptor_pad, 32u);
  EXPECT_EQ(desc->cut_point, 128000u);
  EXPECT_EQ(desc->data_length, 256000u);
  EXPECT_EQ(desc->block_count, 2u);
  EXPECT_EQ(desc->sent_descriptor, 0);

  writeable_descriptor_destroy(desc);
  block_cache_destroy(bc);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}