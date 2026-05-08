#include <gtest/gtest.h>
#include <future>
#include <cstring>
extern "C" {
#include "../src/OFFStreams/block_recipe.h"
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/ori.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
}

static void on_block_data(void* ctx, void* data) {
  auto* blocks = static_cast<std::vector<block_t*>*>(ctx);
  block_t* block = (block_t*)refcounter_reference((refcounter_t*)data);
  blocks->push_back(block);
}

static void on_close_set_promise(void* ctx, void*) {
  auto* prom = static_cast<std::promise<void>*>(ctx);
  prom->set_value();
}

// --- NewBlocksRecipe Tests ---

TEST(NewBlocksRecipe, CreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, NULL, standard);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->recipe.block_type, standard);

  new_blocks_recipe_destroy(recipe);

  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(NewBlocksRecipe, PullCreatesBlock) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  char* path = (char*)"/tmp/test_newblocksrecipe_pull";
  rm_rf(path);
  mkdir_p(path);

  timer_actor_t* timer = timer_actor_create();
  block_cache_t* bc = block_cache_create(
      (config_t){.lru_size = 50, .section_size = 128000, .cache_size = 50,
                 .max_tuple_size = 30, .index_bucket_size = 10,
                 .section_wait = 0, .section_max_wait = 0,
                 .index_wait = 0, .index_max_wait = 0},
      path, standard, timer);

  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, bc, standard);

  std::vector<block_t*> blocks;
  stream_subscribe((stream_t*)recipe, data_event, &blocks, on_block_data, NULL);
  std::promise<void> close_promise;
  stream_subscribe((stream_t*)recipe, close_event, &close_promise, on_close_set_promise, NULL);

  new_blocks_recipe_pull(recipe);
  // Close the recipe so we get a reliable sync point
  message_t close_msg;
  close_msg.type = CLOSE_STREAM;
  close_msg.payload = NULL;
  close_msg.payload_destroy = NULL;
  actor_send(&recipe->recipe.stream.actor, &close_msg);
  scheduler_inject(pool, &recipe->recipe.stream.actor);

  auto future = close_promise.get_future();
  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

  EXPECT_GE(blocks.size(), 1u) << "Expected at least one block from pull";

  if (!blocks.empty()) {
    block_t* block = blocks[0];
    EXPECT_EQ(block->data->size, 128000u) << "Block should be standard size";
  }

  for (auto* b : blocks) {
    block_destroy(b);
  }

  scheduler_pool_stop(pool);
  new_blocks_recipe_destroy(recipe);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

TEST(NewBlocksRecipe, PullMultipleBlocks) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  char* path = (char*)"/tmp/test_newblocksrecipe_multi";
  rm_rf(path);
  mkdir_p(path);

  timer_actor_t* timer = timer_actor_create();
  block_cache_t* bc = block_cache_create(
      (config_t){.lru_size = 50, .section_size = 128000, .cache_size = 50,
                 .max_tuple_size = 30, .index_bucket_size = 10,
                 .section_wait = 0, .section_max_wait = 0,
                 .index_wait = 0, .index_max_wait = 0},
      path, standard, timer);

  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, bc, standard);

  std::vector<block_t*> blocks;
  stream_subscribe((stream_t*)recipe, data_event, &blocks, on_block_data, NULL);
  std::promise<void> close_promise;
  stream_subscribe((stream_t*)recipe, close_event, &close_promise, on_close_set_promise, NULL);

  for (int i = 0; i < 5; i++) {
    new_blocks_recipe_pull(recipe);
  }
  // Close the recipe for reliable sync
  message_t close_msg;
  close_msg.type = CLOSE_STREAM;
  close_msg.payload = NULL;
  close_msg.payload_destroy = NULL;
  actor_send(&recipe->recipe.stream.actor, &close_msg);
  scheduler_inject(pool, &recipe->recipe.stream.actor);

  auto future = close_promise.get_future();
  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

  EXPECT_EQ(blocks.size(), 5u) << "Expected 5 blocks from 5 pulls";

  for (auto* b : blocks) {
    block_destroy(b);
  }

  scheduler_pool_stop(pool);
  new_blocks_recipe_destroy(recipe);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

// --- RecyclerRecipe Tests ---

