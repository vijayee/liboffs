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

typedef vec_t(ori_t*) vec_ori_t;
typedef vec_t(buffer_t*) vec_buffer_t;

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
  vec_ori_t oris;
  int ori_index;
  vec_buffer_t descriptor;
  int descriptor_index;
  buffer_t* endcap;
  uint8_t descriptor_loaded;
} recycler_recipe_t;

new_blocks_recipe_t* new_blocks_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type);
void new_blocks_recipe_destroy(new_blocks_recipe_t* recipe);
void new_blocks_recipe_dispatch(void* state, message_t* msg);
void new_blocks_recipe_pull(new_blocks_recipe_t* recipe);

recycler_recipe_t* recycler_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type,
    vec_ori_t oris);
void recycler_recipe_destroy(recycler_recipe_t* recipe);
void recycler_recipe_dispatch(void* state, message_t* msg);
void recycler_recipe_pull(recycler_recipe_t* recipe);

void block_recipe_pull(block_recipe_t* recipe);

#endif //OFFS_BLOCK_RECIPE_H