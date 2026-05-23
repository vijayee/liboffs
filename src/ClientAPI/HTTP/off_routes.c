//
// Created by victor on 5/8/25.
//

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "off_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "http_connection.h"
#include "http_server.h"
#include "cors.h"
#include "auth_middleware.h"
#include "../../OFFStreams/off_url.h"
#include "../../OFFStreams/readable_off_stream.h"
#include "../../OFFStreams/readable_descriptor.h"
#include "../../OFFStreams/writeable_off_stream.h"
#include "../../OFFStreams/writeable_descriptor.h"
#include "../../OFFStreams/block_recipe.h"
#include "../../Scheduler/scheduler.h"
#include "../../OFFStreams/ori.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../OFFStreams/tuple.h"
#include "../../OFFStreams/ofd.h"
#include "../../BlockCache/block_cache.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/validation.h"

static int _draining_middleware(http_request_t* request, http_response_t* response,
                                void* user_data) {
  (void)request;
  http_server_t* server = (http_server_t*)user_data;
  if (atomic_load(&server->draining)) {
    http_response_set_status(response, HTTP_STATUS_SERVICE_UNAVAILABLE);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Server is shutting down\n", 22);
    http_response_end(response);
    return -1;
  }
  return 0;
}
#include "../../BlockCache/block.h"
#include "../../Buffer/buffer.h"
#include "../../Util/allocator.h"
#include "../../Actor/actor.h"
#include "../../Actor/message.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <poll-dancer/poll-dancer.h>

// OFF URL regex matching /offsystem/v3/{type}/{length}/{hash1}/{hash2}/{name}
// Type may contain '/' (e.g., "application/octet-stream") or be simple (e.g., "standard")
// Filename group accepts spaces (decoded from %20) and most printable chars
#define OFF_GET_PATTERN "/offsystem/v3/([-+._a-zA-Z0-9]+/[-+._a-zA-Z0-9-]+|[-+._a-zA-Z0-9]+)/([0-9]+)/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]+)/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]+)/([^!`&*()+]+|\\\\[ !$`&*()+]+)+"

typedef struct {
    size_t start;
    size_t end;
    int valid;
} range_request_t;

static range_request_t parse_range_header(const char* range_header, size_t file_size) {
    range_request_t range = {0, 0, 0};
    if (!range_header || strncmp(range_header, "bytes=", 6) != 0) {
        return range;
    }
    const char* spec = range_header + 6;
    while (*spec == ' ') spec++;

    // Reject multi-range requests (contain commas)
    if (strchr(spec, ',')) {
        return range;
    }

    const char* dash = strchr(spec, '-');
    if (!dash) {
        return range;
    }

    // Parse start
    if (dash == spec) {
        // Suffix range: bytes=-N (last N bytes)
        size_t suffix = (size_t)atol(dash + 1);
        if (suffix == 0 || suffix > file_size) {
            if (suffix > file_size) suffix = file_size;
            // suffix > 0 and suffix <= file_size is valid
        }
        if (suffix == 0) return range;
        range.start = file_size - suffix;
        range.end = file_size - 1;
        range.valid = 1;
        return range;
    }

    char* endptr = NULL;
    size_t start = (size_t)strtoull(spec, &endptr, 10);
    if (endptr != dash) {
        return range;
    }

    if (*(dash + 1) == '\0' || *(dash + 1) == '\r' || *(dash + 1) == '\n') {
        // Open-ended range: bytes=start-
        range.start = start;
        range.end = file_size - 1;
        range.valid = 1;
    } else {
        // Explicit range: bytes=start-end
        size_t end = (size_t)strtoull(dash + 1, &endptr, 10);
        range.start = start;
        range.end = end;
        range.valid = 1;
    }

    // Validate range
    if (range.start >= file_size || range.start > range.end) {
        range.valid = 0;
    } else if (range.end >= file_size) {
        range.end = file_size - 1;
    }

    return range;
}

off_routes_context_t* off_routes_context_create(scheduler_pool_t* pool,
                                                  block_cache_t* bc,
                                                  ofd_cache_t* ofd_cache,
                                                  tuple_cache_t* tc) {
    off_routes_context_t* ctx = get_clear_memory(sizeof(off_routes_context_t));
    ctx->pool = pool;
    ctx->bc = bc;
    ctx->ofd_cache = ofd_cache;
    ctx->tc = tc;
    return ctx;
}