TEST(RecyclerRecipe, CreateDestroy) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  vec_ori_t oris;
  vec_init(&oris);

  recycler_recipe_t* recipe = recycler_recipe_create(pool, NULL, standard, oris);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->recipe.block_type, standard);
  EXPECT_EQ(recipe->oris.length, 0);

  recycler_recipe_destroy(recipe);

  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(RecyclerRecipe, PullFromDescriptor) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  char* path = (char*)"/tmp/test_recyclerrecipe_pull";
  rm_rf(path);
  mkdir_p(path);

  timer_actor_t* timer = timer_actor_create();
  block_cache_t* bc = block_cache_create(
      (config_t){.lru_size = 50, .section_size = 128000, .cache_size = 50,
                 .max_tuple_size = 30, .index_bucket_size = 10,
                 .section_wait = 0, .section_max_wait = 0,
                 .index_wait = 0, .index_max_wait = 0},
      path, standard, timer);

  // Create random blocks and put them in the cache
  size_t tuple_size = 3;
  size_t descriptor_pad = 32;
  block_t* random1 = block_create_random_block_by_type(standard);
  block_t* random2 = block_create_random_block_by_type(standard);
  block_t* off_block = block_create_random_block_by_type(standard);
  ASSERT_NE(random1, nullptr);
  ASSERT_NE(random2, nullptr);
  ASSERT_NE(off_block, nullptr);

  block_cache_put(bc, random1);
  block_cache_put(bc, random2);
  block_cache_put(bc, off_block);

  // Build a descriptor block containing the hashes
  // Format: [hash0][hash1][hash2] where hash0,hash1 are random (front) and hash2 is off (back)
  // RecyclerRecipe segregates: front = tuple positions 0..tuple_size-2, back = tuple_size-1
  // So for tuple_size=3: front = [random1, random2], back = [off_block]
  // Then it concatenates front + back for serving
  // Descriptor data must be full block size for block_create_existing_data_by_type
  size_t hash_region_size = tuple_size * descriptor_pad;
  buffer_t* desc_data = buffer_create(128000);
  desc_data->size = 128000;
  memset(desc_data->data, 0, 128000);
  memcpy(desc_data->data, random1->hash->data, descriptor_pad);
  memcpy(desc_data->data + descriptor_pad, random2->hash->data, descriptor_pad);
  memcpy(desc_data->data + 2 * descriptor_pad, off_block->hash->data, descriptor_pad);

  block_t* desc_block = block_create_existing_data_by_type(desc_data, standard);
  DESTROY(desc_data, buffer);
  ASSERT_NE(desc_block, nullptr);
  block_cache_put(bc, desc_block);

  // Create ORI pointing to the descriptor block
  ori_t* ori = ori_create(128000);
  ori->block_type = standard;
  ori->tuple_size = tuple_size;
  ori->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)desc_block->hash);
  ori->descriptor_offset = 0;
  ori->file_hash = NULL;
  ori->file_offset = 0;

  vec_ori_t oris;
  vec_init(&oris);
  vec_push(&oris, ori);

  recycler_recipe_t* recipe = recycler_recipe_create(pool, bc, standard, oris);

  std::vector<block_t*> blocks;
  stream_subscribe((stream_t*)recipe, data_event, &blocks, on_block_data, NULL);
  std::promise<void> close_promise;
  stream_subscribe((stream_t*)recipe, close_event, &close_promise, on_close_set_promise, NULL);

  // Pull blocks: front (random1, random2) then back (off_block) = 3 blocks total
  // But the endcap (last back hash) is saved separately and not served
  // So we should get: random1, random2 (front), and off_block is NOT the endcap here
  // Wait - for tuple_size=3, position 0,1 are front, position 2 is back
  // Only 1 tuple, so back has 1 hash = off_block. Last back hash = off_block = endcap
  // So front = [random1, random2], back (minus endcap) = []
  // Result: random1, random2 served, off_block saved as endcap
  recycler_recipe_pull(recipe);
  recycler_recipe_pull(recipe);

  // Close recipe for sync
  message_t close_msg;
  close_msg.type = CLOSE_STREAM;
  close_msg.payload = NULL;
  close_msg.payload_destroy = NULL;
  actor_send(&recipe->recipe.stream.actor, &close_msg);
  scheduler_inject(pool, &recipe->recipe.stream.actor);

  auto future = close_promise.get_future();
  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

  // Should get 2 blocks (front hashes: random1 and random2)
  EXPECT_EQ(blocks.size(), 2u) << "Expected 2 front blocks from recycler";

  // Verify the blocks match what we put in
  if (blocks.size() >= 2) {
    EXPECT_EQ(memcmp(blocks[0]->hash->data, random1->hash->data, descriptor_pad), 0);
    EXPECT_EQ(memcmp(blocks[1]->hash->data, random2->hash->data, descriptor_pad), 0);
  }

  for (auto* b : blocks) {
    block_destroy(b);
  }
  block_destroy(desc_block);
  block_destroy(off_block);
  block_destroy(random2);
  block_destroy(random1);
  ori_destroy(ori);

  scheduler_pool_stop(pool);
  recycler_recipe_destroy(recipe);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

// --- WriteableOffStream with NewBlocksRecipe integration test ---

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

TEST(BlockRecipeIntegration, WriteableOffStreamWithNewBlocksRecipe) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  char* wstream_path = (char*)"/tmp/test_recipe_integration_wstream";
  rm_rf(wstream_path);
  mkdir_p(wstream_path);

  timer_actor_t* timer = timer_actor_create();
  block_cache_t* bc = block_cache_create(
      (config_t){.lru_size = 50, .section_size = 128000, .cache_size = 50,
                 .max_tuple_size = 30, .index_bucket_size = 10,
                 .section_wait = 1000, .section_max_wait = 5000,
                 .index_wait = 1000, .index_max_wait = 5000},
      wstream_path, standard, timer);
  tuple_cache_t* tc = tuple_cache_create(100);

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