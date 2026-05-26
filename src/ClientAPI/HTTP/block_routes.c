//
// Created by victor on 5/26/26.
//

#include "block_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "http_connection.h"
#include "../../BlockCache/block.h"
#include "../../Buffer/buffer.h"
#include "../../Util/base58.h"
#include "../../Util/allocator.h"
#include "../../Actor/actor.h"
#include "../../Actor/message.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
  block_cache_t* bc;
  scheduler_pool_t* pool;
} block_routes_context_t;

/* Regex pattern for GET and DELETE: /blocks/<base58-hash> */
#define BLOCK_HASH_PATTERN "/blocks/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]+)"

/* --- Async state machine for block operations --- */

typedef struct {
  actor_t actor;
  http_response_t* response;
  http_connection_t* connection;
  block_routes_context_t* ctx;
  uint8_t put_encoding;
  enum { BLOCK_HTTP_PUT, BLOCK_HTTP_GET, BLOCK_HTTP_DELETE } op;
} block_http_state_t;

static void _block_http_state_destroy(block_http_state_t* state) {
  http_connection_t* conn = state->connection;
  http_response_destroy(state->response);
  if (conn) http_connection_destroy(conn);
  atomic_fetch_or(&state->actor.flags, ACTOR_FLAG_DESTROY);
  actor_destroy(&state->actor);
  scheduler_pool_defer_cleanup(state->ctx->pool, state, free);
}

static void _block_http_dispatch(void* vstate, message_t* msg) {
  block_http_state_t* state = (block_http_state_t*)vstate;

  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      if (state->op != BLOCK_HTTP_PUT) break;
      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;

      if (result->result < 0 || result->hash == NULL) {
        http_response_set_status(state->response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        http_response_end(state->response);
        _block_http_state_destroy(state);
        return;
      }

      http_response_set_status(state->response, HTTP_STATUS_CREATED);

      if (state->put_encoding == 1) {
        http_response_set_header(state->response, "Content-Type", "text/plain");
        size_t encoded_len = base58_encoded_length(result->hash->size);
        char* encoded = get_memory(encoded_len + 1);
        int written = base58_encode(result->hash->data, result->hash->size, encoded, encoded_len);
        if (written > 0) {
          http_response_write(state->response, encoded, (size_t)written);
        }
        free(encoded);
      } else {
        http_response_set_header(state->response, "Content-Type", "application/octet-stream");
        http_response_write(state->response, (const char*)result->hash->data, result->hash->size);
      }

      http_response_end(state->response);
      _block_http_state_destroy(state);
      return;
    }

    case CACHE_GET_RESULT: {
      if (state->op != BLOCK_HTTP_GET) break;
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;

      if (result->block == NULL) {
        http_response_set_status(state->response, HTTP_STATUS_NOT_FOUND);
        http_response_end(state->response);
        _block_http_state_destroy(state);
        return;
      }

      http_response_set_status(state->response, HTTP_STATUS_OK);
      http_response_set_header(state->response, "Content-Type", "application/octet-stream");
      http_response_write(state->response, (const char*)result->block->data->data,
                          result->block->data->size);
      http_response_end(state->response);
      _block_http_state_destroy(state);
      return;
    }

    case CACHE_REMOVE_RESULT: {
      if (state->op != BLOCK_HTTP_DELETE) break;
      cache_remove_result_payload_t* result = (cache_remove_result_payload_t*)msg->payload;

      if (result->result != 0) {
        http_response_set_status(state->response, HTTP_STATUS_NOT_FOUND);
      } else {
        http_response_set_status(state->response, HTTP_STATUS_NO_CONTENT);
      }
      http_response_end(state->response);
      _block_http_state_destroy(state);
      return;
    }

    default:
      break;
  }
}

/* --- PUT handler --- */

