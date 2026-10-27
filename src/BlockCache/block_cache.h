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
#include "../Util/vec.h"
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
  uint32_t put_group;        /* 0 = untracked (default); nonzero = put-abort group id from the
                                caller: fresh creations (CACHE_PUT_NEW) record their hash under
                                the group so CACHE_PUT_GROUP_END can force-delete them. */
} cache_put_payload_t;

/* Payload for CACHE_GET message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a completion message is sent back.
   no_data: 1 = metadata-only fetch — increment the rank (index_peek runs
   the hit counter) and report the entry's presence, but never read the
   block bytes (verify-only finds must not ship 2+ GB back).
   peer_serve: 1 = the fetch answers a WIRE find — an entry the cache finds
   non-servable is reported as absent (ephemeral blocks must not leak their
   existence to peers). */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
  block_t* result;
  uint8_t no_data;
  uint8_t peer_serve;
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

/* Payload for CACHE_RESIZE message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a completion message is sent back. */
typedef struct {
  size_t max_capacity_bytes;
  actor_t* reply_to;
  int result;
} cache_resize_payload_t;

/* Result payload for CACHE_RESIZE_RESULT — reports the applied capacity
   and the recomputed live footprint (a shrink below current_bytes sheds
   sheddable entries; pinned/permanent/ephemeral blocks are never shed). */
typedef struct {
  int result;
  size_t max_capacity_bytes;
  size_t current_bytes;
  actor_t* reply_to;
} cache_resize_result_payload_t;

/* Bounded tombstone ring for ended put-abort groups (see block_cache_t). */
#define BLOCK_CACHE_PUT_GROUP_TOMBSTONES 1024

/* ---- put-abort group receipts ----
   One live receipt per emitting PUT (keyed by group id): CACHE_PUT_NEW
   appends an owned 32-byte hash copy; dedup hits (CACHE_PUT_EXISTS) are
   never recorded. CACHE_PUT_GROUP_END closes it: abort=1 force-deletes
   everything recorded REGARDLESS of pin/claim status (verbatim
   CACHE_REMOVE force semantics; the same in-flight-READ hazard the GC
   force sweep documents), abort=0 frees the receipt alone.

   FIFO makes the receipt's record phase complete for puts the cache
   actor queued BEFORE the END message. Puts still in flight when the
   emitting stream ends (recipe actors delivering past the abort) land
   AFTER it: those are dropped against the ended-group tombstone ring
   (see block_cache_t.ended_put_groups) before anything is stored. */
typedef struct {
  uint32_t group;
  vec_t(buffer_t*) hashes;   /* owned hash copies, always 32 bytes */
} bc_put_group_t;

/* Payload for CACHE_PUT_GROUP_END. */
typedef struct {
  uint32_t group;
  uint8_t abort;
} cache_put_group_end_payload_t;

/* Payload for CACHE_GC — a destructive keep-list sweep. `keep` is OWNED BY
   THE DISPATCH: the cache actor tests every indexed block's hash against it,
   deletes the blocks that miss, and destroys the filter before replying, so
   the caller must NULL its copy as soon as it sends. With force == 0, pinned
   and ephemeral-claimed doomed blocks are spared and counted in
   skipped_pinned / skipped_ephemeral_claimed; with force == 1 they are
   deleted (verbatim CACHE_REMOVE force semantics) and never counted.
   Hazard (matched, not newly handled): a force sweep carries the same race
   the single-hash CACHE_REMOVE already carries against pending in-flight
   SECTIONS_READs — a read in progress may find its section deallocated.
   See design 4.1. */
typedef struct {
  elastic_bloom_filter_t* keep;
  actor_t* reply_to;
  uint8_t force;
  int result;   /* 0 = sweep completed */
} cache_gc_payload_t;

/* Result payload for CACHE_GC_RESULT. entries_total is the size of the index
   snapshot the sweep enumerated; blocks_kept includes bloom false positives
   (safe direction — they only spare doomed blocks). */
typedef struct {
  int result;
  size_t blocks_deleted, blocks_kept, skipped_pinned, skipped_ephemeral_claimed, entries_total;
  actor_t* reply_to;
} cache_gc_result_payload_t;

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
  buffer_t* hash;  /* referenced copy of the request's hash — tells the
                      consumer WHICH block the counts describe; NULL when the
                      request carried no hash */
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

