//
// Created by victor on 5/7/26.
//

#ifndef OFFS_WRITEABLE_OFF_STREAM_H
#define OFFS_WRITEABLE_OFF_STREAM_H

#include "../Streams/stream.h"
#include "tuple.h"
#include "tuple_cache.h"
#include "ori.h"
#include "block_recipe.h"
#include "../BlockCache/block_cache.h"
#include "../BlockCache/block.h"

typedef struct {
  block_t* origin;
  vec_t(block_t*) random_blocks;
  int random_capacity;
} off_stream_tuple_entry_t;

typedef struct {
  stream_t stream;
  block_cache_t* bc;
  tuple_cache_t* tc;
  block_size_e block_type;
  size_t block_size;
  size_t tuple_size;
  buffer_t* accumulator;
  vec_t(off_stream_tuple_entry_t*) entries;
  block_t* final_block;
  uint8_t* hash_state;
  uint8_t is_readable;
  vec_block_recipe_t recipes;
  int current_recipe_index;
  block_recipe_t* current_recipe;
  uint8_t has_pulled;
  uint8_t pending_finalize;
  size_t recipe_data_sub_id;
  size_t recipe_close_sub_id;
  size_t recipe_error_sub_id;
} writeable_off_stream_t;

writeable_off_stream_t* writeable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    block_size_e block_type, size_t tuple_size, size_t digest_size,
    vec_block_recipe_t recipes);
void writeable_off_stream_destroy(writeable_off_stream_t* stream);
void writeable_off_stream_dispatch(void* state, message_t* msg);
void writeable_off_stream_write(writeable_off_stream_t* stream, buffer_t* data);
void writeable_off_stream_finalize(writeable_off_stream_t* stream);

#endif //OFFS_WRITEABLE_OFF_STREAM_H