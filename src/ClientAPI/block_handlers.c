//
// Created by victor on 5/26/26.
//

#include "block_handlers.h"
#include "../BlockCache/block.h"
#include "../Buffer/buffer.h"
#include "../Util/base58.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

void block_handle_put_request(block_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_block_put_request_t msg;
  if (client_api_block_put_request_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid block PUT request");
    return;
  }

  if (msg.data_size == 0 || msg.data_size > (size_t)standard) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Block data must be 1-128000 bytes");
    client_api_block_put_request_destroy(&msg);
    return;
  }

  buffer_t* data_buf = buffer_create_from_existing_memory(msg.data, msg.data_size);
  msg.data = NULL; /* ownership transferred to buffer */

  block_t* block = block_create_by_type(data_buf, standard);
  buffer_destroy(data_buf);

  if (block == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Failed to create block");
    client_api_block_put_request_destroy(&msg);
    return;
  }

  ctx->pending_op = BLOCK_OP_PUT;
  ctx->put_encoding = msg.encoding;
  client_api_block_put_request_destroy(&msg);

  block_cache_put(ctx->bc, block, 0, ctx->actor);
}

void block_handle_get_request(block_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_block_get_request_t msg;
  if (client_api_block_get_request_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid block GET request");
    return;
  }

  buffer_t* hash = buffer_create_from_existing_memory(msg.hash_data, msg.hash_len);
  msg.hash_data = NULL; /* ownership transferred */

  ctx->pending_op = BLOCK_OP_GET;
  client_api_block_get_request_destroy(&msg);

  block_cache_get(ctx->bc, hash, ctx->actor);
}

void block_handle_delete_request(block_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_block_delete_request_t msg;
  if (client_api_block_delete_request_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid block DELETE request");
    return;
  }

  buffer_t* hash = buffer_create_from_existing_memory(msg.hash_data, msg.hash_len);
  msg.hash_data = NULL; /* ownership transferred */

  ctx->pending_op = BLOCK_OP_DELETE;
  client_api_block_delete_request_destroy(&msg);

  block_cache_remove(ctx->bc, hash, ctx->actor);
}

int block_handle_cache_result(block_handler_ctx_t* ctx, message_t* msg) {
  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      if (ctx->pending_op != BLOCK_OP_PUT) return 0;
      ctx->pending_op = BLOCK_OP_NONE;

      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
      if (result->result < 0 || result->hash == NULL) {
        ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Failed to store block");
        return 1;
      }

      client_api_block_put_response_t response;
      memset(&response, 0, sizeof(response));
      response.status = CLIENT_API_STATUS_OK;

      if (ctx->put_encoding == 1) {
        /* base58 encoding */
        size_t encoded_len = base58_encoded_length(result->hash->size);
        char* encoded = get_memory(encoded_len + 1);
        int written = base58_encode(result->hash->data, result->hash->size, encoded, encoded_len);
        if (written > 0) {
          encoded[written] = '\0';
          response.hash_data = (uint8_t*)encoded;
          response.hash_len = (size_t)written;
          response.hash_is_text = 1;
        } else {
          free(encoded);
          ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Base58 encoding failed");
          return 1;
        }
      } else {
        /* raw bytes */
        response.hash_data = get_memory(result->hash->size);
        memcpy(response.hash_data, result->hash->data, result->hash->size);
        response.hash_len = result->hash->size;
        response.hash_is_text = 0;
      }

      cbor_item_t* frame = client_api_block_put_response_encode(&response);
      client_api_block_put_response_destroy(&response);
      ctx->send_frame(ctx->conn, frame);
      return 1;
    }

    case CACHE_GET_RESULT: {
      if (ctx->pending_op != BLOCK_OP_GET) return 0;
      ctx->pending_op = BLOCK_OP_NONE;

      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      client_api_block_get_response_t response;
      memset(&response, 0, sizeof(response));

      if (result->block == NULL) {
        response.status = CLIENT_API_STATUS_NOT_FOUND;
      } else {
        response.status = CLIENT_API_STATUS_OK;
        response.data_size = result->block->data->size;
        response.data = get_memory(response.data_size);
        memcpy(response.data, result->block->data->data, response.data_size);
      }

      cbor_item_t* frame = client_api_block_get_response_encode(&response);
      client_api_block_get_response_destroy(&response);
      ctx->send_frame(ctx->conn, frame);
      return 1;
    }

    case CACHE_REMOVE_RESULT: {
      if (ctx->pending_op != BLOCK_OP_DELETE) return 0;
      ctx->pending_op = BLOCK_OP_NONE;

      cache_remove_result_payload_t* result = (cache_remove_result_payload_t*)msg->payload;
      client_api_block_delete_response_t response;
      memset(&response, 0, sizeof(response));
      response.status = (result->result == 0) ? CLIENT_API_STATUS_OK : CLIENT_API_STATUS_NOT_FOUND;

      cbor_item_t* frame = client_api_block_delete_response_encode(&response);
      ctx->send_frame(ctx->conn, frame);
      return 1;
    }

    default:
      return 0;
  }
}
