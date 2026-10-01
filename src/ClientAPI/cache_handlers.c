//
// Created by victor on 9/28/26.
//

#include "cache_handlers.h"
#include "client_api_wire.h"
#include "../Configuration/config_pending.h"
#include "../Util/allocator.h"
#include <cbor.h>
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cache_resize_stage_and_dispatch(block_cache_t* bc, const char* data_dir,
                                    uint64_t capacity_bytes, actor_t* reply_to,
                                    char* err_buf, size_t err_len) {
  /* The HTTP route passes node->block_cache, which may be NULL —
     refuse before staging so a never-applied resize is not persisted. */
  if (bc == NULL) {
    if (err_buf && err_len) snprintf(err_buf, err_len, "cache not available");
    return -1;
  }

  /* Stage first so a restart keeps the new capacity. */
  cJSON* obj = cJSON_CreateObject();
  cJSON_AddNumberToObject(obj, "max_capacity_bytes", (double)capacity_bytes);
  char* obj_str = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (obj_str == NULL) {
    if (err_buf && err_len) snprintf(err_buf, err_len, "failed to serialize config");
    return -1;
  }
  int rc = config_pending_save(data_dir, obj_str, strlen(obj_str));
  free(obj_str);
  if (rc != 0) {
    if (err_buf && err_len) snprintf(err_buf, err_len, "failed to write pending config");
    return -1;
  }

  block_cache_resize(bc, (size_t)capacity_bytes, reply_to);
  return 0;
}

void cache_handle_resize_request(block_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  if (ctx->bc == NULL || ctx->data_dir == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "cache not available");
    return;
  }

  client_api_cache_resize_request_t req;
  if (client_api_cache_resize_request_decode(frame, &req) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "malformed cache resize request");
    return;
  }
  if (req.capacity_bytes == 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "capacity_bytes must be > 0");
    client_api_cache_resize_request_destroy(&req);
    return;
  }

  /* The result handler gates on pending_op — set it before the async
     dispatch so CACHE_RESIZE_RESULT routes to the response. */
  ctx->pending_op = BLOCK_OP_RESIZE;

  char err_buf[128];
  if (cache_resize_stage_and_dispatch(ctx->bc, ctx->data_dir, req.capacity_bytes,
                                      ctx->actor, err_buf, sizeof(err_buf)) != 0) {
    /* Staging failed — nothing was dispatched, so no result will ever
       arrive; the pending gate must not stay armed. */
    ctx->pending_op = BLOCK_OP_NONE;
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, err_buf);
    client_api_cache_resize_request_destroy(&req);
    return;
  }
  client_api_cache_resize_request_destroy(&req);
  /* The response frame is sent by cache_handle_resize_result on the
     async CACHE_RESIZE_RESULT reply. */
}

int cache_handle_resize_result(block_handler_ctx_t* ctx, message_t* msg) {
  if (msg->type != CACHE_RESIZE_RESULT) return 0;
  if (ctx->pending_op != BLOCK_OP_RESIZE) return 0;
  ctx->pending_op = BLOCK_OP_NONE;

  cache_resize_result_payload_t* result = (cache_resize_result_payload_t*)msg->payload;
  client_api_cache_resize_response_t response;
  memset(&response, 0, sizeof(response));
  response.status = CLIENT_API_STATUS_OK;
  response.applied_live = 1;
  response.max_capacity_bytes = (uint64_t)result->max_capacity_bytes;
  response.current_bytes = (uint64_t)result->current_bytes;

  cbor_item_t* frame = client_api_cache_resize_response_encode(&response);
  client_api_cache_resize_response_destroy(&response);
  ctx->send_frame(ctx->conn, frame);
  return 1;
}