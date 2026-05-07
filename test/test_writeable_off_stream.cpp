#include <gtest/gtest.h>
extern "C" {
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/ori.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block.h"
#include "../src/Scheduler/scheduler.h"
}

TEST(WriteableOffStream, TestCreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  block_cache_t* bc = block_cache_create(
      (config_t){.lru_size = 10, .section_size = 128000, .cache_size = 10,
                 .max_tuple_size = 30, .index_bucket_size = 10,
                 .section_wait = 0, .section_max_wait = 0,
                 .index_wait = 0, .index_max_wait = 0},
      "/tmp/test_offs_wstream_bc", standard, NULL);
  tuple_cache_t* tc = tuple_cache_create(100);

  writeable_off_stream_t* stream = writeable_off_stream_create(
      pool, bc, tc, standard, 3, 32);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->block_size, 128000u);
  EXPECT_EQ(stream->tuple_size, 3u);

  writeable_off_stream_destroy(stream);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}