//
// Created by victor on 5/28/26.
//
#include "config_routes.h"
#include "../config_handlers.h"
#include "http_response.h"
#include "http_request.h"
#include "http_headers.h"
#include "../../Configuration/config_pending.h"
#include "../../Configuration/config_json.h"
#include "../../Node/node.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
  offs_node_t* node;
  char* data_dir;
  config_trigger_restart_fn trigger_restart; /* NULL → offs_node_restart fallback */
  void* restart_user_data;
} config_routes_ctx_t;

/* Auth: check Bearer token OR local binding */
static int _config_check_auth(http_request_t* request, http_response_t* response,
                               http_server_t* server) {
  /* Local binding — skip auth */
  if (http_server_is_local_binding(server)) return 0;

  /* Remote — require Bearer token */
  if (!request->is_authenticated) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_end(response);
    return -1;
  }
  return 0;
}

/* Known-field validation and config serialization are shared with the Unix
   config handlers via src/Configuration/config_json.{h,c}, so the two paths
   cannot drift. */

static void _config_get_handler(http_request_t* request, http_response_t* response,
                                 void* user_data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)user_data;

  if (_config_check_auth(request, response, ctx->node->http_server) != 0) return;

  cJSON* json = config_to_json(ctx->node->config);
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);

  if (json_str == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
}

static void _config_put_handler(http_request_t* request, http_response_t* response,
                                 void* user_data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)user_data;

  if (_config_check_auth(request, response, ctx->node->http_server) != 0) return;

  /* Need a body */
  if (request->body == NULL || request->body->size == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  /* Parse the incoming JSON */
  cJSON* incoming = cJSON_ParseWithLength((const char*)request->body->data,
                                           request->body->size);
  if (incoming == NULL) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "error", "invalid JSON body");
    char* err_str = cJSON_Print(err);
    cJSON_Delete(err);
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, err_str, strlen(err_str));
    http_response_end(response);
    free(err_str);
    return;
  }

  /* Validate field names and types */
  cJSON* rejected = cJSON_CreateArray();
  cJSON* staged = cJSON_CreateArray();
  bool has_valid = false;

  cJSON* item = incoming->child;
  while (item != NULL) {
    if (!config_is_known_field(item->string)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "unknown field");
      cJSON_AddItemToArray(rejected, entry);
      item = item->next;
      continue;
    }

    /* Type check driven by the shared field-type classifier so HTTP and Unix
       agree on what each field accepts. String fields also accept null
       (revert to default). */
    config_field_type_t ftype = config_field_type(item->string);
    if (ftype == CONFIG_FIELD_STRING && !cJSON_IsString(item) && !cJSON_IsNull(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected string or null");
      cJSON_AddItemToArray(rejected, entry);
      item = item->next;
      continue;
    }
    if (ftype == CONFIG_FIELD_BOOL && !cJSON_IsBool(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected boolean");
      cJSON_AddItemToArray(rejected, entry);
      item = item->next;
      continue;
    }
    if (ftype == CONFIG_FIELD_NUMBER && !cJSON_IsNumber(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected number");
      cJSON_AddItemToArray(rejected, entry);
      item = item->next;
      continue;
    }

    cJSON_AddItemToArray(staged,
      cJSON_CreateString(item->string));
    has_valid = true;
    item = item->next;
  }

  /* Save to pending config if any valid fields */
  bool restart_required = false;
  if (has_valid) {
    if (config_pending_save(ctx->data_dir,
                            (const char*)request->body->data,
                            request->body->size) == 0) {
      restart_required = true;
    } else {
      /* Save failed — move all staged to rejected */
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", "*");
      cJSON_AddStringToObject(entry, "reason", "failed to write pending config");
      cJSON_AddItemToArray(rejected, entry);
      cJSON_Delete(staged);
      staged = cJSON_CreateArray();
      restart_required = false;
    }
  }

  cJSON_Delete(incoming);

  /* Build response */
  cJSON* result = cJSON_CreateObject();
  cJSON_AddItemToObject(result, "staged", staged);
  cJSON_AddItemToObject(result, "rejected", rejected);
  cJSON_AddBoolToObject(result, "restart_required", restart_required);

  char* json_str = cJSON_Print(result);
  cJSON_Delete(result);

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  if (json_str != NULL) {
    http_response_write(response, json_str, strlen(json_str));
    free(json_str);
  }
  http_response_end(response);
}

static void _config_restart_handler(http_request_t* request, http_response_t* response,
                                     void* user_data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)user_data;

  /* Restart is only allowed on local transports */
  if (!http_server_is_local_binding(ctx->node->http_server)) {
    http_response_set_status(response, HTTP_STATUS_FORBIDDEN);
    http_response_set_header(response, "Content-Type", "application/json");
    const char* msg = "{\"error\":\"restart requires local transport\"}";
    http_response_write(response, msg, strlen(msg));
    http_response_end(response);
    return;
  }

  /* Verify pending config exists */
  if (config_pending_exists(ctx->data_dir) != 1) {
    http_response_set_status(response, HTTP_STATUS_CONFLICT);
    http_response_set_header(response, "Content-Type", "application/json");
    const char* msg = "{\"error\":\"no pending config to apply\"}";
    http_response_write(response, msg, strlen(msg));
    http_response_end(response);
    return;
  }

  /* Send 202 before restart begins */
  http_response_set_status(response, HTTP_STATUS_ACCEPTED);
  http_response_set_header(response, "Content-Type", "application/json");
  const char* msg = "{\"message\":\"restarting\"}";
  http_response_write(response, msg, strlen(msg));
  http_response_end(response);

  /* Trigger restart */
  if (ctx->trigger_restart != NULL) {
    ctx->trigger_restart(ctx->restart_user_data);
  } else {
    offs_node_restart(ctx->node, ctx->data_dir);
  }
}

static void _config_routes_ctx_destroy(void* data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)data;
  free(ctx->data_dir);
  free(ctx);
}

void config_routes_register(http_server_t* server, offs_node_t* node,
                            const config_t* config, const char* data_dir,
                            config_trigger_restart_fn trigger_restart,
                            void* restart_user_data) {
  (void)config; /* unused — config is accessed via node->config */

  config_routes_ctx_t* ctx = get_clear_memory(sizeof(config_routes_ctx_t));
  ctx->node = node;
  ctx->data_dir = strdup(data_dir);
  ctx->trigger_restart = trigger_restart;
  ctx->restart_user_data = restart_user_data;

  http_server_get_with_data(server, "/config", _config_get_handler, ctx,
                            _config_routes_ctx_destroy);
  http_server_put_with_data(server, "/config", _config_put_handler, ctx, NULL);
  http_server_post_with_data(server, "/config/restart", _config_restart_handler, ctx, NULL);
}
