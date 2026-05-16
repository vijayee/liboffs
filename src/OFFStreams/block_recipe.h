//
// Created by victor on 5/7/26.
//

#ifndef OFFS_BLOCK_RECIPE_H
#define OFFS_BLOCK_RECIPE_H

#include "../Streams/stream.h"
#include "../BlockCache/block_cache.h"
#include "../BlockCache/block.h"
#include "../Util/vec.h"
#include "ori.h"

typedef struct network_t network_t;

typedef vec_t(ori_t*) vec_ori_t;
typedef vec_t(buffer_t*) vec_buffer_t;

typedef enum {
  RECIPE_FETCHING_BLOCK,       /* waiting for block_cache_get result */
  RECIPE_AWAITING_NETWORK,     /* waiting for NETWORK_FIND_BLOCK_RESULT */
  RECIPE_PROCESSING,           /* processing descriptor or data */
} recipe_state_e;

typedef struct {
  stream_t stream;
  block_cache_t* bc;
  block_size_e block_type;
} block_recipe_t;

typedef vec_t(block_recipe_t*) vec_block_recipe_t;

typedef struct {
  block_recipe_t recipe;
} new_blocks_recipe_t;

typedef struct {
  block_recipe_t recipe;
  network_t* network;               /* NULL = local-only mode */
  buffer_t* pending_fetch_hash;     /* hash we're fetching from network */
  vec_ori_t oris;
  int ori_index;
  vec_buffer_t descriptor;
  int descriptor_index;
  buffer_t* endcap;
  uint8_t descriptor_loaded;
  uint8_t loading_descriptor;
  int pending_pull;
  size_t descriptor_offset;
  buffer_t* next_descriptor_hash;
  vec_buffer_t front_hashes;
  vec_buffer_t back_hashes;
  size_t block_size;
  size_t descriptor_pad;
  size_t cut_point;
  recipe_state_e state;             /* stream state */
} recycler_recipe_t;

new_blocks_recipe_t* new_blocks_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type);
void new_blocks_recipe_destroy(new_blocks_recipe_t* recipe);
void new_blocks_recipe_dispatch(void* state, message_t* msg);
void new_blocks_recipe_pull(new_blocks_recipe_t* recipe);

recycler_recipe_t* recycler_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type,
    vec_ori_t oris, network_t* network);
void recycler_recipe_destroy(recycler_recipe_t* recipe);
void recycler_recipe_dispatch(void* state, message_t* msg);
void recycler_recipe_pull(recycler_recipe_t* recipe);

void block_recipe_pull(block_recipe_t* recipe);

#endif //OFFS_BLOCK_RECIPE_H