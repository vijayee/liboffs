#include <gtest/gtest.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <vector>
#include <sys/stat.h>
extern "C" {
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/index.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block_gc.h"
#include "../src/BlockCache/sections.h"
#include "../src/Bloom/elastic_bloom_filter.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Util/allocator.h"
#include "../src/Util/base58.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/atomic_compat.h"
#include "../src/Platform/platform_time.h"
#include "../src/Buffer/buffer.h"
#include "../src/Streams/stream.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/block_recipe.h"
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/writeable_descriptor.h"
#include "../src/OFFStreams/representation_actor.h"
#include "../src/OFFStreams/off_url.h"
}

/* Keep-list GC core suite. The fixtures mirror TestEphemeralCache
   (test_ephemeral.cpp): a /tmp cache location, a 4-worker pool, one
   timer actor, and the writeable put pipeline (_test_put_ephemeral below is
   copied from there because the helper is static and cannot link across TUs).

   Coverage per the plan (linear-cuddling-fox):
   (a) direct REPRESENTATION_OP_COLLECT walks return the deduplicated set
       including the descriptor block's own hash — read-only;
   (b) default sweep keeps the kept blocks, deletes the rest, tallies match
       block_cache_count deltas + an index_to_array snapshot;
   (c) pinned / ephemeral-claimed doomed blocks are skipped with per-category
       counters, and a force sweep deletes them;
   (d) a missing descriptor block (-1) becomes a failed line, never aborts the
       run, and the partial keep set is still applied;
   (f) empty-keep refusals — both arms (no text at all, and every line failed);
   (g) the optional defrag chain propagates its summary into the result.

   (e) — the GC_LINE_CYCLE (-3) failed-line mapping has no test here: a cycle
   cannot be fabricated through the cache. The walk fetches descriptor blocks
   through block_cache_get, whose read path re-verifies BLAKE3 over the stored
   data against the stored hash, so patching a descriptor block's trailing
   next-descriptor pointer in place turns the block undreadable (read miss,
   result -1) rather than cyclic — and rewriting it as a new put changes the
   block's hash, so the descriptor hash the URL names would no longer exist.
   The -3 arm (partial set already merged below push_failure) is exercised
   through the shared machinery the moment a real cyclic descriptor appears. */

/* ---- shared plumbing (mirrors test_ephemeral.cpp) ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  int put_result;
  block_t* get_block;
  buffer_t* get_hash;
  int remove_result;
  int ephemeral_result;
  uint16_t ephemeral_previous;
  uint16_t ephemeral_new;
  int pin_result;
  uint32_t pin_previous;
  uint32_t pin_new;
} gc_bc_completion_t;

static void gc_bc_completion_dispatch(void* state, message_t* msg) {
  gc_bc_completion_t* cs = (gc_bc_completion_t*)state;
  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* r = (cache_put_result_payload_t*)msg->payload;
      cs->put_result = r->result;
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* r = (cache_get_result_payload_t*)msg->payload;
      cs->get_block = r->block;
      cs->get_hash = r->hash;
      r->block = NULL;
      r->hash = NULL;
      break;
    }
    case CACHE_REMOVE_RESULT: {
      cache_remove_result_payload_t* r = (cache_remove_result_payload_t*)msg->payload;
      cs->remove_result = r->result;
      break;
    }
    case CACHE_EPHEMERAL_RESULT: {
      cache_ephemeral_result_payload_t* r = (cache_ephemeral_result_payload_t*)msg->payload;
      cs->ephemeral_result = r->result;
      cs->ephemeral_previous = r->previous_count;
      cs->ephemeral_new = r->new_count;
      break;
    }
    case CACHE_PIN_RESULT: {
      cache_pin_result_payload_t* r = (cache_pin_result_payload_t*)msg->payload;
      cs->pin_result = r->result;
      cs->pin_previous = r->previous_count;
      cs->pin_new = r->new_count;
      break;
    }
    default:
      break;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Completion-actor wait: poll done, then barrier on pool idle before
   actor_destroy (a worker may still be in the tail of actor_run). comp is
   scheduled by actor_send when the cache actor delivers the result — do NOT
   pre-inject it with an empty queue. */
