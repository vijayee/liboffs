//
// Created by victor on 5/8/25.
//

#include "ofd_cache.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Buffer/buffer.h"
#include <hashmap.h>
#include <string.h>
#include <time.h>

static uint64_t _now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

ofd_cache_t* ofd_cache_create(scheduler_pool_t* pool, block_cache_t* bc, uint64_t ttl_ms) {
    ofd_cache_t* cache = get_clear_memory(sizeof(ofd_cache_t));
    if (!cache) return NULL;
    cache->pool = pool;
    cache->bc = bc;
    cache->max_entries = 256;
    cache->ttl_ms = ttl_ms > 0 ? ttl_ms : 300000;
    hashmap_init(&cache->cache, (void*)hash_buffer, (void*)compare_buffer);
    hashmap_set_key_alloc_funcs(&cache->cache, (void*)refcounter_reference, (void*)buffer_destroy);
    return cache;
}

void ofd_cache_destroy(ofd_cache_t* cache) {
    if (!cache) return;

    ofd_cache_entry_t* entry;
    hashmap_foreach_data(entry, &cache->cache) {
        ofd_destroy(entry->ofd);
        free(entry);
    }
    hashmap_cleanup(&cache->cache);
    free(cache);
}

ofd_t* ofd_cache_get(ofd_cache_t* cache, buffer_t* hash) {
    if (!cache || !hash) return NULL;

    ofd_cache_entry_t* entry = hashmap_get(&cache->cache, hash);
    if (!entry) return NULL;

    if (_now_ms() >= entry->expires_at) {
        hashmap_remove(&cache->cache, hash);
        ofd_destroy(entry->ofd);
        free(entry);
        return NULL;
    }

    entry->expires_at = _now_ms() + cache->ttl_ms;
    return entry->ofd;
}

void ofd_cache_put(ofd_cache_t* cache, buffer_t* hash, ofd_t* ofd) {
    if (!cache || !hash || !ofd) return;

    ofd_cache_entry_t* existing = hashmap_get(&cache->cache, hash);
    if (existing) {
        ofd_destroy(existing->ofd);
        existing->ofd = ofd;
        existing->expires_at = _now_ms() + cache->ttl_ms;
        return;
    }

    ofd_cache_entry_t* entry = get_clear_memory(sizeof(ofd_cache_entry_t));
    entry->hash = hash;
    entry->ofd = ofd;
    entry->expires_at = _now_ms() + cache->ttl_ms;
    hashmap_put(&cache->cache, hash, entry);
}

static ofd_t* _fetch_ofd_from_block_cache(ofd_cache_t* cache, buffer_t* hash) {
    ofd_t* cached = ofd_cache_get(cache, hash);
    if (cached) return cached;

    block_t* block = block_cache_get(cache->bc, hash);
    if (!block) return NULL;

    ofd_t* ofd = ofd_decode(block->data);
    block_destroy(block);

    if (!ofd) return NULL;

    ofd_cache_put(cache, hash, ofd);
    return ofd;
}

ori_t* ofd_cache_resolve(ofd_cache_t* cache, buffer_t* root_hash, const char* path) {
    if (!cache || !root_hash || !path) return NULL;

    ofd_t* current_ofd = _fetch_ofd_from_block_cache(cache, root_hash);
    if (!current_ofd) return NULL;

    if (strlen(path) == 0) return NULL;

    char* path_copy = strdup(path);
    char* saveptr = NULL;
    char* segment = strtok_r(path_copy, "/", &saveptr);

    while (segment) {
        ofd_entry_t* entry = ofd_find(current_ofd, segment);
        if (!entry) {
            free(path_copy);
            return NULL;
        }

        if (entry->type == OFD_ENTRY_FILE) {
            if (saveptr && *saveptr) {
                free(path_copy);
                return NULL;
            }
            ori_t* result = entry->file_ori;
            free(path_copy);
            return result;
        }

        if (entry->type == OFD_ENTRY_DIRECTORY) {
            current_ofd = _fetch_ofd_from_block_cache(cache, entry->dir_hash);
            if (!current_ofd) {
                free(path_copy);
                return NULL;
            }
        }

        segment = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);
    return NULL;
}