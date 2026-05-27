//
// Created by victor on 5/27/26.
//

#include "peer_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "http_headers.h"
#include "../../Network/peer_info.h"
#include "../../Network/network.h"
#include "../../Network/authority.h"
#include "../../Network/connection_manager.h"
#include "../../Network/node_id.h"
#include "../../Node/node.h"
#include "../../Util/allocator.h"
#include "../../Util/base58.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <cbor.h>

typedef struct {
  offs_node_t* node;
} peer_routes_ctx_t;

/* --- Connect status codes --- */
#define CONNECT_STATUS_OK               0
#define CONNECT_STATUS_ALREADY          1
#define CONNECT_STATUS_INVALID_INFO     2
#define CONNECT_STATUS_FAILED           3
#define CONNECT_STATUS_REJECTED         4

#define JSON_BUF_SIZE 65536

/* --- Auth helper --- */

static int _check_auth(http_request_t* request, http_response_t* response) {
  if (!request->is_authenticated) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_end(response);
    return -1;
  }
  return 0;
}

/* --- Build local peer info --- */

static peer_info_t* _build_local_peer_info(offs_node_t* node) {
  peer_info_t* info = get_clear_memory(sizeof(peer_info_t));

  memcpy(&info->node_id, &node->authority->local_id, sizeof(node_id_t));

  if (node->authority->public_key != NULL) {
    info->public_key_len = node->authority->public_key_len;
    info->public_key = get_clear_memory(info->public_key_len);
    memcpy(info->public_key, node->authority->public_key, info->public_key_len);
  }

  info->addresses = get_clear_memory(PEER_INFO_MAX_ADDRESSES * sizeof(peer_address_t));
  info->addresses[0].type = PEER_ADDR_DIRECT;
  info->addresses[0].host = get_clear_memory(10);
  memcpy(info->addresses[0].host, "127.0.0.1", 9);
  info->addresses[0].port = 0;
  info->address_count = 1;

  return info;
}

/* --- CBOR serialization helper --- */

static size_t _serialize_cbor(cbor_item_t* item, uint8_t** out_bytes) {
  size_t item_size = cbor_serialized_size(item);
  uint8_t* bytes = get_clear_memory(item_size);
  size_t serialized = cbor_serialize(item, bytes, item_size);
  if (serialized == 0) {
    free(bytes);
    return 0;
  }
  *out_bytes = bytes;
  return serialized;
}

/* --- Peer info decoding helper --- */

static int _decode_peer_info_body(http_request_t* request, peer_info_t* info) {
  const char* content_type = http_request_header(request, "Content-Type");

  if (content_type != NULL && strstr(content_type, "application/cbor") != NULL) {
    /* CBOR body */
    if (request->body == NULL || request->body->size == 0) return -1;

    struct cbor_load_result load_result;
    cbor_item_t* item = cbor_load(request->body->data, request->body->size, &load_result);
    if (item == NULL || load_result.error.code != CBOR_ERR_NONE) {
      if (item != NULL) cbor_decref(&item);
      return -1;
    }

    int rc = peer_info_decode(item, info);
    cbor_decref(&item);
    return rc;
  }

  /* Default: try Base58 (text/plain or no Content-Type) */
  if (request->body == NULL || request->body->size == 0) return -1;

  /* Null-terminate body for base58_decode */
  char* body_str = get_clear_memory(request->body->size + 1);
  memcpy(body_str, request->body->data, request->body->size);
  body_str[request->body->size] = '\0';

  int rc = peer_info_from_base58(body_str, info);
  free(body_str);
  return rc;
}

/* --- Find first direct address --- */

static int _find_first_direct_addr(peer_info_t* info, const char** out_host,
                                   uint16_t* out_port) {
  for (size_t index = 0; index < info->address_count; index++) {
    peer_address_t* addr = &info->addresses[index];
    if (addr->type == PEER_ADDR_DIRECT && addr->host != NULL) {
      *out_host = addr->host;
      *out_port = addr->port;
      return 0;
    }
  }
  return -1;
}

