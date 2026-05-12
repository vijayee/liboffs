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
#include "../Scheduler/scheduler.h"
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

/* Pending get request — tracks CACHE_GET requests awaiting SECTIONS_READ_RESULT */
typedef struct pending_get_t {
  buffer_t* hash;
  index_entry_t* entry;
  actor_t* reply_to;
  struct pending_get_t* next;
} pending_get_t;

typedef struct {
  refcounter_t refcounter;
  block_lru_cache_t* lru;
  sections_t* sections;
  index_t* index;
  block_size_e type;
  uint32_t canary;
  struct scheduler_pool_t* pool;
  actor_t actor;
  pending_get_t* pending_gets;
} block_cache_t;

/* Async result payload for CACHE_GET_RESULT */
typedef struct {
  buffer_t* hash;
  block_t* block;
  actor_t* reply_to;
} cache_get_result_payload_t;

/* Async result payload for CACHE_PUT_RESULT */
typedef struct {
  int result;
  actor_t* reply_to;
} cache_put_result_payload_t;

/* Async result payload for CACHE_REMOVE_RESULT */
typedef struct {
  int result;
  actor_t* reply_to;
} cache_remove_result_payload_t;

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, timer_actor_t* timer_actor, scheduler_pool_t* pool);
void block_cache_destroy(block_cache_t* block_cache);
size_t block_cache_count(block_cache_t* block_cache);
void block_cache_dispatch(void* state, message_t* msg);

/* Async API — send message and inject actor into scheduler */
void block_cache_get_async(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);
void block_cache_put_async(block_cache_t* block_cache, block_t* block, actor_t* reply_to);
void block_cache_remove_async(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);

/* Sync API — direct dispatch, caller must not be inside block_cache actor.
   These are temporary and will be removed as callers convert to async. */
block_t* block_cache_get(block_cache_t* block_cache, buffer_t* hash);
int block_cache_put(block_cache_t* block_cache, block_t* block);
int block_cache_remove(block_cache_t* block_cache, buffer_t* hash);

#define BLOCK_CACHE_CANARY 0x424B4348u
void block_cache_validate(block_cache_t* block_cache, const char* func, int line);

#endif //OFFS_BLOCK_CACHE_H