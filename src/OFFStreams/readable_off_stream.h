//
// Created by victor on 5/7/26.
//

#ifndef OFFS_READABLE_OFF_STREAM_H
#define OFFS_READABLE_OFF_STREAM_H

#include "../Streams/stream.h"
#include "tuple.h"
#include "tuple_cache.h"
#include "ori.h"
#include "../BlockCache/block_cache.h"

typedef struct network_t network_t;

typedef enum {
  OFF_STREAM_FETCHING_BLOCKS,      /* waiting for block_cache_get results */
  OFF_STREAM_AWAITING_NETWORK,     /* waiting for NETWORK_FIND_BLOCK_RESULT */
  OFF_STREAM_AWAITING_TUPLE,       /* waiting for next tuple */
} off_stream_state_e;

/* Tracks an in-progress block fetch for async decode */
typedef struct pending_block_fetch_t {
  buffer_t* hash;
  size_t index;
  struct pending_block_fetch_t* next;
} pending_block_fetch_t;

/* Queued tuple waiting to be processed */
typedef struct pending_tuple_t {
  tuple_t* tuple;
  struct pending_tuple_t* next;
} pending_tuple_t;

/* Progress payload for load_tuple_event (load mode only). Refcounted: the
 * notify machinery holds references while dispatching, so handlers should
 * copy the two counters out or take their own reference. */
typedef struct {
  refcounter_t refcounter;
  size_t tuples_loaded;
  size_t tuples_skipped;
} load_tuple_payload_t;

load_tuple_payload_t* load_tuple_payload_create(size_t tuples_loaded, size_t tuples_skipped);
void load_tuple_payload_destroy(void* payload);

typedef struct {
  stream_t stream;
  block_cache_t* bc;
  tuple_cache_t* tc;
  ori_t* ori;
  network_t* network;               /* NULL = local-only mode */
  size_t descriptor_pad;
  size_t sent_bytes;
  size_t offset_remainder;
  uint8_t offset_applied;
  uint8_t load_mode;                /* 1 = missing data tuples are skipped and tallied */
  size_t tuples_loaded;             /* tuples whose data was served / rendered */
  size_t tuples_skipped;            /* tuples abandoned because blocks were missing */
  off_stream_state_e state;         /* stream state */
  /* Async decode state */
  tuple_t* pending_tuple;
  buffer_t* xor_accumulator;
  size_t blocks_expected;
  size_t blocks_received;
  pending_block_fetch_t* pending_fetches;
  /* Hashes of an already-skipped tuple: late results matching these are
   * dropped so they cannot XOR-accumulate into the next tuple. */
  pending_block_fetch_t* stale_fetches;
  /* Queue of tuples waiting to be processed */
  pending_tuple_t* tuple_queue;
} readable_off_stream_t;

/* Load mode: missing data tuples are skipped and tallied (tuples_skipped)
 * instead of deactivating the stream; each resolved tuple (loaded or skipped)
 * emits load_tuple_event with a load_tuple_payload_t. Descriptor misses
 * remain fatal. */
readable_off_stream_t* readable_off_stream_create_ex(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network, uint8_t load_mode);

readable_off_stream_t* readable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network);
void readable_off_stream_destroy(readable_off_stream_t* stream);
void readable_off_stream_dispatch(void* state, message_t* msg);
void readable_off_stream_write(readable_off_stream_t* stream, tuple_t* tuple);

#endif //OFFS_READABLE_OFF_STREAM_H