/* --- Connect to peer and return status code --- */

static int _connect_to_peer(offs_node_t* node, peer_info_t* info) {
  /* Check if already connected */
  if (connection_manager_lookup(&node->network->conn_mgr, &info->node_id) != NULL) {
    return CONNECT_STATUS_ALREADY;
  }

  const char* host = NULL;
  uint16_t port = 0;
  if (_find_first_direct_addr(info, &host, &port) != 0) {
    return CONNECT_STATUS_FAILED;
  }

  int rc = network_connect_peer(node->network, host, port);
  if (rc != 0) {
    return CONNECT_STATUS_FAILED;
  }

  return CONNECT_STATUS_OK;
}

/* --- GET /peer/info --- */

static void _peer_info_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  const char* format = "cbor";
  if (request->query_string != NULL) {
    if (strstr(request->query_string, "format=base58") != NULL) {
      format = "base58";
    } else if (strstr(request->query_string, "format=qrcode") != NULL) {
      format = "qrcode";
    }
  }

  peer_info_t* info = _build_local_peer_info(ctx->node);

  if (strcmp(format, "qrcode") == 0) {
    http_response_set_status(response, HTTP_STATUS_NOT_IMPLEMENTED);
    http_response_end(response);
    peer_info_destroy(info);
    return;
  }

  if (strcmp(format, "base58") == 0) {
    char* b58 = peer_info_to_base58(info);
    if (b58 == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_end(response);
      peer_info_destroy(info);
      return;
    }

    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, b58, strlen(b58));
    http_response_end(response);

    free(b58);
    peer_info_destroy(info);
    return;
  }

  /* Default: CBOR */
  cbor_item_t* item = peer_info_encode(info);
  if (item == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    peer_info_destroy(info);
    return;
  }

  uint8_t* cbor_bytes = NULL;
  size_t cbor_size = _serialize_cbor(item, &cbor_bytes);
  cbor_decref(&item);

  if (cbor_size == 0 || cbor_bytes == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    peer_info_destroy(info);
    return;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/cbor");
  http_response_write(response, (const char*)cbor_bytes, cbor_size);
  http_response_end(response);

  free(cbor_bytes);
  peer_info_destroy(info);
}

/* --- POST /peer/connect --- */

static void _peer_connect_handler(http_request_t* request, http_response_t* response,
                                   void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  peer_info_t info;
  if (_decode_peer_info_body(request, &info) != 0) {
    char* json = NULL;
    int written = asprintf(&json,
                           "{\"status\":%d,\"message\":\"Invalid peer info\"}",
                           CONNECT_STATUS_INVALID_INFO);
    if (written < 0 || json == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_end(response);
      return;
    }
    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, json, (size_t)written);
    http_response_end(response);
    free(json);
    return;
  }

  int status = _connect_to_peer(ctx->node, &info);

  const char* message = NULL;
  switch (status) {
    case CONNECT_STATUS_OK:
      message = "Connection initiated";
      break;
    case CONNECT_STATUS_ALREADY:
      message = "Already connected";
      break;
    case CONNECT_STATUS_FAILED:
      message = "Connection failed";
      break;
    case CONNECT_STATUS_REJECTED:
      message = "Rejected";
      break;
    default:
      message = "Unknown status";
      break;
  }

  char* json = NULL;
  int written = asprintf(&json, "{\"status\":%d,\"message\":\"%s\"}", status, message);
  peer_info_destroy(&info);

  if (written < 0 || json == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json, (size_t)written);
  http_response_end(response);
  free(json);
}

/* --- GET /peers --- */

static const char* _bool_str(bool value) {
  return value ? "true" : "false";
}

