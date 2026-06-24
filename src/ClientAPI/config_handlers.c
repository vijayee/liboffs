//
// Created by victor on 6/20/26.
//

#include "config_handlers.h"
#include "../Configuration/config_json.h"
#include "../Configuration/config_pending.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

/* Common guard: the handler can only run if the transport wired a node and a
   data_dir. main.c is responsible for calling unix_transport_set_config_ctx;
   if it did not, every config frame fails with INTERNAL_ERROR rather than
   crashing on a NULL dereference. */
static int _config_ctx_ready(config_handler_ctx_t* ctx) {
  return ctx != NULL && ctx->node != NULL && ctx->node->config != NULL &&
         ctx->data_dir != NULL;
}

/* Config management is privileged — reading the full config (which includes
   api_key_hash) and mutating/restarting are not for unauthenticated peers.
   On an auth-disabled socket (api_key_hash == NULL, the default local socket)
   connections are marked authenticated at create time, so this guard is a
   no-op there; on an auth-enabled socket it rejects frames until AUTH_REQUEST
   succeeds, matching the block handlers. */
static int _config_require_auth(config_handler_ctx_t* ctx) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return -1;
  }
  return 0;
}

void config_handle_show_request(config_handler_ctx_t* ctx, cbor_item_t* frame) {
  (void)frame; /* CONFIG_SHOW_REQUEST carries no payload */

  if (_config_require_auth(ctx) != 0) return;
  if (!_config_ctx_ready(ctx)) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "config not available");
    return;
  }

  cJSON* json = config_to_json(ctx->node->config);
  if (json == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "config serialization failed");
    return;
  }
  char* json_str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (json_str == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "config serialization failed");
    return;
  }

  client_api_config_show_response_t resp;
  resp.json_data = json_str;
  cbor_item_t* resp_frame = client_api_config_show_response_encode(&resp);
  ctx->send_frame(ctx->conn, resp_frame);
  free(json_str);
}

void config_handle_set_request(config_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (_config_require_auth(ctx) != 0) return;
  if (!_config_ctx_ready(ctx)) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "config not available");
    return;
  }

  client_api_config_set_request_t req;
  if (client_api_config_set_request_decode(frame, &req) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "malformed config set request");
    return;
  }

  client_api_config_set_response_t resp;
  memset(&resp, 0, sizeof(resp));
  resp.restart_required = 0;

  if (!config_is_known_field(req.field)) {
    resp.status = 1;
    resp.message = strdup("unknown field");
  } else {
    char err_buf[128];
    err_buf[0] = '\0';
    cJSON* value = config_field_value_from_string(req.field, req.value,
                                                   err_buf, sizeof(err_buf));
    if (value == NULL) {
      resp.status = 1;
      resp.message = strdup(err_buf[0] != '\0' ? err_buf : "invalid value");
    } else {
      cJSON* obj = cJSON_CreateObject();
      cJSON_AddItemToObject(obj, req.field, value);
      char* obj_str = cJSON_PrintUnformatted(obj);
      cJSON_Delete(obj);
      if (obj_str == NULL) {
        resp.status = 1;
        resp.message = strdup("failed to serialize config");
      } else {
        int rc = config_pending_save(ctx->data_dir, obj_str, strlen(obj_str));
        free(obj_str);
        if (rc == 0) {
          /* Pending config only applies on restart, so every successful stage
             requires a restart to take effect. */
          resp.status = 0;
          resp.restart_required = 1;
          resp.message = strdup("staged; restart to apply");
        } else {
          resp.status = 1;
          resp.message = strdup("failed to write pending config");
        }
      }
    }
  }

  cbor_item_t* resp_frame = client_api_config_set_response_encode(&resp);
  ctx->send_frame(ctx->conn, resp_frame);
  client_api_config_set_request_destroy(&req);
  client_api_config_set_response_destroy(&resp);
}

void config_handle_reload_request(config_handler_ctx_t* ctx, cbor_item_t* frame) {
  (void)frame; /* CONFIG_RELOAD_REQUEST carries no payload */

  if (_config_require_auth(ctx) != 0) return;
  if (!_config_ctx_ready(ctx)) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "config not available");
    return;
  }

  client_api_config_reload_response_t resp;
  memset(&resp, 0, sizeof(resp));

  if (config_pending_exists(ctx->data_dir) != 1) {
    resp.status = 1;
    resp.message = strdup("no pending config to apply");
    cbor_item_t* resp_frame = client_api_config_reload_response_encode(&resp);
    ctx->send_frame(ctx->conn, resp_frame);
    client_api_config_reload_response_destroy(&resp);
    return;
  }

  /* Send the response before triggering the restart, which tears the
     transport down — mirrors the HTTP /config/restart path. */
  resp.status = 0;
  resp.message = strdup("restarting");
  cbor_item_t* resp_frame = client_api_config_reload_response_encode(&resp);
  ctx->send_frame(ctx->conn, resp_frame);
  client_api_config_reload_response_destroy(&resp);

  /* Hand the restart to the daemon (which runs it on a non-pool thread) if it
     wired a trigger; otherwise run it inline. The inline path is only safe from
     a non-pool thread (e.g. a standalone test's main thread) — a pool worker
     would self-deadlock in offs_node_stop's scheduler_pool_wait_for_idle and
     destroy a shared scheduler pool. */
  if (ctx->trigger_restart != NULL) {
    ctx->trigger_restart(ctx->restart_user_data);
  } else {
    offs_node_restart(ctx->node, ctx->data_dir);
  }
}