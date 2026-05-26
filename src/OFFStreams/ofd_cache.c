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

static uint64_t _now_ms(void) {
    return platform_monotonic_ns() / UINT64_C(1000000);
}

static void _ofd_cache_dispatch(void* state, message_t* msg);

/* ---- Resolver state (declared early so pending_resolver_t can reference it) ---- */

typedef struct resolver_state_t {
    buffer_t* root_hash;
    char* path;
    char* saveptr;
    ofd_t* current_ofd;
    actor_t* reply_to;
} resolver_state_t;

/* ---- Payload destroy functions ---- */

static void _ofd_cache_get_payload_destroy(void* ptr) {
    ofd_cache_get_payload_t* payload = (ofd_cache_get_payload_t*)ptr;
    if (payload->hash) DESTROY(payload->hash, buffer);
    free(payload);
}

static void _ofd_cache_put_payload_destroy(void* ptr) {
    ofd_cache_put_payload_t* payload = (ofd_cache_put_payload_t*)ptr;
    if (payload->hash) DESTROY(payload->hash, buffer);
    if (payload->ofd) ofd_destroy(payload->ofd);
    free(payload);
}

static void _ofd_cache_resolve_payload_destroy(void* ptr) {
    ofd_cache_resolve_payload_t* payload = (ofd_cache_resolve_payload_t*)ptr;
    if (payload->hash) DESTROY(payload->hash, buffer);
    if (payload->path) free(payload->path);
    free(payload);
}

static void _ofd_cache_get_result_payload_destroy(void* ptr) {
    ofd_cache_get_result_payload_t* payload = (ofd_cache_get_result_payload_t*)ptr;
    if (payload->hash) DESTROY(payload->hash, buffer);
    if (payload->ofd) DESTROY(payload->ofd, ofd);
    free(payload);
}

static void _ofd_resolve_result_destroy(void* ptr) {
    ofd_resolve_result_t* result = (ofd_resolve_result_t*)ptr;
    if (result->ori) DESTROY(result->ori, ori);
    if (result->hash) DESTROY(result->hash, buffer);
    if (result->path) free(result->path);
    free(result);
}

/* ---- Pending resolver list helpers ---- */

static void _resolver_save_pending(ofd_cache_t* cache, buffer_t* waiting_hash,
                                   resolver_state_t* resolver) {
    pending_resolver_t* pending = get_clear_memory(sizeof(*pending));
    pending->waiting_hash = (buffer_t*)refcounter_reference((refcounter_t*)waiting_hash);
    pending->resolver = resolver;
    pending->next = cache->pending_resolvers;
    cache->pending_resolvers = pending;
}

static void _resolver_remove_pending(ofd_cache_t* cache, resolver_state_t* resolver) {
    pending_resolver_t** prev = &cache->pending_resolvers;
    while (*prev) {
        if ((*prev)->resolver == resolver) {
            pending_resolver_t* to_free = *prev;
            *prev = to_free->next;
            DESTROY(to_free->waiting_hash, buffer);
            free(to_free);
            return;
        }
        prev = &(*prev)->next;
    }
}

/* ---- Resolver helpers ---- */

static void _resolver_send_result(ofd_cache_t* cache, resolver_state_t* resolver, ori_t* ori) {
    (void)cache;
    if (!resolver->reply_to) {
        if (ori) DESTROY(ori, ori);
        return;
    }

    ofd_resolve_result_t* result = get_clear_memory(sizeof(*result));
    result->ori = ori ? (ori_t*)refcounter_reference((refcounter_t*)ori) : NULL;
    result->hash = (buffer_t*)refcounter_reference((refcounter_t*)resolver->root_hash);
    result->path = resolver->path ? strdup(resolver->path) : NULL;

    message_t msg;
    msg.type = OFD_CACHE_RESOLVE_RESULT;
    msg.payload = result;
    msg.payload_destroy = _ofd_resolve_result_destroy;
    actor_send(resolver->reply_to, &msg);
}

static void _resolver_cleanup(ofd_cache_t* cache, resolver_state_t* resolver) {
    _resolver_remove_pending(cache, resolver);
    DESTROY(resolver->root_hash, buffer);
    if (resolver->path) {
        free(resolver->path);
        resolver->path = NULL;
    }
    free(resolver);
}

static void _resolver_continue_path(ofd_cache_t* cache, resolver_state_t* resolver);