void off_routes_context_destroy(off_routes_context_t* ctx) {
    free(ctx);
}

typedef struct {
    refcounter_t refcounter;
    readable_descriptor_t* desc;
    readable_off_stream_t* rs;
    tuple_cache_t* tc;
    http_response_t* response;
    ori_t* ori;
} get_pipeline_t;

static void _pipeline_on_tuple(void* ctx, void* data) {
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    tuple_t* tuple = (tuple_t*)data;
    readable_off_stream_write(pipeline->rs, tuple);
}

static void _pipeline_on_desc_close(void* ctx, void* unused) {
    (void)unused;
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    readable_descriptor_t* desc = pipeline->desc;
    int is_zero = refcounter_dereference_is_zero((refcounter_t*)pipeline);
    stream_deferred_deref((stream_t*)desc);
    if (is_zero) {
        DESTROY(pipeline->ori, ori);
        free(pipeline);
    }
}

static void _pipeline_on_desc_error(void* ctx, void* error) {
    (void)error;
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    http_response_t* response = pipeline->response;
    http_connection_t* conn = response->connection;
    if (!response->headers_sent) {
        http_response_set_status(response, 404);
        http_response_set_header(response, "Content-Length", "0");
    }
    http_response_end(response);
    response->connection = NULL;
    http_response_destroy(response);
    if (conn) {
        http_connection_destroy(conn);
    }
    stream_deactivate((stream_t*)pipeline->rs, NULL);
    // Don't defer-deref desc here — stream_deactivate sends close_event too,
    // and _pipeline_on_desc_close will handle the deref.
    if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
        DESTROY(pipeline->ori, ori);
        free(pipeline);
    }
}

static void _pipeline_on_rs_close(void* ctx, void* unused) {
    (void)unused;
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    readable_off_stream_t* rs = pipeline->rs;
    int is_zero = refcounter_dereference_is_zero((refcounter_t*)pipeline);
    stream_deferred_deref((stream_t*)rs);
    if (is_zero) {
        DESTROY(pipeline->ori, ori);
        free(pipeline);
    }
}

static void _setup_stream_pipeline(http_response_t* response, scheduler_pool_t* pool,
                                   block_cache_t* bc, tuple_cache_t* tc, ori_t* stream_ori,
                                   size_t descriptor_pad) {
    readable_off_stream_t* rs = readable_off_stream_create(pool, bc, tc, stream_ori, descriptor_pad, NULL);
    readable_descriptor_t* desc = readable_descriptor_create(pool, bc, stream_ori, descriptor_pad, NULL);

    get_pipeline_t* pipeline = get_clear_memory(sizeof(get_pipeline_t));
    pipeline->desc = desc;
    pipeline->rs = rs;
    pipeline->tc = tc;
    pipeline->response = response;
    pipeline->ori = stream_ori;
    refcounter_init((refcounter_t*)pipeline);
    refcounter_reference((refcounter_t*)pipeline);

    stream_subscribe((stream_t*)desc, data_event, pipeline,
                     (void (*)(void*, void*))_pipeline_on_tuple, NULL);
    stream_once((stream_t*)desc, close_event, pipeline,
                (void (*)(void*, void*))_pipeline_on_desc_close, NULL);
    stream_once((stream_t*)desc, error_event, pipeline,
                (void (*)(void*, void*))_pipeline_on_desc_error, NULL);
    stream_once((stream_t*)rs, close_event, pipeline,
                (void (*)(void*, void*))_pipeline_on_rs_close, NULL);
    http_response_pipe(response, (stream_t*)rs);

    readable_descriptor_push(desc);
}

/* ---- Async GET handler state ---- */

typedef enum {
    OFF_GET_RESOLVE_DIR,      /* Resolving directory path */
    OFF_GET_RESOLVE_INDEX,    /* Resolving index.html in .ofd directory */
    OFF_GET_FETCH_RAW_OFD     /* Fetching raw OFD block (?ofd=raw) */
} off_get_phase_t;

