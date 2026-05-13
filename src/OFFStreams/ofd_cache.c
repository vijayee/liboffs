//
// Created by victor on 5/8/25.
//

#include "ofd_cache.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Buffer/buffer.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Scheduler/scheduler.h"
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

/* ---- Async resolver actor ---- */

/* ---- Async resolver actor ---- */

typedef struct {
    actor_t actor;
    ofd_cache_t* cache;
    buffer_t* root_hash;
    char* path;
    char* saveptr;
    ofd_t* current_ofd;
    actor_t* reply_to;
    uint8_t resolving_root;  /* 1 if fetching root hash, 0 if fetching directory */
} ofd_resolver_t;

static void _resolver_send_result(ofd_resolver_t* resolver, ori_t* ori) {
    ofd_resolve_result_t* result = get_clear_memory(sizeof(ofd_resolve_result_t));
    result->ori = ori ? (ori_t*)refcounter_reference((refcounter_t*)ori) : NULL;
    result->hash = (buffer_t*)refcounter_reference((refcounter_t*)resolver->root_hash);
    result->path = resolver->path ? strdup(resolver->path) : NULL;

    message_t msg;
    msg.type = OFD_CACHE_RESOLVE_RESULT;
    msg.payload = result;
    msg.payload_destroy = free;

    if (resolver->reply_to) {
        actor_send(resolver->reply_to, &msg);
    }
}

static void _resolver_cleanup(ofd_resolver_t* resolver) {
    /* current_ofd is owned by the cache — do not destroy it here */
    DESTROY(resolver->root_hash, buffer);
    if (resolver->path) {
        free(resolver->path);
        resolver->path = NULL;
    }
    atomic_fetch_or(&resolver->actor.flags, ACTOR_FLAG_DESTROY);
    actor_destroy(&resolver->actor);
    scheduler_pool_defer_cleanup(resolver->cache->pool, resolver, free);
}

static void _resolver_dispatch(void* state, message_t* msg);

static void _resolver_continue_path(ofd_resolver_t* resolver) {
    if (resolver->path == NULL) {
        /* Path was empty — no match */
        _resolver_send_result(resolver, NULL);
        _resolver_cleanup(resolver);
        return;
    }

    /* First call uses strtok_r(path, ...), subsequent calls use strtok_r(NULL, ...) */
    char* segment = (resolver->saveptr == NULL)
        ? strtok_r(resolver->path, "/", &resolver->saveptr)
        : strtok_r(NULL, "/", &resolver->saveptr);

    while (segment) {
        ofd_entry_t* entry = ofd_find(resolver->current_ofd, segment);
        if (!entry) {
            _resolver_send_result(resolver, NULL);
            _resolver_cleanup(resolver);
            return;
        }

        if (entry->type == OFD_ENTRY_FILE) {
            /* If there are more segments, path doesn't exist */
            if (resolver->saveptr && *resolver->saveptr) {
                _resolver_send_result(resolver, NULL);
                _resolver_cleanup(resolver);
                return;
            }
            _resolver_send_result(resolver, entry->file_ori);
            _resolver_cleanup(resolver);
            return;
        }

        if (entry->type == OFD_ENTRY_DIRECTORY) {
            /* Check in-memory cache first */
            ofd_t* cached = ofd_cache_get(resolver->cache, entry->dir_hash);
            if (cached) {
                /* cached is borrowed from the cache — do not destroy current_ofd
                   since it is also in the cache */
                resolver->current_ofd = cached;
                segment = strtok_r(NULL, "/", &resolver->saveptr);
                continue;
            }

            /* Need to fetch from block cache asynchronously */
            buffer_t* hash_ref = (buffer_t*)refcounter_reference((refcounter_t*)entry->dir_hash);
            block_cache_get(resolver->cache->bc, hash_ref, &resolver->actor);
            DESTROY(hash_ref, buffer);
            resolver->resolving_root = 0;
            return;
        }

        segment = strtok_r(NULL, "/", &resolver->saveptr);
    }

    /* Path fully traversed without finding a file */
    _resolver_send_result(resolver, NULL);
    _resolver_cleanup(resolver);
}