/* ---- Cache actor message handlers ---- */

static void _ofd_cache_handle_get(ofd_cache_t* cache, message_t* msg) {
    ofd_cache_get_payload_t* payload = (ofd_cache_get_payload_t*)msg->payload;
    msg->payload = NULL;

    ofd_cache_entry_t* entry = hashmap_get(&cache->cache, payload->hash);
    ofd_t* result_ofd = NULL;

    if (entry) {
        if (_now_ms() >= entry->expires_at) {
            hashmap_remove(&cache->cache, payload->hash);
            DESTROY(entry->hash, buffer);
            ofd_destroy(entry->ofd);
            free(entry);
        } else {
            entry->expires_at = _now_ms() + cache->ttl_ms;
            result_ofd = entry->ofd;
        }
    }

    ofd_cache_get_result_payload_t* result = get_clear_memory(sizeof(*result));
    result->hash = payload->hash;
    result->ofd = result_ofd ? (ofd_t*)refcounter_reference((refcounter_t*)result_ofd) : NULL;

    message_t reply;
    reply.type = OFD_CACHE_GET_RESULT;
    reply.payload = result;
    reply.payload_destroy = _ofd_cache_get_result_payload_destroy;
    actor_send(payload->reply_to, &reply);

    free(payload);
}

static void _ofd_cache_handle_put(ofd_cache_t* cache, message_t* msg) {
    ofd_cache_put_payload_t* payload = (ofd_cache_put_payload_t*)msg->payload;
    msg->payload = NULL;

    ofd_cache_entry_t* existing = hashmap_get(&cache->cache, payload->hash);
    if (existing) {
        ofd_destroy(existing->ofd);
        existing->ofd = payload->ofd;
        existing->expires_at = _now_ms() + cache->ttl_ms;
        DESTROY(payload->hash, buffer);
        free(payload);
        return;
    }

    ofd_cache_entry_t* entry = get_clear_memory(sizeof(*entry));
    entry->hash = payload->hash;
    entry->ofd = payload->ofd;
    entry->expires_at = _now_ms() + cache->ttl_ms;
    hashmap_put(&cache->cache, payload->hash, entry);
    free(payload);
}

static void _ofd_cache_handle_resolve(ofd_cache_t* cache, message_t* msg) {
    ofd_cache_resolve_payload_t* payload = (ofd_cache_resolve_payload_t*)msg->payload;
    msg->payload = NULL;

    buffer_t* root_hash = payload->hash;
    char* path = payload->path;
    actor_t* reply_to = payload->reply_to;

    resolver_state_t* resolver = get_clear_memory(sizeof(*resolver));
    resolver->root_hash = (buffer_t*)refcounter_reference((refcounter_t*)root_hash);
    resolver->path = path;
    resolver->saveptr = NULL;
    resolver->current_ofd = NULL;
    resolver->reply_to = reply_to;

    DESTROY(root_hash, buffer);
    free(payload);

    if (resolver->path == NULL || strlen(resolver->path) == 0) {
        _resolver_send_result(cache, resolver, NULL);
        _resolver_cleanup(cache, resolver);
        return;
    }

    ofd_cache_entry_t* entry = hashmap_get(&cache->cache, resolver->root_hash);
    if (entry && _now_ms() < entry->expires_at) {
        resolver->current_ofd = entry->ofd;
        _resolver_continue_path(cache, resolver);
        return;
    }

    /* Root hash not cached — save resolver and fetch from block_cache */
    _resolver_save_pending(cache, resolver->root_hash, resolver);
    buffer_t* hash_ref = (buffer_t*)refcounter_reference((refcounter_t*)resolver->root_hash);
    block_cache_get(cache->bc, hash_ref, &cache->actor);
    DESTROY(hash_ref, buffer);
}