typedef struct {
    actor_t actor;
    off_routes_context_t* ctx;
    http_response_t* response;
    http_connection_t* connection;
    off_url_t* url;
    char* resolve_path;
    off_get_phase_t phase;
} off_get_state_t;

static void _off_get_state_destroy(off_get_state_t* state) {
    if (state->url) off_url_destroy(state->url);
    if (state->resolve_path) free(state->resolve_path);
    http_connection_t* conn = state->connection;
    http_response_destroy(state->response);
    if (conn) http_connection_destroy(conn);
    atomic_fetch_or(&state->actor.flags, ACTOR_FLAG_DESTROY);
    actor_destroy(&state->actor);
    scheduler_pool_defer_cleanup(state->ctx->pool, state, free);
}

static void _send_stream_response(http_response_t* response, off_routes_context_t* ctx,
                                   ori_t* file_ori, const char* content_type) {
    size_t file_size = file_ori->final_byte;
    const char* range_header = http_request_header(response->connection->request, "Range");
    range_request_t range = parse_range_header(range_header, file_size);
    http_response_set_header(response, "Content-Type", content_type);
    http_response_set_header(response, "Accept-Ranges", "bytes");

    if (range_header && !range.valid) {
        http_response_set_status(response, HTTP_STATUS_RANGE_NOT_SATISFIABLE);
        char cr_str[64];
        snprintf(cr_str, sizeof(cr_str), "bytes */%zu", file_size);
        http_response_set_header(response, "Content-Range", cr_str);
        http_response_end(response);
        DESTROY(file_ori, ori);
        return;
    }

    ori_t* stream_ori = ori_create(file_size);
    stream_ori->descriptor_hash = buffer_copy(file_ori->descriptor_hash);
    stream_ori->file_hash = buffer_copy(file_ori->file_hash);
    stream_ori->file_name = strdup(file_ori->file_name);
    stream_ori->block_type = file_ori->block_type;
    stream_ori->tuple_size = file_ori->tuple_size;
    DESTROY(file_ori, ori);

    if (range.valid) {
        http_response_set_status(response, HTTP_STATUS_PARTIAL_CONTENT);
        char cr_str[128];
        snprintf(cr_str, sizeof(cr_str), "bytes %zu-%zu/%zu",
                 range.start, range.end, file_size);
        http_response_set_header(response, "Content-Range", cr_str);
        char len_str[32];
        snprintf(len_str, sizeof(len_str), "%zu", range.end - range.start + 1);
        http_response_set_header(response, "Content-Length", len_str);
        stream_ori->file_offset = range.start;
        stream_ori->final_byte = range.end + 1;
    } else {
        char len_str[32];
        snprintf(len_str, sizeof(len_str), "%zu", file_size);
        http_response_set_header(response, "Content-Length", len_str);
    }

    _setup_stream_pipeline(response, ctx->pool, ctx->bc, ctx->tc, stream_ori, 32);
}

static void _off_get_dispatch(void* state, message_t* msg);

