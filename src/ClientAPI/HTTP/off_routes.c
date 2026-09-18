//
// Created by victor on 5/8/25.
//

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "off_routes.h"
#include "http_response.h"
#include "../client_api_wire.h"
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
#include "../../OFFStreams/representation_actor.h"
#include "../../Scheduler/scheduler.h"
#include "../../OFFStreams/ori.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../OFFStreams/tuple.h"
#include "../../OFFStreams/ofd.h"
#include "../../BlockCache/block_cache.h"
#include "../../BlockCache/ephemeral_registry.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/log.h"
#include "../../Util/validation.h"

/* Upper bound on the client-declared `stream-length` header for streaming PUT.
 * Matches the non-streaming OFFS_MAX_CBOR_MESSAGE_SIZE bound so a client cannot
 * declare a tiny stream-length and then stream gigabytes through the pipeline. */
#define OFFS_MAX_STREAM_LENGTH  OFFS_MAX_CBOR_MESSAGE_SIZE

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
#ifdef _WIN32
  #include "../../Platform/platform_posix_compat.h"
  #include <winsock2.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
#endif
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
                                                  tuple_cache_t* tc,
                                                  network_t* network,
                                                  size_t max_tuple_size) {
    off_routes_context_t* ctx = get_clear_memory(sizeof(off_routes_context_t));
    ctx->pool = pool;
    ctx->bc = bc;
    ctx->ofd_cache = ofd_cache;
    ctx->tc = tc;
    ctx->network = network;
    ctx->max_tuple_size = max_tuple_size;
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
    /* desc_done ensures desc contributes exactly one pipeline deref,
       whether close or error fires first. stream_deactivate emits both
       close_event and error_event, so without this flag the pipeline
       would be dereffed twice for desc. */
    uint8_t desc_done;
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
    int is_zero = 0;
    if (!pipeline->desc_done) {
        pipeline->desc_done = 1;
        is_zero = refcounter_dereference_is_zero((refcounter_t*)pipeline);
    }
    stream_deferred_deref((stream_t*)desc);
    if (is_zero) {
        DESTROY(pipeline->ori, ori);
        free(pipeline);
    }
}