static void _ofd_cache_handle_block_result(ofd_cache_t* cache, message_t* msg) {
    cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
    buffer_t* hash = result->hash;
    result->hash = NULL;

    /* Dequeue any pending resolver waiting for this hash */
    pending_resolver_t* pending = NULL;
    pending_resolver_t** prev = &cache->pending_resolvers;
    while (*prev) {
        if (compare_buffer((*prev)->waiting_hash, hash) == 0) {
            pending = *prev;
            *prev = pending->next;
            break;
        }
        prev = &(*prev)->next;
    }

    if (result->block == NULL) {
        if (pending) {
            _resolver_send_result(cache, pending->resolver, NULL);
            DESTROY(pending->waiting_hash, buffer);
            _resolver_cleanup(cache, pending->resolver);
            free(pending);
        }
        DESTROY(hash, buffer);
        return;
    }

    ofd_t* ofd = ofd_decode(result->block->data);
    block_destroy(result->block);
    result->block = NULL;

    if (ofd == NULL) {
        if (pending) {
            _resolver_send_result(cache, pending->resolver, NULL);
            DESTROY(pending->waiting_hash, buffer);
            _resolver_cleanup(cache, pending->resolver);
            free(pending);
        }
        DESTROY(hash, buffer);
        return;
    }

    /* Store in cache */
    ofd_cache_entry_t* existing = hashmap_get(&cache->cache, hash);
    if (existing) {
        ofd_destroy(existing->ofd);
        existing->ofd = ofd;
        existing->expires_at = _now_ms() + cache->ttl_ms;
    } else {
        ofd_cache_entry_t* entry = get_clear_memory(sizeof(*entry));
        entry->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
        entry->ofd = ofd;
        entry->expires_at = _now_ms() + cache->ttl_ms;
        hashmap_put(&cache->cache, hash, entry);
    }

    /* Resume pending resolver */
    if (pending) {
        pending->resolver->current_ofd = ofd;
        DESTROY(pending->waiting_hash, buffer);
        resolver_state_t* saved_resolver = pending->resolver;
        free(pending);
        _resolver_continue_path(cache, saved_resolver);
    }

    DESTROY(hash, buffer);
}

static void _resolver_continue_path(ofd_cache_t* cache, resolver_state_t* resolver) {
    char* segment = (resolver->saveptr == NULL)
        ? strtok_r(resolver->path, "/", &resolver->saveptr)
        : strtok_r(NULL, "/", &resolver->saveptr);

    while (segment) {
        ofd_entry_t* entry = ofd_find(resolver->current_ofd, segment);
        if (!entry) {
            _resolver_send_result(cache, resolver, NULL);
            _resolver_cleanup(cache, resolver);
            return;
        }

        if (entry->type == OFD_ENTRY_FILE) {
            if (resolver->saveptr && *resolver->saveptr) {
                _resolver_send_result(cache, resolver, NULL);
                _resolver_cleanup(cache, resolver);
                return;
            }
            _resolver_send_result(cache, resolver, entry->file_ori);
            _resolver_cleanup(cache, resolver);
            return;
        }

        if (entry->type == OFD_ENTRY_DIRECTORY) {
            ofd_cache_entry_t* cached = hashmap_get(&cache->cache, entry->dir_hash);
            if (cached && _now_ms() < cached->expires_at) {
                resolver->current_ofd = cached->ofd;
                segment = strtok_r(NULL, "/", &resolver->saveptr);
                continue;
            }

            if (cached) {
                hashmap_remove(&cache->cache, entry->dir_hash);
                DESTROY(cached->hash, buffer);
                ofd_destroy(cached->ofd);
                free(cached);
            }

            /* Directory not cached — save resolver and fetch from block_cache */
            _resolver_save_pending(cache, entry->dir_hash, resolver);
            buffer_t* hash_ref = (buffer_t*)refcounter_reference((refcounter_t*)entry->dir_hash);
            block_cache_get(cache->bc, hash_ref, &cache->actor);
            DESTROY(hash_ref, buffer);
            return;
        }

        segment = strtok_r(NULL, "/", &resolver->saveptr);
    }

    _resolver_send_result(cache, resolver, NULL);
    _resolver_cleanup(cache, resolver);
}

static void _ofd_cache_dispatch(void* state, message_t* msg) {
    ofd_cache_t* cache = (ofd_cache_t*)state;

    switch (msg->type) {
        case OFD_CACHE_GET:
            _ofd_cache_handle_get(cache, msg);
            break;

        case OFD_CACHE_PUT:
            _ofd_cache_handle_put(cache, msg);
            break;

        case OFD_CACHE_RESOLVE:
            _ofd_cache_handle_resolve(cache, msg);
            break;

        case CACHE_GET_RESULT:
            _ofd_cache_handle_block_result(cache, msg);
            break;

        default:
            break;
    }
}

/* ---- Public API ---- */