static void _off_get_handler(http_request_t* request, http_response_t* response, void* user_data) {
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;
    off_url_t* url = off_url_parse(request->path);
    if (!url) {
        http_response_set_status(response, 400);
        http_response_end(response);
        return;
    }

    /* Directory content type — needs async resolution */
    if (url->content_type && strstr(url->content_type, "offsystem/directory") != NULL) {
        off_get_state_t* state = get_clear_memory(sizeof(off_get_state_t));
        actor_init(&state->actor, state, _off_get_dispatch, ctx->pool);
        state->ctx = ctx;
        state->response = response;
        state->connection = response->connection;
        refcounter_reference((refcounter_t*)state->connection);
        refcounter_reference((refcounter_t*)state->response);
        state->url = url;

        /* ?ofd=raw — fetch block directly */
        if (request->query_string && strstr(request->query_string, "ofd=raw") != NULL) {
            state->phase = OFF_GET_FETCH_RAW_OFD;
            buffer_t* hash_ref = (buffer_t*)refcounter_reference((refcounter_t*)url->file_hash);
            block_cache_get(ctx->bc, hash_ref, &state->actor);
            DESTROY(hash_ref, buffer);
            return;
        }

        /* Directory resolve — use async resolver */
        const char* resolve_path = url->file_name;
        size_t name_len = strlen(url->file_name);

        if (name_len > 4 && strcmp(url->file_name + name_len - 4, ".ofd") == 0) {
            /* Try index.html first */
            state->phase = OFF_GET_RESOLVE_INDEX;
            ofd_cache_resolve(ctx->ofd_cache, url->file_hash, "index.html", &state->actor);
            return;
        }

        /* Resolve the path within the OFD */
        state->phase = OFF_GET_RESOLVE_DIR;
        state->resolve_path = strdup(resolve_path);
        ofd_cache_resolve(ctx->ofd_cache, url->file_hash, resolve_path, &state->actor);
        return;
    }

    /* Regular file — synchronous, no async needed */
    size_t file_size = url->stream_length;
    const char* range_header = http_request_header(request, "Range");
    range_request_t range = parse_range_header(range_header, file_size);
    const char* mime = (url->content_type && url->content_type[0]) ?
                       url->content_type : mime_type_from_extension(url->file_name);
    http_response_set_header(response, "Content-Type", mime);
    http_response_set_header(response, "Accept-Ranges", "bytes");

    if (range_header && !range.valid) {
        http_response_set_status(response, HTTP_STATUS_RANGE_NOT_SATISFIABLE);
        char cr_str[64];
        snprintf(cr_str, sizeof(cr_str), "bytes */%zu", file_size);
        http_response_set_header(response, "Content-Range", cr_str);
        http_response_end(response);
        off_url_destroy(url);
        return;
    }

    ori_t* stream_ori = ori_create(file_size);
    stream_ori->descriptor_hash = buffer_copy(url->descriptor_hash);
    stream_ori->file_hash = buffer_copy(url->file_hash);
    stream_ori->file_name = strdup(url->file_name);
    stream_ori->block_type = standard;
    stream_ori->tuple_size = 3;

    if (range.valid) {
        http_response_set_status(response, HTTP_STATUS_PARTIAL_CONTENT);
        char cr_str[128];
        snprintf(cr_str, sizeof(cr_str), "bytes %zu-%zu/%zu",
                 range.start, range.end, file_size);
        http_response_set_header(response, "Content-Range", cr_str);
        char len_str[32];
        snprintf(len_str, sizeof(len_str), "%zu", range.end - range.start + 1);
        http_response_set_header(response, "Content-Length", len_str);
        stream_ori->file_offset = range.start;
        stream_ori->final_byte = range.end + 1;
    } else {
        char content_length_str[32];
        snprintf(content_length_str, sizeof(content_length_str), "%zu", file_size);
        http_response_set_header(response, "Content-Length", content_length_str);
    }

    _setup_stream_pipeline(response, ctx->pool, ctx->bc, ctx->tc, stream_ori, 32);
    off_url_destroy(url);
}

static void _off_get_dispatch(void* state, message_t* msg) {
    off_get_state_t* ctx = (off_get_state_t*)state;

    switch (msg->type) {
        case CACHE_GET_RESULT: {
            /* ?ofd=raw — send the block data as CBOR */
            if (ctx->phase == OFF_GET_FETCH_RAW_OFD) {
                cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
                block_t* block = result->block;

                if (!block) {
                    http_response_set_status(ctx->response, 404);
                    http_response_end(ctx->response);
                    DESTROY(result->hash, buffer);
                    _off_get_state_destroy(ctx);
                    return;
                }

                http_response_set_header(ctx->response, "Content-Type", "application/cbor");
                http_response_write(ctx->response, (const char*)block->data->data, block->data->size);
                http_response_end(ctx->response);
                block_destroy(block);
                DESTROY(result->hash, buffer);
                _off_get_state_destroy(ctx);
                return;
            }
            break;
        }

        case OFD_CACHE_RESOLVE_RESULT: {
            ofd_resolve_result_t* result = (ofd_resolve_result_t*)msg->payload;

            if (ctx->phase == OFF_GET_RESOLVE_INDEX) {
                if (result->ori != NULL) {
                    /* Found index.html — stream it.
                       _send_stream_response takes ownership of the ori reference. */
                    _send_stream_response(ctx->response, ctx->ctx, result->ori, "text/html");
                    DESTROY(result->hash, buffer);
                    if (result->path) free(result->path);
                    _off_get_state_destroy(ctx);
                    return;
                }

                /* index.html not found — try resolving the path directly */
                DESTROY(result->hash, buffer);
                if (result->path) free(result->path);

                ctx->phase = OFF_GET_RESOLVE_DIR;
                ctx->resolve_path = strdup(ctx->url->file_name);
                ofd_cache_resolve(ctx->ctx->ofd_cache, ctx->url->file_hash,
                                        ctx->resolve_path, &ctx->actor);
                return;
            }

            if (ctx->phase == OFF_GET_RESOLVE_DIR) {
                if (result->ori != NULL) {
                    const char* mime = mime_type_from_extension(ctx->resolve_path);
                    /* _send_stream_response takes ownership of the ori reference. */
                    _send_stream_response(ctx->response, ctx->ctx, result->ori, mime);
                } else {
                    http_response_set_status(ctx->response, 404);
                    http_response_end(ctx->response);
                }
                DESTROY(result->hash, buffer);
                if (result->path) free(result->path);
                _off_get_state_destroy(ctx);
                return;
            }
            break;
        }

        default:
            break;
    }
}

