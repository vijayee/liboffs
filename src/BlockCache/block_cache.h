//
// Created by victor on 9/10/25.
//

#ifndef OFFS_BLOCK_CACHE_H
#define OFFS_BLOCK_CACHE_H
#include "block.h"
#include "ephemeral_registry.h"
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

/* CACHE_FIT result codes for block_cache_can_fit */
#define CACHE_FIT_OK    0   /* Required bytes fit within capacity */
#define CACHE_FIT_FULL 1   /* Required bytes exceed capacity */

/* Payload for CACHE_PUT message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a completion message is sent back.
   incoming_fib: FIB counter from network (0 for local puts). */
typedef struct {
  block_t* block;
  actor_t* reply_to;
  uint32_t incoming_fib;
  int result;
  uint8_t acquire_ephemeral; /* 1 = acquire an ephemeral claim (count += 1) on the stored block */
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
  uint8_t force; /* 1 = remove even when pinned/claimed (pin count is reset) */
} cache_remove_payload_t;

/* CACHE_EPHEMERAL / CACHE_PIN op semantics */
#define CACHE_EPHEMERAL_OK        0
#define CACHE_EPHEMERAL_NOT_FOUND -1
#define CACHE_EPHEMERAL_OVERFLOW  -2

/* CACHE_REMOVE extended results (result is otherwise 0 / -1) */
#define CACHE_REMOVE_PINNED             -3
#define CACHE_REMOVE_EPHEMERAL_CLAIMED  -4

typedef enum {
  CACHE_EPHEMERAL_ACQUIRE = 0, /* count += 1; error at UINT16_MAX */
  CACHE_EPHEMERAL_RELEASE = 1, /* count -= 1; block deleted when it reaches 0 */
  CACHE_EPHEMERAL_CLEAR = 2    /* count = 0; commit (announce is caller's job) */
} cache_ephemeral_op_e;

/* Payload for CACHE_EPHEMERAL message */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
  cache_ephemeral_op_e op;
  int result;
  uint16_t previous_count;
  uint16_t new_count;
} cache_ephemeral_payload_t;

/* Result payload for CACHE_EPHEMERAL_RESULT */
typedef struct {
  int result;
  uint16_t previous_count;
  uint16_t new_count;
  actor_t* reply_to;
} cache_ephemeral_result_payload_t;

/* Payload shared by CACHE_PIN / CACHE_UNPIN (and their results) */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
  int result;
  uint32_t previous_count;
  uint32_t new_count;
} cache_pin_payload_t;

typedef struct {
  int result;
  uint32_t previous_count;
  uint32_t new_count;
  actor_t* reply_to;
} cache_pin_result_payload_t;

/* Payload for CACHE_EPHEMERAL_LIST — enumerates every ephemeral block.
   hashes[i] are referenced buffers; arrays are owned by the payload. */
typedef struct {
  actor_t* reply_to;
  size_t count;
  buffer_t** hashes;
  uint16_t* ephemeral_counts;
  uint32_t* pin_counts;
} cache_ephemeral_list_payload_t;

void cache_ephemeral_list_payload_destroy(cache_ephemeral_list_payload_t* payload);

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
  bool fsync_data;
  struct scheduler_pool_t* pool;
  timer_actor_t* timer_actor;
  uint64_t index_wait;
  actor_t actor;
  pending_get_t* pending_gets;
  size_t current_bytes;
  size_t max_capacity_bytes;
  authority_t* authority;
  respiration_actor_t* respiration;
  ephemeral_registry_t* registry;
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

/* Payload for CACHE_DEFRAGMENT message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a CACHE_DEFRAGMENT_RESULT is sent back. */
typedef struct {
  float occupancy_threshold;
  actor_t* reply_to;
  int result;
  size_t sections_defragmented;
  size_t blocks_relocated;
} cache_defragment_payload_t;

/* Result payload for CACHE_DEFRAGMENT_RESULT */
typedef struct {
  int result;
  size_t sections_defragmented;
  size_t blocks_relocated;
  actor_t* reply_to;
} cache_defragment_result_payload_t;

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
void block_cache_remove_ex(block_cache_t* block_cache, buffer_t* hash, uint8_t force, actor_t* reply_to);
void block_cache_ephemeral(block_cache_t* block_cache, buffer_t* hash, cache_ephemeral_op_e op, actor_t* reply_to);
void block_cache_pin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);
void block_cache_unpin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);

/* Enumerate every ephemeral (ephemeral_count > 0) block in the cache.
   Consumer contract for the mirrored reply: the cache actor processes the
   message, fills the payload, and mirrors the SAME message (and payload)
   back to reply_to, nulling its own copy — from that point the consumer's
   actor_run owns the payload. In your dispatch: copy out what you need,
   then free the stolen contents (each hashes[i] is a referenced buffer —
   DESTROY it; free the hashes/ephemeral_counts/pin_counts arrays), NULL
   the three array pointers and set count = 0 in the payload shell, and
   leave msg->payload untouched — your actor_run's payload_destroy call
   then frees the emptied shell exactly once
   (cache_ephemeral_list_payload_destroy tolerates the emptied shell).
   Alternatively you may set msg->payload = NULL and free the shell
   yourself; actor_run guards a NULL payload either way. */
void block_cache_list_ephemeral(block_cache_t* block_cache, actor_t* reply_to);

/* Victim-candidate check for respiration exhale: pinned permanent blocks and
   ephemeral blocks are never shed. LRU/capacity behavior ignores both fields. */
bool block_cache_entry_is_sheddable(const index_entry_t* entry);

/* Put that acquires one claim (count += 1) on the stored block; the first
   claim on a fresh block makes it 1. Works for both a new put and a block
   that already exists. */
void block_cache_put_ephemeral(block_cache_t* block_cache, block_t* block, actor_t* reply_to);

/* Advisory capacity check (unsynchronized): returns CACHE_FIT_OK if
 * current_bytes + required_bytes <= max_capacity_bytes, else CACHE_FIT_FULL.
 * When max_capacity_bytes == 0 (disabled), always returns CACHE_FIT_OK.
 * Reads current_bytes/max_capacity_bytes without locking — those fields are
 * mutated on the cache actor thread. Callers use this as a best-effort
 * pre-flight reject before enqueuing a PUT; the authoritative capacity
 * decision is still made inside block_cache_dispatch on the actor thread. */
int block_cache_can_fit(block_cache_t* block_cache, size_t required_bytes);
void block_cache_defragment(block_cache_t* block_cache, float occupancy_threshold, actor_t* reply_to);

// Recompute BLAKE3 over read data and compare against the stored hash. Used on
// the cache read path to reject on-disk corruption (bit rot, torn writes) as a
// miss rather than returning corrupt data as valid. See design 4.1.
bool block_cache_verify_read_hash(const buffer_t* data, const buffer_t* stored_hash);

#endif //OFFS_BLOCK_CACHE_H