static void gc_bc_wait(gc_bc_completion_t* cs, actor_t* comp, scheduler_pool_t* pool) {
  while (!ATOMIC_LOAD(&cs->done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(comp);
}

static int gc_put_sync(block_cache_t* bc, block_t* block, scheduler_pool_t* pool) {
  gc_bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, gc_bc_completion_dispatch, pool);
  block_t* ref_block = (block_t*)refcounter_reference((refcounter_t*)block);
  refcounter_yield((refcounter_t*)ref_block);
  block_cache_put(bc, ref_block, 0, &comp);
  gc_bc_wait(&cs, &comp, pool);
  return cs.put_result;
}

static void gc_ephemeral_sync(block_cache_t* bc, buffer_t* hash, cache_ephemeral_op_e op,
                              int* result, uint16_t* previous, uint16_t* new_count,
                              scheduler_pool_t* pool) {
  gc_bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, gc_bc_completion_dispatch, pool);
  block_cache_ephemeral(bc, hash, op, &comp);
  gc_bc_wait(&cs, &comp, pool);
  *result = cs.ephemeral_result;
  *previous = cs.ephemeral_previous;
  *new_count = cs.ephemeral_new;
}

static void gc_pin_sync(block_cache_t* bc, buffer_t* hash,
                        int* result, uint32_t* previous, uint32_t* new_count,
                        scheduler_pool_t* pool) {
  gc_bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, gc_bc_completion_dispatch, pool);
  block_cache_pin(bc, hash, &comp);
  gc_bc_wait(&cs, &comp, pool);
  *result = cs.pin_result;
  *previous = cs.pin_previous;
  *new_count = cs.pin_new;
}

/* ---- keep-list line text: a parseable OFF URL over a descriptor hash ---- */

/* off_url_to_string requires file_hash AND descriptor_hash AND a file name;
   GC parsing consumes only the descriptor hash, so file_hash is a copy of the
   same buffer. The resulting text round-trips through off_url_parse the way
   the orchestrator consumes it. */
static char* gc_url_for(buffer_t* descriptor_hash, size_t stream_length) {
  off_url_t* url = off_url_create();
  url->file_hash = buffer_copy(descriptor_hash);
  url->descriptor_hash = buffer_copy(descriptor_hash);
  url->stream_length = stream_length;
  free(url->file_name);
  url->file_name = strdup("keep.bin");
  char* text = off_url_to_string(url);
  EXPECT_NE(text, nullptr) << "off_url_to_string refused a round-trippable url";
  off_url_destroy(url);
  return text;
}

/* ---- put pipeline (copied from test_ephemeral.cpp) ---- */

typedef struct {
  buffer_t* descriptor_hash;
  void* desc_handle;
  ATOMIC(uint8_t) done;
} gc_put_context_t;

static void gc_capture_descriptor_hash(void* ctx, void* data) {
  gc_put_context_t* put_ctx = (gc_put_context_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (put_ctx->descriptor_hash != NULL) buffer_destroy(put_ctx->descriptor_hash);
  put_ctx->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
}

static void gc_capture_tuple(void* ctx, void* data) {
  gc_put_context_t* put_ctx = (gc_put_context_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (payload->size == 32) {
    /* WRITEABLE_FINALIZE emits the 32-byte file hash on data_event — not a
       tuple (tuple payloads reach here as a tuple_t whose cast "size" field
       is the tuple hash count). */
    return;
  }
  tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)data);
  writeable_descriptor_write((writeable_descriptor_t*)put_ctx->desc_handle, tuple);
  tuple_destroy(tuple);
}

static void gc_rep_put_ws_close(void* ctx, void* unused) {
  (void)unused;
  gc_put_context_t* put_ctx = (gc_put_context_t*)ctx;
  writeable_descriptor_close((writeable_descriptor_t*)put_ctx->desc_handle);
}

static void gc_rep_put_close(void* ctx, void* unused) {
  (void)unused;
  ATOMIC_STORE(&((gc_put_context_t*)ctx)->done, 1);
}

/* Run one complete ephemeral put of data_size bytes and return a referenced
   descriptor hash of the finished representation (NULL when the pipeline
   failed). One tuple = two random blocks + one off block, plus one descriptor
   block — 4 entries. */