typedef struct {
    http_response_t* response;
    http_connection_t* connection;
    buffer_t* file_hash;
    buffer_t* descriptor_hash;
    char* content_type;
    char* file_name;
    size_t stream_length;
    char* server_address;
    writeable_descriptor_t* desc;
    writeable_off_stream_t* ws;
    new_blocks_recipe_t* recipe;
    tuple_cache_t* tc;
} put_context_t;

static void _put_on_descriptor_close(void* ctx, void* unused) {
    (void)unused;
    put_context_t* put_ctx = (put_context_t*)ctx;

    off_url_t* url = off_url_create();
    free(url->content_type);
    url->content_type = strdup(put_ctx->content_type);
    url->file_name = strdup(put_ctx->file_name);
    url->stream_length = put_ctx->stream_length;
    if (put_ctx->server_address) {
        free(url->server_address);
        url->server_address = strdup(put_ctx->server_address);
    }
    url->file_hash = buffer_copy(put_ctx->file_hash);
    url->descriptor_hash = buffer_copy(put_ctx->descriptor_hash);

    char* result_url = off_url_to_string(url);
    if (result_url) {
        http_response_set_header(put_ctx->response, "Content-Type", "text/plain");
        http_response_write(put_ctx->response, result_url, strlen(result_url));
        free(result_url);
    }
    http_connection_t* conn = put_ctx->connection;
    http_response_end(put_ctx->response);
    http_response_destroy(put_ctx->response);
    if (conn) {
        http_connection_destroy(conn);
    }

    /* Order matters: pending list is LIFO. Add recipe first so its destructor
       runs LAST — after ws's destructor drops its recipe ref. */
    refcounter_dereference((refcounter_t*)put_ctx->recipe);
    scheduler_pool_defer_cleanup(((stream_t*)put_ctx->ws)->pool, put_ctx->recipe,
                                 (void (*)(void*))new_blocks_recipe_destroy);
    stream_deferred_deref((stream_t*)put_ctx->desc);
    stream_deferred_deref((stream_t*)put_ctx->ws);

    off_url_destroy(url);
    buffer_destroy(put_ctx->file_hash);
    buffer_destroy(put_ctx->descriptor_hash);
    free(put_ctx->content_type);
    free(put_ctx->file_name);
    free(put_ctx->server_address);
    free(put_ctx);
}

static void _put_on_descriptor_data(void* ctx, void* data) {
    put_context_t* put_ctx = (put_context_t*)ctx;
    buffer_t* payload = (buffer_t*)data;
    if (put_ctx->descriptor_hash != NULL) {
        buffer_destroy(put_ctx->descriptor_hash);
    }
    put_ctx->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
}

static void _put_on_stream_close(void* ctx, void* unused) {
    (void)unused;
    put_context_t* put_ctx = (put_context_t*)ctx;
    writeable_descriptor_close(put_ctx->desc);
}

static void _put_on_stream_data(void* ctx, void* data) {
    put_context_t* put_ctx = (put_context_t*)ctx;
    buffer_t* payload = (buffer_t*)data;
    if (payload->size == 32 && put_ctx->file_hash == NULL) {
        put_ctx->file_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
    } else {
        tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)payload);
        writeable_descriptor_write(put_ctx->desc, tuple);
        tuple_destroy(tuple);
    }
}

