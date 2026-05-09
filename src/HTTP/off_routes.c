//
// Created by victor on 5/8/25.
//

#include "off_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "cors.h"
#include "../OFFStreams/off_url.h"
#include "../OFFStreams/readable_off_stream.h"
#include "../OFFStreams/readable_descriptor.h"
#include "../OFFStreams/writeable_off_stream.h"
#include "../OFFStreams/writeable_descriptor.h"
#include "../OFFStreams/block_recipe.h"
#include "../OFFStreams/ori.h"
#include "../OFFStreams/tuple_cache.h"
#include "../OFFStreams/tuple.h"
#include "../BlockCache/block_cache.h"
#include "../BlockCache/block.h"
#include "../Buffer/buffer.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

// OFF URL regex matching /offsystem/v3/{type}/{length}/{hash1}/{hash2}/{name}
#define OFF_GET_PATTERN "/offsystem/v3/([-+._a-zA-Z0-9]+/[-+._a-zA-Z0-9-]+)/([0-9]+)/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]+)/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]+)/([^ !$`&*()+]*|\\\\[ !$`&*()+]*)+"

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
                                                  ofd_cache_t* ofd_cache) {
    off_routes_context_t* ctx = get_clear_memory(sizeof(off_routes_context_t));
    ctx->pool = pool;
    ctx->bc = bc;
    ctx->ofd_cache = ofd_cache;
    return ctx;
}

void off_routes_context_destroy(off_routes_context_t* ctx) {
    free(ctx);
}

typedef struct {
    readable_descriptor_t* desc;
    readable_off_stream_t* rs;
    tuple_cache_t* tc;
    http_response_t* response;
} get_pipeline_t;

static void _pipeline_on_tuple(void* ctx, void* data) {
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    tuple_t* tuple = (tuple_t*)data;
    readable_off_stream_write(pipeline->rs, tuple);
}

static void _pipeline_on_desc_close(void* ctx, void* unused) {
    (void)unused;
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    // When the descriptor closes, close the off stream so the HTTP response
    // gets terminated. The off stream will emit close_event, which triggers
    // _pipe_on_close to send the response.
    if (pipeline->rs != NULL) {
        stream_close((stream_t*)pipeline->rs);
    }
    // Can't destroy the descriptor here — stream_notify still holds its handler
    // list lock. Use deferred deref to clean up after the dispatch completes.
    stream_deferred_deref((stream_t*)pipeline->desc);
    pipeline->desc = NULL;
}

static void _pipeline_on_desc_error(void* ctx, void* error) {
    (void)error;
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    if (!pipeline->response->headers_sent) {
        http_response_set_status(pipeline->response, 404);
        http_response_set_header(pipeline->response, "Content-Length", "0");
    }
}

static void _pipeline_on_rs_close(void* ctx, void* unused) {
    (void)unused;
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    // Off stream cleanup must also be deferred for the same reason
    stream_deferred_deref((stream_t*)pipeline->rs);
    tuple_cache_destroy(pipeline->tc);
    free(pipeline);
}