static void _pipeline_on_desc_error(void* ctx, void* error) {
    (void)error;
    get_pipeline_t* pipeline = (get_pipeline_t*)ctx;
    /* Deactivate the readable stream — the pipe's error handler will
       send the 404 response and clean up the connection.  Do NOT
       end/destroy the response here; _pipe_on_error and _pipe_on_close
       both fire on stream deactivation and would double-free. */
    stream_deactivate((stream_t*)pipeline->rs, NULL);
    int is_zero = 0;
    if (!pipeline->desc_done) {
        pipeline->desc_done = 1;
        is_zero = refcounter_dereference_is_zero((refcounter_t*)pipeline);
    }
    if (is_zero) {
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
                                   size_t descriptor_pad, network_t* network) {
    readable_off_stream_t* rs = readable_off_stream_create(pool, bc, tc, stream_ori, descriptor_pad, network);
    readable_descriptor_t* desc = readable_descriptor_create(pool, bc, stream_ori, descriptor_pad, network);

    get_pipeline_t* pipeline = get_clear_memory(sizeof(get_pipeline_t));
    pipeline->desc = desc;
    pipeline->rs = rs;
    pipeline->tc = tc;
    pipeline->response = response;
    pipeline->ori = stream_ori;
    /* Two derefs total: one for desc-done (close or error, whichever
       fires first — guarded by desc_done), one for rs-done. */
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

/* ---- ?load=1 cache-load pipeline ---- */

/* True when the request's query string enables the given parameter: either a
   bare token ("?load") or "load=1". Any other value ("?load=0") is treated as
   NOT enabled. Parameters are '&'-separated, so a file NAME that happens to
   contain "load" can never match (names live in the path, not the query
   string). */
static int _query_has_param(const char* query_string, const char* name) {
    if (query_string == NULL) return 0;
    size_t name_len = strlen(name);
    const char* cursor = query_string;
    while (*cursor != '\0') {
        const char* separator = strchr(cursor, '&');
        size_t token_len = separator != NULL ? (size_t)(separator - cursor) : strlen(cursor);
        if (token_len == name_len && strncmp(cursor, name, name_len) == 0) {
            return 1;
        }
        if (token_len > name_len && strncmp(cursor, name, name_len) == 0 &&
            cursor[name_len] == '=') {
            /* Bare token matched above; here only "?load=1" enables —
               "?load=0" and any other value do not. */
            return token_len == name_len + 2 && cursor[name_len + 1] == '1';
        }
        if (separator == NULL) break;
        cursor = separator + 1;
    }
    return 0;
}

/* Pipeline context for GET ...?load=1: pulls a file's blocks into the block
 * cache without serving file data, forwarding tuple-level progress as ndjson
 * lines and terminating with exactly one terminal line. Refcount discipline
 * mirrors get_pipeline_t; the terminal state machine mirrors the unix
 * _unix_load_pipeline_t. */
typedef struct {
    refcounter_t refcounter;
    http_response_t* response;
    /* Snapshot of response->connection at setup time. Held so the terminal
       can release the setup's connection reference even if another teardown
       has since detached the response (response->connection == NULL). */
    http_connection_t* connection;
    readable_off_stream_t* rs;
    readable_descriptor_t* desc;
    ori_t* ori;
    size_t tuples_total;   /* ceil(final_byte / block_size) - offset tuples */
    size_t tuples_loaded;  /* maintained from load_tuple_event payloads */
    size_t tuples_skipped; /* maintained from load_tuple_event payloads */
    uint8_t failed;           /* an error_event fired on desc or rs */
    uint8_t terminal_written; /* guards the single terminal line */
    /* desc_done ensures desc contributes exactly one pipeline deref,
       whether close or error fires first (stream_deactivate emits both). */
    uint8_t desc_done;
} off_load_pipeline_t;

static void _load_pipeline_free(off_load_pipeline_t* pipeline) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
}

/* The single ndjson terminal line and the end of the response. The
   connection lifetime mirrors http_response_pipe's _pipe_on_close: the load
   setup took one reference each on the response and connection, released
   here once the response is ended (keep_alive is 0, so end() closes the
   socket — the body is close-delimited). */
static void _load_pipeline_terminal(off_load_pipeline_t* pipeline) {
    if (pipeline->terminal_written) {
        return;
    }
    pipeline->terminal_written = 1;
    uint8_t status = CLIENT_API_LOAD_STATUS_LOADED;
    if (pipeline->failed) {
        status = CLIENT_API_LOAD_STATUS_FAILED;
    } else if (pipeline->tuples_total > 0 && pipeline->tuples_loaded == 0) {
        status = CLIENT_API_LOAD_STATUS_FAILED;
    } else if (pipeline->tuples_skipped > 0) {
        status = CLIENT_API_LOAD_STATUS_PARTIAL;
    }
    char line[128];
    int line_len = snprintf(line, sizeof(line),
                            "{\"status\":\"%s\",\"tuples_loaded\":%zu,\"tuples_total\":%zu}\n",
                            status == CLIENT_API_LOAD_STATUS_FAILED ? "failed" :
                            status == CLIENT_API_LOAD_STATUS_PARTIAL ? "partial" : "loaded",
                            pipeline->tuples_loaded, pipeline->tuples_total);
    http_response_t* response = pipeline->response;
    http_connection_t* connection = pipeline->connection;
    pipeline->response = NULL;
    pipeline->connection = NULL;
    /* Client-vanished guard: whenever another teardown detached the response
       (response->connection == NULL), its write path is dead — skip the
       final line and just drop this pipeline's setup references, exactly
       like the live path releases them below. */
    if (response->connection != NULL) {
        http_response_write(response, line, (size_t)line_len);
        http_response_end(response);
    }
    response->connection = NULL;
    http_response_destroy(response);
    http_connection_destroy(connection);
}

/* The load_tuple_event payload is CONSUME-transferred: the notify machinery
 * holds the reference and destroys it after dispatch. Copy the counters,
 * never destroy or dereference the payload here. */
static void _load_pipeline_on_tuple_loaded(void* ctx, void* data) {
    off_load_pipeline_t* pipeline = (off_load_pipeline_t*)ctx;
    load_tuple_payload_t* progress = (load_tuple_payload_t*)data;
    if (progress != NULL) {
        pipeline->tuples_loaded = progress->tuples_loaded;
        pipeline->tuples_skipped = progress->tuples_skipped;
    }
    char line[64];
    int line_len = snprintf(line, sizeof(line),
                            "{\"tuples_loaded\":%zu,\"tuples_total\":%zu}\n",
                            pipeline->tuples_loaded, pipeline->tuples_total);
    /* The terminal tears the response down (exactly once) — never touch it
       past that point. */
    if (!pipeline->terminal_written && pipeline->response->connection != NULL) {
        http_response_write(pipeline->response, line, (size_t)line_len);
    }

    /* Pipeline-driven completion: a skipped tuple never renders and never
       advances sent_bytes, so the render path cannot close the stream once
       the tally completes. close_event — not this tally — is the single
       terminal trigger; request_close is idempotent, so the all-loaded path
       (already closed by render) is unaffected. */
    if (pipeline->tuples_loaded + pipeline->tuples_skipped >= pipeline->tuples_total) {
        readable_off_stream_request_close(pipeline->rs);
    }
}

static void _load_pipeline_on_tuple(void* ctx, void* data) {
    off_load_pipeline_t* pipeline = (off_load_pipeline_t*)ctx;
    tuple_t* tuple = (tuple_t*)data;
    readable_off_stream_write(pipeline->rs, tuple);
}

static void _load_pipeline_on_rs_close(void* ctx, void* unused) {
    (void)unused;
    off_load_pipeline_t* pipeline = (off_load_pipeline_t*)ctx;
    readable_off_stream_t* rs = pipeline->rs;
    /* Tally-before-close ordering (Task 2) guarantees the tuple counters
       were updated before this terminal line is written. */
    _load_pipeline_terminal(pipeline);
    int is_zero = refcounter_dereference_is_zero((refcounter_t*)pipeline);
    stream_deferred_deref((stream_t*)rs);
    if (is_zero) {
        _load_pipeline_free(pipeline);
    }
}

static void _load_pipeline_on_rs_error(void* ctx, void* error) {
    (void)error;
    off_load_pipeline_t* pipeline = (off_load_pipeline_t*)ctx;
    pipeline->failed = 1;
    /* Deactivating queues rs close_event right after this error; that close
       writes the single terminal line and tears down the response. Only
       deactivate when the error did not already come from one —
       stream_deactivate re-notifies error_event UNCONDITIONALLY, so an
       unguarded re-deactivate here would loop forever. */
    if (!pipeline->rs->stream.is_deactivated) {
        stream_deactivate((stream_t*)pipeline->rs, NULL);
    }
}

static void _load_pipeline_on_desc_close(void* ctx, void* unused) {
    (void)unused;
    off_load_pipeline_t* pipeline = (off_load_pipeline_t*)ctx;
    readable_descriptor_t* desc = pipeline->desc;
    int is_zero = 0;
    if (!pipeline->desc_done) {
        pipeline->desc_done = 1;
        is_zero = refcounter_dereference_is_zero((refcounter_t*)pipeline);
    }
    /* desc close while the final tuple is still in flight must NOT end the
       response — only the rs close_event does that. */
    stream_deferred_deref((stream_t*)desc);
    if (is_zero) {
        _load_pipeline_free(pipeline);
    }
}

static void _load_pipeline_on_desc_error(void* ctx, void* error) {
    (void)error;
    off_load_pipeline_t* pipeline = (off_load_pipeline_t*)ctx;
    pipeline->failed = 1;
    /* Mark failed and end the load via the rs close path (exactly one
       terminal); DESC_ERROR implies DESC_CLOSE right after. No desc
       re-deactivate here — the error already fired FROM a deactivated
       descriptor, and re-deactivating would re-queue error_event. */
    if (!pipeline->rs->stream.is_deactivated) {
        stream_deactivate((stream_t*)pipeline->rs, NULL);
    }
    int is_zero = 0;
    if (!pipeline->desc_done) {
        pipeline->desc_done = 1;
        is_zero = refcounter_dereference_is_zero((refcounter_t*)pipeline);
    }
    if (is_zero) {
        _load_pipeline_free(pipeline);
    }
}

static void _setup_load_pipeline(http_response_t* response, scheduler_pool_t* pool,
                                 block_cache_t* bc, tuple_cache_t* tc, ori_t* stream_ori,
                                 size_t descriptor_pad, network_t* network) {
    readable_off_stream_t* rs = readable_off_stream_create_ex(pool, bc, tc, stream_ori,
                                                              descriptor_pad, network, 1);
    readable_descriptor_t* desc = readable_descriptor_create(pool, bc, stream_ori,
                                                             descriptor_pad, network);

    off_load_pipeline_t* pipeline = get_clear_memory(sizeof(off_load_pipeline_t));
    pipeline->desc = desc;
    pipeline->rs = rs;
    pipeline->response = response;
    pipeline->connection = response->connection;
    pipeline->ori = stream_ori;
    size_t block_size = off_block_size_for_type(stream_ori->block_type);
    pipeline->tuples_total = (stream_ori->final_byte / block_size) +
                             ((stream_ori->final_byte % block_size) > 0 ? 1 : 0) -
                             (stream_ori->file_offset / block_size);
    /* Two derefs total: one for rs-done (close), one for desc-done (close or
       error, whichever fires first — guarded by desc_done). */
    refcounter_init((refcounter_t*)pipeline);
    refcounter_reference((refcounter_t*)pipeline);

    stream_subscribe((stream_t*)rs, load_tuple_event, pipeline,
                     (void (*)(void*, void*))_load_pipeline_on_tuple_loaded, NULL);
    stream_once((stream_t*)rs, close_event, pipeline,
                (void (*)(void*, void*))_load_pipeline_on_rs_close, NULL);
    stream_subscribe((stream_t*)rs, error_event, pipeline,
                     (void (*)(void*, void*))_load_pipeline_on_rs_error, NULL);
    stream_once((stream_t*)desc, close_event, pipeline,
                (void (*)(void*, void*))_load_pipeline_on_desc_close, NULL);
    stream_once((stream_t*)desc, error_event, pipeline,
                (void (*)(void*, void*))_load_pipeline_on_desc_error, NULL);
    /* The descriptor feeds tuples into the off_stream; in load mode the
       stream tallies/skips them instead of rendering file data. */
    stream_subscribe((stream_t*)desc, data_event, pipeline,
                     (void (*)(void*, void*))_load_pipeline_on_tuple, NULL);

    /* Unknown-length streaming body: no Content-Length is possible (the
       terminal status and line count settle only at the end), so the
       response is close-delimited and the connection is closed at the
       terminal. Hold the response/connection for the duration, exactly like
       http_response_pipe does. */
    response->unknown_length = 1;
    response->keep_alive = 0;
    response->is_piped = 1;
    response->connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response);
    refcounter_reference((refcounter_t*)response->connection);

    readable_descriptor_push(desc);
}

/* GET ...?load=1 — pull the file's blocks into the block cache and stream
   tuple-level progress as application/x-ndjson. Same ORI construction as
   the GET data path, minus content-length framing. */