static void _block_put_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  block_routes_context_t* ctx = (block_routes_context_t*)user_data;

  if (request->body == NULL || request->body->size == 0
      || request->body->size > (size_t)standard) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  uint8_t encoding = 0;
  if (request->query_string && strstr(request->query_string, "encoding=base58") != NULL) {
    encoding = 1;
  }

  buffer_t* data_buf = buffer_create_from_existing_memory(
      get_memory(request->body->size), request->body->size);
  memcpy(data_buf->data, request->body->data, request->body->size);

  block_t* block = block_create_by_type(data_buf, standard);
  buffer_destroy(data_buf);

  if (block == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  block_http_state_t* state = get_clear_memory(sizeof(block_http_state_t));
  state->ctx = ctx;
  state->response = response;
  state->connection = response->connection;
  state->put_encoding = encoding;
  state->op = BLOCK_HTTP_PUT;
  refcounter_reference((refcounter_t*)state->connection);
  refcounter_reference((refcounter_t*)state->response);

  actor_init(&state->actor, state, _block_http_dispatch, ctx->pool);
  block_cache_put(ctx->bc, block, 0, &state->actor);
}

/* --- GET handler --- */

static void _block_get_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  block_routes_context_t* ctx = (block_routes_context_t*)user_data;

  /* Extract base58 hash from path: /blocks/<hash> */
  const char* path = request->path;
  const char* hash_start = strrchr(path, '/');
  if (hash_start == NULL || strlen(hash_start + 1) == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  hash_start++; /* skip '/' */

  uint8_t hash_bytes[32];
  size_t bytes_written = 0;
  if (base58_decode(hash_start, hash_bytes, sizeof(hash_bytes), &bytes_written) != 0
      || bytes_written != 32) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  uint8_t* hash_copy = get_memory(32);
  memcpy(hash_copy, hash_bytes, 32);
  buffer_t* hash = buffer_create_from_existing_memory(hash_copy, 32);

  block_http_state_t* state = get_clear_memory(sizeof(block_http_state_t));
  state->ctx = ctx;
  state->response = response;
  state->connection = response->connection;
  state->op = BLOCK_HTTP_GET;
  refcounter_reference((refcounter_t*)state->connection);
  refcounter_reference((refcounter_t*)state->response);

  actor_init(&state->actor, state, _block_http_dispatch, ctx->pool);
  block_cache_get(ctx->bc, hash, &state->actor);
}

/* --- DELETE handler --- */

static void _block_delete_handler(http_request_t* request, http_response_t* response,
                                   void* user_data) {
  block_routes_context_t* ctx = (block_routes_context_t*)user_data;

  const char* path = request->path;
  const char* hash_start = strrchr(path, '/');
  if (hash_start == NULL || strlen(hash_start + 1) == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  hash_start++;

  uint8_t hash_bytes[32];
  size_t bytes_written = 0;
  if (base58_decode(hash_start, hash_bytes, sizeof(hash_bytes), &bytes_written) != 0
      || bytes_written != 32) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  uint8_t* hash_copy = get_memory(32);
  memcpy(hash_copy, hash_bytes, 32);
  buffer_t* hash = buffer_create_from_existing_memory(hash_copy, 32);

  block_http_state_t* state = get_clear_memory(sizeof(block_http_state_t));
  state->ctx = ctx;
  state->response = response;
  state->connection = response->connection;
  state->op = BLOCK_HTTP_DELETE;
  refcounter_reference((refcounter_t*)state->connection);
  refcounter_reference((refcounter_t*)state->response);

  actor_init(&state->actor, state, _block_http_dispatch, ctx->pool);
  block_cache_remove(ctx->bc, hash, &state->actor);
}

/* --- Registration --- */

void block_routes_register(http_server_t* server, scheduler_pool_t* pool,
                           block_cache_t* bc, const config_t* config,
                           const char* api_key) {
  /* Only register block routes if auth is enabled */
  if (config == NULL || config->api_key_hash == NULL || api_key == NULL) {
    return;
  }

  block_routes_context_t* ctx = get_clear_memory(sizeof(block_routes_context_t));
  ctx->bc = bc;
  ctx->pool = pool;

  /* Register PUT first with free() as the user_data_destroy callback.
     GET and DELETE share the same ctx but use NULL for destroy to avoid double-free.
     The server destroys routes in registration order, so PUT's destroy frees ctx. */
  http_server_put_with_data(server, "/blocks",
                             _block_put_handler, ctx, free);
  http_server_get_with_data(server, BLOCK_HASH_PATTERN,
                             _block_get_handler, ctx, NULL);
  http_server_delete_with_data(server, BLOCK_HASH_PATTERN,
                                _block_delete_handler, ctx, NULL);
}