static buffer_t* _test_put_ephemeral(block_cache_t* bc, scheduler_pool_t* pool, size_t data_size) {
  gc_put_context_t put_ctx;
  memset(&put_ctx, 0, sizeof(put_ctx));
  tuple_cache_t* tc = tuple_cache_create(16, pool);
  if (tc == NULL) {
    return NULL;
  }
  writeable_descriptor_t* desc = writeable_descriptor_create(pool, bc, standard, 32, 3, data_size, NULL);
  if (desc == NULL) {
    tuple_cache_destroy(tc);
    return NULL;
  }
  writeable_descriptor_set_ephemeral(desc, 1);
  put_ctx.desc_handle = desc;
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, bc, standard);
  if (recipe == NULL) {
    stream_deferred_deref((stream_t*)desc);
    scheduler_pool_wait_for_idle(pool);
    tuple_cache_destroy(tc);
    return NULL;
  }
  vec_push(&recipes, (block_recipe_t*)recipe);
  writeable_off_stream_t* ws = writeable_off_stream_create(pool, bc, tc, standard, 3, 32, recipes, NULL);
  if (ws == NULL) {
    refcounter_dereference((refcounter_t*)recipe);
    scheduler_pool_defer_cleanup(pool, recipe, (void (*)(void*))new_blocks_recipe_destroy);
    stream_deferred_deref((stream_t*)desc);
    scheduler_pool_wait_for_idle(pool);
    tuple_cache_destroy(tc);
    return NULL;
  }
  writeable_off_stream_set_ephemeral(ws, 1);
  stream_subscribe((stream_t*)ws, data_event, &put_ctx, gc_capture_tuple, NULL);
  stream_subscribe((stream_t*)ws, close_event, &put_ctx, gc_rep_put_ws_close, NULL);
  stream_subscribe((stream_t*)desc, data_event, &put_ctx, gc_capture_descriptor_hash, NULL);
  stream_once((stream_t*)desc, close_event, &put_ctx, gc_rep_put_close, NULL);
  buffer_t* upload = buffer_create(data_size);
  upload->size = data_size;
  writeable_off_stream_write(ws, upload);
  writeable_off_stream_finalize(ws);
  buffer_destroy(upload);
  while (!ATOMIC_LOAD(&put_ctx.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  /* Release the recipe's creation reference, deferring its destructor so it
     runs LAST (the pending list is LIFO). Mirrors off_routes.c's order. */
  refcounter_dereference((refcounter_t*)recipe);
  scheduler_pool_defer_cleanup(pool, recipe, (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)ws);
  stream_deferred_deref((stream_t*)desc);
  scheduler_pool_wait_for_idle(pool);
  tuple_cache_destroy(tc);
  return put_ctx.descriptor_hash;
}

/* ---- direct COLLECT walk (test a) ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  int result;
  size_t blocks_touched;
  size_t count;
  buffer_t** hashes;  /* the transferred set — the test owns it */
} gc_collect_completion_t;

