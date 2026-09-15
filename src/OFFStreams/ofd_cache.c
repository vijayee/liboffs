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
#include "../OFFStreams/readable_descriptor.h"
#include "../OFFStreams/readable_off_stream.h"
#include "../OFFStreams/ori.h"
#include "../OFFStreams/ofd.h"
#include "../BlockCache/block.h"
#include <hashmap.h>
#include <string.h>

/* MSVC's <string.h> provides strtok_s instead of strtok_r; map the name. */
#ifdef _WIN32
  #define strtok_r(str, delim, saveptr) strtok_s((str), (delim), (saveptr))
#endif

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
    pending->cache_ctx = cache;
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
            if (to_free->dir_buffer) buffer_destroy(to_free->dir_buffer);
            if (to_free->dir_rs) stream_deferred_deref((stream_t*)to_free->dir_rs);
            if (to_free->dir_desc) stream_deferred_deref((stream_t*)to_free->dir_desc);
            if (to_free->dir_ori) DESTROY(to_free->dir_ori, ori);
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

/* ---- Nested directory stream fallback ----
 * When a directory entry carries a descriptor hash, stream the directory CBOR
 * from the block cache (descriptor -> tuples -> blocks) instead of trying to
 * look it up by file hash. This keeps OFD metadata out of a separate disk
 * cache; the block cache is the single source of truth. */

typedef struct {
    pending_resolver_t* pending;
    buffer_t* dir_buffer;
    uint8_t error;
} ofd_cache_dir_fetched_payload_t;

static void _ofd_cache_dir_fetched_payload_destroy(void* ptr) {
    ofd_cache_dir_fetched_payload_t* p = (ofd_cache_dir_fetched_payload_t*)ptr;
    if (p->dir_buffer) buffer_destroy(p->dir_buffer);
    free(p);
}

static pending_resolver_t* _resolver_find_pending(ofd_cache_t* cache,
                                                  resolver_state_t* resolver) {
    pending_resolver_t* pending = cache->pending_resolvers;
    while (pending) {
        if (pending->resolver == resolver) return pending;
        pending = pending->next;
    }
    return NULL;
}

static void _dir_stream_data(void* ctx, void* data) {
    pending_resolver_t* pending = (pending_resolver_t*)ctx;
    buffer_t* chunk = (buffer_t*)data;
    if (pending->dir_fetch_error || !chunk || chunk->size == 0) return;

    if (pending->dir_buffer == NULL) {
        pending->dir_buffer = buffer_create_with_capacity(0, chunk->size);
    }
    size_t old_size = pending->dir_buffer->size;
    buffer_ensure_capacity(pending->dir_buffer, old_size + chunk->size);
    memcpy(pending->dir_buffer->data + old_size, chunk->data, chunk->size);
    pending->dir_buffer->size = old_size + chunk->size;
}

static void _dir_stream_send_done(ofd_cache_t* cache, pending_resolver_t* pending,
                                  buffer_t* buf, uint8_t error) {
    ofd_cache_dir_fetched_payload_t* payload = get_clear_memory(sizeof(*payload));
    payload->pending = pending;
    payload->dir_buffer = buf;
    payload->error = error;

    message_t msg;
    msg.type = OFD_CACHE_DIR_FETCHED;
    msg.payload = payload;
    msg.payload_destroy = _ofd_cache_dir_fetched_payload_destroy;
    actor_send(&cache->actor, &msg);
}

static void _dir_stream_error(void* ctx, void* unused) {
    (void)unused;
    pending_resolver_t* pending = (pending_resolver_t*)ctx;
    if (pending->dir_fetch_error) return;
    pending->dir_fetch_error = 1;
    _dir_stream_send_done((ofd_cache_t*)pending->cache_ctx, pending, NULL, 1);
}

static void _dir_stream_close(void* ctx, void* unused) {
    (void)unused;
    pending_resolver_t* pending = (pending_resolver_t*)ctx;
    if (pending->dir_fetch_error) return;
    buffer_t* buf = pending->dir_buffer;
    pending->dir_buffer = NULL;
    _dir_stream_send_done((ofd_cache_t*)pending->cache_ctx, pending, buf, 0);
}

static void _dir_stream_on_tuple(void* ctx, void* data) {
    pending_resolver_t* pending = (pending_resolver_t*)ctx;
    tuple_t* tuple = (tuple_t*)data;
    if (pending->dir_rs) {
        readable_off_stream_write((readable_off_stream_t*)pending->dir_rs, tuple);
    }
}

static void _dir_stream_desc_close(void* ctx, void* unused) {
    (void)unused;
    pending_resolver_t* pending = (pending_resolver_t*)ctx;
    if (pending->dir_desc) {
        stream_deferred_deref((stream_t*)pending->dir_desc);
        pending->dir_desc = NULL;
    }
}