static void _setup_stream_pipeline(http_response_t* response, scheduler_pool_t* pool,
                                   block_cache_t* bc, ori_t* stream_ori,
                                   size_t descriptor_pad) {
    tuple_cache_t* tc = tuple_cache_create(100);
    readable_off_stream_t* rs = readable_off_stream_create(pool, bc, tc, stream_ori, descriptor_pad);
    readable_descriptor_t* desc = readable_descriptor_create(pool, bc, stream_ori, descriptor_pad);

    get_pipeline_t* pipeline = get_clear_memory(sizeof(get_pipeline_t));
    pipeline->desc = desc;
    pipeline->rs = rs;
    pipeline->tc = tc;
    pipeline->response = response;

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

static void _off_get_handler(http_request_t* request, http_response_t* response, void* user_data) {
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;
    off_url_t* url = off_url_parse(request->path);
    if (!url) {
        http_response_set_status(response, 400);
        http_response_end(response);
        return;
    }

    // Check if this is a directory content type
    if (url->content_type && strstr(url->content_type, "offsystem/directory") != NULL) {
        // Resolve path within the OFD hierarchy
        // Check for ?ofd=raw query parameter
        if (request->query_string && strstr(request->query_string, "ofd=raw") != NULL) {
            // Serve raw CBOR of the OFD block
            block_t* block = block_cache_get(ctx->bc, url->file_hash);
            if (!block) {
                http_response_set_status(response, 404);
                http_response_end(response);
                off_url_destroy(url);
                return;
            }
            http_response_set_header(response, "Content-Type", "application/cbor");
            http_response_write(response, (const char*)block->data->data, block->data->size);
            http_response_end(response);
            block_destroy(block);
            off_url_destroy(url);
            return;
        }

        // Try to find index.html for root requests, or resolve path
        const char* resolve_path = url->file_name;
        size_t name_len = strlen(url->file_name);
        if (name_len > 4 && strcmp(url->file_name + name_len - 4, ".ofd") == 0) {
            // Try index.html first
            ori_t* index_ori = ofd_cache_resolve(ctx->ofd_cache, url->file_hash, "index.html");
            if (index_ori) {
                size_t file_size = index_ori->final_byte;
                const char* range_header = http_request_header(request, "Range");
                range_request_t range = parse_range_header(range_header, file_size);
                http_response_set_header(response, "Accept-Ranges", "bytes");

                if (range_header && !range.valid) {
                    // Invalid range request
                    http_response_set_status(response, HTTP_STATUS_RANGE_NOT_SATISFIABLE);
                    char cr_str[64];
                    snprintf(cr_str, sizeof(cr_str), "bytes */%zu", file_size);
                    http_response_set_header(response, "Content-Range", cr_str);
                    http_response_end(response);
                    off_url_destroy(url);
                    return;
                }

                http_response_set_header(response, "Content-Type", "text/html");

                ori_t* stream_ori = ori_create(file_size);
                stream_ori->descriptor_hash = buffer_copy(index_ori->descriptor_hash);
                stream_ori->file_hash = buffer_copy(index_ori->file_hash);
                stream_ori->file_name = strdup(index_ori->file_name);
                stream_ori->block_type = index_ori->block_type;
                stream_ori->tuple_size = index_ori->tuple_size;

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

                _setup_stream_pipeline(response, ctx->pool, ctx->bc, stream_ori, 32);
                off_url_destroy(url);
                return;
            }
        }

        // Resolve the file within the OFD
        ori_t* file_ori = ofd_cache_resolve(ctx->ofd_cache, url->file_hash, resolve_path);
        if (!file_ori) {
            http_response_set_status(response, 404);
            http_response_end(response);
            off_url_destroy(url);
            return;
        }

        // Stream the file
        size_t file_size = file_ori->final_byte;
        const char* range_header = http_request_header(request, "Range");
        range_request_t range = parse_range_header(range_header, file_size);
        const char* mime = mime_type_from_extension(resolve_path);
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
        stream_ori->descriptor_hash = buffer_copy(file_ori->descriptor_hash);
        stream_ori->file_hash = buffer_copy(file_ori->file_hash);
        stream_ori->file_name = strdup(file_ori->file_name);
        stream_ori->block_type = file_ori->block_type;
        stream_ori->tuple_size = file_ori->tuple_size;

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

        _setup_stream_pipeline(response, ctx->pool, ctx->bc, stream_ori, 32);
        off_url_destroy(url);
        return;
    }

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

    _setup_stream_pipeline(response, ctx->pool, ctx->bc, stream_ori, 32);
    off_url_destroy(url);
}

typedef struct {
    http_response_t* response;
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
    url->content_type = strdup(put_ctx->content_type);
    url->file_name = strdup(put_ctx->file_name);
    url->stream_length = put_ctx->stream_length;
    if (put_ctx->server_address) {
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
    http_response_end(put_ctx->response);
    http_response_destroy(put_ctx->response);

    off_url_destroy(url);
    buffer_destroy(put_ctx->file_hash);
    buffer_destroy(put_ctx->descriptor_hash);
    free(put_ctx->content_type);
    free(put_ctx->file_name);
    free(put_ctx->server_address);
    // Streams are cleaned up when their refcount drops to zero after deactivation.
    // Destroying them here would cause use-after-free since stream_notify may still
    // be iterating the handler list.
    free(put_ctx);
}

static void _put_on_descriptor_data(void* ctx, void* data) {
    put_context_t* put_ctx = (put_context_t*)ctx;
    buffer_t* payload = (buffer_t*)data;
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

static void _off_put_handler(http_request_t* request, http_response_t* response, void* user_data) {
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;

    const char* type = http_request_header(request, "type");
    const char* file_name = http_request_header(request, "file-name");
    const char* stream_length_str = http_request_header(request, "stream-length");
    const char* server_address = http_request_header(request, "server-address");

    if (!type || !file_name || !stream_length_str) {
        http_response_set_status(response, 400);
        http_response_write(response, "Missing required headers: type, file-name, stream-length", 56);
        http_response_end(response);
        return;
    }

    size_t stream_length = (size_t)atol(stream_length_str);
    if (stream_length == 0) {
        http_response_set_status(response, 400);
        http_response_write(response, "Empty stream", 12);
        http_response_end(response);
        return;
    }

    tuple_cache_t* tc = tuple_cache_create(100);
    new_blocks_recipe_t* recipe = new_blocks_recipe_create(ctx->pool, ctx->bc, standard);
    vec_block_recipe_t recipes;
    vec_init(&recipes);
    vec_push(&recipes, (block_recipe_t*)recipe);

    writeable_off_stream_t* ws = writeable_off_stream_create(
        ctx->pool, ctx->bc, tc, standard, 3, 32, recipes);

    writeable_descriptor_t* desc = writeable_descriptor_create(
        ctx->pool, ctx->bc, standard, 32, 3, stream_length);

    put_context_t* put_ctx = get_clear_memory(sizeof(put_context_t));
    put_ctx->response = response;
    response->is_piped = 1;
    refcounter_reference((refcounter_t*)response);
    put_ctx->content_type = strdup(type);
    put_ctx->file_name = strdup(file_name);
    put_ctx->stream_length = stream_length;
    put_ctx->server_address = server_address ? strdup(server_address) : NULL;
    put_ctx->desc = desc;
    put_ctx->ws = ws;
    put_ctx->recipe = recipe;
    put_ctx->tc = tc;

    stream_subscribe((stream_t*)ws, data_event, put_ctx,
                     (void (*)(void*, void*))_put_on_stream_data, NULL);
    stream_subscribe((stream_t*)ws, close_event, put_ctx,
                     (void (*)(void*, void*))_put_on_stream_close, NULL);
    stream_subscribe((stream_t*)desc, data_event, put_ctx,
                     (void (*)(void*, void*))_put_on_descriptor_data, NULL);
    stream_once((stream_t*)desc, close_event, put_ctx,
                (void (*)(void*, void*))_put_on_descriptor_close, NULL);

    if (request->body && request->body->data && request->body->size > 0) {
        writeable_off_stream_write(ws, request->body);
    }

    writeable_off_stream_finalize(ws);
}

static void _off_delete_handler(http_request_t* request, http_response_t* response, void* user_data) {
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;

    off_url_t* url = off_url_parse(request->path);
    if (!url) {
        http_response_set_status(response, 400);
        http_response_end(response);
        return;
    }

    block_cache_remove(ctx->bc, url->file_hash);
    http_response_set_status(response, 200);
    http_response_end(response);
    off_url_destroy(url);
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
                         block_cache_t* bc, ofd_cache_t* ofd_cache) {
    off_routes_context_t* ctx = off_routes_context_create(pool, bc, ofd_cache);

    cors_config_t* cors_config = cors_config_offsystem();
    http_server_use(server, cors_middleware, cors_config,
                    (void (*)(void*))cors_config_destroy);

    http_server_get_with_data(server, OFF_GET_PATTERN,
                               _off_get_handler, ctx,
                               (void(*)(void*))off_routes_context_destroy);
    http_server_put_with_data(server, "/offsystem",
                               _off_put_handler, ctx, NULL);
    http_server_delete_with_data(server, OFF_GET_PATTERN,
                                  _off_delete_handler, ctx, NULL);
    http_server_post_with_data(server, OFF_GET_PATTERN,
                                _off_post_handler, ctx, NULL);
}