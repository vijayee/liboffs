#include <gtest/gtest.h>
extern "C" {
#include "../src/OFFStreams/readable_off_stream.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/ori.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block.h"
#include "../src/Scheduler/scheduler.h"
}

TEST(ReadableOffStream, TestCreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  block_cache_t* bc = block_cache_create((config_t){.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0, .section_size = 128000, .section_wait = 0, .section_max_wait = 0, .cache_size = 10, .max_tuple_size = 30, .lru_size = 10}, "/tmp/test_offs_stream_bc", standard, NULL, NULL, NULL, 0);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t hash_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                          0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
                          0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                          0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20};
  buffer_t* file_hash = buffer_create_from_pointer_copy(hash_data, 32);
  ori_t* ori = ori_create(1024);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->final_byte = 1024;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  readable_off_stream_t* stream = readable_off_stream_create(pool, bc, tc, ori, 0, NULL);
  ASSERT_NE(stream, nullptr);

  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  DESTROY(file_hash, buffer);
  ori_destroy(ori);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(ReadableOffStream, TestBlockSizeForType) {
  EXPECT_EQ(standard, 128000);
  EXPECT_EQ(mega, 1000000);
  EXPECT_EQ(mini, 64000);
  EXPECT_EQ(nano, 136);
}