static void _resolver_dispatch(void* state, message_t* msg) {
    ofd_resolver_t* resolver = (ofd_resolver_t*)state;

    switch (msg->type) {
        case OFD_CACHE_RESOLVE: {
            /* Initial message to start resolution */
            ofd_resolve_result_t* payload = (ofd_resolve_result_t*)msg->payload;

            /* Check in-memory cache for root hash */
            ofd_t* cached = ofd_cache_get(resolver->cache, resolver->root_hash);
            if (cached) {
                resolver->current_ofd = cached;
                _resolver_continue_path(resolver);
                return;
            }

            /* Fetch root OFD from block cache */
            buffer_t* hash_ref = (buffer_t*)refcounter_reference((refcounter_t*)resolver->root_hash);
            block_cache_get(resolver->cache->bc, hash_ref, &resolver->actor);
            DESTROY(hash_ref, buffer);
            resolver->resolving_root = 1;
            return;
        }

        case CACHE_GET_RESULT: {
            cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
            block_t* block = result->block;
            buffer_t* hash = result->hash;

            if (!block) {
                /* Block not found — resolution failed */
                DESTROY(hash, buffer);
                _resolver_send_result(resolver, NULL);
                _resolver_cleanup(resolver);
                return;
            }

            ofd_t* ofd = ofd_decode(block->data);
            block_destroy(block);
            DESTROY(hash, buffer);

            if (!ofd) {
                _resolver_send_result(resolver, NULL);
                _resolver_cleanup(resolver);
                return;
            }

            /* Cache the decoded OFD */
            ofd_cache_put(resolver->cache, resolver->resolving_root ? resolver->root_hash : ofd->hash, ofd);

            if (resolver->resolving_root) {
                /* We just fetched the root OFD */
                resolver->current_ofd = ofd;
                _resolver_continue_path(resolver);
            } else {
                /* We fetched a directory OFD — replace current_ofd and continue.
                   current_ofd is owned by the cache, so we just update the pointer. */
                resolver->current_ofd = ofd;
                _resolver_continue_path(resolver);
            }
            return;
        }

        default:
            break;
    }
}

void ofd_cache_resolve(ofd_cache_t* cache, buffer_t* root_hash, const char* path, actor_t* reply_to) {
    if (!cache || !root_hash || !path) {
        if (reply_to) {
            ofd_resolve_result_t* result = get_clear_memory(sizeof(ofd_resolve_result_t));
            result->ori = NULL;
            result->hash = NULL;
            result->path = path ? strdup(path) : NULL;
            message_t msg;
            msg.type = OFD_CACHE_RESOLVE_RESULT;
            msg.payload = result;
            msg.payload_destroy = free;
            actor_send(reply_to, &msg);
        }
        return;
    }

    ofd_resolver_t* resolver = get_clear_memory(sizeof(ofd_resolver_t));
    actor_init(&resolver->actor, resolver, _resolver_dispatch, cache->pool);
    resolver->cache = cache;
    resolver->root_hash = (buffer_t*)refcounter_reference((refcounter_t*)root_hash);
    resolver->path = strdup(path);
    resolver->saveptr = NULL;
    resolver->current_ofd = NULL;
    resolver->reply_to = reply_to;
    resolver->resolving_root = 1;

    if (strlen(path) == 0) {
        /* Empty path — send NULL result immediately */
        _resolver_send_result(resolver, NULL);
        _resolver_cleanup(resolver);
        return;
    }

    /* Kick off resolution — _resolver_continue_path will tokenize from the start */
    message_t msg;
    msg.type = OFD_CACHE_RESOLVE;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&resolver->actor, &msg);
}