static buffer_t* _extract_multipart_file(buffer_t* body, const char* content_type) {
    const char* boundary_prefix = "boundary=";
    const char* boundary_start = strstr(content_type, boundary_prefix);
    if (!boundary_start) return NULL;
    boundary_start += strlen(boundary_prefix);
    while (*boundary_start == ' ' || *boundary_start == '"') boundary_start++;

    size_t boundary_len = 0;
    while (boundary_start[boundary_len] && boundary_start[boundary_len] != ';' && boundary_start[boundary_len] != ' ' && boundary_start[boundary_len] != '"' && boundary_start[boundary_len] != '\r' && boundary_start[boundary_len] != '\n') {
        boundary_len++;
    }

    char boundary[512];
    snprintf(boundary, sizeof(boundary), "--%.*s", (int)boundary_len, boundary_start);
    size_t boundary_str_len = strlen(boundary);

    uint8_t* data = body->data;
    size_t size = body->size;

    uint8_t* pos = (uint8_t*)memmem(data, size, boundary, boundary_str_len);
    if (!pos) return NULL;
    pos += boundary_str_len;

    while (pos < data + size && *pos == '\r') pos++;
    if (pos < data + size && *pos == '\n') pos++;

    uint8_t* headers_end = (uint8_t*)memmem(pos, (size_t)((data + size) - pos), "\r\n\r\n", 4);
    if (headers_end) {
        pos = headers_end + 4;
    } else {
        headers_end = (uint8_t*)memmem(pos, (size_t)((data + size) - pos), "\n\n", 2);
        if (headers_end) {
            pos = headers_end + 2;
        } else {
            return NULL;
        }
    }

    char end_boundary[520];
    snprintf(end_boundary, sizeof(end_boundary), "\r\n--%.*s--", (int)boundary_len, boundary_start);
    size_t end_len = strlen(end_boundary);
    uint8_t* end_pos = (uint8_t*)memmem(pos, (size_t)((data + size) - pos), end_boundary, end_len);
    if (!end_pos) {
        snprintf(end_boundary, sizeof(end_boundary), "\n--%.*s--", (int)boundary_len, boundary_start);
        end_len = strlen(end_boundary);
        end_pos = (uint8_t*)memmem(pos, (size_t)((data + size) - pos), end_boundary, end_len);
    }
    if (!end_pos) {
        snprintf(end_boundary, sizeof(end_boundary), "\r\n--%.*s", (int)boundary_len, boundary_start);
        end_len = strlen(end_boundary);
        end_pos = (uint8_t*)memmem(pos, (size_t)((data + size) - pos), end_boundary, end_len);
    }
    if (!end_pos) {
        snprintf(end_boundary, sizeof(end_boundary), "\n--%.*s", (int)boundary_len, boundary_start);
        end_len = strlen(end_boundary);
        end_pos = (uint8_t*)memmem(pos, (size_t)((data + size) - pos), end_boundary, end_len);
    }
    if (!end_pos) return NULL;

    size_t file_size = end_pos - pos;
    buffer_t* result = buffer_create(file_size);
    memcpy(result->data, pos, file_size);
    result->size = file_size;
    return result;
}