static void gc_collect_completion_dispatch(void* state, message_t* msg) {
  gc_collect_completion_t* cs = (gc_collect_completion_t*)state;
  if (msg->type == REPRESENTATION_COLLECT_RESULT) {
    representation_collect_result_payload_t* payload =
        (representation_collect_result_payload_t*)msg->payload;
    cs->result = payload->result;
    cs->blocks_touched = payload->blocks_touched;
    cs->count = payload->count;
    cs->hashes = payload->hashes;
    msg->payload = NULL;  /* consumed here; actor_run skips its destroy */
    free(payload);
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Run one collect walk and hand the transferred set back (count hashes the
   test must DESTROY + free). The producing rep actor is returned so the test
   can destroy it after asserting (see _test_rep_op in test_ephemeral.cpp). */
static representation_actor_t* gc_collect_sync(block_cache_t* bc, scheduler_pool_t* pool,
                                               buffer_t* descriptor_hash, int* out_result,
                                               size_t* out_touch, size_t* out_count,
                                               buffer_t*** out_hashes) {
  gc_collect_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, gc_collect_completion_dispatch, pool);
  representation_actor_t* rep =
      representation_actor_create(bc, NULL, descriptor_hash, REPRESENTATION_OP_COLLECT, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  *out_result = cs.result;
  *out_touch = cs.blocks_touched;
  *out_count = cs.count;
  *out_hashes = cs.hashes;
  return rep;
}

static void gcCollect_hashes_free(buffer_t** hashes, size_t count) {
  if (hashes == NULL) return;
  for (size_t idx = 0; idx < count; idx++) {
    if (hashes[idx] != NULL) {
      DESTROY(hashes[idx], buffer);
    }
  }
  free(hashes);
}

/* ---- orchestrator runner (tests b/c/d/f/g) ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  block_gc_result_t* result;  /* transferred; freed with block_gc_result_destroy */
} gc_completion_t;

static void gc_completion_dispatch(void* state, message_t* msg) {
  gc_completion_t* cs = (gc_completion_t*)state;
  if (msg->type == BLOCK_GC_RESULT) {
    cs->result = (block_gc_result_t*)msg->payload;
    msg->payload = NULL;  /* consumed here; actor_run skips its destroy */
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Run one keep-list GC to completion. The orchestrator always schedules its
   own deferred self-destroy at report time, so the test never calls
   block_gc_destroy — it only consumes the transferred result. */
static void gc_run_sync(block_cache_t* bc, scheduler_pool_t* pool, const char* text,
                        uint8_t force, uint8_t defrag, float threshold,
                        block_gc_result_t** out) {
  gc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, gc_completion_dispatch, pool);
  block_gc_create(bc, NULL, pool, text, force, defrag, threshold, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  *out = cs.result;
}

/* ---- fixture ---- */

#define GC_BLOCK_COUNT 4

/* OFFS_BC_TMP lets a caller redirect the /tmp base to a private directory per
   process (mirrors test_block_cache.cpp's bc_tmp_base). */
static const char* gc_tmp_base() {
  const char* t = getenv("OFFS_BC_TMP");
  return (t != NULL && t[0] != '\0') ? t : "/tmp";
}

class TestGcCollect : public testing::Test {
public:
  block_size_e type = standard;
  char* location;
  timer_actor_t* timer_actor;
  scheduler_pool_t* pool;
  block_cache_t* block_cache;
  block_t* blocks[GC_BLOCK_COUNT];
  config_t config;
  void SetUp() override {
    location = path_join(gc_tmp_base(), "GcCollectTest");
    rm_rf(location);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    timer_actor = timer_actor_create(pool);
    mkdir_p(location);
    block_cache = NULL;
    config = config_default();
    config.index_wait = 100;
    config.index_max_wait = 100;
    for (size_t i = 0; i < GC_BLOCK_COUNT; i++) {
      blocks[i] = block_create_random_block_by_type(type);
    }
  }
  void TearDown() override {
    scheduler_pool_wait_for_idle(pool);
    if (block_cache != NULL) {
      block_cache_sync(block_cache);
      block_cache_destroy(block_cache);
    }
    timer_actor_destroy(timer_actor);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    free(location);
    for (size_t i = 0; i < GC_BLOCK_COUNT; i++) {
      block_destroy(blocks[i]);
    }
  }
};

/* (a) A COLLECT walk returns the deduplicated keep set — the descriptor's
   data hashes PLUS the descriptor block's own hash — and issues no
   block-level op (claims and counts are untouched). */
TEST_F(TestGcCollect, CollectWalkReturnsDedupedSetIncludingDescriptor) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  ASSERT_EQ(block_cache_count(block_cache), 4u);

  int result;
  size_t touch;
  size_t count;
  buffer_t** hashes;
  representation_actor_t* rep =
      gc_collect_sync(block_cache, pool, descriptor_hash, &result, &touch, &count, &hashes);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(touch, 4u);
  EXPECT_EQ(count, 4u);
  ASSERT_NE(hashes, nullptr);

  /* The set equals the cache's 4-entry snapshot exactly, and one member is
     the descriptor block's own hash. */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  ASSERT_EQ((size_t)entries->length, 4u);
  for (int idx = 0; idx < entries->length; idx++) {
    /* The walk is read-only: A's claims are exactly where the put left them. */
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 1u) << "block " << idx;
    buffer_t* entry_hash = entries->data[idx]->hash;
    int found = 0;
    for (size_t h = 0; h < count && !found; h++) {
      if (buffer_compare(hashes[h], entry_hash) == 0) {
        found = 1;
      }
    }
    EXPECT_TRUE(found) << "entry " << idx << " missed by the walk";
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);
  int descriptor_in_set = 0;
  for (size_t h = 0; h < count; h++) {
    if (buffer_compare(hashes[h], descriptor_hash) == 0) {
      descriptor_in_set = 1;
    }
  }
  EXPECT_TRUE(descriptor_in_set) << "the descriptor block's own hash was not kept";

  gcCollect_hashes_free(hashes, count);
  representation_actor_destroy(rep);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* (b) Default sweep: the keep list spares A's 4 blocks, deletes 3 decoys, and
   the tallies match the index deltas. A whitespace-only line is dropped at
   split time — it is not a failed row and does not count in urls_request. */
TEST_F(TestGcCollect, SweepKeepsKeptBlocksAndDeletesTheRest) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  for (size_t i = 0; i < 3; i++) {
    ASSERT_EQ(gc_put_sync(block_cache, blocks[i], pool), CACHE_PUT_NEW);
  }
  ASSERT_EQ(block_cache_count(block_cache), 7u);

  char* url = gc_url_for(descriptor_hash, standard);
  ASSERT_NE(url, nullptr);
  /* url + one padding-only line the splitter must drop. */
  std::string text = std::string(url) + "\n \t \n";
  free(url);

  block_gc_result_t* out = NULL;
  gc_run_sync(block_cache, pool, text.c_str(), 0, 0, 0.5f, &out);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->status, 0);
  EXPECT_EQ(out->urls_request, 1u);      /* the whitespace line never existed */
  EXPECT_EQ(out->urls_collected, 1u);
  EXPECT_EQ(out->failed_count, 0u);
  EXPECT_EQ(out->blocks_deleted, 3u);
  EXPECT_EQ(out->blocks_kept, 4u);
  EXPECT_EQ(out->skipped_pinned, 0u);
  EXPECT_EQ(out->skipped_ephemeral_claimed, 0u);
  EXPECT_EQ(out->defrag_applied, 0);
  block_gc_result_destroy(out);

  EXPECT_EQ(block_cache_count(block_cache), 4u);
  /* The descriptor block is a keep hit — still present. */
  EXPECT_NE(index_peek(block_cache->index, descriptor_hash), (index_entry_t*)NULL);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(index_peek(block_cache->index, blocks[i]->hash), (index_entry_t*)NULL)
        << "decoy " << i << " survived the sweep";
  }
  /* Every survivor is one of A's 4 blocks — none is a decoy (count 4 +
     decoys absent pins the set down to A's blocks below, which are otherwise
     opaque to the test). */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  ASSERT_EQ((size_t)entries->length, 4u);
  for (int idx = 0; idx < entries->length; idx++) {
    for (size_t i = 0; i < 3; i++) {
      EXPECT_NE(buffer_compare(entries->data[idx]->hash, blocks[i]->hash), 0)
          << "decoy " << i << " survived as entry " << idx;
    }
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);

  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* (c) Default sweep skips a pinned decoy and an ephemeral-claimed decoy with
   per-category counters — while A's own claimed blocks are kept (the
   keep-filter check runs before the skip gates). The force sweep of the same
   keep list deletes both. */
