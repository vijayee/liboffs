//
// Created by victor on 5/7/26.
//

#ifndef OFFS_TUPLE_CACHE_H
#define OFFS_TUPLE_CACHE_H

#include "tuple.h"
#include "../Buffer/buffer.h"
#include "../RefCounter/refcounter.h"
#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include <hashmap.h>
#include <stddef.h>
#include <stdint.h>

typedef struct tuple_cache_lru_node_t tuple_cache_lru_node_t;
struct tuple_cache_lru_node_t {
  buffer_t* value;
  tuple_t* key;
  tuple_cache_lru_node_t* next;
  tuple_cache_lru_node_t* previous;
};

typedef HASHMAP(tuple_t, tuple_cache_lru_node_t) tuple_cache_map_t;

typedef struct {
  tuple_cache_map_t cache;
  tuple_cache_lru_node_t* first;
  tuple_cache_lru_node_t* last;
  size_t capacity;
} tuple_cache_lru_t;

typedef struct {
  refcounter_t refcounter;
  tuple_cache_lru_t* lru;
  struct scheduler_pool_t* pool;
  actor_t actor;
} tuple_cache_t;

/* LRU cache operations */
tuple_cache_lru_t* tuple_cache_lru_create(size_t capacity);
void tuple_cache_lru_destroy(tuple_cache_lru_t* lru);
buffer_t* tuple_cache_lru_get(tuple_cache_lru_t* lru, tuple_t* key);
void tuple_cache_lru_put(tuple_cache_lru_t* lru, tuple_t* key, buffer_t* value);
void tuple_cache_lru_remove(tuple_cache_lru_t* lru, tuple_t* key);
uint8_t tuple_cache_lru_contains(tuple_cache_lru_t* lru, tuple_t* key);
size_t tuple_cache_lru_size(tuple_cache_lru_t* lru);

/* Actor wrapper */
tuple_cache_t* tuple_cache_create(size_t capacity, scheduler_pool_t* pool);
void tuple_cache_destroy(tuple_cache_t* tc);
void tuple_cache_dispatch(void* state, message_t* msg);

/* Async API — send message and inject actor into scheduler */
void tuple_cache_get(tuple_cache_t* tc, tuple_t* key, actor_t* reply_to);
void tuple_cache_put(tuple_cache_t* tc, tuple_t* key, buffer_t* value);

/* Result payload for TUPLE_CACHE_GET_RESULT */
typedef struct {
  tuple_t* key;
  buffer_t* value;
  actor_t* reply_to;
} tuple_cache_get_result_payload_t;

#endif //OFFS_TUPLE_CACHE_H