//
// Created by victor on 9/10/25.
//

#ifndef OFFS_BLOCK_CACHE_H
#define OFFS_BLOCK_CACHE_H
#include "block.h"
#include "index.h"
#include "sections.h"
#include "../RefCounter/refcounter.h"
#include "../Timer/timer_actor.h"
#include "../Configuration/config.h"
#include "../Actor/actor.h"
#include <hashmap.h>

typedef struct block_lru_node_t block_lru_node_t;
struct block_lru_node_t {
  block_t* value;
  index_entry_t* entry;
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
index_entry_t* block_lru_cache_put(block_lru_cache_t* lru, block_t* block, index_entry_t* entry);
uint8_t block_lru_cache_contains(block_lru_cache_t* lru, buffer_t* hash);
index_entry_t* block_lru_cache_peek_entry(block_lru_cache_t* lru, buffer_t* hash);

/* Payload for CACHE_PUT message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a completion message is sent back. */
typedef struct {
  block_t* block;
  actor_t* reply_to;
  int result;
} cache_put_payload_t;

/* Payload for CACHE_GET message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a completion message is sent back. */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
  block_t* result;
} cache_get_payload_t;

/* Payload for CACHE_REMOVE message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a completion message is sent back. */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
  int result;
} cache_remove_payload_t;

typedef struct {
  refcounter_t refcounter;
  block_lru_cache_t* lru;
  sections_t* sections;
  index_t* index;
  block_size_e type;
  uint32_t canary;
  actor_t actor;
} block_cache_t;

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, timer_actor_t* timer_actor);
void block_cache_destroy(block_cache_t* block_cache);
int block_cache_put(block_cache_t* block_cache, block_t* block);
block_t* block_cache_get(block_cache_t* block_cache, buffer_t* hash);
int block_cache_remove(block_cache_t* block_cache, buffer_t* hash);
size_t block_cache_count(block_cache_t* block_cache);
void block_cache_dispatch(void* state, message_t* msg);

#define BLOCK_CACHE_CANARY 0x424B4348u
void block_cache_validate(block_cache_t* block_cache, const char* func, int line);

#endif //OFFS_BLOCK_CACHE_H