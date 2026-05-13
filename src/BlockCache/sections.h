//
// Created by victor on 8/4/25.
//

#ifndef OFFS_SECTIONS_H
#define OFFS_SECTIONS_H
#include <stddef.h>
#include <hashmap.h>
#include "../Buffer/buffer.h"
#include "section.h"
#include "../Timer/timer_actor.h"
#include "../Scheduler/scheduler.h"
#include <cbor.h>
typedef struct sections_lru_node_t sections_lru_node_t;
struct sections_lru_node_t {
  section_t* value;
  sections_lru_node_t* next;
  sections_lru_node_t* previous;
};

typedef HASHMAP(size_t, sections_lru_node_t) section_cache_t;

typedef struct {
  section_cache_t cache;
  sections_lru_node_t* first;
  sections_lru_node_t* last;
  size_t size;
} sections_lru_cache_t;

sections_lru_cache_t* sections_lru_cache_create(size_t size);
void sections_lru_cache_destroy(sections_lru_cache_t* lru);
section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id);
void  sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id);
void sections_lru_cache_put(sections_lru_cache_t* lru, section_t* section);
uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id);

typedef struct round_robin_node_t round_robin_node_t;
struct round_robin_node_t {
  size_t id;
  round_robin_node_t* next;
  round_robin_node_t* previous;
};

typedef struct {
  timer_actor_t* timer_actor;
  actor_t* save_target;
  uint64_t wait;
  uint64_t max_wait;
  char* path;
  size_t size;
  round_robin_node_t* first;
  round_robin_node_t* last;
} round_robin_t;

round_robin_t* round_robin_create(char* robin_path, timer_actor_t* timer_actor, actor_t* save_target, uint64_t wait, uint64_t max_wait);
void round_robin_destroy(round_robin_t* robin);
void round_robin_add(round_robin_t* robin, size_t id);
size_t round_robin_next(round_robin_t* robin);
void round_robin_remove(round_robin_t* robin, size_t id);
uint8_t round_robin_contains(round_robin_t* robin, size_t id);
cbor_item_t* round_robin_to_cbor(round_robin_t* robin);
round_robin_t* cbor_to_round_robin(cbor_item_t* cbor, char* robin_path, timer_actor_t* timer_actor, actor_t* save_target, uint64_t wait, uint64_t max_wait);

/* Payload for SECTIONS_WRITE message.
   When reply_to is NULL (sync), result/section_id/section_index are filled
   by dispatch. When reply_to is set (async), a SECTIONS_WRITE_COMPLETE is
   sent back. */
typedef struct {
  buffer_t* data;
  actor_t* reply_to;
  int result;
  size_t section_id;
  size_t section_index;
} sections_write_payload_t;

/* Payload for SECTIONS_READ message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a SECTIONS_READ_COMPLETE is sent back. */
typedef struct {
  size_t section_id;
  size_t section_index;
  actor_t* reply_to;
  buffer_t* result;
} sections_read_payload_t;

/* Payload for SECTIONS_DEALLOCATE message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a SECTIONS_DEALLOCATE_COMPLETE is sent back. */
typedef struct {
  size_t section_id;
  size_t section_index;
  actor_t* reply_to;
  int result;
} sections_deallocate_payload_t;

/* Completion result for SECTIONS_WRITE_COMPLETE */
typedef struct {
  int result;
  size_t section_id;
  size_t section_index;
} sections_write_result_t;

/* Completion result for SECTIONS_READ_COMPLETE */
typedef struct {
  buffer_t* data;
} sections_read_result_t;

/* Completion result for SECTIONS_DEALLOCATE_COMPLETE */
typedef struct {
  int result;
} sections_deallocate_result_t;

/* Result payload for SECTIONS_READ_RESULT */
typedef struct {
  size_t section_id;
  size_t section_index;
  buffer_t* data;
  actor_t* reply_to;
} sections_read_result_payload_t;

/* Result payload for SECTIONS_WRITE_RESULT */
typedef struct {
  int result;
  size_t section_id;
  size_t section_index;
  actor_t* reply_to;
} sections_write_result_payload_t;

/* Result payload for SECTIONS_DEALLOCATE_RESULT */
typedef struct {
  int result;
  actor_t* reply_to;
} sections_deallocate_result_payload_t;

typedef struct {
  sections_lru_cache_t* lru;
  round_robin_t* robin;
  size_t max_tuple_size;
  size_t next_id;
  size_t size;
  block_size_e type;
  size_t wait;
  size_t max_wait;
  timer_actor_t* timer_actor;
  struct scheduler_pool_t* pool;
  actor_t actor;
  char* data_path;
  char* meta_path;
  char* robin_path;
} sections_t;

sections_t* sections_create(char* path, size_t size, size_t cache_size, size_t max_tuple_size, block_size_e type, timer_actor_t* timer_actor, scheduler_pool_t* pool, size_t wait, size_t max_wait);
void sections_destroy(sections_t* sections);
void sections_dispatch(void* state, message_t* msg);

/* Async API — send message and inject actor into scheduler */
void sections_read(sections_t* sections, size_t section_id, size_t section_index, actor_t* reply_to);
void sections_write(sections_t* sections, buffer_t* data, actor_t* reply_to);
void sections_deallocate(sections_t* sections, size_t section_id, size_t section_index, actor_t* reply_to);

#endif //OFFS_SECTIONS_H
