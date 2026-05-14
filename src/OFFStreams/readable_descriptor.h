//
// Created by victor on 5/7/26.
//

#ifndef OFFS_READABLE_DESCRIPTOR_H
#define OFFS_READABLE_DESCRIPTOR_H

#include "../Streams/stream.h"
#include "tuple.h"
#include "ori.h"
#include "../BlockCache/block_cache.h"
#include "../Buffer/buffer.h"

typedef struct {
  stream_t stream;
  block_cache_t* bc;
  ori_t* ori;
  size_t block_size;
  size_t cut_point;
  size_t tuple_count;
  size_t tuple_counter;
  size_t offset_tuple;
  size_t descriptor_pad;
  buffer_t* current_descriptor;
  tuple_t* current_tuple;
  buffer_t* offset_remainder;
  buffer_t* next_descriptor_hash;
  buffer_t* expected_hash;
  uint8_t is_readable;
} readable_descriptor_t;

readable_descriptor_t* readable_descriptor_create(
    scheduler_pool_t* pool, block_cache_t* bc, ori_t* ori, size_t descriptor_pad);
void readable_descriptor_destroy(readable_descriptor_t* desc);
void readable_descriptor_dispatch(void* state, message_t* msg);
void readable_descriptor_push(readable_descriptor_t* desc);

#endif //OFFS_READABLE_DESCRIPTOR_H