static void _off_load_stream(http_request_t* request, http_response_t* response,
                             off_routes_context_t* ctx, off_url_t* url) {
    size_t file_size = url->stream_length;
    const char* range_header = http_request_header(request, "Range");
    range_request_t range = parse_range_header(range_header, file_size);

    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "application/x-ndjson");
    http_response_set_header(response, "Cache-Control", "no-store");

    if (range_header != NULL && !range.valid) {
        http_response_set_status(response, HTTP_STATUS_RANGE_NOT_SATISFIABLE);
        char cr_str[64];
        snprintf(cr_str, sizeof(cr_str), "bytes */%zu", file_size);
        http_response_set_header(response, "Content-Range", cr_str);
        http_response_end(response);
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
        stream_ori->file_offset = range.start;
        stream_ori->final_byte = range.end + 1;
    }

    _setup_load_pipeline(response, ctx->pool, ctx->bc, ctx->tc, stream_ori, 32, ctx->network);
}

/* ---- Async GET handler state ---- */

typedef enum {
    OFF_GET_RESOLVE_DIR,      /* Resolving directory path */
    OFF_GET_RESOLVE_INDEX,    /* Resolving index.html in .ofd directory */
    OFF_GET_FETCH_RAW_OFD,    /* Fetching raw OFD bytes (?ofd=raw) */
    OFF_GET_FETCH_DIR         /* Fetching directory bytes from block cache on miss */
} off_get_phase_t;

typedef struct {
    actor_t actor;
    off_routes_context_t* ctx;
    http_response_t* response;
    http_connection_t* connection;
    off_url_t* url;
    char* resolve_path;
    off_get_phase_t phase;
    off_get_phase_t resolve_phase;  /* Original RESOLVE_* phase before fetch */
    /* Used when the in-memory OFD cache misses and we must stream the directory
       CBOR from the block cache via its descriptor hash. */
    buffer_t* dir_buffer;
    readable_off_stream_t* dir_rs;
    readable_descriptor_t* dir_desc;
    ori_t* dir_ori;
    uint8_t dir_fetch_error;
    uint8_t dir_raw_return;    /* 1 = return raw CBOR, 0 = decode and resolve */
} off_get_state_t;

static void _off_get_state_destroy(off_get_state_t* state) {
    if (state->url) off_url_destroy(state->url);
    if (state->resolve_path) free(state->resolve_path);
    if (state->dir_buffer) buffer_destroy(state->dir_buffer);
    if (state->dir_rs) stream_deferred_deref((stream_t*)state->dir_rs);
    if (state->dir_desc) stream_deferred_deref((stream_t*)state->dir_desc);
    if (state->dir_ori) DESTROY(state->dir_ori, ori);
    http_connection_t* conn = state->connection;
    http_response_destroy(state->response);
    if (conn) http_connection_destroy(conn);
    atomic_fetch_or(&state->actor.flags, ACTOR_FLAG_DESTROY);
    actor_destroy(&state->actor);
    scheduler_pool_defer_cleanup(state->ctx->pool, state, free);
}

/* ---- Directory block-cache fallback ----
 * When the in-memory OFD cache misses, treat the directory CBOR as a regular
 * file stream (descriptor_hash -> blocks), accumulate the bytes, decode the
 * OFD, cache it, and continue resolving the requested internal path. */

static void _dir_fetch_data(void* ctx, void* data) {
    off_get_state_t* state = (off_get_state_t*)ctx;
    buffer_t* chunk = (buffer_t*)data;
    if (state->dir_fetch_error || !chunk || chunk->size == 0) return;

    if (state->dir_buffer == NULL) {
        state->dir_buffer = buffer_create_with_capacity(0, chunk->size);
    }
    size_t old_size = state->dir_buffer->size;
    buffer_ensure_capacity(state->dir_buffer, old_size + chunk->size);
    memcpy(state->dir_buffer->data + old_size, chunk->data, chunk->size);
    state->dir_buffer->size = old_size + chunk->size;
}

static void _dir_fetch_error_handler(void* ctx, void* unused) {
    (void)unused;
    off_get_state_t* state = (off_get_state_t*)ctx;
    if (state->dir_fetch_error) return;
    state->dir_fetch_error = 1;
    http_response_set_status(state->response, 404);
    http_response_end(state->response);
    _off_get_state_destroy(state);
}

static void _dir_fetch_finished(off_get_state_t* state);

static void _dir_fetch_close(void* ctx, void* unused) {
    (void)unused;
    off_get_state_t* state = (off_get_state_t*)ctx;
    if (state->dir_fetch_error) return;
    _dir_fetch_finished(state);
}

static void _dir_fetch_resolve_and_serve(off_get_state_t* state);

static void _dir_fetch_finished(off_get_state_t* state) {
    buffer_t* buf = state->dir_buffer;
    state->dir_buffer = NULL;

    if (buf == NULL || buf->size == 0) {
        if (buf) buffer_destroy(buf);
        http_response_set_status(state->response, 404);
        http_response_end(state->response);
        _off_get_state_destroy(state);
        return;
    }

    if (state->dir_raw_return) {
        /* ?ofd=raw: return the CBOR bytes directly but also cache the decoded
           OFD for future requests. */
        ofd_t* ofd = ofd_decode(buf);
        if (ofd != NULL) {
            ofd_cache_put(state->ctx->ofd_cache, state->url->file_hash, ofd);
        }
        http_response_set_header(state->response, "Content-Type", "application/cbor");
        http_response_write(state->response, (const char*)buf->data, buf->size);
        http_response_end(state->response);
        buffer_destroy(buf);
        _off_get_state_destroy(state);
        return;
    }

    ofd_t* ofd = ofd_decode(buf);
    buffer_destroy(buf);
    if (ofd == NULL) {
        http_response_set_status(state->response, 404);
        http_response_end(state->response);
        _off_get_state_destroy(state);
        return;
    }

    ofd_cache_put(state->ctx->ofd_cache, state->url->file_hash, ofd);

    /* Now that the OFD is cached, continue resolving the original request. */
    _dir_fetch_resolve_and_serve(state);
}

static void _dir_fetch_resolve_and_serve(off_get_state_t* state) {
    if (state->resolve_phase == OFF_GET_RESOLVE_INDEX) {
        ofd_cache_resolve(state->ctx->ofd_cache, state->url->file_hash, "index.html", &state->actor);
        return;
    }

    if (state->resolve_phase == OFF_GET_RESOLVE_DIR) {
        ofd_cache_resolve(state->ctx->ofd_cache, state->url->file_hash,
                          state->resolve_path, &state->actor);
        return;
    }

    /* Should not happen */
    http_response_set_status(state->response, 404);
    http_response_end(state->response);
    _off_get_state_destroy(state);
}

static void _dir_fetch_on_tuple(void* ctx, void* data) {
    off_get_state_t* state = (off_get_state_t*)ctx;
    tuple_t* tuple = (tuple_t*)data;
    if (state->dir_rs) {
        readable_off_stream_write(state->dir_rs, tuple);
    }
}

static void _dir_fetch_desc_close(void* ctx, void* unused) {
    (void)unused;
    off_get_state_t* state = (off_get_state_t*)ctx;
    if (state->dir_desc) {
        stream_deferred_deref((stream_t*)state->dir_desc);
        state->dir_desc = NULL;
    }
}

