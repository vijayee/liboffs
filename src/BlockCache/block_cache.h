//
// Created by victor on 9/10/25.
//

#ifndef OFFS_BLOCK_CACHE_H
#define OFFS_BLOCK_CACHE_H
#include "block.h"
#include "index.h"
#include "sections.h"
#include "../RefCounter/refcounter.h"
#include "../Workers/pool.h"
#include "../Time/wheel.h"
#include "../Util/threadding.h"
#include "../Configuration/config.h"
#include "../Workers/promise.h"
#include "../Workers/priority.h"
#include <hashmap.h>

typedef struct block_lru_node_t block_lru_node_t;
struct block_lru_node_t {
  block_t* value;
  block_lru_node_t* next;
  block_lru_node_t* previous;
};

typedef HASHMAP(buffer_t, block_lru_node_t) block_map_t;

typedef struct {
  PLATFORMLOCKTYPE(lock);
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

typedef struct {
  refcounter_t refcounter;
  block_lru_cache_t* lru;
  sections_t* sections;
  index_t* index;
  work_pool_t* pool;
  block_size_e type;
} block_cache_t;

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, work_pool_t* pool, hierarchical_timing_wheel_t* wheel);
void block_cache_destroy(block_cache_t* block_cache);
void block_cache_put(block_cache_t* block_cache, priority_t priority, block_t* block, promise_t* promise);
void block_cache_get(block_cache_t* block_cache, priority_t priority, buffer_t* hash, promise_t* promise);
void block_cache_remove(block_cache_t* block_cache, priority_t priority, buffer_t* hash, promise_t* promise);
#endif //OFFS_BLOCK_CACHE_H