ofd_cache_t* ofd_cache_create(scheduler_pool_t* pool, block_cache_t* bc, uint64_t ttl_ms) {
    ofd_cache_t* cache = get_clear_memory(sizeof(ofd_cache_t));
    if (!cache) return NULL;
    actor_init(&cache->actor, cache, _ofd_cache_dispatch, pool);
    cache->pool = pool;
    cache->bc = bc;
    cache->max_entries = 256;
    cache->ttl_ms = ttl_ms > 0 ? ttl_ms : 300000;
    cache->pending_resolvers = NULL;
    hashmap_init(&cache->cache, (void*)hash_buffer, (void*)compare_buffer);
    hashmap_set_key_alloc_funcs(&cache->cache, (void*)refcounter_reference, (void*)buffer_destroy);
    return cache;
}

void ofd_cache_destroy(ofd_cache_t* cache) {
    if (!cache) return;

    actor_destroy(&cache->actor);

    /* Clean up pending resolvers */
    pending_resolver_t* pending = cache->pending_resolvers;
    while (pending) {
        pending_resolver_t* next = pending->next;
        DESTROY(pending->resolver->root_hash, buffer);
        if (pending->resolver->path) free(pending->resolver->path);
        free(pending->resolver);
        DESTROY(pending->waiting_hash, buffer);
        free(pending);
        pending = next;
    }

    ofd_cache_entry_t* entry;
    PLATFORM_DIAGNOSTIC_PUSH
    PLATFORM_DIAGNOSTIC_IGNORE(-Wmissing-field-initializers)
    hashmap_foreach_data(entry, &cache->cache) {
        DESTROY(entry->hash, buffer);
        ofd_destroy(entry->ofd);
        free(entry);
    }
    PLATFORM_DIAGNOSTIC_POP
    hashmap_cleanup(&cache->cache);
    free(cache);
}

void ofd_cache_get(ofd_cache_t* cache, buffer_t* hash, actor_t* reply_to) {
    if (!cache || !hash || !reply_to) return;

    ofd_cache_get_payload_t* payload = get_clear_memory(sizeof(*payload));
    payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
    payload->reply_to = reply_to;

    message_t msg;
    msg.type = OFD_CACHE_GET;
    msg.payload = payload;
    msg.payload_destroy = _ofd_cache_get_payload_destroy;
    actor_send(&cache->actor, &msg);
}

void ofd_cache_put(ofd_cache_t* cache, buffer_t* hash, ofd_t* ofd) {
    if (!cache || !hash || !ofd) return;

    ofd_cache_put_payload_t* payload = get_clear_memory(sizeof(*payload));
    payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
    payload->ofd = ofd;

    message_t msg;
    msg.type = OFD_CACHE_PUT;
    msg.payload = payload;
    msg.payload_destroy = _ofd_cache_put_payload_destroy;
    actor_send(&cache->actor, &msg);
}

void ofd_cache_resolve(ofd_cache_t* cache, buffer_t* root_hash, const char* path, actor_t* reply_to) {
    if (!cache || !root_hash || !path) {
        if (reply_to) {
            ofd_resolve_result_t* result = get_clear_memory(sizeof(*result));
            message_t msg;
            msg.type = OFD_CACHE_RESOLVE_RESULT;
            msg.payload = result;
            msg.payload_destroy = _ofd_resolve_result_destroy;
            actor_send(reply_to, &msg);
        }
        return;
    }

    if (strlen(path) == 0) {
        if (reply_to) {
            ofd_resolve_result_t* result = get_clear_memory(sizeof(*result));
            result->hash = (buffer_t*)refcounter_reference((refcounter_t*)root_hash);
            result->path = strdup(path);
            message_t msg;
            msg.type = OFD_CACHE_RESOLVE_RESULT;
            msg.payload = result;
            msg.payload_destroy = _ofd_resolve_result_destroy;
            actor_send(reply_to, &msg);
        }
        return;
    }

    ofd_cache_resolve_payload_t* payload = get_clear_memory(sizeof(*payload));
    payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)root_hash);
    payload->path = strdup(path);
    payload->reply_to = reply_to;

    message_t msg;
    msg.type = OFD_CACHE_RESOLVE;
    msg.payload = payload;
    msg.payload_destroy = _ofd_cache_resolve_payload_destroy;
    actor_send(&cache->actor, &msg);
}