static void _dir_fetch_start(off_get_state_t* state) {
    if (state->phase != OFF_GET_FETCH_DIR) {
        state->resolve_phase = state->phase;
    }
    state->phase = OFF_GET_FETCH_DIR;

    ori_t* dir_ori = ori_create(state->url->stream_length);
    dir_ori->descriptor_hash = buffer_copy(state->url->descriptor_hash);
    dir_ori->file_hash = buffer_copy(state->url->file_hash);
    dir_ori->file_name = strdup(state->url->file_name);
    dir_ori->block_type = standard;
    dir_ori->tuple_size = 3;
    state->dir_ori = dir_ori;

    readable_off_stream_t* rs = readable_off_stream_create(
        state->ctx->pool, state->ctx->bc, state->ctx->tc, dir_ori, 32, state->ctx->network);
    readable_descriptor_t* desc = readable_descriptor_create(
        state->ctx->pool, state->ctx->bc, dir_ori, 32, state->ctx->network);
    state->dir_rs = rs;
    state->dir_desc = desc;

    /* Reference streams so they stay alive until we're done. */
    refcounter_reference((refcounter_t*)rs);
    refcounter_reference((refcounter_t*)desc);

    stream_subscribe((stream_t*)desc, data_event, state,
                     (void (*)(void*, void*))_dir_fetch_on_tuple, NULL);
    stream_once((stream_t*)desc, close_event, state,
                (void (*)(void*, void*))_dir_fetch_desc_close, NULL);
    stream_once((stream_t*)desc, error_event, state,
                (void (*)(void*, void*))_dir_fetch_error_handler, NULL);
    stream_subscribe((stream_t*)rs, data_event, state,
                     (void (*)(void*, void*))_dir_fetch_data, NULL);
    stream_once((stream_t*)rs, close_event, state,
                (void (*)(void*, void*))_dir_fetch_close, NULL);
    stream_once((stream_t*)rs, error_event, state,
                (void (*)(void*, void*))_dir_fetch_error_handler, NULL);

    readable_descriptor_push(desc);
}