/* Payload for CACHE_LIST_SERVABLE — the seek-responder's offer list.
   Request: reply_to gets the result; message_id correlates the reply (the
   seek's wire message id); max_offers caps the list; exclude carries the
   seeker's exclude_hashes flat blob (exclude_count * 32 raw bytes, owned
   by the payload, NULL when none). The cache actor enumerates its own
   index, keeps servable entries not in the exclude list, sorts them by
   FIB descending, and answers the top max_offers. */
typedef struct {
  uint64_t message_id;
  actor_t* reply_to;
  size_t max_offers;
  uint8_t* exclude;
  size_t exclude_count;
} cache_list_servable_payload_t;

/* Result: flat offer arrays (offer_count <= max_offers); hashes are the raw
   32-byte block hashes, fibs the matching ranks; message_id mirrors the
   request's correlation id. */
typedef struct {
  uint64_t message_id;
  actor_t* reply_to;
  size_t offer_count;
  uint8_t* hashes;   /* offer_count * 32 */
  uint32_t* fibs;    /* offer_count */
} cache_list_servable_result_payload_t;

void cache_list_servable_result_payload_destroy(cache_list_servable_result_payload_t* payload);


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
  /* Config, create-time immutable, read by other actors' put machinery — the
     config-field-read class the actor-boundary rule blesses.
     delete_blocks_on_put_failure: a failed/aborted PUT force-deletes the
     blocks it freshly created regardless of pin/claim status, so a client
     disconnect or mid-put failure leaves no orphaned block data (default on;
     off = the failed put's blocks stay, as before this flag existed). */
  uint8_t delete_blocks_on_put_failure;
  vec_t(bc_put_group_t) put_groups;   /* live put-abort receipts, freed at CACHE_PUT_GROUP_END */
  /* Bounded tombstone ring of ended put-groups (circular, newest last).
     Group ids are process-monotonic (one writeable_off_stream counter) so an
     id is never reused: a CACHE_PUT_NEW for a tombstoned id is a LATE
     delivery from a recipe whose emitting put already ended — dropped before
     storing, instead of silently re-creating a ghost receipt. */
  uint32_t ended_put_groups[BLOCK_CACHE_PUT_GROUP_TOMBSTONES];
  size_t   ended_put_groups_count;    /* ring length (≤ tombstone cap) */
  size_t   ended_put_groups_next;     /* write position once the ring is full */
  bool fsync_data;
  struct scheduler_pool_t* pool;
  timer_actor_t* timer_actor;
  uint64_t index_wait;
  actor_t actor;
  pending_get_t* pending_gets;
  /* Written on the cache actor (single writer); read cross-actor by the
     health handler (actor-boundary rule: atomics where the direct
    read is genuinely required). */
  ATOMIC(size_t) current_bytes;
  ATOMIC(size_t) max_capacity_bytes;
  /* Live entry count, maintained on the cache actor at the same points
     current_bytes moves (+put-new, -delete-entry) and recomputed from the
     index at create/gc paths. block_cache_count() reads this instead of
     walking the live btree from another actor. */
  ATOMIC(uint64_t) entry_count;
  /* True from the moment capacity crosses the exhale threshold until
     the shed drives it back to the 50% floor (respiration_blocks_to_free
     targets). Between batches the gate is this flag, not the raw
     >=0.8 threshold, or the episode would stop one batch past 0.8
     instead of reaching the 50% resting point; clears at <=50%. */
  uint8_t exhale_shedding;
  authority_t* authority;
  respiration_actor_t* respiration;
  ephemeral_registry_t* registry;
} block_cache_t;

/* Result payload for CACHE_GET_RESULT.
   fib: the entry's rank after the fetch's hit-counter increment (0 when the
   hash was absent). entry_existed: 1 = the hash was in the index at fetch
   time (a NULL block then means the disk read failed), 0 = the hash was
   never indexed — consumers use it to tell a miss from a failed read.
   ephemeral_entry: 1 = the entry still holds ephemeral claims — the
   recycling flow's fetch-time exclusion check reads this (a metadata-only
   no_data GET reports it through here instead of any actor reading the
   index entry directly).
   no_data: echoes the REQUEST's no_data — distinguishes a metadata report
   from an ordinary block-less miss (a descriptor/data fetch miss). */
