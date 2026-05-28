//
// Created by victor on 5/28/26.
//
#include "config_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "http_headers.h"
#include "../../Configuration/config_pending.h"
#include "../../Node/node.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
  offs_node_t* node;
  char* data_dir;
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

/* Known config field names for validation */
static bool _is_known_field(const char* name) {
  static const char* known[] = {
    "api_key_hash",
    "cache_size", "max_snapshots", "max_wals", "max_capacity_bytes",
    "scheduler_thread_count",
    "http_enabled", "http_port", "https_enabled", "https_port",
    "https_cert_path", "https_key_path", "unix_enabled",
    "tcp_enabled", "tcp_port", "ws_enabled", "ws_port",
    "wt_enabled", "wt_port",
    "tcp_tls_enabled", "tcp_tls_cert_path", "tcp_tls_key_path",
    NULL
  };
  for (size_t i = 0; known[i] != NULL; i++) {
    if (strcmp(name, known[i]) == 0) return true;
  }
  return false;
}

/* Serialize current config_t to cJSON */
static cJSON* _config_to_json(const config_t* config) {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddNumberToObject(root, "cache_size", (double)config->cache_size);
  cJSON_AddNumberToObject(root, "max_snapshots", (double)config->max_snapshots);
  cJSON_AddNumberToObject(root, "max_wals", (double)config->max_wals);
  cJSON_AddNumberToObject(root, "max_capacity_bytes", (double)config->max_capacity_bytes);
  cJSON_AddNumberToObject(root, "scheduler_thread_count", (double)config->scheduler_thread_count);

  cJSON_AddBoolToObject(root, "http_enabled", config->http_enabled);
  cJSON_AddNumberToObject(root, "http_port", (double)config->http_port);
  cJSON_AddBoolToObject(root, "https_enabled", config->https_enabled);
  cJSON_AddNumberToObject(root, "https_port", (double)config->https_port);
  if (config->https_cert_path)
    cJSON_AddStringToObject(root, "https_cert_path", config->https_cert_path);
  else
    cJSON_AddNullToObject(root, "https_cert_path");
  if (config->https_key_path)
    cJSON_AddStringToObject(root, "https_key_path", config->https_key_path);
  else
    cJSON_AddNullToObject(root, "https_key_path");

  cJSON_AddBoolToObject(root, "unix_enabled", config->unix_enabled);
  cJSON_AddBoolToObject(root, "tcp_enabled", config->tcp_enabled);
  cJSON_AddNumberToObject(root, "tcp_port", (double)config->tcp_port);
  cJSON_AddBoolToObject(root, "ws_enabled", config->ws_enabled);
  cJSON_AddNumberToObject(root, "ws_port", (double)config->ws_port);
  cJSON_AddBoolToObject(root, "wt_enabled", config->wt_enabled);
  cJSON_AddNumberToObject(root, "wt_port", (double)config->wt_port);

  cJSON_AddBoolToObject(root, "tcp_tls_enabled", config->tcp_tls_enabled);
  if (config->tcp_tls_cert_path)
    cJSON_AddStringToObject(root, "tcp_tls_cert_path", config->tcp_tls_cert_path);
  else
    cJSON_AddNullToObject(root, "tcp_tls_cert_path");
  if (config->tcp_tls_key_path)
    cJSON_AddStringToObject(root, "tcp_tls_key_path", config->tcp_tls_key_path);
  else
    cJSON_AddNullToObject(root, "tcp_tls_key_path");

  if (config->api_key_hash)
    cJSON_AddStringToObject(root, "api_key_hash", config->api_key_hash);
  else
    cJSON_AddNullToObject(root, "api_key_hash");

  return root;
}

static void _config_get_handler(http_request_t* request, http_response_t* response,
                                 void* user_data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)user_data;

  if (_config_check_auth(request, response, ctx->node->http_server) != 0) return;

  cJSON* json = _config_to_json(ctx->node->config);
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
    if (!_is_known_field(item->string)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "unknown field");
      cJSON_AddItemToArray(rejected, entry);
      item = item->next;
      continue;
    }

    /* Type check: strings for paths and api_key_hash */
    bool is_string_field = (strcmp(item->string, "api_key_hash") == 0 ||
                            strcmp(item->string, "https_cert_path") == 0 ||
                            strcmp(item->string, "https_key_path") == 0 ||
                            strcmp(item->string, "tcp_tls_cert_path") == 0 ||
                            strcmp(item->string, "tcp_tls_key_path") == 0);
    /* Accept null for string fields (means revert to default) */
    if (is_string_field && !cJSON_IsString(item) && !cJSON_IsNull(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected string or null");
      cJSON_AddItemToArray(rejected, entry);
      item = item->next;
      continue;
    }

    bool is_bool_field = (strcmp(item->string, "http_enabled") == 0 ||
                          strcmp(item->string, "https_enabled") == 0 ||
                          strcmp(item->string, "unix_enabled") == 0 ||
                          strcmp(item->string, "tcp_enabled") == 0 ||
                          strcmp(item->string, "ws_enabled") == 0 ||
                          strcmp(item->string, "wt_enabled") == 0 ||
                          strcmp(item->string, "tcp_tls_enabled") == 0);
    if (is_bool_field && !cJSON_IsBool(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected boolean");
      cJSON_AddItemToArray(rejected, entry);
      item = item->next;
      continue;
    }

    bool is_number_field = (!is_string_field && !is_bool_field);
    if (is_number_field && !cJSON_IsNumber(item)) {
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
  offs_node_restart(ctx->node, ctx->data_dir);
}

static void _config_routes_ctx_destroy(void* data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)data;
  free(ctx->data_dir);
  free(ctx);
}

void config_routes_register(http_server_t* server, offs_node_t* node,
                            const config_t* config, const char* data_dir) {
  (void)config; /* unused — config is accessed via node->config */

  config_routes_ctx_t* ctx = get_clear_memory(sizeof(config_routes_ctx_t));
  ctx->node = node;
  ctx->data_dir = strdup(data_dir);

  http_server_get_with_data(server, "/config", _config_get_handler, ctx,
                            _config_routes_ctx_destroy);
  http_server_put_with_data(server, "/config", _config_put_handler, ctx, NULL);
  http_server_post_with_data(server, "/config/restart", _config_restart_handler, ctx, NULL);
}