static void _dir_stream_start(ofd_cache_t* cache, resolver_state_t* resolver,
                              buffer_t* dir_hash, buffer_t* descriptor_hash,
                              size_t dir_size) {
    pending_resolver_t* pending = _resolver_find_pending(cache, resolver);
    if (!pending) {
        /* Should not happen; create a pending entry to hold stream state. */
        _resolver_save_pending(cache, dir_hash, resolver);
        pending = _resolver_find_pending(cache, resolver);
    }
    if (!pending) return;

    ori_t* dir_ori = ori_create(dir_size);
    dir_ori->descriptor_hash = buffer_copy(descriptor_hash);
    dir_ori->file_hash = buffer_copy(dir_hash);
    dir_ori->file_name = strdup("dir.ofd");
    dir_ori->block_type = standard;
    dir_ori->tuple_size = 3;

    readable_off_stream_t* rs = readable_off_stream_create(
        cache->pool, cache->bc, NULL, dir_ori, 32, NULL);
    readable_descriptor_t* desc = readable_descriptor_create(
        cache->pool, cache->bc, dir_ori, 32, NULL);

    pending->dir_rs = rs;
    pending->dir_desc = desc;
    pending->dir_ori = dir_ori;
    refcounter_reference((refcounter_t*)rs);
    refcounter_reference((refcounter_t*)desc);

    stream_subscribe((stream_t*)desc, data_event, pending,
                     (void (*)(void*, void*))_dir_stream_on_tuple, NULL);
    stream_once((stream_t*)desc, close_event, pending,
                (void (*)(void*, void*))_dir_stream_desc_close, NULL);
    stream_once((stream_t*)desc, error_event, pending,
                (void (*)(void*, void*))_dir_stream_error, NULL);
    stream_subscribe((stream_t*)rs, data_event, pending,
                     (void (*)(void*, void*))_dir_stream_data, NULL);
    stream_once((stream_t*)rs, close_event, pending,
                (void (*)(void*, void*))_dir_stream_close, NULL);
    stream_once((stream_t*)rs, error_event, pending,
                (void (*)(void*, void*))_dir_stream_error, NULL);

    readable_descriptor_push(desc);
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

static void _ofd_cache_handle_dir_fetched(ofd_cache_t* cache, message_t* msg) {
    ofd_cache_dir_fetched_payload_t* payload = (ofd_cache_dir_fetched_payload_t*)msg->payload;
    msg->payload = NULL;

    pending_resolver_t* pending = payload->pending;
    buffer_t* buf = payload->dir_buffer;
    payload->dir_buffer = NULL;
    uint8_t error = payload->error;
    free(payload);

    if (!pending) {
        if (buf) buffer_destroy(buf);
        return;
    }

    /* Remove pending from the list and take ownership of its resolver. */
    pending_resolver_t** prev = &cache->pending_resolvers;
    while (*prev) {
        if (*prev == pending) {
            *prev = pending->next;
            break;
        }
        prev = &(*prev)->next;
    }

    resolver_state_t* resolver = pending->resolver;
    buffer_t* waiting_hash = pending->waiting_hash;
    pending->resolver = NULL;
    pending->waiting_hash = NULL;
    if (pending->dir_rs) stream_deferred_deref((stream_t*)pending->dir_rs);
    if (pending->dir_desc) stream_deferred_deref((stream_t*)pending->dir_desc);
    if (pending->dir_ori) DESTROY(pending->dir_ori, ori);
    if (pending->dir_buffer) buffer_destroy(pending->dir_buffer);
    free(pending);

    if (error || buf == NULL || buf->size == 0) {
        if (buf) buffer_destroy(buf);
        DESTROY(waiting_hash, buffer);
        _resolver_send_result(cache, resolver, NULL);
        _resolver_cleanup(cache, resolver);
        return;
    }

    ofd_t* ofd = ofd_decode(buf);
    buffer_destroy(buf);
    if (ofd == NULL) {
        DESTROY(waiting_hash, buffer);
        _resolver_send_result(cache, resolver, NULL);
        _resolver_cleanup(cache, resolver);
        return;
    }

    ofd_cache_entry_t* existing = hashmap_get(&cache->cache, waiting_hash);
    if (existing) {
        ofd_destroy(existing->ofd);
        existing->ofd = ofd;
        existing->expires_at = _now_ms() + cache->ttl_ms;
    } else {
        ofd_cache_entry_t* entry = get_clear_memory(sizeof(*entry));
        entry->hash = (buffer_t*)refcounter_reference((refcounter_t*)waiting_hash);
        entry->ofd = ofd;
        entry->expires_at = _now_ms() + cache->ttl_ms;
        hashmap_put(&cache->cache, waiting_hash, entry);
    }
    DESTROY(waiting_hash, buffer);

    resolver->current_ofd = ofd;
    _resolver_continue_path(cache, resolver);
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

            /* Directory not cached. If the OFD entry carries a descriptor hash,
               stream the directory CBOR from the block cache. Otherwise fall
               back to the legacy file-hash lookup (will fail for directories
               whose blocks are keyed by padded block hashes, but preserves old
               behavior for any OFDs that happen to store dir_hash as a block). */
            _resolver_save_pending(cache, entry->dir_hash, resolver);
            if (entry->dir_descriptor_hash != NULL && entry->dir_size > 0) {
                _dir_stream_start(cache, resolver, entry->dir_hash,
                                  entry->dir_descriptor_hash, entry->dir_size);
                return;
            }
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

        case OFD_CACHE_DIR_FETCHED:
            _ofd_cache_handle_dir_fetched(cache, msg);
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
        if (pending->dir_buffer) buffer_destroy(pending->dir_buffer);
        if (pending->dir_rs) stream_deferred_deref((stream_t*)pending->dir_rs);
        if (pending->dir_desc) stream_deferred_deref((stream_t*)pending->dir_desc);
        if (pending->dir_ori) DESTROY(pending->dir_ori, ori);
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