static void _send_stream_response(http_response_t* response, off_routes_context_t* ctx,
                                   ori_t* file_ori, const char* content_type) {
    size_t file_size = file_ori->final_byte;
    const char* range_header = NULL;
    if (response->connection != NULL) {
      range_header = http_request_header(response->connection->request, "Range");
    }
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
    stream_ori->descriptor_hash = file_ori->descriptor_hash
        ? buffer_copy(file_ori->descriptor_hash)
        : buffer_copy(file_ori->file_hash);
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

    _setup_stream_pipeline(response, ctx->pool, ctx->bc, ctx->tc, stream_ori, 32, ctx->network);
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

    /* ?load=1 (or bare ?load) — cache-load flow: pull the file's blocks into
       the block cache without serving file data; progress streams as ndjson.
       Checked BEFORE the directory branch: v1 rejects directory ORIs with a
       clear 400 (parity with the socket LOAD handler). */
    if (_query_has_param(request->query_string, "load")) {
        if (url->content_type != NULL &&
            strstr(url->content_type, "offsystem/directory") != NULL) {
            http_response_set_status(response, 400);
            http_response_write(response, "Load requires a file ORI, not a directory",
                                strlen("Load requires a file ORI, not a directory"));
            http_response_end(response);
            off_url_destroy(url);
            return;
        }
        _off_load_stream(request, response, ctx, url);
        off_url_destroy(url);
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

        /* Directory resolve — use async resolver.
           The URL file_name may be "dir.ofd" (serve index.html) or
           "dir.ofd/<internal/path>" (resolve the internal path). Split on the
           first slash to separate the OFD name from the path inside it. */
        const char* file_name = url->file_name;
        size_t name_len = strlen(file_name);
        const char* subpath = NULL;
        char* slash = strchr(file_name, '/');
        if (slash != NULL) {
            name_len = (size_t)(slash - file_name);
            subpath = slash + 1;
        }

        uint8_t is_bare_ofd = (name_len > 4 &&
                               strncmp(file_name + name_len - 4, ".ofd", 4) == 0 &&
                               (subpath == NULL || subpath[0] == '\0'));

        /* Redirect bare directory URLs to a trailing slash so browser relative
           links (css/style.css, js/deck.js, assets/...) resolve inside the
           directory instead of above it. Preserve any query string except
           the raw OFD request. */
        if (is_bare_ofd &&
            (!request->query_string || strstr(request->query_string, "ofd=raw") == NULL) &&
            request->path && request->path[0] &&
            request->path[strlen(request->path) - 1] != '/') {
            size_t path_len = strlen(request->path);
            size_t query_len = request->query_string ? strlen(request->query_string) : 0;
            char* location = get_clear_memory(path_len + 2 + (query_len ? query_len + 1 : 0));
            memcpy(location, request->path, path_len);
            location[path_len] = '/';
            if (query_len) {
                location[path_len + 1] = '?';
                memcpy(location + path_len + 2, request->query_string, query_len);
            }
            http_response_set_status(response, HTTP_STATUS_FOUND);
            http_response_set_header(response, "Location", location);
            http_response_set_header(response, "Content-Type", "text/plain");
            http_response_write(response, "Redirect", 8);
            http_response_end(response);
            off_url_destroy(url);
            free(location);
            return;
        }

        /* ?ofd=raw — serve raw OFD bytes. Try the in-memory cache first; on
           miss stream the directory CBOR from the block cache via its
           descriptor hash and return it directly. */
        if (request->query_string && strstr(request->query_string, "ofd=raw") != NULL) {
            state->phase = OFF_GET_FETCH_RAW_OFD;
            state->resolve_phase = OFF_GET_FETCH_RAW_OFD;
            ofd_cache_get(ctx->ofd_cache, url->file_hash, &state->actor);
            return;
        }

        if (is_bare_ofd) {
            /* Bare .ofd URL — try index.html first */
            state->phase = OFF_GET_RESOLVE_INDEX;
            state->resolve_phase = OFF_GET_RESOLVE_INDEX;
            ofd_cache_resolve(ctx->ofd_cache, url->file_hash, "index.html", &state->actor);
            return;
        }

        /* Resolve the internal path within the OFD */
        state->phase = OFF_GET_RESOLVE_DIR;
        state->resolve_phase = OFF_GET_RESOLVE_DIR;
        state->resolve_path = strdup(subpath != NULL ? subpath : file_name);
        ofd_cache_resolve(ctx->ofd_cache, url->file_hash,
                          subpath != NULL ? subpath : file_name, &state->actor);
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

    _setup_stream_pipeline(response, ctx->pool, ctx->bc, ctx->tc, stream_ori, 32, ctx->network);
    off_url_destroy(url);
}

static void _off_get_dispatch(void* state, message_t* msg) {
    off_get_state_t* ctx = (off_get_state_t*)state;

    switch (msg->type) {
        case OFD_CACHE_GET_RESULT: {
            /* ?ofd=raw — OFD cache hit, encode and send */
            if (ctx->phase == OFF_GET_FETCH_RAW_OFD) {
                ofd_cache_get_result_payload_t* result = (ofd_cache_get_result_payload_t*)msg->payload;

                if (result->ofd != NULL) {
                    buffer_t* encoded = ofd_encode(result->ofd);
                    http_response_set_header(ctx->response, "Content-Type", "application/cbor");
                    http_response_write(ctx->response, (const char*)encoded->data, encoded->size);
                    http_response_end(ctx->response);
                    buffer_destroy(encoded);
                    /* Null after transfer — payload_destroy handles cleanup */
                    result->ofd = NULL;
                    result->hash = NULL;
                    _off_get_state_destroy(ctx);
                    return;
                }

                /* Cache miss — stream the directory CBOR from the block cache
                   using its descriptor hash. */
                ctx->dir_raw_return = 1;
                _dir_fetch_start(ctx);
                return;
            }
            break;
        }

        case OFD_CACHE_RESOLVE_RESULT: {
            ofd_resolve_result_t* result = (ofd_resolve_result_t*)msg->payload;
            /* payload_destroy cleans up hash, path, and ori.
               Transfer ori ownership by clearing the pointer before destroy. */

            if (ctx->resolve_phase == OFF_GET_RESOLVE_INDEX) {
                if (result->ori != NULL) {
                    _send_stream_response(ctx->response, ctx->ctx, result->ori, "text/html");
                    result->ori = NULL;
                    _off_get_state_destroy(ctx);
                    return;
                }

                if (ctx->phase != OFF_GET_FETCH_DIR) {
                    /* First miss — fetch directory bytes from the block cache,
                       decode, cache, and try again. */
                    _dir_fetch_start(ctx);
                    return;
                }

                /* Already fetched once and still missing — give up. */
                http_response_set_status(ctx->response, 404);
                http_response_end(ctx->response);
                _off_get_state_destroy(ctx);
                return;
            }

            if (ctx->resolve_phase == OFF_GET_RESOLVE_DIR) {
                if (result->ori != NULL) {
                    const char* mime = mime_type_from_extension(ctx->resolve_path);
                    _send_stream_response(ctx->response, ctx->ctx, result->ori, mime);
                    result->ori = NULL;
                } else if (ctx->phase != OFF_GET_FETCH_DIR) {
                    /* First miss — fetch directory bytes from the block cache,
                       decode, cache, and try again. */
                    _dir_fetch_start(ctx);
                    return;
                } else {
                    /* Already fetched once and path still missing — give up. */
                    http_response_set_status(ctx->response, 404);
                    http_response_end(ctx->response);
                }
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
    http_request_t* request;
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
    ofd_cache_t* ofd_cache;
    block_cache_t* bc;
    buffer_t* upload_data;
    uint8_t temporary;
    uint64_t bytes_received;  /* actual bytes received in the streaming body, bounded by stream_length */
    uint8_t stream_exceeded;  /* set when bytes_received crosses stream_length; further chunks are dropped */
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

    /* Temporary puts: register the completed representation so the ephemeral
       registry tracks it for respiration exclusion and servability. */
    if (put_ctx->temporary && put_ctx->descriptor_hash != NULL &&
        put_ctx->bc != NULL && put_ctx->bc->registry != NULL) {
        ephemeral_registry_add(put_ctx->bc->registry, put_ctx->descriptor_hash);
    }

    /* Populate OFD cache on directory upload — decode from saved upload data.
       Cannot use block_cache because block hashes differ from file_hash
       (blocks are padded to block_size before hashing). */
    if (put_ctx->content_type != NULL &&
        strstr(put_ctx->content_type, "offsystem/directory") != NULL &&
        put_ctx->ofd_cache != NULL && put_ctx->file_hash != NULL) {
        if (put_ctx->upload_data != NULL) {
            ofd_t* ofd = ofd_decode(put_ctx->upload_data);
            if (ofd != NULL) {
                ofd_cache_put(put_ctx->ofd_cache, put_ctx->file_hash, ofd);
            }
        }
    }

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
    if (put_ctx->upload_data != NULL) {
        buffer_destroy(put_ctx->upload_data);
    }
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

/* Parse the optional `recycle-ephemeral` header: `commit` clears source
   blocks to permanent (and announces them), `propagate` acquires the
   consuming representation's claim. Absent or unknown values mean the
   default mode — a fetch-time hard error on an ephemeral source. */
static recycle_ephemeral_e _parse_recycle_ephemeral_header(http_request_t* request) {
    const char* header = http_request_header(request, "recycle-ephemeral");
    if (header == NULL) return RECYCLE_EPHEMERAL_NONE;
    if (strcmp(header, "commit") == 0) return RECYCLE_EPHEMERAL_COMMIT;
    if (strcmp(header, "propagate") == 0) return RECYCLE_EPHEMERAL_PROPAGATE;
    return RECYCLE_EPHEMERAL_NONE;
}

static void _parse_recycler_header(const char* recycler_header, vec_ori_t* oris) {
    vec_init(oris);
    if (recycler_header == NULL || recycler_header[0] == '\0') return;

    const char* cursor = recycler_header;
    while (*cursor) {
        const char* start = strchr(cursor, '"');
        if (start == NULL) break;
        start++;
        const char* end = strchr(start, '"');
        if (end == NULL) break;

        size_t url_len = (size_t)(end - start);
        if (url_len > 0 && url_len < 4096) {
            char* url_str = get_memory(url_len + 1);
            memcpy(url_str, start, url_len);
            url_str[url_len] = '\0';

            off_url_t* parsed = off_url_parse(url_str);
            if (parsed != NULL) {
                ori_t* ori = ori_create(parsed->stream_length);
                ori->descriptor_hash = buffer_copy(parsed->descriptor_hash);
                ori->file_hash = buffer_copy(parsed->file_hash);
                ori->file_name = strdup(parsed->file_name);
                ori->block_type = standard;
                ori->tuple_size = 3;
                vec_push(oris, ori);
                off_url_destroy(parsed);
            }
            free(url_str);
        }
        cursor = end + 1;
    }
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

    const char* recycler_header = http_request_header(request, "recycler");
    const char* temporary_header = http_request_header(request, "temporary");
    uint8_t is_temporary = (temporary_header != NULL && strcmp(temporary_header, "true") == 0);

    /* Resolve tuple_size from the optional `tuple-size` header, default 3,
     * and enforce the configured max_tuple_size bound. */
    const char* tuple_size_header = http_request_header(request, "tuple-size");
    size_t tuple_size = 3;
    if (tuple_size_header != NULL) {
        long parsed = atol(tuple_size_header);
        if (parsed > 0) {
            tuple_size = (size_t)parsed;
        }
    }
    if (ctx->max_tuple_size != 0 && tuple_size > ctx->max_tuple_size) {
        http_response_set_status(response, 400);
        http_response_write(response, "tuple-size exceeds configured max_tuple_size", 44);
        http_response_end(response);
        if (upload_data != NULL) {
            buffer_destroy(upload_data);
        }
        return;
    }

    /* Pre-flight space check: reject if the cache cannot fit the estimated bytes */
    size_t required = writeable_off_stream_estimate_required_bytes(
        stream_length, tuple_size, /*descriptor_pad=*/32);
    if (block_cache_can_fit(ctx->bc, required) != CACHE_FIT_OK) {
        http_response_set_status(response, 500);
        http_response_write(response, "cache full: configure larger max_capacity_bytes", 48);
        http_response_end(response);
        if (upload_data != NULL) {
            buffer_destroy(upload_data);
        }
        return;
    }

    vec_block_recipe_t recipes;
    vec_init(&recipes);

    recycle_ephemeral_e recycle_mode = _parse_recycle_ephemeral_header(request);
    vec_ori_t recycler_oris;
    _parse_recycler_header(recycler_header, &recycler_oris);
    if (recycle_mode != RECYCLE_EPHEMERAL_NONE && recycler_oris.length == 0) {
        /* An explicit recycle mode without a recycler makes no sense. */
        http_response_set_status(response, 400);
        http_response_write(response, "recycle-ephemeral requires a recycler", 37);
        http_response_end(response);
        vec_deinit(&recycler_oris);
        if (upload_data != NULL) {
            buffer_destroy(upload_data);
        }
        return;
    }
    if (recycler_oris.length > 0) {
        /* Temporary+recycler is legal now: an ephemeral put behaves as
           propagate (the recipe claims its source blocks at fetch time),
           regardless of the explicit recycle mode. */
        recycler_recipe_t* recycler = recycler_recipe_create(ctx->pool, ctx->bc, standard,
                                                              recycler_oris, ctx->network,
                                                              is_temporary, recycle_mode);
        vec_push(&recipes, (block_recipe_t*)recycler);
    }

    new_blocks_recipe_t* recipe = new_blocks_recipe_create(ctx->pool, ctx->bc, standard);
    vec_push(&recipes, (block_recipe_t*)recipe);

    writeable_off_stream_t* ws = writeable_off_stream_create(
        ctx->pool, ctx->bc, ctx->tc, standard, tuple_size, 32, recipes, NULL);

    writeable_descriptor_t* desc = writeable_descriptor_create(
        ctx->pool, ctx->bc, standard, 32, tuple_size, stream_length, NULL);

    if (is_temporary) {
        writeable_off_stream_set_ephemeral(ws, 1);
        writeable_descriptor_set_ephemeral(desc, 1);
    }

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
    put_ctx->ofd_cache = ctx->ofd_cache;
    put_ctx->bc = ctx->bc;
    put_ctx->upload_data = upload_data;  /* saved for OFD cache population */
    put_ctx->temporary = is_temporary;

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
        /* Don't buffer_destroy here — put_ctx->upload_data still needs the reference */
    }

    writeable_off_stream_finalize(ws);
}

static void _put_on_request_data(void* ctx, void* data) {
    put_context_t* put_ctx = (put_context_t*)ctx;
    buffer_t* chunk = (buffer_t*)data;

    /* Defensive sentinel: historically a heap-corruption bug left buffer->data
       NULL in the streamed-PUT body handler (see docs/OPERATIONS.md "Known
       issues"). The root cause is under investigation; guard the entry so a
       corrupted chunk logs and drops instead of dereferencing NULL inside
       writeable_off_stream_write / buffer_concat. */
    if (chunk == NULL || chunk->data == NULL) {
        log_error("_put_on_request_data: NULL chunk or chunk->data (chunk=%p data=%p size=%zu) — heap corruption suspected",
                 (void*)chunk, chunk ? (void*)chunk->data : NULL,
                 chunk ? (size_t)chunk->size : (size_t)0);
        return;
    }

    /* Bound the actual streamed bytes against the client-declared stream-length.
     * The pre-flight headers-complete handler already rejects stream-length
     * values above OFFS_MAX_STREAM_LENGTH, so a malicious client cannot declare
     * a tiny stream-length and then stream gigabytes through the pipeline —
     * once bytes_received crosses stream_length, subsequent chunks are dropped
     * (stream_exceeded gates further writes) so the writeable_off_stream only
     * ever stores up to the declared stream_length bytes. */
    if (put_ctx->stream_exceeded) {
        return;
    }
    put_ctx->bytes_received += chunk->size;
    if (put_ctx->bytes_received > put_ctx->stream_length) {
        put_ctx->stream_exceeded = 1;
        log_warn("_put_on_request_data: streamed body exceeded declared stream-length (%zu > %zu); dropping excess",
                 (size_t)put_ctx->bytes_received, put_ctx->stream_length);
        return;
    }

    writeable_off_stream_write(put_ctx->ws, chunk);

    /* Accumulate raw data for OFD cache population on directory uploads */
    if (put_ctx->content_type != NULL &&
        strstr(put_ctx->content_type, "offsystem/directory") != NULL) {
        if (put_ctx->upload_data == NULL) {
            put_ctx->upload_data = buffer_copy(chunk);
        } else {
            buffer_t* concatenated = buffer_concat(put_ctx->upload_data, chunk);
            buffer_destroy(put_ctx->upload_data);
            put_ctx->upload_data = concatenated;
        }
    }
}

static void _put_on_request_close(void* ctx, void* unused) {
    (void)unused;
    put_context_t* put_ctx = (put_context_t*)ctx;
    writeable_off_stream_finalize(put_ctx->ws);
    /* The request stream has delivered all its body chunks and is now closed;
       release the pipeline's reference. Use a deferred deref so the request is
       freed after this dispatch returns, not while _stream_notify_dispatch is
       still iterating the request's handler list on the request actor thread. */
    stream_deferred_deref((stream_t*)put_ctx->request);
    put_ctx->request = NULL;
}

static void _set_cors_headers(http_response_t* response) {
    http_response_set_header(response, "Access-Control-Allow-Origin", "*");
    http_response_set_header(response, "Access-Control-Expose-Headers",
                             "Content-Type, Content-Range, Content-Length");
}

static int _off_put_headers_complete_error(http_response_t* response, int status, const char* message) {
    _set_cors_headers(response);
    http_response_set_status(response, status);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, message, strlen(message));
    http_response_end(response);
    return 1;
}

static int _off_put_headers_complete(http_connection_t* connection,
                                      http_request_t* request,
                                      http_response_t* response) {
    _set_cors_headers(response);

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
        return _off_put_headers_complete_error(response, 400,
            "Missing required headers: type, file-name, stream-length");
    }

    if (validate_content_type(type) != 0) {
        return _off_put_headers_complete_error(response, 400, "Invalid content type");
    }
    if (validate_file_name(file_name) != 0) {
        return _off_put_headers_complete_error(response, 400, "Invalid file name");
    }

    size_t stream_length = (size_t)atol(stream_length_str);
    if (stream_length == 0 || stream_length > OFFS_MAX_STREAM_LENGTH) {
        return _off_put_headers_complete_error(response, 400, "Invalid stream length");
    }

    // Get the off_routes_context from the matched route
    off_routes_context_t* routes_ctx = (off_routes_context_t*)connection->streaming_route->user_data;

    const char* recycler_header = http_request_header(request, "recycler");
    const char* temporary_header = http_request_header(request, "temporary");
    uint8_t is_temporary = (temporary_header != NULL && strcmp(temporary_header, "true") == 0);

    /* Resolve tuple_size from the optional `tuple-size` header, default 3,
     * and enforce the configured max_tuple_size bound. */
    const char* tuple_size_header = http_request_header(request, "tuple-size");
    size_t tuple_size = 3;
    if (tuple_size_header != NULL) {
        long parsed = atol(tuple_size_header);
        if (parsed > 0) {
            tuple_size = (size_t)parsed;
        }
    }
    if (routes_ctx->max_tuple_size != 0 && tuple_size > routes_ctx->max_tuple_size) {
        return _off_put_headers_complete_error(response, 400,
            "tuple-size exceeds configured max_tuple_size");
    }

    /* Pre-flight space check: reject immediately if the cache cannot fit the
     * estimated bytes. Returning the error from the streaming handler avoids
     * buffering a chunked body only to discard it later. */
    size_t required = writeable_off_stream_estimate_required_bytes(
        stream_length, tuple_size, /*descriptor_pad=*/32);
    if (block_cache_can_fit(routes_ctx->bc, required) != CACHE_FIT_OK) {
        return _off_put_headers_complete_error(response, 500,
            "cache full: configure larger max_capacity_bytes");
    }

    vec_block_recipe_t recipes;
    vec_init(&recipes);

    recycle_ephemeral_e recycle_mode = _parse_recycle_ephemeral_header(request);
    vec_ori_t recycler_oris;
    _parse_recycler_header(recycler_header, &recycler_oris);
    if (recycle_mode != RECYCLE_EPHEMERAL_NONE && recycler_oris.length == 0) {
        /* An explicit recycle mode without a recycler makes no sense. */
        vec_deinit(&recycler_oris);
        return _off_put_headers_complete_error(response, 400,
            "recycle-ephemeral requires a recycler");
    }
    if (recycler_oris.length > 0) {
        /* Temporary+recycler is legal now: an ephemeral put behaves as
           propagate (the recipe claims its source blocks at fetch time),
           regardless of the explicit recycle mode. */
        recycler_recipe_t* recycler = recycler_recipe_create(routes_ctx->pool, routes_ctx->bc, standard,
                                                              recycler_oris, routes_ctx->network,
                                                              is_temporary, recycle_mode);
        vec_push(&recipes, (block_recipe_t*)recycler);
    }

    new_blocks_recipe_t* recipe = new_blocks_recipe_create(routes_ctx->pool, routes_ctx->bc, standard);
    vec_push(&recipes, (block_recipe_t*)recipe);

    writeable_off_stream_t* ws = writeable_off_stream_create(
        routes_ctx->pool, routes_ctx->bc, routes_ctx->tc, standard, tuple_size, 32, recipes, NULL);

    writeable_descriptor_t* desc = writeable_descriptor_create(
        routes_ctx->pool, routes_ctx->bc, standard, 32, tuple_size, stream_length, NULL);

    if (is_temporary) {
        writeable_off_stream_set_ephemeral(ws, 1);
        writeable_descriptor_set_ephemeral(desc, 1);
    }

    put_context_t* put_ctx = get_clear_memory(sizeof(put_context_t));
    put_ctx->response = response;
    put_ctx->connection = connection;
    /* Hold a reference to the request stream for the streaming pipeline. The
       HTTP connection releases its own reference in _on_message_complete right
       after queueing the request's close_event (stream_notify is async, so the
       actor has not processed data_event / close_event yet). Without this
       reference the request would be freed before its actor delivered the
       body chunks and the close to the pipeline, and the PUT would hang. The
       reference is released (deferred) in _put_on_request_close once the
       request's close_event has actually been dispatched. */
    put_ctx->request = REFERENCE(request, http_request_t);
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
    put_ctx->ofd_cache = routes_ctx->ofd_cache;
    put_ctx->bc = routes_ctx->bc;
    put_ctx->upload_data = NULL;
    put_ctx->temporary = is_temporary;

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

/* ---- representation op routes: ephemeral commit / delete, pin / unpin ---- */

/* Deferred-destruction wrapper for a representation actor. The completion
   dispatch runs on a pool worker, so it can NEVER call
   representation_actor_destroy inline: that destroy parks the pool via
   scheduler_pool_wait_for_idle, and a worker waiting for its own pool's
   idleness (itself included) would never return. Instead the completion
   queues this wrapper through scheduler_pool_defer_cleanup and the drain —
   which only ever runs when every worker is already idle — performs the
   destroy.

   The wrapper's leading refcounter_t satisfies defer_cleanup's
   hold-a-reference contract: defer_cleanup refcounter_references the object
   it is handed, so handing it a bare representation_actor_t (whose first
   member is a live actor mailbox, not a refcounter) would corrupt the
   mailbox's head pointer. The reference is never released — the drain's
   destructor frees the wrapper unconditionally, matching how every other
   defer_cleanup consumer in this directory treats the reference as a
   formality. */
typedef struct {
    refcounter_t refcounter;
    representation_actor_t* rep;
} rep_route_defer_t;

/* Drain-context safety of the internal wait: the drain runs either at the
   tail of scheduler_pool_wait_for_idle — where the idle predicate
   (all workers idle, zero pending messages) already holds, so
   representation_actor_destroy's internal wait returns immediately without
   parking — or from scheduler_pool_destroy after scheduler_pool_stop, where
   the pool's terminate flag makes the internal wait return 0 at once. Neither
   can deadlock, so no wait-free destroy variant is needed; the public destroy
   (external callers that may park) and this deferred path share it. */
static void _rep_route_deferred_destroy(rep_route_defer_t* defer) {
    representation_actor_t* rep = defer->rep;
    refcounter_destroy_lock(&defer->refcounter);
    free(defer);
    representation_actor_destroy(rep);
}

/* Route context for the four representation-op routes. actor stays the FIRST
   member: _rep_route_context_destroy runs actor_destroy (tearing the mailbox
   down) before deferring the struct to the pool's cleanup drain, and
   defer_cleanup's hold-a-reference then lands on the mailbox's dead head
   field — memory nothing reads again before free. This mirrors the
   off_get_state_t / block_http_state_t lifecycle exactly. */
typedef struct {
    actor_t actor;
    http_response_t* response;
    http_connection_t* connection;
    scheduler_pool_t* pool;
} rep_route_context_t;

static void _rep_route_context_destroy(rep_route_context_t* route_ctx) {
    http_connection_t* conn = route_ctx->connection;
    http_response_destroy(route_ctx->response);
    if (conn != NULL) {
        http_connection_destroy(conn);
    }
    atomic_fetch_or(&route_ctx->actor.flags, ACTOR_FLAG_DESTROY);
    actor_destroy(&route_ctx->actor);
    scheduler_pool_defer_cleanup(route_ctx->pool, route_ctx, free);
}

static void _rep_route_dispatch(void* state, message_t* msg) {
    rep_route_context_t* route_ctx = (rep_route_context_t*)state;
    if (msg->type != REPRESENTATION_OP_RESULT) {
        return;
    }
    representation_op_result_payload_t* result =
        (representation_op_result_payload_t*)msg->payload;

    char body[96];
    int body_len = snprintf(body, sizeof(body), "{\"result\":\"%s\",\"blocks\":%zu}",
                            result->result == 0 ? "ok" : "error",
                            result->blocks_touched);
    http_response_set_header(route_ctx->response, "Content-Type", "application/json");
    http_response_set_status(route_ctx->response, result->result == 0 ?
                             HTTP_STATUS_OK : HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_write(route_ctx->response, body, (size_t)body_len);
    http_response_end(route_ctx->response);

    /* Queue the representation actor's deferred destruction. The pointer
       comes from the result payload itself (payload->source), so this works
       even when the whole walk completed before representation_actor_create
       returned. */
    if (result->source != NULL) {
        rep_route_defer_t* defer = get_clear_memory(sizeof(rep_route_defer_t));
        refcounter_init(&defer->refcounter);
        defer->rep = result->source;
        scheduler_pool_defer_cleanup(route_ctx->pool, defer,
                                     (void (*)(void*))_rep_route_deferred_destroy);
    }

    _rep_route_context_destroy(route_ctx);
}

/* Shared entry for the four op handlers. The request body is the OFF URL of
   the representation to operate on; the response completes asynchronously
   once the representation actor's walk finishes (piped pattern, mirroring
   _put_on_descriptor_close's reference lifecycle). */
static void _rep_route_start(http_request_t* request, http_response_t* response,
                             off_routes_context_t* ctx, representation_op_e op) {
    if (request->body == NULL || request->body->data == NULL || request->body->size == 0) {
        http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
        http_response_set_header(response, "Content-Type", "text/plain");
        http_response_write(response, "missing OFF URL body", 20);
        http_response_end(response);
        return;
    }

    /* The body buffer is not NUL-terminated — copy it into a string for
       off_url_parse. */
    size_t url_len = request->body->size;
    char* url_str = get_memory(url_len + 1);
    memcpy(url_str, request->body->data, url_len);
    url_str[url_len] = '\0';
    off_url_t* url = off_url_parse(url_str);
    free(url_str);
    if (url == NULL || url->descriptor_hash == NULL) {
        if (url != NULL) {
            off_url_destroy(url);
        }
        http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
        http_response_set_header(response, "Content-Type", "text/plain");
        http_response_write(response, "unparseable OFF URL", 19);
        http_response_end(response);
        return;
    }

    rep_route_context_t* route_ctx = get_clear_memory(sizeof(rep_route_context_t));
    route_ctx->response = response;
    route_ctx->connection = response->connection;
    route_ctx->pool = ctx->pool;
    response->is_piped = 1;
    response->connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response);
    refcounter_reference((refcounter_t*)response->connection);
    actor_init(&route_ctx->actor, route_ctx, _rep_route_dispatch, ctx->pool);

    /* representation_actor_create references the descriptor hash itself; it
       kicks the walk before returning, and the completion can fire before
       create returns — the reply carries the actor pointer (payload->source),
       so nothing here needs the create's return value. */
    representation_actor_create(ctx->bc, ctx->network, url->descriptor_hash, op,
                                &route_ctx->actor);
    off_url_destroy(url);
}

static void _off_mark_permanent_handler(http_request_t* request, http_response_t* response,
                                        void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data,
                     REPRESENTATION_OP_MARK_PERMANENT);
}

static void _off_delete_ephemeral_handler(http_request_t* request, http_response_t* response,
                                           void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data,
                     REPRESENTATION_OP_DELETE_EPHEMERAL);
}

static void _off_pin_handler(http_request_t* request, http_response_t* response,
                             void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data,
                     REPRESENTATION_OP_PIN);
}

