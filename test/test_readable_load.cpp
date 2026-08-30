//
// Created by victor on 8/29/26.
//
// Tests for readable_off_stream load mode: missing data tuples are skipped and
// tallied instead of deactivating the stream, and each resolved tuple (loaded
// or skipped) emits a load_tuple_event carrying load_tuple_payload_t.

#include <gtest/gtest.h>
#include <future>
#include <vector>
#include <cstring>
#include <cstdlib>
extern "C" {
#include "../src/OFFStreams/readable_off_stream.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/ori.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Util/allocator.h"
#include "../../deps/BLAKE3/c/blake3.h"
}

/* ---- recorders ---- */

struct LoadEntry {
  size_t loaded;
  size_t skipped;
};

struct LoadRecorder {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  std::vector<LoadEntry> entries;
  std::promise<void> done_promise;
  size_t expected;
  std::promise<void> skip_promise;
  uint8_t skip_seen;

  explicit LoadRecorder(size_t expected_count) : expected(expected_count), skip_seen(0) {}

  void record(size_t tuples_loaded, size_t tuples_skipped) {
    pthread_mutex_lock(&mutex);
    entries.push_back(LoadEntry{tuples_loaded, tuples_skipped});
    if (tuples_skipped > 0 && !skip_seen) {
      skip_seen = 1;
      skip_promise.set_value();
    }
    if (entries.size() >= expected) {
      done_promise.set_value();
    }
    pthread_mutex_unlock(&mutex);
  }
};

/* ---- helpers ---- */

static const size_t BLOCK_SIZE = 128000;
static const size_t TUPLE_SIZE = 3;

/* A tuple whose blocks are seeded into the block cache such that XOR of the
 * tuple's three blocks renders `expected` (filled with `fill_byte`). */
struct SeededTuple {
  tuple_t* tuple;
  block_t* blocks[TUPLE_SIZE];
  buffer_t* expected;   /* full block-size buffer, fill_byte filled */
};

static SeededTuple seed_tuple(block_cache_t* block_cache, uint8_t fill_byte) {
  SeededTuple seeded;
  seeded.expected = buffer_create(BLOCK_SIZE);
  memset(seeded.expected->data, fill_byte, BLOCK_SIZE);

  block_t* random0 = block_create_random_block_by_type(standard);
  block_t* random1 = block_create_random_block_by_type(standard);
  /* parity = expected XOR random0 XOR random1, so XOR of the three = expected */
  buffer_t* first_xor = buffer_xor(seeded.expected, random0->data);
  buffer_t* parity_data = buffer_xor(first_xor, random1->data);
  DESTROY(first_xor, buffer);
  block_t* parity = block_create_existing_data_by_type(parity_data, standard);
  DESTROY(parity_data, buffer);
  EXPECT_NE(random0, nullptr);
  EXPECT_NE(random1, nullptr);
  EXPECT_NE(parity, nullptr);
  if (random0 == NULL || random1 == NULL || parity == NULL) {
    seeded.tuple = tuple_create(0);
    seeded.blocks[0] = NULL;
    seeded.blocks[1] = NULL;
    seeded.blocks[2] = NULL;
    return seeded;
  }

  block_cache_put(block_cache, random0, 0, NULL);
  block_cache_put(block_cache, random1, 0, NULL);
  block_cache_put(block_cache, parity, 0, NULL);

  seeded.blocks[0] = random0;
  seeded.blocks[1] = random1;
  seeded.blocks[2] = parity;

  seeded.tuple = tuple_create(TUPLE_SIZE);
  tuple_push(seeded.tuple, random0->hash);
  tuple_push(seeded.tuple, random1->hash);
  tuple_push(seeded.tuple, parity->hash);
  return seeded;
}

/* A tuple with hashes that exist nowhere — every block fetch misses. */
static tuple_t* make_missing_tuple(uint8_t hash_seed) {
  tuple_t* tuple = tuple_create(TUPLE_SIZE);
  for (size_t index = 0; index < TUPLE_SIZE; index++) {
    uint8_t hash_bytes[BLAKE3_OUT_LEN];
    memset(hash_bytes, hash_seed, sizeof(hash_bytes));
    hash_bytes[0] = (uint8_t)(hash_seed + index);
    buffer_t* hash = buffer_create_from_pointer_copy(hash_bytes, BLAKE3_OUT_LEN);
    tuple_push(tuple, hash);
    DESTROY(hash, buffer);
  }
  return tuple;
}

static void destroy_seeded(SeededTuple* seeded) {
  for (size_t index = 0; index < TUPLE_SIZE; index++) {
    block_destroy(seeded->blocks[index]);
  }
  DESTROY(seeded->tuple, tuple);
  DESTROY(seeded->expected, buffer);
}

