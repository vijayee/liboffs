//
// Created by victor on 5/8/25.
//

#ifndef OFFS_OFD_CACHE_H
#define OFFS_OFD_CACHE_H

#include "ofd.h"
#include "../BlockCache/block_cache.h"
#include "../Scheduler/scheduler.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    buffer_t* hash;
    ofd_t* ofd;
    uint64_t expires_at;
} ofd_cache_entry_t;

typedef HASHMAP(buffer_t, ofd_cache_entry_t) ofd_map_t;

/* Pending resolver — waiting for a block_cache response */
typedef struct pending_resolver_t {
    buffer_t* waiting_hash;
    struct resolver_state_t* resolver;
    struct pending_resolver_t* next;
} pending_resolver_t;

typedef struct {
    actor_t actor;         /* Must be first member */
    scheduler_pool_t* pool;
    block_cache_t* bc;
    ofd_map_t cache;
    size_t max_entries;
    uint64_t ttl_ms;
    pending_resolver_t* pending_resolvers;  /* linked list of resolvers awaiting block_cache */
} ofd_cache_t;

/* Result payload sent back from ofd_cache_resolve */
typedef struct {
    ori_t* ori;       /* NULL if resolution failed; caller takes ownership */
    buffer_t* hash;   /* root hash that was resolved (referenced) */
    char* path;       /* path that was resolved (copied) */
} ofd_resolve_result_t;

/* Internal: resolve request queued to the cache actor */
typedef struct {
    buffer_t* hash;
    char* path;
    actor_t* reply_to;
} ofd_cache_resolve_payload_t;

ofd_cache_t* ofd_cache_create(scheduler_pool_t* pool, block_cache_t* bc, uint64_t ttl_ms);
void ofd_cache_destroy(ofd_cache_t* cache);

/* Async lookup — sends OFD_CACHE_GET_RESULT to reply_to */
void ofd_cache_get(ofd_cache_t* cache, buffer_t* hash, actor_t* reply_to);

/* Fire-and-forget store — ownership of ofd transfers to the cache */
void ofd_cache_put(ofd_cache_t* cache, buffer_t* hash, ofd_t* ofd);

/* Async resolve — sends OFD_CACHE_RESOLVE_RESULT to reply_to */
void ofd_cache_resolve(ofd_cache_t* cache, buffer_t* root_hash, const char* path, actor_t* reply_to);

#ifdef __cplusplus
}
#endif

#endif //OFFS_OFD_CACHE_H
