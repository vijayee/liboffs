//
// Created by victor on 9/10/25.
//

#ifndef OFFS_BLOCK_CACHE_H
#define OFFS_BLOCK_CACHE_H
#include "block.h"
#include <hashmap.h>

typedef struct block_lru_node_t block_lru_node_t;
struct block_lru_node_t {
  block_t* value;
  block_lru_node_t* next;
  block_lru_node_t* previous;
};

typedef HASHMAP(buffer_t, block_lru_node_t) block_map_t;

typedef struct {
  block_map_t cache;
  block_lru_node_t* first;
  block_lru_node_t* last;
  size_t size;
} block_lru_cache_t;

block_lru_cache_t* block_lru_cache_create(size_t size);
void block_lru_cache_destroy(block_lru_cache_t* lru);
block_t* block_lru_cache_get(block_lru_cache_t* lru, buffer_t* hash);
void  block_lru_cache_delete(block_lru_cache_t* lru, buffer_t* hash);
void block_lru_cache_put(block_lru_cache_t* lru, block_t* block);
uint8_t block_lru_cache_contains(block_lru_cache_t* lru, buffer_t* hash);
#endif //OFFS_BLOCK_CACHE_H
