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
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Network/network.h"
#include "../src/Network/wanted_list.h"
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
  uint8_t done_seen;         /* guards done_promise against extra events */
  uint8_t close_before_loads; /* set if close fired before the full tally */
  uint8_t close_seen;

  explicit LoadRecorder(size_t expected_count)
      : expected(expected_count), skip_seen(0), done_seen(0),
        close_before_loads(0), close_seen(0) {}

  void record(size_t tuples_loaded, size_t tuples_skipped) {
    pthread_mutex_lock(&mutex);
    entries.push_back(LoadEntry{tuples_loaded, tuples_skipped});
    if (tuples_skipped > 0 && !skip_seen) {
      skip_seen = 1;
      skip_promise.set_value();
    }
    if (entries.size() >= expected && !done_seen) {
      done_seen = 1;
      done_promise.set_value();
    }
    pthread_mutex_unlock(&mutex);
  }

  /* Close handler: load_mode counts a tuple before it renders, so by the time
   * close fires every load event must already have been observed. */
  void note_close() {
    pthread_mutex_lock(&mutex);
    close_seen = 1;
    if (entries.size() < expected) {
      close_before_loads = 1;
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

static void on_load_count(void* ctx, void* data) {
  (void)data;  /* load payload lifetime is owned by the notify machinery */
  (*static_cast<size_t*>(ctx))++;
}

static void on_close_note_loads(void* ctx, void* data) {
  (void)data;
  auto* recorder = static_cast<LoadRecorder*>(ctx);
  recorder->note_close();
}

/* A single 32-byte hash that exists nowhere — for building custom tuples. */
static buffer_t* make_missing_hash(uint8_t hash_seed) {
  uint8_t hash_bytes[BLAKE3_OUT_LEN];
  memset(hash_bytes, hash_seed, sizeof(hash_bytes));
  return buffer_create_from_pointer_copy(hash_bytes, BLAKE3_OUT_LEN);
}

static buffer_t* make_file_hash(void) {
  uint8_t hash_bytes[BLAKE3_OUT_LEN];
  for (size_t index = 0; index < BLAKE3_OUT_LEN; index++) {
    hash_bytes[index] = (uint8_t)(0x40 + index);
  }
  return buffer_create_from_pointer_copy(hash_bytes, BLAKE3_OUT_LEN);
}

/* No-op actor dispatch for the tests' stub network_t: a worker may pull the
 * network actor after actor_send injects it, so a valid no-op dispatch is
 * required. The dispatch must NOT call msg->payload_destroy — actor_run frees
 * the payload itself after dispatch returns. */
static void load_test_network_noop_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
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
  stream_subscribe((stream_t*)stream, close_event, &load_recorder, on_close_note_loads, NULL);

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
  /* Every tuple written resolves: loaded + skipped == tuples written. */
  EXPECT_EQ(load_recorder.entries.back().loaded + load_recorder.entries.back().skipped,
            TUPLE_SIZE);
  /* All load events were observed before close fired. */
  EXPECT_EQ(load_recorder.close_before_loads, 0);
  EXPECT_EQ(load_recorder.close_seen, 1);

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
  /* True descriptor-derived final_byte: the descriptor enumerates three
   * tuples, so the file spans three blocks. Tuple 2 is skipped — the rendered
   * bytes (2 blocks) can never reach final_byte, so the stream cannot close
   * via the render path and pipeline-driven completion is required. */
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
  stream_subscribe((stream_t*)stream, close_event, &load_recorder, on_close_note_loads, NULL);

  std::promise<void> close_promise;
  stream_subscribe((stream_t*)stream, close_event, &close_promise, on_close_set_promise, NULL);

  std::promise<void> error_promise;
  stream_subscribe((stream_t*)stream, error_event, &error_promise, on_error_set_promise, NULL);

  readable_off_stream_write(stream, first.tuple);
  readable_off_stream_write(stream, missing);
  readable_off_stream_write(stream, last.tuple);

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
  EXPECT_EQ(load_recorder.entries[2].loaded + load_recorder.entries[2].skipped, TUPLE_SIZE);
  /* The skip must not have torn the stream down on its own. */
  EXPECT_EQ(stream->stream.is_deactivated, 0);
  EXPECT_EQ(error_promise.get_future().wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout)
      << "stream must not surface an error event";

  /* Pipeline-driven completion: the tally reached the descriptor total but
   * sent_bytes (2 rendered blocks) < final_byte (3 blocks), so the render
   * path cannot close. The consumer requests the close exactly as the LOAD
   * pipelines do. */
  EXPECT_LT(stream->sent_bytes, stream->ori->final_byte);
  readable_off_stream_request_close(stream);

  auto close_future = close_promise.get_future();
  EXPECT_EQ(close_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "load stream must close once every tuple resolved, even with a "
         "skipped tuple short of final_byte";
  /* Idempotent: a second request after the close must be a no-op. */
  std::promise<void> close_promise2;
  stream_subscribe((stream_t*)stream, close_event, &close_promise2, on_close_set_promise, NULL);
  readable_off_stream_request_close(stream);
  EXPECT_EQ(close_promise2.get_future().wait_for(std::chrono::milliseconds(200)),
            std::future_status::timeout)
      << "request_close must not fire close_event twice";

  /* All load events (including the skip) were observed before close fired. */
  EXPECT_EQ(load_recorder.close_before_loads, 0);
  EXPECT_EQ(load_recorder.close_seen, 1);
  EXPECT_EQ(stream->stream.is_deactivated, 1);

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

  /* Normal mode never emits load-mode progress events. */
  size_t load_event_count = 0;
  stream_subscribe((stream_t*)stream, load_tuple_event, &load_event_count, on_load_count, NULL);

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
  EXPECT_EQ(load_event_count, 0u);

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

/* Destroy a heap-allocated NETWORK_FIND_BLOCK_RESULT payload the test built.
 * The dispatch already consumed (destroyed and nulled) the hash/block fields
 * in every branch the tests exercise; this frees whatever is left. */
static void late_network_result_destroy(void* ptr) {
  network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)ptr;
  if (result->block != NULL) {
    DESTROY(result->block, block);
  }
  if (result->hash != NULL) {
    DESTROY(result->hash, buffer);
  }
  free(result);
}

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

  /* A no-reply stub network: the skip is driven through the network's
   * found=0 path (injected by this test), so the parked hashes NEVER get a
   * real reply. That makes the stale entries persist deterministically until
   * the injected late result is dispatched — no race with real backend
   * replies draining them first. */
  network_t* network = (network_t*)get_clear_memory(sizeof(network_t));
  actor_init(&network->actor, network, load_test_network_noop_dispatch, pool);
  network->wanted_list = wanted_list_create();

  SeededTuple last = seed_tuple(block_cache, 3);
  tuple_t* missing = make_missing_tuple(0xAB);
  scheduler_pool_wait_for_idle(pool);

  buffer_t* file_hash = make_file_hash();
  /* True descriptor-derived final_byte: two tuples are enumerated (the
   * missing one is skipped), so the file spans two blocks. */
  ori_t* ori = ori_create(2 * BLOCK_SIZE);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  readable_off_stream_t* stream =
      readable_off_stream_create_ex(pool, block_cache, tuple_cache, ori, 0, network, 1);
  ASSERT_NE(stream, nullptr);

  std::vector<uint8_t> data_events;
  stream_subscribe((stream_t*)stream, data_event, &data_events, on_data_record, NULL);

  std::promise<void> error_promise;
  stream_subscribe((stream_t*)stream, error_event, &error_promise, on_error_set_promise, NULL);

  LoadRecorder load_recorder(2);
  stream_subscribe((stream_t*)stream, load_tuple_event, &load_recorder, on_load_record, NULL);
  stream_subscribe((stream_t*)stream, close_event, &load_recorder, on_close_note_loads, NULL);
  /* Capture the skip future before any writes: the promise is set once a
   * skipped tuple is observed. */
  auto skip_future = load_recorder.skip_promise.get_future();

  std::promise<void> close_promise;
  stream_subscribe((stream_t*)stream, close_event, &close_promise, on_close_set_promise, NULL);

  readable_off_stream_write(stream, missing);
  readable_off_stream_write(stream, last.tuple);
  /* Let the three block misses reach the (no-reply) network. */
  scheduler_pool_wait_for_idle(pool);

  /* Inject the skip: a found=0 NETWORK_FIND_BLOCK_RESULT for the FIRST
   * missing hash — the same reply a real network would deliver for the
   * requester whose FIND is outstanding. Per the review's finding, this
   * reply IS an answer: the fix prunes the triggering node before parking,
   * so a later tuple's fresh request for the same hash can never be
   * consumed as a late result of this skip. The other two missing hashes
   * stay parked as unanswered stale entries (their FINDs got no reply from
   * the stub network). */
  buffer_t* trigger_hash = tuple_get(missing, 0);
  network_find_block_result_payload_t* miss_result =
      (network_find_block_result_payload_t*)get_clear_memory(
          sizeof(network_find_block_result_payload_t));
  miss_result->hash = REFERENCE(trigger_hash, buffer_t);
  miss_result->found = 0;
  message_t miss_msg;
  miss_msg.type = NETWORK_FIND_BLOCK_RESULT;
  miss_msg.payload = miss_result;
  miss_msg.payload_destroy = late_network_result_destroy;
  actor_send(&stream->stream.actor, &miss_msg);

  /* Wait for the skip to be observed, then inject a LATE result for the
   * SECOND missing hash — one of the hashes that stayed PARKED as
   * unanswered stale — carrying garbage block data. If the stale-fetch
   * guard works, the result is dropped and the seeded tuple's rendered
   * bytes are unaffected. */
  EXPECT_EQ(skip_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  buffer_t* garbage_data = buffer_create(BLOCK_SIZE);
  memset(garbage_data->data, 0xEE, BLOCK_SIZE);
  buffer_t* stale_hash = tuple_get(missing, 1);
  block_t* garbage_block = block_create_existing_data_hash(garbage_data, stale_hash);
  ASSERT_NE(garbage_block, nullptr);

  network_find_block_result_payload_t* late_result =
      (network_find_block_result_payload_t*)get_clear_memory(
          sizeof(network_find_block_result_payload_t));
  late_result->hash = REFERENCE(stale_hash, buffer_t);
  late_result->found = 1;
  late_result->block = garbage_block;  /* ownership moves to the message below */
  garbage_block = NULL;
  message_t late_msg;
  late_msg.type = NETWORK_FIND_BLOCK_RESULT;
  late_msg.payload = late_result;
  late_msg.payload_destroy = late_network_result_destroy;
  actor_send(&stream->stream.actor, &late_msg);

  auto load_future = load_recorder.done_promise.get_future();
  EXPECT_EQ(load_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(error_promise.get_future().wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout)
      << "stream must not surface an error event";

  scheduler_pool_wait_for_idle(pool);

  /* The late block must never reach the live tuple's accumulator. */
  EXPECT_EQ(data_events.size(), 1u);
  EXPECT_EQ(data_events[0], 3);
  ASSERT_EQ(load_recorder.entries.size(), 2u);
  EXPECT_EQ(load_recorder.entries[0].loaded, 0u);
  EXPECT_EQ(load_recorder.entries[0].skipped, 1u);
  EXPECT_EQ(load_recorder.entries[1].loaded, 1u);
  EXPECT_EQ(load_recorder.entries[1].skipped, 1u);
  EXPECT_EQ(load_recorder.entries[1].loaded + load_recorder.entries[1].skipped, 2u);
  /* The skip must not have torn the stream down on its own. */
  EXPECT_EQ(stream->stream.is_deactivated, 0);

  /* Pipeline-driven completion: the tally reached the descriptor total but
   * the skipped tuple means sent_bytes < final_byte, so the consumer requests
   * the close exactly as the LOAD pipelines do. */
  EXPECT_LT(stream->sent_bytes, stream->ori->final_byte);
  readable_off_stream_request_close(stream);

  auto close_future = close_promise.get_future();
  EXPECT_EQ(close_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "load stream must close once every tuple resolved";
  /* All load events (including the skip) were observed before close fired. */
  EXPECT_EQ(load_recorder.close_before_loads, 0);
  EXPECT_EQ(load_recorder.close_seen, 1);
  EXPECT_EQ(stream->stream.is_deactivated, 1);

  scheduler_pool_stop(pool);

  if (garbage_block != NULL) {
    DESTROY(garbage_block, block);
  }
  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tuple_cache);
  block_cache_destroy(block_cache);
  wanted_list_destroy(network->wanted_list);
  actor_destroy(&network->actor);
  free(network);
  destroy_seeded(&last);
  DESTROY(missing, tuple);
  DESTROY(garbage_data, buffer);
  DESTROY(file_hash, buffer);
  ori_destroy(ori);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}
/* ---- load mode: a block hash shared by a skipped tuple and a live tuple ----
 * Regression for the answered-fetch prune (see _prune_answered_fetch in
 * readable_off_stream.c). Tuple A [h1,h2,h3] consumes h1's hit BEFORE h2's
 * miss skips it. Without the prune the answered h1 node stayed in
 * pending_fetches, the skip parked it in stale_fetches, and tuple B's
 * legitimate h1 reply was stale-dropped forever — B wedged at 2/3 blocks with
 * no error (silent hang). With the prune, h1 never reaches stale_fetches and
 * tuple B completes and renders correctly. */
TEST(ReadableOffLoadModeLoad, SharedHashAcrossSkipDoesNotHang) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  char* cache_path = (char*)"test_readable_load_bc_shared";
  rm_rf(cache_path);
  mkdir_p(cache_path);
  block_cache_t* block_cache = block_cache_create(
      config_t{.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0,
               .section_size = 128000, .section_wait = 0, .section_max_wait = 0,
               .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      cache_path, standard, timer, pool, NULL, 0);
  tuple_cache_t* tuple_cache = tuple_cache_create(100, pool);

  /* The shared block: its hash h1 is requested by BOTH tuples. Tuple A's
   * request is answered (and the fetch node pruned) while A is still live;
   * tuple B then re-requests the same hash after the skip. */
  buffer_t* shared_data = buffer_create(BLOCK_SIZE);
  memset(shared_data->data, 0x22, BLOCK_SIZE);
  block_t* shared_block = block_create_existing_data_by_type(shared_data, standard);
  block_t* random4 = block_create_random_block_by_type(standard);
  block_t* random5 = block_create_random_block_by_type(standard);
  ASSERT_NE(shared_block, nullptr);
  ASSERT_NE(random4, nullptr);
  ASSERT_NE(random5, nullptr);
  block_cache_put(block_cache, shared_block, 0, NULL);
  block_cache_put(block_cache, random4, 0, NULL);
  block_cache_put(block_cache, random5, 0, NULL);

  buffer_t* first_xor = buffer_xor(shared_data, random4->data);
  buffer_t* expected = buffer_xor(first_xor, random5->data);
  DESTROY(first_xor, buffer);

  buffer_t* missing2 = make_missing_hash(0x71);
  buffer_t* missing3 = make_missing_hash(0x72);
  tuple_t* tuple_a = tuple_create(TUPLE_SIZE);
  tuple_push(tuple_a, shared_block->hash);
  tuple_push(tuple_a, missing2);
  tuple_push(tuple_a, missing3);
  tuple_t* tuple_b = tuple_create(TUPLE_SIZE);
  tuple_push(tuple_b, shared_block->hash);
  tuple_push(tuple_b, random4->hash);
  tuple_push(tuple_b, random5->hash);
  scheduler_pool_wait_for_idle(pool);

  buffer_t* file_hash = make_file_hash();
  /* True descriptor-derived final_byte: two tuples are enumerated (A is
   * skipped, B is loaded), so the file spans two blocks. */
  ori_t* ori = ori_create(2 * BLOCK_SIZE);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  readable_off_stream_t* stream =
      readable_off_stream_create_ex(pool, block_cache, tuple_cache, ori, 0, NULL, 1);
  ASSERT_NE(stream, nullptr);

  std::vector<uint8_t> data_events;
  stream_subscribe((stream_t*)stream, data_event, &data_events, on_data_record, NULL);

  LoadRecorder load_recorder(2);
  stream_subscribe((stream_t*)stream, load_tuple_event, &load_recorder, on_load_record, NULL);
  stream_subscribe((stream_t*)stream, close_event, &load_recorder, on_close_note_loads, NULL);

  std::promise<void> close_promise;
  stream_subscribe((stream_t*)stream, close_event, &close_promise, on_close_set_promise, NULL);

  readable_off_stream_write(stream, tuple_a);
  readable_off_stream_write(stream, tuple_b);

  auto load_future = load_recorder.done_promise.get_future();
  EXPECT_EQ(load_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "tuple B wedged below blocks_expected — an answered fetch was parked as stale";

  scheduler_pool_wait_for_idle(pool);

  /* Tuple B completed: the shared h1 result was not dropped as stale. */
  ASSERT_EQ(data_events.size(), 1u);
  EXPECT_EQ(data_events[0], expected->data[0]);
  ASSERT_EQ(load_recorder.entries.size(), 2u);
  EXPECT_EQ(load_recorder.entries[0].loaded, 0u);
  EXPECT_EQ(load_recorder.entries[0].skipped, 1u);
  EXPECT_EQ(load_recorder.entries[1].loaded, 1u);
  EXPECT_EQ(load_recorder.entries[1].skipped, 1u);
  EXPECT_EQ(load_recorder.entries[1].loaded + load_recorder.entries[1].skipped, 2u);
  EXPECT_EQ(stream->stream.is_deactivated, 0);
  /* The skip must not leave the state dead at AWAITING_NETWORK. */
  EXPECT_EQ(stream->state, OFF_STREAM_FETCHING_BLOCKS);

  /* Pipeline-driven completion: the tally reached the descriptor total but
   * the skipped tuple means sent_bytes < final_byte, so the consumer requests
   * the close exactly as the LOAD pipelines do. */
  EXPECT_LT(stream->sent_bytes, stream->ori->final_byte);
  readable_off_stream_request_close(stream);
  std::future<void> close_future = close_promise.get_future();
  EXPECT_EQ(close_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "load stream must close once every tuple resolved";
  EXPECT_EQ(load_recorder.close_before_loads, 0);
  EXPECT_EQ(load_recorder.close_seen, 1);
  EXPECT_EQ(stream->stream.is_deactivated, 1);

  scheduler_pool_stop(pool);

  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tuple_cache);
  block_cache_destroy(block_cache);
  block_destroy(shared_block);
  block_destroy(random4);
  block_destroy(random5);
  DESTROY(tuple_a, tuple);
  DESTROY(tuple_b, tuple);
  DESTROY(missing2, buffer);
  DESTROY(missing3, buffer);
  DESTROY(shared_data, buffer);
  DESTROY(expected, buffer);
  DESTROY(file_hash, buffer);
  ori_destroy(ori);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

/* ---- load mode: the shared hash across the skip boundary is the MISSING one ---- */

TEST(ReadableOffLoadModeLoad, SharedMissingHashAcrossSkipDoesNotHang) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  char* cache_path = (char*)"test_readable_load_bc_shared_missing";
  rm_rf(cache_path);
  mkdir_p(cache_path);
  block_cache_t* block_cache = block_cache_create(
      config_t{.index_bucket_size = 10, .index_wait = 0, .index_max_wait = 0,
               .section_size = 128000, .section_wait = 0, .section_max_wait = 0,
               .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
      cache_path, standard, timer, pool, NULL, 0);
  tuple_cache_t* tuple_cache = tuple_cache_create(100, pool);

  /* The shared hash M is MISSING from the cache and is requested by BOTH
   * tuples. Tuple A = [M, a1, a2] — M is local-only (no network, load mode),
   * so A skips on M's index-miss. Tuple B = [M, b1, b2] is queued behind A
   * and re-requests M, so M's hash appears both in A's parked stale entry
   * (if unpruned) and in B's live request set. */
  block_t* a1 = block_create_random_block_by_type(standard);
  block_t* a2 = block_create_random_block_by_type(standard);
  block_t* b1 = block_create_random_block_by_type(standard);
  block_t* b2 = block_create_random_block_by_type(standard);
  ASSERT_NE(a1, nullptr);
  ASSERT_NE(a2, nullptr);
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(b2, nullptr);
  block_cache_put(block_cache, a1, 0, NULL);
  block_cache_put(block_cache, a2, 0, NULL);
  block_cache_put(block_cache, b1, 0, NULL);
  block_cache_put(block_cache, b2, 0, NULL);

  SeededTuple tuple_c = seed_tuple(block_cache, 5);
  SeededTuple tuple_d = seed_tuple(block_cache, 6);
  buffer_t* missing_shared = make_missing_hash(0x83);
  tuple_t* tuple_a = tuple_create(TUPLE_SIZE);
  tuple_push(tuple_a, missing_shared);
  tuple_push(tuple_a, a1->hash);
  tuple_push(tuple_a, a2->hash);
  tuple_t* tuple_b = tuple_create(TUPLE_SIZE);
  tuple_push(tuple_b, missing_shared);
  tuple_push(tuple_b, b1->hash);
  tuple_push(tuple_b, b2->hash);
  scheduler_pool_wait_for_idle(pool);

  buffer_t* file_hash = make_file_hash();
  /* True descriptor-derived final_byte: four tuples are enumerated (A and B
   * are skipped, C and D are loaded), so the file spans four blocks. */
  ori_t* ori = ori_create(4 * BLOCK_SIZE);
  ori->block_type = standard;
  ori->file_offset = 0;
  ori->file_hash = REFERENCE(file_hash, buffer_t);

  readable_off_stream_t* stream =
      readable_off_stream_create_ex(pool, block_cache, tuple_cache, ori, 0, NULL, 1);
  ASSERT_NE(stream, nullptr);

  std::vector<uint8_t> data_events;
  stream_subscribe((stream_t*)stream, data_event, &data_events, on_data_record, NULL);

  std::promise<void> error_promise;
  stream_subscribe((stream_t*)stream, error_event, &error_promise, on_error_set_promise, NULL);

  LoadRecorder load_recorder(4);
  stream_subscribe((stream_t*)stream, load_tuple_event, &load_recorder, on_load_record, NULL);
  stream_subscribe((stream_t*)stream, close_event, &load_recorder, on_close_note_loads, NULL);

  std::promise<void> close_promise;
  stream_subscribe((stream_t*)stream, close_event, &close_promise, on_close_set_promise, NULL);

  readable_off_stream_write(stream, tuple_a);
  readable_off_stream_write(stream, tuple_b);
  readable_off_stream_write(stream, tuple_c.tuple);
  readable_off_stream_write(stream, tuple_d.tuple);

  auto load_future = load_recorder.done_promise.get_future();
  EXPECT_EQ(load_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "tuple B wedged below blocks_expected — its own miss for the shared "
         "hash was consumed as a late result of tuple A's skip";
  EXPECT_EQ(error_promise.get_future().wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout)
      << "stream must not surface an error event";

  scheduler_pool_wait_for_idle(pool);

  /* A skipped on M's miss; B skipped on ITS OWN miss for M (not dropped as a
   * late result); the two seeded tuples loaded and rendered in order. */
  ASSERT_EQ(data_events.size(), 2u);
  EXPECT_EQ(data_events[0], 5);
  EXPECT_EQ(data_events[1], 6);
  ASSERT_EQ(load_recorder.entries.size(), 4u);
  EXPECT_EQ(load_recorder.entries[0].loaded, 0u);
  EXPECT_EQ(load_recorder.entries[0].skipped, 1u);
  EXPECT_EQ(load_recorder.entries[1].loaded, 0u);
  EXPECT_EQ(load_recorder.entries[1].skipped, 2u);
  EXPECT_EQ(load_recorder.entries[2].loaded, 1u);
  EXPECT_EQ(load_recorder.entries[2].skipped, 2u);
  EXPECT_EQ(load_recorder.entries[3].loaded, 2u);
  EXPECT_EQ(load_recorder.entries[3].skipped, 2u);
  EXPECT_EQ(load_recorder.entries[3].loaded + load_recorder.entries[3].skipped, 4u);
  EXPECT_EQ(stream->stream.is_deactivated, 0);
  /* The skips must not leave the state dead at AWAITING_NETWORK. */
  EXPECT_EQ(stream->state, OFF_STREAM_FETCHING_BLOCKS);

  /* Pipeline-driven completion: the tally reached the descriptor total but
   * the skipped tuples mean sent_bytes < final_byte, so the consumer requests
   * the close exactly as the LOAD pipelines do. */
  EXPECT_LT(stream->sent_bytes, stream->ori->final_byte);
  readable_off_stream_request_close(stream);
  std::future<void> close_future = close_promise.get_future();
  EXPECT_EQ(close_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "load stream must close once every tuple resolved";
  EXPECT_EQ(load_recorder.close_before_loads, 0);
  EXPECT_EQ(load_recorder.close_seen, 1);
  EXPECT_EQ(stream->stream.is_deactivated, 1);

  scheduler_pool_stop(pool);

  readable_off_stream_destroy(stream);
  tuple_cache_destroy(tuple_cache);
  block_cache_destroy(block_cache);
  destroy_seeded(&tuple_c);
  destroy_seeded(&tuple_d);
  block_destroy(a1);
  block_destroy(a2);
  block_destroy(b1);
  block_destroy(b2);
  DESTROY(tuple_a, tuple);
  DESTROY(tuple_b, tuple);
  DESTROY(missing_shared, buffer);
  DESTROY(file_hash, buffer);
  ori_destroy(ori);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}