TEST_F(TestGcCollect, SweepSkipsPinnedAndClaimedThenForceDeletesThem) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  ASSERT_EQ(block_cache_count(block_cache), 4u);
  /* A claimed+doomed decoy, a pinned+doomed decoy, a plain doomed decoy. */
  ASSERT_EQ(gc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);
  ASSERT_EQ(gc_put_sync(block_cache, blocks[1], pool), CACHE_PUT_NEW);
  ASSERT_EQ(gc_put_sync(block_cache, blocks[2], pool), CACHE_PUT_NEW);
  int ephemeral_result;
  uint16_t previous;
  uint16_t new_count;
  gc_ephemeral_sync(block_cache, blocks[0]->hash, CACHE_EPHEMERAL_ACQUIRE,
                    &ephemeral_result, &previous, &new_count, pool);
  ASSERT_EQ(ephemeral_result, CACHE_EPHEMERAL_OK);
  ASSERT_EQ(new_count, 1);
  int pin_result;
  uint32_t pin_previous;
  uint32_t pin_new;
  gc_pin_sync(block_cache, blocks[1]->hash, &pin_result, &pin_previous, &pin_new, pool);
  ASSERT_EQ(pin_result, 0);
  ASSERT_EQ(pin_new, 1u);

  char* url = gc_url_for(descriptor_hash, standard);
  ASSERT_NE(url, nullptr);

  block_gc_result_t* out = NULL;
  gc_run_sync(block_cache, pool, url, 0, 0, 0.5f, &out);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->status, 0);
  EXPECT_EQ(out->urls_request, 1u);
  EXPECT_EQ(out->urls_collected, 1u);
  EXPECT_EQ(out->failed_count, 0u);
  EXPECT_EQ(out->blocks_deleted, 1u);        /* only the plain decoy */
  EXPECT_EQ(out->blocks_kept, 4u);           /* A — claimed, but kept */
  EXPECT_EQ(out->skipped_pinned, 1u);
  EXPECT_EQ(out->skipped_ephemeral_claimed, 1u);
  block_gc_result_destroy(out);
  EXPECT_EQ(block_cache_count(block_cache), 6u);

  /* Same keep list, force: the pinned and claimed doomed blocks go. */
  gc_run_sync(block_cache, pool, url, 1, 0, 0.5f, &out);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->status, 0);
  EXPECT_EQ(out->blocks_deleted, 2u);
  EXPECT_EQ(out->blocks_kept, 4u);
  EXPECT_EQ(out->skipped_pinned, 0u);
  EXPECT_EQ(out->skipped_ephemeral_claimed, 0u);
  block_gc_result_destroy(out);
  EXPECT_EQ(block_cache_count(block_cache), 4u);
  EXPECT_EQ(index_peek(block_cache->index, blocks[0]->hash), (index_entry_t*)NULL);
  EXPECT_EQ(index_peek(block_cache->index, blocks[1]->hash), (index_entry_t*)NULL);
  EXPECT_EQ(index_peek(block_cache->index, blocks[2]->hash), (index_entry_t*)NULL);
  /* A's claimed blocks are exactly where the put left them. */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_EQ((size_t)entries->length, 4u);
  for (int idx = 0; idx < entries->length; idx++) {
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 1u) << "block " << idx;
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);

  free(url);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* (d) A keep list naming a descriptor block that is not in the cache: that
   line fails with GC_LINE_MISSING_DESCRIPTOR, the run does NOT abort, and the
   resolvable lines' keep set is still applied. */