typedef struct {
  buffer_t* hash;
  block_t* block;
  actor_t* reply_to;
  uint32_t fib;
  uint8_t entry_existed;
  uint8_t ephemeral_entry;
  uint8_t no_data;
} cache_get_result_payload_t;

/* Result payload for CACHE_PUT_RESULT */
typedef struct {
  int result;          /* CACHE_PUT_NEW, CACHE_PUT_EXISTS, or CACHE_PUT_ERROR */
  uint32_t fib;        /* Final FIB counter after max(local, incoming) */
  uint8_t promoted;    /* 1 = the rank rose (incoming_fib > local) on this put —
                          the design's block-promotion event: the block
                          advertises itself data-less so seekers may pull. */
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
/* Resize via the cache actor (async; replies CACHE_RESIZE_RESULT). */
void block_cache_resize(block_cache_t* block_cache, size_t max_capacity_bytes, actor_t* reply_to);
/* Destructive keep-list sweep via the cache actor (async; replies
   CACHE_GC_RESULT). Consumes the keep filter on the cache actor thread — the
   caller must NULL its copy as soon as it calls this. */
void block_cache_gc(block_cache_t* block_cache, elastic_bloom_filter_t* keep,
                    uint8_t force, actor_t* reply_to);
void cache_get_payload_destroy(void* ptr);
void block_cache_dispatch(void* state, message_t* msg);

/* Async API — send message and inject actor into scheduler */
void block_cache_get(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);
/* Metadata-only / peer-serving fetches (see cache_get_payload_t's no_data /
   peer_serve): an async fetch with byte loading suppressed or with wire
   servability rules. */
void block_cache_get_ex(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to,
                        uint8_t no_data, uint8_t peer_serve);
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

/* PUT under a put-abort group (writeable_off_stream's failed-PUT cleanup):
   the cache records fresh creations (CACHE_PUT_NEW) under the group so the
   emitting PUT's CACHE_PUT_GROUP_END can force-delete them. block_cache_put
   delegates here with group 0. */
void block_cache_put_group(block_cache_t* block_cache, block_t* block,
                           uint32_t incoming_fib, actor_t* reply_to,
                           uint32_t put_group);
/* Close the emitting PUT's abort receipt. abort = 1: the PUT failed/aborted —
   force-delete everything the group freshly created REGARDLESS of pin/claim
   status (gated by config delete_blocks_on_put_failure, default on).
   abort = 0: the PUT completed — keep the blocks. */
void block_cache_put_group_end(block_cache_t* block_cache, uint32_t put_group,
                               uint8_t abort);

/* Advisory capacity check (unsynchronized): returns CACHE_FIT_OK if
 * current_bytes + required_bytes <= max_capacity_bytes, else CACHE_FIT_FULL.
 * When max_capacity_bytes == 0 (disabled), always returns CACHE_FIT_OK.
 * Reads current_bytes/max_capacity_bytes without locking — those fields are
 * mutated on the cache actor thread. Callers use this as a best-effort
 * pre-flight reject before enqueuing a PUT; the authoritative capacity
 * decision is still made inside block_cache_dispatch on the actor thread. */
int block_cache_can_fit(block_cache_t* block_cache, size_t required_bytes);
/* Seek-responder offer list (async): enumerates the cache's own index on the
   cache actor and replies CACHE_LIST_SERVABLE_RESULT to reply_to, carrying
   the request's message_id back for correlation. */
void block_cache_list_servable(block_cache_t* block_cache, size_t max_offers,
                               const uint8_t* exclude, size_t exclude_count,
                               uint64_t message_id, actor_t* reply_to);
void block_cache_defragment(block_cache_t* block_cache, float occupancy_threshold, actor_t* reply_to);

// Recompute BLAKE3 over read data and compare against the stored hash. Used on
// the cache read path to reject on-disk corruption (bit rot, torn writes) as a
// miss rather than returning corrupt data as valid. See design 4.1.
bool block_cache_verify_read_hash(const buffer_t* data, const buffer_t* stored_hash);

#endif //OFFS_BLOCK_CACHE_H