static void on_close_set_promise(void* ctx, void* data) {
  (void)data;
  auto* promise = static_cast<std::promise<void>*>(ctx);
  promise->set_value();
}

static void on_error_set_promise(void* ctx, void* data) {
  (void)data;  /* error payload is owned by the notify wrapper */
  auto* promise = static_cast<std::promise<void>*>(ctx);
  promise->set_value();
}

static void on_data_record(void* ctx, void* data) {
  auto* recorder = static_cast<std::vector<uint8_t>*>(ctx);
  buffer_t* payload = (buffer_t*)data;
  recorder->push_back(payload->size > 0 ? payload->data[0] : 0);
}

static void on_load_record(void* ctx, void* data) {
  auto* recorder = static_cast<LoadRecorder*>(ctx);
  load_tuple_payload_t* payload = (load_tuple_payload_t*)data;
  if (payload != NULL) {
    recorder->record(payload->tuples_loaded, payload->tuples_skipped);
  }
}

static buffer_t* make_file_hash(void) {
  uint8_t hash_bytes[BLAKE3_OUT_LEN];
  for (size_t index = 0; index < BLAKE3_OUT_LEN; index++) {
    hash_bytes[index] = (uint8_t)(0x40 + index);
  }
  return buffer_create_from_pointer_copy(hash_bytes, BLAKE3_OUT_LEN);
}

/* Destroy the message payload the test built for a late CACHE_GET_RESULT. */
static void late_result_destroy(void* ptr) {
  cache_get_result_payload_t* result = (cache_get_result_payload_t*)ptr;
  if (result->block != NULL) {
    DESTROY(result->block, block);
  }
  if (result->hash != NULL) {
    DESTROY(result->hash, buffer);
  }
  free(result);
}

/* ---- load mode: counts and emits ---- */