static void _off_unpin_handler(http_request_t* request, http_response_t* response,
                               void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data,
                     REPRESENTATION_OP_UNPIN);
}

/* ---- ephemeral list route ---- */

typedef struct {
    actor_t actor;  /* first member — same deferred-cleanup rationale as above */
    http_response_t* response;
    http_connection_t* connection;
    scheduler_pool_t* pool;
} list_route_context_t;

static void _list_route_context_destroy(list_route_context_t* route_ctx) {
    http_connection_t* conn = route_ctx->connection;
    http_response_destroy(route_ctx->response);
    if (conn != NULL) {
        http_connection_destroy(conn);
    }
    atomic_fetch_or(&route_ctx->actor.flags, ACTOR_FLAG_DESTROY);
    actor_destroy(&route_ctx->actor);
    scheduler_pool_defer_cleanup(route_ctx->pool, route_ctx, free);
}

static void _off_list_ephemeral_dispatch(void* state, message_t* msg) {
    list_route_context_t* route_ctx = (list_route_context_t*)state;
    if (msg->type != CACHE_EPHEMERAL_LIST) {
        return;
    }
    if (msg->payload == NULL) {
        /* The mirror always carries the payload; NULL means something is
           deeply wrong upstream — fail the request rather than crash. */
        http_response_set_status(route_ctx->response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        http_response_end(route_ctx->response);
        _list_route_context_destroy(route_ctx);
        return;
    }
    cache_ephemeral_list_payload_t* payload =
        (cache_ephemeral_list_payload_t*)msg->payload;

    /* [{"hash":"<hex>","claims":N,"pins":M},...] — 64 hex chars plus the
       number text per entry. */
    static const char hex_digits[] = "0123456789abcdef";
    buffer_t* json = buffer_create_with_capacity(0, 2 + payload->count * 100);
    json->data[json->size++] = '[';
    for (size_t idx = 0; idx < payload->count; idx++) {
        if (idx > 0) {
            json->data[json->size++] = ',';
        }
        /* Per-entry bound: 64 hex chars + the worst-case number text. */
        buffer_ensure_capacity(json, json->size + 128);
        json->data[json->size++] = '{';
        memcpy(json->data + json->size, "\"hash\":\"", 8);
        json->size += 8;
        buffer_t* hash = payload->hashes != NULL ? payload->hashes[idx] : NULL;
        if (hash != NULL) {
            for (size_t byte_index = 0; byte_index < hash->size; byte_index++) {
                json->data[json->size++] =
                    hex_digits[(hash->data[byte_index] >> 4) & 0x0F];
                json->data[json->size++] = hex_digits[hash->data[byte_index] & 0x0F];
            }
        }
        int number_len = snprintf((char*)json->data + json->size,
                                  json->capacity - json->size,
                                  "\",\"claims\":%u,\"pins\":%u}",
                                  payload->ephemeral_counts != NULL ?
                                      (unsigned)payload->ephemeral_counts[idx] : 0u,
                                  payload->pin_counts != NULL ?
                                      (unsigned)payload->pin_counts[idx] : 0u);
        if (number_len > 0) {
            json->size += (size_t)number_len;
        }
    }
    buffer_ensure_capacity(json, json->size + 2);
    json->data[json->size++] = ']';

    http_response_set_status(route_ctx->response, HTTP_STATUS_OK);
    http_response_set_header(route_ctx->response, "Content-Type", "application/json");
    http_response_write(route_ctx->response, (const char*)json->data, json->size);
    http_response_end(route_ctx->response);
    buffer_destroy(json);

    /* Consumer contract (block_cache.h): steal the arrays — destroy each
       referenced hash, free the three arrays — then empty the shell (NULL the
       pointers, zero the count) and leave msg->payload for actor_run's
       payload_destroy, which frees the emptied shell exactly once. */
    if (payload->hashes != NULL) {
        for (size_t idx = 0; idx < payload->count; idx++) {
            if (payload->hashes[idx] != NULL) {
                DESTROY(payload->hashes[idx], buffer);
            }
        }
        free(payload->hashes);
        payload->hashes = NULL;
    }
    if (payload->ephemeral_counts != NULL) {
        free(payload->ephemeral_counts);
        payload->ephemeral_counts = NULL;
    }
    if (payload->pin_counts != NULL) {
        free(payload->pin_counts);
        payload->pin_counts = NULL;
    }
    payload->count = 0;

    _list_route_context_destroy(route_ctx);
}

static void _off_list_ephemeral_handler(http_request_t* request, http_response_t* response,
                                       void* user_data) {
    (void)request;
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;
    list_route_context_t* route_ctx = get_clear_memory(sizeof(list_route_context_t));
    route_ctx->response = response;
    route_ctx->connection = response->connection;
    route_ctx->pool = ctx->pool;
    response->is_piped = 1;
    response->connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response);
    refcounter_reference((refcounter_t*)response->connection);
    actor_init(&route_ctx->actor, route_ctx, _off_list_ephemeral_dispatch, ctx->pool);
    block_cache_list_ephemeral(ctx->bc, &route_ctx->actor);
}