TEST_F(TestGcCollect, FailedLineMissingDescriptorPartialKeep) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  for (size_t i = 0; i < 3; i++) {
    ASSERT_EQ(gc_put_sync(block_cache, blocks[i], pool), CACHE_PUT_NEW);
  }
  ASSERT_EQ(block_cache_count(block_cache), 7u);

  char* url = gc_url_for(descriptor_hash, standard);
  ASSERT_NE(url, nullptr);
  /* A parseable URL whose descriptor hash matches no block in the cache. */
  buffer_t* ghost = buffer_create(32);
  ASSERT_NE(ghost, nullptr);
  for (size_t idx = 0; idx < ghost->size; idx++) {
    ghost->data[idx] = (uint8_t)(0x11 * (idx + 1));
  }
  off_url_t* ghost_url = off_url_create();
  ghost_url->file_hash = buffer_copy(ghost);
  ghost_url->descriptor_hash = buffer_copy(ghost);
  ghost_url->stream_length = standard;
  free(ghost_url->file_name);
  ghost_url->file_name = strdup("ghost.bin");
  char* ghost_text = off_url_to_string(ghost_url);
  off_url_destroy(ghost_url);
  ASSERT_NE(ghost_text, nullptr);
  /* The orchestrator parses the text verbatim — sanity-check it here too. */
  off_url_t* parsed = off_url_parse(ghost_text);
  ASSERT_NE(parsed, nullptr);
  EXPECT_NE(parsed->descriptor_hash, nullptr);
  off_url_destroy(parsed);
  char* text = (char*)malloc(strlen(url) + 1 + strlen(ghost_text) + 1);
  snprintf(text, strlen(url) + 1 + strlen(ghost_text) + 1,
           "%s\n%s\n", url, ghost_text);
  const size_t ghost_line_offset = strlen(url) + 1;  /* line 2 starts here */
  free(url);
  free(ghost_text);
  buffer_destroy(ghost);

  block_gc_result_t* out = NULL;
  gc_run_sync(block_cache, pool, text, 0, 0, 0.5f, &out);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->status, 0);
  EXPECT_EQ(out->urls_request, 2u);
  EXPECT_EQ(out->urls_collected, 1u);
  ASSERT_EQ(out->failed_count, 1u);
  EXPECT_EQ(out->failed_line[0], 2u);
  EXPECT_EQ(out->failed_reason[0], GC_LINE_MISSING_DESCRIPTOR);
  EXPECT_STREQ(out->failed_lines[0], text + ghost_line_offset);
  block_gc_result_destroy(out);

  /* Partial keep still applied: only A's 4 blocks survive. */
  EXPECT_EQ(block_cache_count(block_cache), 4u);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(index_peek(block_cache->index, blocks[i]->hash), (index_entry_t*)NULL);
  }
  EXPECT_NE(index_peek(block_cache->index, descriptor_hash), (index_entry_t*)NULL);

  free(text);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* (f) Empty-keep refusals. Arm 1: no lines at all — no keep filter could be
   built. Arm 2: every line failed to resolve. Both respond status 1 with
   blocks_deleted 0 and leave the cache untouched. */