static void _off_put_handler(http_request_t* request, http_response_t* response, void* user_data) {
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;

    const char* type = http_request_header(request, "type");
    const char* file_name = http_request_header(request, "file-name");
    const char* stream_length_str = http_request_header(request, "stream-length");
    const char* server_address = http_request_header(request, "server-address");
    const char* content_type_header = http_request_header(request, "Content-Type");

    if (!type || !file_name || !stream_length_str) {
        http_response_set_status(response, 400);
        http_response_write(response, "Missing required headers: type, file-name, stream-length", 56);
        http_response_end(response);
        return;
    }

    if (validate_content_type(type) != 0 || validate_file_name(file_name) != 0) {
        http_response_set_status(response, 400);
        http_response_write(response, "Invalid content type or file name", 35);
        http_response_end(response);
        return;
    }

    size_t stream_length = (size_t)atol(stream_length_str);
    if (stream_length == 0 || stream_length > OFFS_MAX_CBOR_MESSAGE_SIZE) {
        http_response_set_status(response, 400);
        http_response_write(response, "Invalid stream length", 20);
        http_response_end(response);
        return;
    }

    buffer_t* upload_data = NULL;
    if (content_type_header && strncmp(content_type_header, "multipart/form-data", 19) == 0) {
        upload_data = _extract_multipart_file(request->body, content_type_header);
        if (!upload_data) {
            http_response_set_status(response, 400);
            http_response_write(response, "Failed to parse multipart data", 30);
            http_response_end(response);
            return;
        }
    } else if (request->body && request->body->data && request->body->size > 0) {
        upload_data = (buffer_t*)refcounter_reference((refcounter_t*)request->body);
    }

    new_blocks_recipe_t* recipe = new_blocks_recipe_create(ctx->pool, ctx->bc, standard);
    vec_block_recipe_t recipes;
    vec_init(&recipes);
    vec_push(&recipes, (block_recipe_t*)recipe);

    writeable_off_stream_t* ws = writeable_off_stream_create(
        ctx->pool, ctx->bc, ctx->tc, standard, 3, 32, recipes, NULL);

    writeable_descriptor_t* desc = writeable_descriptor_create(
        ctx->pool, ctx->bc, standard, 32, 3, stream_length, NULL);

    put_context_t* put_ctx = get_clear_memory(sizeof(put_context_t));
    put_ctx->response = response;
    put_ctx->connection = response->connection;
    response->is_piped = 1;
    response->connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response);
    refcounter_reference((refcounter_t*)response->connection);
    put_ctx->content_type = strdup(type);
    put_ctx->file_name = strdup(file_name);
    put_ctx->stream_length = stream_length;
    put_ctx->server_address = server_address ? strdup(server_address) : NULL;
    put_ctx->desc = desc;
    put_ctx->ws = ws;
    put_ctx->recipe = recipe;
    put_ctx->tc = ctx->tc;

    stream_subscribe((stream_t*)ws, data_event, put_ctx,
                     (void (*)(void*, void*))_put_on_stream_data, NULL);
    stream_subscribe((stream_t*)ws, close_event, put_ctx,
                     (void (*)(void*, void*))_put_on_stream_close, NULL);
    stream_subscribe((stream_t*)desc, data_event, put_ctx,
                     (void (*)(void*, void*))_put_on_descriptor_data, NULL);
    stream_once((stream_t*)desc, close_event, put_ctx,
                (void (*)(void*, void*))_put_on_descriptor_close, NULL);

    if (upload_data != NULL) {
        writeable_off_stream_write(ws, upload_data);
        buffer_destroy(upload_data);
    }

    writeable_off_stream_finalize(ws);
}

static void _put_on_request_data(void* ctx, void* data) {
    put_context_t* put_ctx = (put_context_t*)ctx;
    buffer_t* chunk = (buffer_t*)data;
    writeable_off_stream_write(put_ctx->ws, chunk);
}

static void _put_on_request_close(void* ctx, void* unused) {
    (void)unused;
    put_context_t* put_ctx = (put_context_t*)ctx;
    writeable_off_stream_finalize(put_ctx->ws);
}