void off_routes_register(http_server_t* server, scheduler_pool_t* pool,
                         block_cache_t* bc, ofd_cache_t* ofd_cache, tuple_cache_t* tc,
                         network_t* network,
                         const config_t* config, const char* api_key,
                         ATOMIC(uint32_t)* open_stream_count) {
    (void)open_stream_count;
    if (server == NULL || pool == NULL || bc == NULL) {
        log_error("off_routes_register: required parameter is NULL (server=%p, pool=%p, bc=%p)",
                  (void*)server, (void*)pool, (void*)bc);
        return;
    }
    off_routes_context_t* ctx = off_routes_context_create(pool, bc, ofd_cache, tc, network,
                                                            config != NULL ? config->max_tuple_size : 0);

    http_server_use(server, _draining_middleware, server, NULL);

    cors_config_t* cors_config = cors_config_offsystem();
    http_server_use(server, cors_middleware, cors_config,
                    (void (*)(void*))cors_config_destroy);

    /* Register auth middleware if an API key hash is configured. The plaintext
       api_key is no longer passed to the middleware (it was unused); the
       middleware validates the bearer token against the bcrypt hash and, when
       config_local_binding_no_auth is set, skips bearer on loopback. */
    if (config != NULL && config->api_key_hash != NULL) {
        auth_middleware_t* auth = auth_middleware_create(config->api_key_hash,
                                                          config->config_local_binding_no_auth,
                                                          server);
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

    /* Representation-level ephemeral/pin operations and the ephemeral list.
       The four op routes take the OFF URL (of the representation to operate
       on) as the request body; the list route reports every block still
       carrying an ephemeral claim. All share ctx — the first registration
       above owns the context's destroy callback, these pass NULL (the server
       destroys routes in registration order, so freeing once at GET-data
       destroy time covers them all). */
    http_server_post_with_data(server, "/offsystem/ephemeral/commit",
                               _off_mark_permanent_handler, ctx, NULL);
    http_server_post_with_data(server, "/offsystem/ephemeral/delete",
                               _off_delete_ephemeral_handler, ctx, NULL);
    http_server_post_with_data(server, "/offsystem/pin", _off_pin_handler, ctx, NULL);
    http_server_post_with_data(server, "/offsystem/unpin", _off_unpin_handler, ctx, NULL);
    http_server_get_with_data(server, "/offsystem/ephemeral/list",
                              _off_list_ephemeral_handler, ctx, NULL);
}