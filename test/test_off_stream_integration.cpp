#include <gtest/gtest.h>
#include <future>
#include <vector>
#include <cstring>
extern "C" {
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/readable_off_stream.h"
#include "../src/OFFStreams/writeable_descriptor.h"
#include "../src/OFFStreams/readable_descriptor.h"
#include "../src/OFFStreams/block_recipe.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/ori.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Streams/stream.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
}

static void on_close_set_promise(void* ctx, void*) {
  auto* prom = static_cast<std::promise<void>*>(ctx);
  prom->set_value();
}

struct TupleCollector {
  std::vector<tuple_t*> tuples;
  buffer_t* file_hash;
  std::promise<void> close_promise;

  TupleCollector() : file_hash(NULL) {}
};

static void on_tuple_data(void* ctx, void* data) {
  auto* collector = static_cast<TupleCollector*>(ctx);
  buffer_t* payload = (buffer_t*)data;
  if (payload->size == 32 && collector->file_hash == NULL) {
    collector->file_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
  } else {
    tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)payload);
    collector->tuples.push_back(tuple);
  }
}

static void on_close_collector(void* ctx, void*) {
  auto* collector = static_cast<TupleCollector*>(ctx);
  collector->close_promise.set_value();
}

struct BufferCollector {
  std::vector<buffer_t*> buffers;
  std::promise<void> close_promise;
};

static void on_buffer_data(void* ctx, void* data) {
  auto* collector = static_cast<BufferCollector*>(ctx);
  buffer_t* payload = (buffer_t*)refcounter_reference((refcounter_t*)data);
  collector->buffers.push_back(payload);
}

static void on_close_buffer(void* ctx, void*) {
  auto* collector = static_cast<BufferCollector*>(ctx);
  collector->close_promise.set_value();
}

TEST(OffStreamIntegration, WriteableOffStreamEncodesData) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  char* wstream_path = (char*)"/tmp/test_integration_wstream";
  rm_rf(wstream_path);
  mkdir_p(wstream_path);

  timer_actor_t* timer = timer_actor_create();
  block_cache_t* bc = block_cache_create(
      (config_t){.index_bucket_size = 10, .index_wait = 1000, .index_max_wait = 5000, .section_size = 128000, .section_wait = 1000, .section_max_wait = 5000, .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      wstream_path, standard, timer, pool);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, bc, standard);
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  vec_push(&recipes, (block_recipe_t*)recipe);

  writeable_off_stream_t* stream = writeable_off_stream_create(
      pool, bc, tc, standard, 3, 32, recipes);

  TupleCollector collector;
  stream_subscribe((stream_t*)stream, data_event, &collector,
                   on_tuple_data, NULL);
  stream_subscribe((stream_t*)stream, close_event, &collector,
                   on_close_collector, NULL);

  uint8_t data[128000];
  memset(data, 0xAB, sizeof(data));
  buffer_t* buf = buffer_create_from_pointer_copy(data, 128000);
  writeable_off_stream_write(stream, buf);
  DESTROY(buf, buffer);

  writeable_off_stream_finalize(stream);

  auto future = collector.close_promise.get_future();
  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

  EXPECT_GE(collector.tuples.size(), 1u)
      << "Expected at least one tuple from encoding";

  for (auto* t : collector.tuples) {
    tuple_destroy(t);
  }
  if (collector.file_hash != NULL) {
    DESTROY(collector.file_hash, buffer);
  }

  scheduler_pool_stop(pool);
  new_blocks_recipe_destroy(recipe);
  writeable_off_stream_destroy(stream);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

TEST(OffStreamIntegration, ReadableOffStreamDecodesBlock) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  char* rstream_path = (char*)"/tmp/test_integration_rstream";
  rm_rf(rstream_path);
  mkdir_p(rstream_path);

  timer_actor_t* timer = timer_actor_create();
  block_cache_t* bc = block_cache_create(
      (config_t){.index_bucket_size = 10, .index_wait = 1000, .index_max_wait = 5000, .section_size = 128000, .section_wait = 1000, .section_max_wait = 5000, .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      rstream_path, standard, timer, pool);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t origin_data[128000];
  memset(origin_data, 0xCD, sizeof(origin_data));

  buffer_t* origin_buf = buffer_create_from_pointer_copy(origin_data, 128000);
  block_t* origin_block = block_create_existing_data_by_type(origin_buf, standard);
  DESTROY(origin_buf, buffer);
  ASSERT_NE(origin_block, nullptr);

  block_t* random1 = block_create_random_block_by_type(standard);
  block_t* random2 = block_create_random_block_by_type(standard);
  ASSERT_NE(random1, nullptr);
  ASSERT_NE(random2, nullptr);

  buffer_t* off_data = buffer_copy(origin_block->data);
  buffer_t* xored1 = buffer_xor(off_data, random1->data);
  DESTROY(off_data, buffer);
  buffer_t* xored2 = buffer_xor(xored1, random2->data);
  DESTROY(xored1, buffer);
  block_t* off_block = block_create_existing_data_by_type(xored2, standard);
  DESTROY(xored2, buffer);
  ASSERT_NE(off_block, nullptr);

  block_cache_put_async(bc, origin_block, NULL);
  block_cache_put_async(bc, random1, NULL);
  block_cache_put_async(bc, random2, NULL);
  block_cache_put_async(bc, off_block, NULL);

  ori_t* ori = ori_create(128000);
  ori->block_type = standard;
  ori->tuple_size = 3;
  ori->file_hash = NULL;
  ori->file_offset = 0;
  ori->descriptor_hash = NULL;

  readable_off_stream_t* stream = readable_off_stream_create(
      pool, bc, tc, ori, 32);

  BufferCollector collector;
  stream_subscribe((stream_t*)stream, data_event, &collector,
                   on_buffer_data, NULL);
  stream_subscribe((stream_t*)stream, close_event, &collector,
                   on_close_buffer, NULL);

  tuple_t* tuple = tuple_create(3);
  tuple_push(tuple, random1->hash);
  tuple_push(tuple, random2->hash);
  tuple_push(tuple, off_block->hash);

  readable_off_stream_write(stream, tuple);

  scheduler_pool_wait_for_idle(pool);

  auto future = collector.close_promise.get_future();
  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

  EXPECT_GE(collector.buffers.size(), 1u)
      << "Expected at least one decoded buffer";

  if (!collector.buffers.empty()) {
    buffer_t* decoded = collector.buffers[0];
    EXPECT_EQ(decoded->size, 128000u);
    uint8_t expected[128000];
    memset(expected, 0xCD, sizeof(expected));
    EXPECT_EQ(memcmp(decoded->data, expected, 128000), 0)
        << "Decoded data should match original";
  }

  for (auto* b : collector.buffers) {
    DESTROY(b, buffer);
  }
  tuple_destroy(tuple);
  block_destroy(off_block);
  block_destroy(random2);
  block_destroy(random1);
  block_destroy(origin_block);
  readable_off_stream_destroy(stream);
  ori_destroy(ori);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);

  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}