TEST(ReadableOffLoadModeLoad, CountsAndEmits) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  char* cache_path = (char*)"test_readable_load_bc_counts";
  rm_rf(cache_path);
  mkdir_p(cache_path);
  block_cache_t* block_cache = block_cache_create(
      config_t{.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0,
               .section_size = 128000, .section_wait = 0, .section_max_wait = 0,
               .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      cache_path, standard, timer, pool, NULL, 0);
  tuple_cache_t* tuple_cache = tuple_cache_create(100, pool);

  SeededTuple seeded[TUPLE_SIZE];
  for (size_t index = 0; index < TUPLE_SIZE; index++) {
    seeded[index] = seed_tuple(block_cache, (uint8_t)(index + 1));
  }
  /* Let the async puts land before driving the stream. */
  scheduler_pool_wait_for_idle(pool);

  buffer_t* file_hash = make_file_hash();
  ori_t* ori = ori_create(TUPLE_SIZE * BLOCK_SIZE);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  readable_off_stream_t* stream =
      readable_off_stream_create_ex(pool, block_cache, tuple_cache, ori, 0, NULL, 1);
  ASSERT_NE(stream, nullptr);

  std::vector<uint8_t> data_events;
  stream_subscribe((stream_t*)stream, data_event, &data_events, on_data_record, NULL);

  LoadRecorder load_recorder(TUPLE_SIZE);
  stream_subscribe((stream_t*)stream, load_tuple_event, &load_recorder, on_load_record, NULL);

  std::promise<void> close_promise;
  stream_subscribe((stream_t*)stream, close_event, &close_promise, on_close_set_promise, NULL);

  for (size_t index = 0; index < TUPLE_SIZE; index++) {
    readable_off_stream_write(stream, seeded[index].tuple);
  }

  auto close_future = close_promise.get_future();
  EXPECT_EQ(close_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto load_future = load_recorder.done_promise.get_future();
  EXPECT_EQ(load_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  scheduler_pool_wait_for_idle(pool);

  EXPECT_EQ(data_events.size(), TUPLE_SIZE);
  for (size_t index = 0; index < TUPLE_SIZE; index++) {
    EXPECT_EQ(data_events[index], index + 1);
  }
  ASSERT_EQ(load_recorder.entries.size(), TUPLE_SIZE);
  for (size_t index = 0; index < TUPLE_SIZE; index++) {
    EXPECT_EQ(load_recorder.entries[index].loaded, index + 1);
    EXPECT_EQ(load_recorder.entries[index].skipped, 0u);
  }

  scheduler_pool_stop(pool);

  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tuple_cache);
  block_cache_destroy(block_cache);
  for (size_t index = 0; index < TUPLE_SIZE; index++) {
    destroy_seeded(&seeded[index]);
  }
  DESTROY(file_hash, buffer);
  ori_destroy(ori);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

/* ---- load mode: missing tuple is skipped, stream continues ---- */

TEST(ReadableOffLoadModeLoad, SkipMissingTupleContinues) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  char* cache_path = (char*)"test_readable_load_bc_skip";
  rm_rf(cache_path);
  mkdir_p(cache_path);
  block_cache_t* block_cache = block_cache_create(
      config_t{.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0,
               .section_size = 128000, .section_wait = 0, .section_max_wait = 0,
               .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      cache_path, standard, timer, pool, NULL, 0);
  tuple_cache_t* tuple_cache = tuple_cache_create(100, pool);

  SeededTuple first = seed_tuple(block_cache, 1);
  SeededTuple last = seed_tuple(block_cache, 3);
  tuple_t* missing = make_missing_tuple(0xAB);  /* blocks exist nowhere */
  scheduler_pool_wait_for_idle(pool);

  buffer_t* file_hash = make_file_hash();
  ori_t* ori = ori_create(2 * BLOCK_SIZE);  /* only tuples 1 and 3 are served */
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  readable_off_stream_t* stream =
      readable_off_stream_create_ex(pool, block_cache, tuple_cache, ori, 0, NULL, 1);
  ASSERT_NE(stream, nullptr);

  std::vector<uint8_t> data_events;
  stream_subscribe((stream_t*)stream, data_event, &data_events, on_data_record, NULL);

  LoadRecorder load_recorder(TUPLE_SIZE);
  stream_subscribe((stream_t*)stream, load_tuple_event, &load_recorder, on_load_record, NULL);

  std::promise<void> close_promise;
  stream_subscribe((stream_t*)stream, close_event, &close_promise, on_close_set_promise, NULL);

  readable_off_stream_write(stream, first.tuple);
  readable_off_stream_write(stream, missing);
  readable_off_stream_write(stream, last.tuple);

  auto close_future = close_promise.get_future();
  EXPECT_EQ(close_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto load_future = load_recorder.done_promise.get_future();
  EXPECT_EQ(load_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  scheduler_pool_wait_for_idle(pool);

  /* Stream kept going: two tuples rendered, one skipped, no deactivation. */
  ASSERT_EQ(data_events.size(), 2u);
  EXPECT_EQ(data_events[0], 1);
  EXPECT_EQ(data_events[1], 3);
  ASSERT_EQ(load_recorder.entries.size(), TUPLE_SIZE);
  EXPECT_EQ(load_recorder.entries[0].loaded, 1u);
  EXPECT_EQ(load_recorder.entries[0].skipped, 0u);
  EXPECT_EQ(load_recorder.entries[1].loaded, 1u);
  EXPECT_EQ(load_recorder.entries[1].skipped, 1u);
  EXPECT_EQ(load_recorder.entries[2].loaded, 2u);
  EXPECT_EQ(load_recorder.entries[2].skipped, 1u);
  EXPECT_EQ(stream->stream.is_deactivated, 0);

  scheduler_pool_stop(pool);

  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tuple_cache);
  block_cache_destroy(block_cache);
  destroy_seeded(&first);
  destroy_seeded(&last);
  DESTROY(missing, tuple);
  DESTROY(file_hash, buffer);
  ori_destroy(ori);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

/* ---- normal mode: a miss still tears the stream down ---- */

TEST(ReadableOffNormalMode, MissStillDeactivates) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  char* cache_path = (char*)"test_readable_load_bc_normal";
  rm_rf(cache_path);
  mkdir_p(cache_path);
  block_cache_t* block_cache = block_cache_create(
      config_t{.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0,
               .section_size = 128000, .section_wait = 0, .section_max_wait = 0,
               .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      cache_path, standard, timer, pool, NULL, 0);
  tuple_cache_t* tuple_cache = tuple_cache_create(100, pool);

  SeededTuple first = seed_tuple(block_cache, 1);
  tuple_t* missing = make_missing_tuple(0xCD);
  scheduler_pool_wait_for_idle(pool);

  buffer_t* file_hash = make_file_hash();
  ori_t* ori = ori_create(2 * BLOCK_SIZE);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  /* Default constructor — load_mode 0, existing behavior. */
  readable_off_stream_t* stream =
      readable_off_stream_create(pool, block_cache, tuple_cache, ori, 0, NULL);
  ASSERT_NE(stream, nullptr);

  std::vector<uint8_t> data_events;
  stream_subscribe((stream_t*)stream, data_event, &data_events, on_data_record, NULL);

  std::promise<void> error_promise;
  stream_subscribe((stream_t*)stream, error_event, &error_promise, on_error_set_promise, NULL);

  readable_off_stream_write(stream, first.tuple);
  readable_off_stream_write(stream, missing);

  auto error_future = error_promise.get_future();
  EXPECT_EQ(error_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  scheduler_pool_wait_for_idle(pool);

  EXPECT_EQ(stream->stream.is_deactivated, 1);
  EXPECT_EQ(data_events.size(), 1u);
  EXPECT_EQ(data_events[0], 1);

  scheduler_pool_stop(pool);

  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tuple_cache);
  block_cache_destroy(block_cache);
  destroy_seeded(&first);
  DESTROY(missing, tuple);
  DESTROY(file_hash, buffer);
  ori_destroy(ori);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

/* ---- load mode: a late result for a skipped tuple must not pollute the next tuple ---- */

TEST(ReadableOffLoadModeLoad, SkippedTupleLateResultDropped) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  char* cache_path = (char*)"test_readable_load_bc_late";
  rm_rf(cache_path);
  mkdir_p(cache_path);
  block_cache_t* block_cache = block_cache_create(
      config_t{.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0,
               .section_size = 128000, .section_wait = 0, .section_max_wait = 0,
               .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      cache_path, standard, timer, pool, NULL, 0);
  tuple_cache_t* tuple_cache = tuple_cache_create(100, pool);

  SeededTuple first = seed_tuple(block_cache, 1);
  SeededTuple last = seed_tuple(block_cache, 3);
  tuple_t* missing = make_missing_tuple(0xAB);
  scheduler_pool_wait_for_idle(pool);

  buffer_t* file_hash = make_file_hash();
  ori_t* ori = ori_create(2 * BLOCK_SIZE);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  readable_off_stream_t* stream =
      readable_off_stream_create_ex(pool, block_cache, tuple_cache, ori, 0, NULL, 1);
  ASSERT_NE(stream, nullptr);

  std::vector<uint8_t> data_events;
  stream_subscribe((stream_t*)stream, data_event, &data_events, on_data_record, NULL);

  LoadRecorder load_recorder(TUPLE_SIZE);
  stream_subscribe((stream_t*)stream, load_tuple_event, &load_recorder, on_load_record, NULL);
  /* Capture the skip future before any writes: the promise is set once a
   * skipped tuple is observed. */
  auto skip_future = load_recorder.skip_promise.get_future();

  std::promise<void> close_promise;
  stream_subscribe((stream_t*)stream, close_event, &close_promise, on_close_set_promise, NULL);

  readable_off_stream_write(stream, first.tuple);
  readable_off_stream_write(stream, missing);
  readable_off_stream_write(stream, last.tuple);

  /* Wait for the skip to be observed, then inject a late CACHE_GET_RESULT for
   * one of the skipped tuple's hashes carrying garbage block data. If the
   * stale-fetch guard works, the result is dropped and the last tuple's
   * rendered bytes are unaffected. */
  EXPECT_EQ(skip_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  buffer_t* garbage_data = buffer_create(BLOCK_SIZE);
  memset(garbage_data->data, 0xEE, BLOCK_SIZE);
  buffer_t* stale_hash = tuple_get(missing, 0);
  block_t* garbage_block = block_create_existing_data_hash(garbage_data, stale_hash);
  ASSERT_NE(garbage_block, nullptr);

  cache_get_result_payload_t* late_result =
      (cache_get_result_payload_t*)get_clear_memory(sizeof(cache_get_result_payload_t));
  late_result->hash = REFERENCE(stale_hash, buffer_t);
  late_result->block = garbage_block;  /* ownership moves to the message below */
  garbage_block = NULL;
  message_t late_msg;
  late_msg.type = CACHE_GET_RESULT;
  late_msg.payload = late_result;
  late_msg.payload_destroy = late_result_destroy;
  actor_send(&stream->stream.actor, &late_msg);

  auto close_future = close_promise.get_future();
  EXPECT_EQ(close_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto load_future = load_recorder.done_promise.get_future();
  EXPECT_EQ(load_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  scheduler_pool_wait_for_idle(pool);

  /* The late block must never reach the live tuple's accumulator. */
  EXPECT_EQ(data_events.size(), 2u);
  EXPECT_EQ(data_events[0], 1);
  EXPECT_EQ(data_events[1], 3);
  ASSERT_EQ(load_recorder.entries.size(), TUPLE_SIZE);
  EXPECT_EQ(load_recorder.entries[0].loaded, 1u);
  EXPECT_EQ(load_recorder.entries[0].skipped, 0u);
  EXPECT_EQ(load_recorder.entries[1].loaded, 1u);
  EXPECT_EQ(load_recorder.entries[1].skipped, 1u);
  EXPECT_EQ(load_recorder.entries[2].loaded, 2u);
  EXPECT_EQ(load_recorder.entries[2].skipped, 1u);

  scheduler_pool_stop(pool);

  if (garbage_block != NULL) {
    DESTROY(garbage_block, block);
  }
  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tuple_cache);
  block_cache_destroy(block_cache);
  destroy_seeded(&first);
  destroy_seeded(&last);
  DESTROY(missing, tuple);
  DESTROY(garbage_data, buffer);
  DESTROY(file_hash, buffer);
  ori_destroy(ori);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}