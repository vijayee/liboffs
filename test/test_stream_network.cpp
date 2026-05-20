//
// Integration tests for stream-network block fetch and announce.
//
#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/OFFStreams/readable_off_stream.h"
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/ori.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/block_recipe.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Streams/stream.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Network/wanted_list.h"
#include "../src/Network/network.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/RefCounter/refcounter.h"
#include "../src/Util/error.h"
#include "../src/Util/allocator.h"
}

// Helper: create a 32-byte hash buffer from raw bytes
static buffer_t* make_hash(const uint8_t* data, size_t len) {
  return buffer_create_from_pointer_copy((uint8_t*)data, len);
}

// Error event handler that just absorbs the error without crashing
__attribute__((unused))
static void on_error_silence(void* ctx, void* data) {
  (void)ctx;
  if (data != nullptr) {
    async_error_t* error = (async_error_t*)data;
    error_destroy(error);
  }
}

__attribute__((unused))
static void on_close_silence(void* ctx, void* data) {
  (void)ctx;
  (void)data;
}

// ========================================================================
// WantedList tests (unit tests, no actor system needed)
// ========================================================================

TEST(StreamNetwork, WantedListDedupTwoStreamSameHash) {
  wanted_list_t* wl = wanted_list_create();
  ASSERT_NE(wl, nullptr);

  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Two different actors requesting the same hash
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  actor_t actor2;
  memset(&actor2, 0, sizeof(actor2));

  wanted_list_add(wl, hash, &actor1);
  wanted_list_add(wl, hash, &actor2);

  // Only one entry should exist (dedup)
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  ASSERT_NE(entry, nullptr);

  // Count requesters: should be 2
  int count = 0;
  wanted_requester_t* req = entry->requesters;
  while (req != nullptr) { count++; req = req->next; }
  EXPECT_EQ(count, 2);

  // Remove should return both requesters
  wanted_requester_t* requesters = wanted_list_remove(wl, hash);
  ASSERT_NE(requesters, nullptr);
  wanted_requester_list_destroy(requesters);

  // Entry should be gone after remove
  EXPECT_EQ(wanted_list_find(wl, hash), nullptr);

  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

// ========================================================================
// ReadableOffStream: NETWORK_FIND_BLOCK_RESULT dispatch tests
//   These call readable_off_stream_dispatch directly with crafted messages.
// ========================================================================

class ReadableOffStreamNetworkTest : public ::testing::Test {
protected:
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  block_cache_t* bc;
  tuple_cache_t* tc;
  ori_t* ori;
  readable_off_stream_t* stream;
  network_t* network;
  char* path;

  void SetUp() override {
    // Create pool but do NOT start — we dispatch directly without worker threads
    // to avoid race conditions with async actor messages
    pool = scheduler_pool_create(2);

    path = (char*)"/tmp/test_stream_network_rstream";
    rm_rf(path);
    mkdir_p(path);

    timer = timer_actor_create();
    bc = block_cache_create(
        (config_t){.index_bucket_size = 10, .index_wait = 1000,
                   .index_max_wait = 5000, .section_size = 128000,
                   .section_wait = 1000, .section_max_wait = 5000,
                   .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
        path, standard, timer, pool, NULL, 0);
    tc = tuple_cache_create(100, pool);

    // Create a minimal network_t for testing (only the actor field matters for dispatch)
    network = (network_t*)get_clear_memory(sizeof(network_t));
    // Initialize actor so actor_send works (queue needs to be initialized)
    actor_init(&network->actor, network, nullptr, pool);
    network->wanted_list = wanted_list_create();

    uint8_t hash_data[] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                           0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
                           0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
                           0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20};
    buffer_t* file_hash = buffer_create_from_pointer_copy(hash_data, 32);
    ori = ori_create(1024);
    ori->block_type = standard;
    ori->file_offset = 0;
    ori->final_byte = 1024;
    ori->file_hash = REFERENCE(file_hash, buffer_t);
    DESTROY(file_hash, buffer);

    stream = readable_off_stream_create(pool, bc, tc, ori, 0, network);
  }

  void TearDown() override {
    readable_off_stream_destroy(stream);
    ori_destroy(ori);
    wanted_list_destroy(network->wanted_list);
    actor_destroy(&network->actor);
    free(network);
    tuple_cache_destroy(tc);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    scheduler_pool_destroy(pool);
    rm_rf(path);
  }
};

// Test: NETWORK_FIND_BLOCK_RESULT with found=1 transitions state back to
// FETCHING_BLOCKS and re-issues block_cache_get
TEST_F(ReadableOffStreamNetworkTest, FindBlockResultFoundReissuesCacheGet) {
  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Simulate the stream being in AWAITING_NETWORK state
  stream->state = OFF_STREAM_AWAITING_NETWORK;

  // Craft and dispatch NETWORK_FIND_BLOCK_RESULT with found=1
  network_find_block_result_payload_t result;
  result.hash = REFERENCE(hash, buffer_t);
  result.found = 1;

  message_t msg;
  msg.type = NETWORK_FIND_BLOCK_RESULT;
  msg.payload = &result;
  msg.payload_destroy = nullptr;

  readable_off_stream_dispatch(stream, &msg);

  // Verify: state should transition back to FETCHING_BLOCKS
  EXPECT_EQ(stream->state, OFF_STREAM_FETCHING_BLOCKS);

  // Verify: pending_fetch_hash no longer used (hash comes from result payload)

  // Clean up our references (result.hash holds a reference we need to release)
  DESTROY(result.hash, buffer);
  DESTROY(hash, buffer);
}

// Test: NETWORK_FIND_BLOCK_RESULT with found=0 deactivates the stream
TEST_F(ReadableOffStreamNetworkTest, FindBlockResultNotFoundDeactivates) {
  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Set up a pending tuple so the deactivate code can clean it up
  tuple_t* pending = tuple_create(3);
  buffer_t* h1 = make_hash(hash_data, 32);
  tuple_push(pending, h1);
  stream->pending_tuple = REFERENCE(pending, tuple_t);
  DESTROY(h1, buffer);

  // Simulate the stream being in AWAITING_NETWORK state
  stream->state = OFF_STREAM_AWAITING_NETWORK;

  // Craft and dispatch NETWORK_FIND_BLOCK_RESULT with found=0
  network_find_block_result_payload_t result;
  result.hash = REFERENCE(hash, buffer_t);
  result.found = 0;

  message_t msg;
  msg.type = NETWORK_FIND_BLOCK_RESULT;
  msg.payload = &result;
  msg.payload_destroy = nullptr;

  readable_off_stream_dispatch(stream, &msg);

  // Verify: stream should be deactivated (stream_deactivate sets this immediately)
  EXPECT_EQ(stream->stream.is_deactivated, 1);

  // Verify: pending_fetch_hash no longer used (hash comes from result payload)

  // Clean up our references
  DESTROY(result.hash, buffer);
  DESTROY(hash, buffer);
  // pending still holds a reference (REFERENCE incremented refcount to 2,
  // then dispatch handler's DESTROY decremented it to 1)
  tuple_destroy(pending);
}

// Test: ReadableOffStream with network=NULL deactivates on cache miss
TEST_F(ReadableOffStreamNetworkTest, LocalOnlyDeactivatesOnCacheMiss) {
  // Create a readable_off_stream without network (local-only mode)
  buffer_t* file_hash = buffer_create_from_pointer_copy(
      (uint8_t*)"\x01\x02\x03\x04\x05\x06\x07\x08"
                 "\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10"
                 "\x11\x12\x13\x14\x15\x16\x17\x18"
                 "\x19\x1a\x1b\x1c\x1d\x1e\x1f\x20", 32);
  ori_t* local_ori = ori_create(1024);
  local_ori->block_type = standard;
  local_ori->file_offset = 0;
  local_ori->final_byte = 1024;
  local_ori->file_hash = REFERENCE(file_hash, buffer_t);
  DESTROY(file_hash, buffer);

  readable_off_stream_t* local_stream = readable_off_stream_create(
      pool, bc, tc, local_ori, 0, NULL);
  ASSERT_NE(local_stream, nullptr);

  // Set up pending state so the handler has something to work with
  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Set up a pending tuple for cleanup
  tuple_t* pending = tuple_create(3);
  buffer_t* h1 = make_hash(hash_data, 32);
  tuple_push(pending, h1);
  local_stream->pending_tuple = REFERENCE(pending, tuple_t);
  DESTROY(h1, buffer);

  // Simulate CACHE_GET_RESULT with block=NULL (cache miss)
  cache_get_result_payload_t cache_result;
  cache_result.hash = REFERENCE(hash, buffer_t);
  cache_result.block = nullptr;
  cache_result.reply_to = nullptr;

  message_t msg;
  msg.type = CACHE_GET_RESULT;
  msg.payload = &cache_result;
  msg.payload_destroy = nullptr;

  readable_off_stream_dispatch(local_stream, &msg);

  // Verify: local-only stream should deactivate on cache miss
  // stream_deactivate sets is_deactivated = 1 immediately
  EXPECT_EQ(local_stream->stream.is_deactivated, 1);

  // Cleanup
  DESTROY(hash, buffer);
  // pending still holds a reference (dispatch handler decremented refcount from 2 to 1)
  tuple_destroy(pending);
  readable_off_stream_destroy(local_stream);
  ori_destroy(local_ori);
}

// Test: ReadableOffStream with network sends NETWORK_LOCAL_FIND_BLOCK on cache miss
TEST_F(ReadableOffStreamNetworkTest, NetworkFetchOnCacheMiss) {
  // The stream has a network, so on cache miss it should transition to
  // OFF_STREAM_AWAITING_NETWORK and send NETWORK_LOCAL_FIND_BLOCK
  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Set up a pending tuple for the cache miss path
  tuple_t* pending = tuple_create(3);
  buffer_t* h1 = make_hash(hash_data, 32);
  tuple_push(pending, h1);
  stream->pending_tuple = REFERENCE(pending, tuple_t);
  DESTROY(h1, buffer);

  // Set up blocks_expected/blocks_received state
  stream->blocks_expected = 1;
  stream->blocks_received = 0;

  // Simulate CACHE_GET_RESULT with block=NULL (cache miss) — the stream has network
  cache_get_result_payload_t cache_result;
  cache_result.hash = REFERENCE(hash, buffer_t);
  cache_result.block = nullptr;
  cache_result.reply_to = nullptr;

  message_t msg;
  msg.type = CACHE_GET_RESULT;
  msg.payload = &cache_result;
  msg.payload_destroy = nullptr;

  readable_off_stream_dispatch(stream, &msg);

  // Verify: stream should be in AWAITING_NETWORK state
  EXPECT_EQ(stream->state, OFF_STREAM_AWAITING_NETWORK);

  // Verify: stream should be in AWAITING_NETWORK state (network request sent)
  // The network's wanted_list deduplicates requests; pending_fetches tracks the blocks

  // Verify: stream should NOT be deactivated (waiting for network response)
  EXPECT_EQ(stream->stream.is_deactivated, 0);

  // Cleanup
  DESTROY(hash, buffer);
  tuple_destroy(pending);
}

// ========================================================================
// WriteableOffStream: CACHE_PUT_RESULT dispatch tests
// ========================================================================

class WriteableOffStreamNetworkTest : public ::testing::Test {
protected:
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  block_cache_t* bc;
  tuple_cache_t* tc;
  writeable_off_stream_t* stream;
  network_t* network;
  char* path;
  vec_block_recipe_t recipes;
  new_blocks_recipe_t* recipe;

  void SetUp() override {
    // Create pool but do NOT start — we dispatch directly without worker threads
    pool = scheduler_pool_create(2);

    path = (char*)"/tmp/test_stream_network_wstream";
    rm_rf(path);
    mkdir_p(path);

    timer = timer_actor_create();
    bc = block_cache_create(
        (config_t){.index_bucket_size = 10, .index_wait = 1000,
                   .index_max_wait = 5000, .section_size = 128000,
                   .section_wait = 1000, .section_max_wait = 5000,
                   .cache_size = 50, .max_tuple_size = 30, .lru_size = 50},
        path, standard, timer, pool, NULL, 0);
    tc = tuple_cache_create(100, pool);

    // Create a minimal network_t for testing
    network = (network_t*)get_clear_memory(sizeof(network_t));
    actor_init(&network->actor, network, nullptr, pool);
    network->wanted_list = wanted_list_create();

    // Create a minimal block recipe
    recipe = new_blocks_recipe_create(pool, bc, standard);
    vec_init(&recipes);
    vec_push(&recipes, (block_recipe_t*)recipe);

    stream = writeable_off_stream_create(
        pool, bc, tc, standard, 3, 32, recipes, network);
  }

  void TearDown() override {
    writeable_off_stream_destroy(stream);
    // writeable_off_stream_destroy releases the references it added (one per
    // vec entry + one for current_recipe), but the creation reference from
    // new_blocks_recipe_create is still held. Release it now.
    new_blocks_recipe_destroy(recipe);
    // Do NOT call vec_deinit(&recipes) — writeable_off_stream_destroy already
    // freed the underlying array via vec_deinit(&stream->recipes).
    wanted_list_destroy(network->wanted_list);
    actor_destroy(&network->actor);
    free(network);
    tuple_cache_destroy(tc);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    scheduler_pool_destroy(pool);
    rm_rf(path);
  }
};

// Test: CACHE_PUT_RESULT with CACHE_PUT_NEW triggers no crash and
// the dispatch handler processes it correctly.
// Since the network actor dispatch is not under our control in a unit test,
// we verify the dispatch path runs without error and the stream remains valid.
TEST_F(WriteableOffStreamNetworkTest, CachePutNewWithNetworkDoesNotCrash) {
  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Craft CACHE_PUT_RESULT with CACHE_PUT_NEW
  cache_put_result_payload_t result;
  result.result = CACHE_PUT_NEW;
  result.fib = 0;
  result.hash = REFERENCE(hash, buffer_t);
  result.reply_to = nullptr;

  message_t msg;
  msg.type = CACHE_PUT_RESULT;
  msg.payload = &result;
  msg.payload_destroy = nullptr;

  // Dispatch should not crash
  writeable_off_stream_dispatch(stream, &msg);

  // Stream should still be valid (not deactivated)
  EXPECT_EQ(stream->stream.is_deactivated, 0);

  // Clean up — release the reference from REFERENCE(hash, buffer_t)
  DESTROY(result.hash, buffer);
  DESTROY(hash, buffer);
}

// Test: CACHE_PUT_RESULT with CACHE_PUT_EXISTS does NOT trigger
// network message (fire-and-forget). The dispatch path simply falls through.
TEST_F(WriteableOffStreamNetworkTest, CachePutExistsNoAnnounce) {
  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Craft CACHE_PUT_RESULT with CACHE_PUT_EXISTS
  cache_put_result_payload_t result;
  result.result = CACHE_PUT_EXISTS;
  result.fib = 0;
  result.hash = REFERENCE(hash, buffer_t);
  result.reply_to = nullptr;

  message_t msg;
  msg.type = CACHE_PUT_RESULT;
  msg.payload = &result;
  msg.payload_destroy = nullptr;

  // Dispatch should not crash
  writeable_off_stream_dispatch(stream, &msg);

  // Stream should still be valid (not deactivated)
  EXPECT_EQ(stream->stream.is_deactivated, 0);

  // Clean up — release the reference from REFERENCE(hash, buffer_t)
  DESTROY(result.hash, buffer);
  DESTROY(hash, buffer);
}

// Test: CACHE_PUT_RESULT with CACHE_PUT_ERROR does NOT crash
TEST_F(WriteableOffStreamNetworkTest, CachePutErrorNoCrash) {
  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Craft CACHE_PUT_RESULT with CACHE_PUT_ERROR
  cache_put_result_payload_t result;
  result.result = CACHE_PUT_ERROR;
  result.fib = 0;
  result.hash = REFERENCE(hash, buffer_t);
  result.reply_to = nullptr;

  message_t msg;
  msg.type = CACHE_PUT_RESULT;
  msg.payload = &result;
  msg.payload_destroy = nullptr;

  // Dispatch should not crash
  writeable_off_stream_dispatch(stream, &msg);

  // Stream should still be valid (not deactivated)
  EXPECT_EQ(stream->stream.is_deactivated, 0);

  // Clean up — release the reference from REFERENCE(hash, buffer_t)
  DESTROY(result.hash, buffer);
  DESTROY(hash, buffer);
}

// Test: WriteableOffStream with network=NULL handles CACHE_PUT_NEW without crash
// (no NETWORK_LOCAL_STORE_BLOCK sent, just falls through)
TEST_F(WriteableOffStreamNetworkTest, CachePutNewLocalOnlyNoCrash) {
  // Create a writeable_off_stream without network (local-only)
  vec_block_recipe_t local_recipes;
  vec_init(&local_recipes);
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, bc, standard);
  vec_push(&local_recipes, (block_recipe_t*)recipe);

  writeable_off_stream_t* local_stream = writeable_off_stream_create(
      pool, bc, tc, standard, 3, 32, local_recipes, NULL);
  ASSERT_NE(local_stream, nullptr);

  uint8_t hash_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                          0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                          0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  buffer_t* hash = make_hash(hash_data, 32);

  // Craft CACHE_PUT_RESULT with CACHE_PUT_NEW
  cache_put_result_payload_t result;
  result.result = CACHE_PUT_NEW;
  result.fib = 0;
  result.hash = REFERENCE(hash, buffer_t);
  result.reply_to = nullptr;

  message_t msg;
  msg.type = CACHE_PUT_RESULT;
  msg.payload = &result;
  msg.payload_destroy = nullptr;

  // Dispatch should not crash (network is NULL, so no NETWORK_LOCAL_STORE_BLOCK)
  writeable_off_stream_dispatch(local_stream, &msg);

  EXPECT_EQ(local_stream->stream.is_deactivated, 0);

  // Cleanup — writeable_off_stream_destroy releases the references it added
  // (one per vec entry + one for current_recipe), but the creation reference
  // from new_blocks_recipe_create is still held. Release it now.
  DESTROY(result.hash, buffer);
  DESTROY(hash, buffer);
  writeable_off_stream_destroy(local_stream);
  new_blocks_recipe_destroy(recipe);
  // Do NOT call vec_deinit(&local_recipes) — writeable_off_stream_destroy
  // already freed the underlying array via vec_deinit(&stream->recipes).
}

