//
// Created by victor on 5/8/25.
//

#ifndef OFFS_OFD_CACHE_H
#define OFFS_OFD_CACHE_H

#include "ofd.h"
#include "../BlockCache/block_cache.h"
#include "../Scheduler/scheduler.h"
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

ofd_cache_t* ofd_cache_create(scheduler_pool_t* pool, block_cache_t* bc, uint64_t ttl_ms);
void ofd_cache_destroy(ofd_cache_t* cache);

ofd_t* ofd_cache_get(ofd_cache_t* cache, buffer_t* hash);
void ofd_cache_put(ofd_cache_t* cache, buffer_t* hash, ofd_t* ofd);
ori_t* ofd_cache_resolve(ofd_cache_t* cache, buffer_t* root_hash, const char* path);

#ifdef __cplusplus
}
#endif

#endif //OFFS_OFD_CACHE_H