static void _peer_list_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  connection_manager_t* mgr = &ctx->node->network->conn_mgr;

  /* Build JSON array */
  char* json = get_clear_memory(JSON_BUF_SIZE);
  size_t offset = 0;
  int written = snprintf(json, JSON_BUF_SIZE, "[");
  if (written < 0) goto json_error;
  offset = (size_t)written;

  size_t entry_index = 0;
  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_t* peer = mgr->peers[index];
    if (!peer->connected) continue;

    const char* comma = (entry_index > 0) ? "," : "";
    written = snprintf(json + offset, JSON_BUF_SIZE - offset,
                       "%s{\"node_id\":\"%s\",\"connected\":true,\"is_friend\":%s}",
                       comma, peer->remote_node_id.str,
                       _bool_str(peer->is_friend));
    if (written < 0 || (size_t)written >= JSON_BUF_SIZE - offset) goto json_error;
    offset += (size_t)written;
    entry_index++;
  }

  written = snprintf(json + offset, JSON_BUF_SIZE - offset, "]");
  if (written < 0) goto json_error;
  offset += (size_t)written;

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json, offset);
  http_response_end(response);
  free(json);
  return;

json_error:
  free(json);
  http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
  http_response_end(response);
}

/* --- POST /friends --- */

static int _add_friend_peer(peer_routes_ctx_t* ctx, peer_info_t* info) {
  authority_t* authority = ctx->node->authority;

  /* Check if already a friend */
  for (size_t index = 0; index < authority->friend_peer_count; index++) {
    if (peer_info_equals(authority->friend_peers[index], info)) {
      return -2;  /* Already a friend — not an error */
    }
  }

  /* Grow array */
  size_t new_count = authority->friend_peer_count + 1;
  peer_info_t** new_array = realloc(authority->friend_peers,
                                    new_count * sizeof(peer_info_t*));
  if (new_array == NULL) return -1;  /* OOM — real error */

  authority->friend_peers = new_array;
  authority->friend_peers[authority->friend_peer_count] = info;
  authority->friend_peer_count = new_count;

  return 0;
}

static void _friend_add_handler(http_request_t* request, http_response_t* response,
                                 void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  /* Decode peer_info from body */
  peer_info_t* info = get_clear_memory(sizeof(peer_info_t));
  if (_decode_peer_info_body(request, info) != 0) {
    free(info);
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  /* Add to friend peers (takes ownership of info) */
  int add_result = _add_friend_peer(ctx, info);
  if (add_result != 0) {
    if (add_result == -2) {
      /* Already a friend */
      const char* json = "{\"status\":\"already_friend\"}";
      http_response_set_status(response, HTTP_STATUS_CONFLICT);
      http_response_set_header(response, "Content-Type", "application/json");
      http_response_write(response, json, strlen(json));
      http_response_end(response);
    } else {
      /* OOM or other internal error */
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_end(response);
    }
    peer_info_destroy(info);
    free(info);
    return;
  }

  /* Persist the updated peer list to disk */
  authority_save_peers(ctx->node->authority, ctx->node->network);

  /* Try to connect immediately */
  _connect_to_peer(ctx->node, info);

  const char* json = "{\"status\":\"added\"}";
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json, strlen(json));
  http_response_end(response);
}

/* --- DELETE /friends/:node_id --- */