static int _off_put_headers_complete(http_connection_t* connection,
                                      http_request_t* request,
                                      http_response_t* response) {
    // Fall back to buffered path for multipart uploads
    const char* content_type_header = http_request_header(request, "Content-Type");
    if (content_type_header && strncmp(content_type_header, "multipart/form-data", 19) == 0) {
        return 0;
    }

    const char* type = http_request_header(request, "type");
    const char* file_name = http_request_header(request, "file-name");
    const char* stream_length_str = http_request_header(request, "stream-length");
    const char* server_address = http_request_header(request, "server-address");

    if (!type || !file_name || !stream_length_str) {
        return 0;
    }

    if (validate_content_type(type) != 0 || validate_file_name(file_name) != 0) {
        return 0;
    }

    size_t stream_length = (size_t)atol(stream_length_str);
    if (stream_length == 0 || stream_length > OFFS_MAX_CBOR_MESSAGE_SIZE) {
        return 0;
    }

    // Get the off_routes_context from the matched route
    off_routes_context_t* routes_ctx = (off_routes_context_t*)connection->streaming_route->user_data;

    new_blocks_recipe_t* recipe = new_blocks_recipe_create(routes_ctx->pool, routes_ctx->bc, standard);
    vec_block_recipe_t recipes;
    vec_init(&recipes);
    vec_push(&recipes, (block_recipe_t*)recipe);

    writeable_off_stream_t* ws = writeable_off_stream_create(
        routes_ctx->pool, routes_ctx->bc, routes_ctx->tc, standard, 3, 32, recipes, NULL);

    writeable_descriptor_t* desc = writeable_descriptor_create(
        routes_ctx->pool, routes_ctx->bc, standard, 32, 3, stream_length, NULL);

    put_context_t* put_ctx = get_clear_memory(sizeof(put_context_t));
    put_ctx->response = response;
    put_ctx->connection = connection;
    response->is_piped = 1;
    connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response);
    refcounter_reference((refcounter_t*)connection);
    put_ctx->content_type = strdup(type);
    put_ctx->file_name = strdup(file_name);
    put_ctx->stream_length = stream_length;
    put_ctx->server_address = server_address ? strdup(server_address) : NULL;
    put_ctx->desc = desc;
    put_ctx->ws = ws;
    put_ctx->recipe = recipe;
    put_ctx->tc = routes_ctx->tc;

    stream_subscribe((stream_t*)ws, data_event, put_ctx,
                     (void (*)(void*, void*))_put_on_stream_data, NULL);
    stream_subscribe((stream_t*)ws, close_event, put_ctx,
                     (void (*)(void*, void*))_put_on_stream_close, NULL);
    stream_subscribe((stream_t*)desc, data_event, put_ctx,
                     (void (*)(void*, void*))_put_on_descriptor_data, NULL);
    stream_once((stream_t*)desc, close_event, put_ctx,
                (void (*)(void*, void*))_put_on_descriptor_close, NULL);

    // Pipe request stream data directly into the writeable_off_stream
    stream_subscribe((stream_t*)request, data_event, put_ctx,
                     (void (*)(void*, void*))_put_on_request_data, NULL);
    stream_once((stream_t*)request, close_event, put_ctx,
                (void (*)(void*, void*))_put_on_request_close, NULL);

    return 1;
}

static void _off_post_handler(http_request_t* request, http_response_t* response, void* user_data) {
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;
    (void)ctx;

    off_url_t* url = off_url_parse(request->path);
    if (!url) {
        http_response_set_status(response, 400);
        http_response_end(response);
        return;
    }

    // POST is for temporary block management
    // For now, just acknowledge
    http_response_set_status(response, 200);
    http_response_end(response);
    off_url_destroy(url);
}

void off_routes_register(http_server_t* server, scheduler_pool_t* pool,
                         block_cache_t* bc, ofd_cache_t* ofd_cache, tuple_cache_t* tc,
                         const config_t* config, const char* api_key) {
    off_routes_context_t* ctx = off_routes_context_create(pool, bc, ofd_cache, tc);

    http_server_use(server, _draining_middleware, server, NULL);

    cors_config_t* cors_config = cors_config_offsystem();
    http_server_use(server, cors_middleware, cors_config,
                    (void (*)(void*))cors_config_destroy);

    /* Register auth middleware if an API key hash is configured */
    if (config != NULL && config->api_key_hash != NULL && api_key != NULL) {
        auth_middleware_t* auth = auth_middleware_create(api_key, config->api_key_hash);
        if (auth != NULL) {
            http_server_use(server, auth_middleware_handler(), auth,
                            (void (*)(void*))auth_middleware_destroy);
        }
    }

    http_server_get_with_data(server, OFF_GET_PATTERN,
                               _off_get_handler, ctx,
                               (void(*)(void*))off_routes_context_destroy);
    http_server_put_with_data(server, "/offsystem",
                               _off_put_handler, ctx, NULL);
    http_route_t* put_route = &server->routes.data[server->routes.length - 1];
    put_route->headers_complete_handler = _off_put_headers_complete;
    http_server_post_with_data(server, OFF_GET_PATTERN,
                                _off_post_handler, ctx, NULL);
}