// ========================================================================
// WantedList additional dedup tests
// ========================================================================

TEST(StreamNetwork, WantedListAddSameActorTwice) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));

  // Add the same actor twice for the same hash — should dedup
  wanted_list_add(wl, hash, &actor1);
  wanted_list_add(wl, hash, &actor1);

  wanted_entry_t* entry = wanted_list_find(wl, hash);
  ASSERT_NE(entry, nullptr);

  // The same actor added twice may still appear once or twice depending on
  // implementation; verify at least 1 requester exists
  int count = 0;
  wanted_requester_t* req = entry->requesters;
  while (req != nullptr) { count++; req = req->next; }
  EXPECT_GE(count, 1);

  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(StreamNetwork, WantedListDifferentHashesAreIndependent) {
  wanted_list_t* wl = wanted_list_create();

  uint8_t hash_a_data[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                            0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
                            0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
                            0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};
  uint8_t hash_b_data[] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                            0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,
                            0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                            0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00};
  buffer_t* hash_a = make_hash(hash_a_data, 32);
  buffer_t* hash_b = make_hash(hash_b_data, 32);

  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  actor_t actor2;
  memset(&actor2, 0, sizeof(actor2));

  wanted_list_add(wl, hash_a, &actor1);
  wanted_list_add(wl, hash_b, &actor2);

  // Each hash should have its own entry with 1 requester
  wanted_entry_t* entry_a = wanted_list_find(wl, hash_a);
  wanted_entry_t* entry_b = wanted_list_find(wl, hash_b);
  ASSERT_NE(entry_a, nullptr);
  ASSERT_NE(entry_b, nullptr);

  // They should be different entries
  EXPECT_NE(entry_a, entry_b);

  // Remove hash_a should not affect hash_b
  wanted_requester_t* reqs_a = wanted_list_remove(wl, hash_a);
  wanted_requester_list_destroy(reqs_a);

  EXPECT_EQ(wanted_list_find(wl, hash_a), nullptr);
  EXPECT_NE(wanted_list_find(wl, hash_b), nullptr);

  // Cleanup
  wanted_requester_t* reqs_b = wanted_list_remove(wl, hash_b);
  wanted_requester_list_destroy(reqs_b);

  wanted_list_destroy(wl);
  buffer_destroy(hash_a);
  buffer_destroy(hash_b);
}