static void _friend_remove_handler(http_request_t* request, http_response_t* response,
                                    void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  /* Extract node_id from path: /friends/<node_id> */
  const char* path = request->path;
  const char* node_id_str = strrchr(path, '/');
  if (node_id_str == NULL || strlen(node_id_str + 1) == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  node_id_str++;

  /* Parse node_id from string */
  node_id_t target_id;
  if (node_id_from_string(node_id_str, &target_id) != 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  /* Find and remove from friend_peers */
  authority_t* authority = ctx->node->authority;
  size_t found_index = authority->friend_peer_count;
  for (size_t index = 0; index < authority->friend_peer_count; index++) {
    if (node_id_equals(&authority->friend_peers[index]->node_id, &target_id)) {
      found_index = index;
      break;
    }
  }

  if (found_index == authority->friend_peer_count) {
    http_response_set_status(response, HTTP_STATUS_NOT_FOUND);
    http_response_end(response);
    return;
  }

  peer_info_t* removed = authority->friend_peers[found_index];

  /* Shift remaining entries */
  for (size_t index = found_index; index < authority->friend_peer_count - 1; index++) {
    authority->friend_peers[index] = authority->friend_peers[index + 1];
  }
  authority->friend_peer_count--;

  /* Shrink array */
  if (authority->friend_peer_count == 0) {
    free(authority->friend_peers);
    authority->friend_peers = NULL;
  } else {
    peer_info_t** new_array = realloc(authority->friend_peers,
                                      authority->friend_peer_count * sizeof(peer_info_t*));
    if (new_array != NULL) {
      authority->friend_peers = new_array;
    }
  }

  /* Remove from connection_manager if connected */
  connection_manager_remove(&ctx->node->network->conn_mgr, &target_id);

  /* Persist the updated peer list to disk */
  authority_save_peers(ctx->node->authority, ctx->node->network);

  peer_info_destroy(removed);
  free(removed);

  const char* json = "{\"status\":\"removed\"}";
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json, strlen(json));
  http_response_end(response);
}

/* --- GET /friends --- */

static void _friend_list_handler(http_request_t* request, http_response_t* response,
                                  void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  authority_t* authority = ctx->node->authority;
  connection_manager_t* mgr = &ctx->node->network->conn_mgr;

  /* Build JSON array */
  char* json = get_clear_memory(JSON_BUF_SIZE);
  size_t offset = 0;
  int written = snprintf(json, JSON_BUF_SIZE, "[");
  if (written < 0) goto friend_json_error;
  offset = (size_t)written;

  for (size_t index = 0; index < authority->friend_peer_count; index++) {
    peer_info_t* friend_peer = authority->friend_peers[index];
    peer_connection_t* conn = connection_manager_lookup(mgr, &friend_peer->node_id);
    bool connected = (conn != NULL && conn->connected);

    const char* comma = (index > 0) ? "," : "";
    written = snprintf(json + offset, JSON_BUF_SIZE - offset,
                       "%s{\"node_id\":\"%s\",\"connected\":%s}",
                       comma, friend_peer->node_id.str,
                       _bool_str(connected));
    if (written < 0 || (size_t)written >= JSON_BUF_SIZE - offset) goto friend_json_error;
    offset += (size_t)written;
  }

  written = snprintf(json + offset, JSON_BUF_SIZE - offset, "]");
  if (written < 0) goto friend_json_error;
  offset += (size_t)written;

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json, offset);
  http_response_end(response);
  free(json);
  return;

friend_json_error:
  free(json);
  http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
  http_response_end(response);
}

/* --- Registration --- */

void peer_routes_register(http_server_t* server, offs_node_t* node,
                          const config_t* config, const char* api_key) {
  if (config == NULL || config->api_key_hash == NULL || api_key == NULL) return;

  peer_routes_ctx_t* ctx = get_clear_memory(sizeof(peer_routes_ctx_t));
  ctx->node = node;

  /* First route gets free() as destroy callback; subsequent routes share ctx */
  http_server_get_with_data(server, "/peer/info", _peer_info_handler, ctx, free);
  http_server_post_with_data(server, "/peer/connect", _peer_connect_handler, ctx, NULL);
  http_server_get_with_data(server, "/peers", _peer_list_handler, ctx, NULL);
  http_server_post_with_data(server, "/friends", _friend_add_handler, ctx, NULL);
  http_server_delete_with_data(server, "/friends/[^/]+", _friend_remove_handler, ctx, NULL);
  http_server_get_with_data(server, "/friends", _friend_list_handler, ctx, NULL);
}
