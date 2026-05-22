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

/* CACHE_PUT result codes */
#define CACHE_PUT_NEW         0   /* Block was newly stored */
#define CACHE_PUT_EXISTS      1   /* Block already existed, no-op */
#define CACHE_PUT_ERROR      -1   /* sections_write failed */
#define CACHE_PUT_FULL       -2   /* Cache at capacity, cannot store */

/* Payload for CACHE_PUT message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a completion message is sent back.
   incoming_fib: FIB counter from network (0 for local puts). */
typedef struct {
  block_t* block;
  actor_t* reply_to;
  uint32_t incoming_fib;
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

typedef struct authority_t authority_t;
typedef struct respiration_actor_t respiration_actor_t;

typedef struct block_cache_t {
  refcounter_t refcounter;
  block_lru_cache_t* lru;
  sections_t* sections;
  index_t* index;
  block_size_e type;
  struct scheduler_pool_t* pool;
  actor_t actor;
  pending_get_t* pending_gets;
  size_t current_bytes;
  size_t max_capacity_bytes;
  authority_t* authority;
  respiration_actor_t* respiration;
} block_cache_t;

/* Result payload for CACHE_GET_RESULT */
typedef struct {
  buffer_t* hash;
  block_t* block;
  actor_t* reply_to;
} cache_get_result_payload_t;

/* Result payload for CACHE_PUT_RESULT */
typedef struct {
  int result;          /* CACHE_PUT_NEW, CACHE_PUT_EXISTS, or CACHE_PUT_ERROR */
  uint32_t fib;        /* Final FIB counter after max(local, incoming) */
  buffer_t* hash;      /* Hash of the stored block (referenced) */
  actor_t* reply_to;
} cache_put_result_payload_t;

/* Result payload for CACHE_REMOVE_RESULT */
typedef struct {
  int result;
  actor_t* reply_to;
} cache_remove_result_payload_t;

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, timer_actor_t* timer_actor, scheduler_pool_t* pool, authority_t* authority, size_t max_capacity_bytes);
void block_cache_destroy(block_cache_t* block_cache);
void block_cache_sync(block_cache_t* block_cache);
size_t block_cache_count(block_cache_t* block_cache);
void block_cache_update_capacity(block_cache_t* block_cache);
void block_cache_set_max_capacity(block_cache_t* block_cache, size_t max_capacity_bytes);
void block_cache_dispatch(void* state, message_t* msg);

/* Async API — send message and inject actor into scheduler */
void block_cache_get(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);
void block_cache_put(block_cache_t* block_cache, block_t* block, uint32_t incoming_fib, actor_t* reply_to);
void block_cache_remove(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);

#endif //OFFS_BLOCK_CACHE_H