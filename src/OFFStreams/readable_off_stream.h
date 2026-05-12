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

/* Tracks an in-progress block fetch for async decode */
typedef struct pending_block_fetch_t {
  buffer_t* hash;
  size_t index;
  struct pending_block_fetch_t* next;
} pending_block_fetch_t;

typedef struct {
  stream_t stream;
  block_cache_t* bc;
  tuple_cache_t* tc;
  ori_t* ori;
  size_t descriptor_pad;
  size_t sent_bytes;
  size_t offset_remainder;
  uint8_t offset_applied;
  /* Async decode state */
  tuple_t* pending_tuple;
  buffer_t* xor_accumulator;
  size_t blocks_expected;
  size_t blocks_received;
  pending_block_fetch_t* pending_fetches;
} readable_off_stream_t;

readable_off_stream_t* readable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad);
void readable_off_stream_destroy(readable_off_stream_t* stream);
void readable_off_stream_dispatch(void* state, message_t* msg);
void readable_off_stream_write(readable_off_stream_t* stream, tuple_t* tuple);

#endif //OFFS_READABLE_OFF_STREAM_H