TEST_F(TestGcCollect, EmptyTextRefuses) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  ASSERT_EQ(gc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);

  block_gc_result_t* out = NULL;
  gc_run_sync(block_cache, pool, "", 0, 0, 0.5f, &out);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->status, 1);
  EXPECT_EQ(out->urls_request, 0u);
  EXPECT_EQ(out->urls_collected, 0u);
  EXPECT_EQ(out->blocks_deleted, 0u);
  EXPECT_EQ(out->failed_count, 0u);
  block_gc_result_destroy(out);
  EXPECT_EQ(block_cache_count(block_cache), 1u);

  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  rm_rf(location);
}

TEST_F(TestGcCollect, EveryLineFailedRefuses) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  ASSERT_EQ(block_cache_count(block_cache), 4u);

  std::string text = "not-an-off-url\n   \nhttp://localhost/nothing\n";
  block_gc_result_t* out = NULL;
  gc_run_sync(block_cache, pool, text.c_str(), 0, 0, 0.5f, &out);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->status, 1);
  EXPECT_EQ(out->urls_request, 2u);        /* the whitespace line is dropped */
  EXPECT_EQ(out->urls_collected, 0u);
  EXPECT_EQ(out->blocks_deleted, 0u);
  ASSERT_EQ(out->failed_count, 2u);
  EXPECT_EQ(out->failed_line[0], 1u);
  EXPECT_EQ(out->failed_reason[0], GC_LINE_MALFORMED_URL);
  EXPECT_STREQ(out->failed_lines[0], "not-an-off-url");
  EXPECT_EQ(out->failed_line[1], 2u);
  EXPECT_EQ(out->failed_reason[1], GC_LINE_MALFORMED_URL);
  EXPECT_STREQ(out->failed_lines[1], "http://localhost/nothing");
  block_gc_result_destroy(out);

  EXPECT_EQ(block_cache_count(block_cache), 4u);

  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* (g) The defrag chain: sweep first (holes left by the decoy deletions), then
   block_cache_defragment reports through the orchestrator's result. */
TEST_F(TestGcCollect, DefragChainPropagatesTheSummary) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  for (size_t i = 0; i < 3; i++) {
    ASSERT_EQ(gc_put_sync(block_cache, blocks[i], pool), CACHE_PUT_NEW);
  }
  ASSERT_EQ(block_cache_count(block_cache), 7u);

  char* url = gc_url_for(descriptor_hash, standard);
  ASSERT_NE(url, nullptr);

  block_gc_result_t* out = NULL;
  gc_run_sync(block_cache, pool, url, 0, /*defrag=*/1, /*threshold=*/0.9f, &out);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->status, 0);
  EXPECT_EQ(out->blocks_deleted, 3u);
  EXPECT_EQ(out->blocks_kept, 4u);
  EXPECT_EQ(out->defrag_applied, 1);
  EXPECT_EQ(out->defrag_result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 4u);
  block_gc_result_destroy(out);

  /* The kept blocks still resolve after the relocation pass. */
  gc_bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, gc_bc_completion_dispatch, pool);
  block_cache_get(block_cache, descriptor_hash, &comp);
  gc_bc_wait(&cs, &comp, pool);
  ASSERT_NE(cs.get_block, nullptr);
  ASSERT_NE(cs.get_hash, nullptr);
  EXPECT_EQ(buffer_compare(cs.get_hash, descriptor_hash), 0);
  DESTROY(cs.get_block, block);
  DESTROY(cs.get_hash, buffer);

  free(url);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}