//
// Created by victor on 5/8/25.
//

#ifndef OFFS_OFD_CACHE_H
#define OFFS_OFD_CACHE_H

#include "ofd.h"
#include "../BlockCache/block_cache.h"
#include "../Scheduler/scheduler.h"
#include "../Actor/actor.h"
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

typedef HASHMAP(buffer_t, ofd_cache_entry_t*) ofd_map_t;

typedef struct {
    scheduler_pool_t* pool;
    block_cache_t* bc;
    ofd_map_t cache;
    size_t max_entries;
    uint64_t ttl_ms;
} ofd_cache_t;

/* Result payload sent back from ofd_cache_resolve_async */
typedef struct {
    ori_t* ori;       /* NULL if resolution failed */
    buffer_t* hash;   /* root hash that was resolved (referenced for caller) */
    char* path;       /* path that was resolved (copied) */
} ofd_resolve_result_t;

ofd_cache_t* ofd_cache_create(scheduler_pool_t* pool, block_cache_t* bc, uint64_t ttl_ms);
void ofd_cache_destroy(ofd_cache_t* cache);

ofd_t* ofd_cache_get(ofd_cache_t* cache, buffer_t* hash);
void ofd_cache_put(ofd_cache_t* cache, buffer_t* hash, ofd_t* ofd);
ori_t* ofd_cache_resolve(ofd_cache_t* cache, buffer_t* root_hash, const char* path);

/* Async resolve — creates a transient resolver actor that sends OFD_CACHE_RESOLVE_RESULT
   to reply_to when complete. The resolver self-destructs after sending the result. */
void ofd_cache_resolve_async(ofd_cache_t* cache, buffer_t* root_hash, const char* path, actor_t* reply_to);

#ifdef __cplusplus
}
#endif

#endif //OFFS_OFD_CACHE_H