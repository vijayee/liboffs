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
#include "../../Network/peer_book.h"
#include "../../Network/endpoint.h"
#include "../../Network/connection_manager.h"
#include "../../Network/node_id.h"
#include "../../Node/node.h"
#include "../../Util/allocator.h"
#include "../../Util/base58.h"
#include "../../QR/qr.h"
#include "../peer_handlers.h"   /* peer_info_from_payload — shared decode helper */
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <cbor.h>

typedef struct {
  offs_node_t* node;
} peer_routes_ctx_t;

/* The peer-book actor lives on the network_t (created with it). NULL for
   cache-only deployments without a network — those routes report peering as
   unavailable, mirroring the wire handlers. */
static peer_book_t* _peer_book_of(peer_routes_ctx_t* ctx) {
  if (ctx->node == NULL || ctx->node->network == NULL) return NULL;
  return ctx->node->network->peer_book;
}

/* --- Connect status codes --- */
#define CONNECT_STATUS_OK               0
#define CONNECT_STATUS_ALREADY          1
#define CONNECT_STATUS_INVALID_INFO     2
#define CONNECT_STATUS_FAILED           3
#define CONNECT_STATUS_REJECTED         4

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

static peer_info_t* _build_local_peer_info(offs_node_t* node, bool include_lan) {
  peer_info_t* info = get_clear_memory(sizeof(peer_info_t));
  if (info == NULL) return NULL;

  memcpy(&info->node_id, &node->authority->local_id, sizeof(node_id_t));

  if (node->authority->public_key != NULL) {
    info->public_key_len = node->authority->public_key_len;
    info->public_key = get_clear_memory(info->public_key_len);
    if (info->public_key != NULL) {
      memcpy(info->public_key, node->authority->public_key, info->public_key_len);
    }
  }

  /* Populate real candidate addresses (HOST/SRFLX/RELAY) from the network.
     Previously this returned a hardcoded 127.0.0.1:0 stub, which made
     /peer/info useless for actual peering — peers would try to connect to
     127.0.0.1:0 and fail. */
  if (node->network != NULL) {
    if (peer_info_from_node(info, node->network, include_lan) != 0) {
      peer_info_destroy(info);
      free(info);
      return NULL;
    }
  }

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

  if (content_type != NULL && strstr(content_type, "image/x-portable-pixmap") != NULL) {
    /* QR image body — decode via the shared payload helper (format 2) */
    if (request->body == NULL || request->body->size == 0) return -1;
    return peer_info_from_payload(2, request->body->data, request->body->size, info);
  }

  if (content_type != NULL && strstr(content_type, "application/cbor") != NULL) {
    if (request->body == NULL || request->body->size == 0) return -1;
    return peer_info_from_payload(0, request->body->data, request->body->size, info);
  }

  /* Default: base58 text (text/plain or no Content-Type) */
  if (request->body == NULL || request->body->size == 0) return -1;
  return peer_info_from_payload(1, request->body->data, request->body->size, info);
}

/* --- Connect to peer and return status code --- */

static int _connect_to_peer(offs_node_t* node, peer_info_t* info) {
  /* Check if already connected */
  if (connection_manager_lookup(&node->network->conn_mgr, &info->node_id) != NULL) {
    return CONNECT_STATUS_ALREADY;
  }

  if (info->address_count == 0 || info->addresses == NULL) {
    return CONNECT_STATUS_FAILED;
  }

  /* Try all candidate addresses in priority order (HOST -> SRFLX -> DIRECT ->
     RELAY). network_connect_peer_candidates handles the fallback chain and
     relay-mediated rendezvous; the old code only looked for PEER_ADDR_DIRECT
     which peer_info_from_node never produces, so /peer/connect always failed. */
  int rc = network_connect_peer_candidates(node->network, &info->node_id,
                                            info->addresses,
                                            info->address_count, false);
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

  peer_info_t* info = _build_local_peer_info(ctx->node, request->is_authenticated != 0);
  if (info == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Failed to populate local addresses", 34);
    http_response_end(response);
    return;
  }

  if (strcmp(format, "qrcode") == 0) {
    cbor_item_t* cbor_map = peer_info_encode(info);
    if (cbor_map == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_set_header(response, "Content-Type", "text/plain");
      http_response_write(response, "Failed to encode peer info", 26);
      http_response_end(response);
      peer_info_destroy(info);
      free(info);
      return;
    }

    uint8_t* serialized = NULL;
    size_t serialized_len = _serialize_cbor(cbor_map, &serialized);
    cbor_decref(&cbor_map);
    if (serialized_len == 0) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_set_header(response, "Content-Type", "text/plain");
      http_response_write(response, "CBOR serialization failed", 25);
      http_response_end(response);
      peer_info_destroy(info);
      free(info);
      return;
    }

    size_t ppm_len = 0;
    uint8_t* ppm = qr_encode_to_ppm(serialized, serialized_len, &ppm_len);
    free(serialized);
    if (ppm == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_set_header(response, "Content-Type", "text/plain");
      http_response_write(response, "QR encoding failed", 18);
      http_response_end(response);
      peer_info_destroy(info);
      free(info);
      return;
    }

    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "image/x-portable-pixmap");
    http_response_write(response, (const char*)ppm, ppm_len);
    free(ppm);
    http_response_end(response);
    peer_info_destroy(info);
    free(info);
    return;
  }

  if (strcmp(format, "base58") == 0) {
    char* b58 = peer_info_to_base58(info);
    if (b58 == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_end(response);
      peer_info_destroy(info);
      free(info);
      return;
    }

    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, b58, strlen(b58));
    http_response_end(response);

    free(b58);
    peer_info_destroy(info);
    free(info);
    return;
  }

  /* Default: CBOR */
  cbor_item_t* item = peer_info_encode(info);
  if (item == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    peer_info_destroy(info);
    free(info);
    return;
  }

  uint8_t* cbor_bytes = NULL;
  size_t cbor_size = _serialize_cbor(item, &cbor_bytes);
  cbor_decref(&item);

  if (cbor_size == 0 || cbor_bytes == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    peer_info_destroy(info);
    free(info);
    return;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/cbor");
  http_response_write(response, (const char*)cbor_bytes, cbor_size);
  http_response_end(response);

  free(cbor_bytes);
  peer_info_destroy(info);
  free(info);
}

/* --- POST /peer/connect --- */

static void _peer_connect_handler(http_request_t* request, http_response_t* response,
                                   void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  peer_info_t info;
  if (_decode_peer_info_body(request, &info) != 0) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "status", CONNECT_STATUS_INVALID_INFO);
    cJSON_AddStringToObject(json, "message", "Invalid peer info");
    char* json_str = cJSON_Print(json);
    cJSON_Delete(json);
    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, json_str, strlen(json_str));
    http_response_end(response);
    free(json_str);
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

  cJSON* json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "status", status);
  cJSON_AddStringToObject(json, "message", message);
  peer_info_destroy(&info);

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

/* --- GET /peers --- */

static void _peer_list_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  network_t* network = ctx->node->network;
  connection_manager_t* mgr = &network->conn_mgr;

  cJSON* arr = cJSON_CreateArray();

  /* Connection-manager peers (directly connected). */
  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_t* peer = mgr->peers[index];
    if (!peer->connected) continue;
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "node_id", peer->remote_node_id.str);
    cJSON_AddBoolToObject(entry, "connected", true);
    cJSON_AddBoolToObject(entry, "is_friend", peer->is_friend);
    cJSON_AddBoolToObject(entry, "in_ring", false);
    cJSON_AddItemToArray(arr, entry);
  }

  /* Ring-set peers (gossip-discovered, may not have a live connection yet).
     These are the peers the node knows about via gossip/FIND_NODE but has
     not necessarily established a direct QUIC connection to. */
  ring_set_t* rings = network->rings;
  if (rings != NULL) {
    for (size_t ring_idx = 0; ring_idx < rings->ring_count; ring_idx++) {
      ring_t* ring = &rings->rings[ring_idx];
      /* Primary members */
      for (int node_idx = 0; node_idx < ring->primary.length; node_idx++) {
        net_node_t* node = ring->primary.data[node_idx];
        if (node == NULL) continue;
        /* Skip if already in the connection manager (dedup) */
        if (connection_manager_lookup(mgr, &node->id) != NULL) continue;
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "node_id", node->id.str);
        cJSON_AddBoolToObject(entry, "connected", false);
        cJSON_AddBoolToObject(entry, "is_friend", false);
        cJSON_AddBoolToObject(entry, "in_ring", true);
        cJSON_AddItemToArray(arr, entry);
      }
      /* Secondary members */
      for (int node_idx = 0; node_idx < ring->secondary.length; node_idx++) {
        net_node_t* node = ring->secondary.data[node_idx];
        if (node == NULL) continue;
        if (connection_manager_lookup(mgr, &node->id) != NULL) continue;
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "node_id", node->id.str);
        cJSON_AddBoolToObject(entry, "connected", false);
        cJSON_AddBoolToObject(entry, "is_friend", false);
        cJSON_AddBoolToObject(entry, "in_ring", true);
        cJSON_AddItemToArray(arr, entry);
      }
    }
  }

  char* json_str = cJSON_Print(arr);
  cJSON_Delete(arr);
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

/* --- POST /friends --- */

static void _friend_add_handler(http_request_t* request, http_response_t* response,
                                 void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  peer_book_t* peer_book = _peer_book_of(ctx);
  if (peer_book == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  /* Decode peer_info from body. The peer-book actor deep-copies it into the
     list, so this copy stays owned here for the best-effort connect below. */
  peer_info_t* info = get_clear_memory(sizeof(peer_info_t));
  if (_decode_peer_info_body(request, info) != 0) {
    free(info);
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  /* Add through the peer-book actor (round-trip; -1 = invalid/OOM/timeout,
     -2 = already a friend). A timed-out add may still have been applied. */
  int add_result = peer_book_friend_add(peer_book, info, PEER_BOOK_TIMEOUT_MS);
  if (add_result != 0) {
    if (add_result == -2) {
      /* Already a friend */
      cJSON* json = cJSON_CreateObject();
      cJSON_AddStringToObject(json, "status", "already_friend");
      char* json_str = cJSON_Print(json);
      cJSON_Delete(json);
      http_response_set_status(response, HTTP_STATUS_CONFLICT);
      http_response_set_header(response, "Content-Type", "application/json");
      http_response_write(response, json_str, strlen(json_str));
      http_response_end(response);
      free(json_str);
    } else {
      /* OOM or other internal error */
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_end(response);
    }
    peer_info_destroy(info);
    free(info);
    return;
  }

  /* Persist via the debounced dirty flag — same mechanism as the wire handlers. */
  network_mark_peer_state_dirty(ctx->node->network);

  /* Try to connect immediately */
  _connect_to_peer(ctx->node, info);

  peer_info_destroy(info);
  free(info);

  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "added");
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
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

  /* Remove through the peer-book actor: 0 = removed, -1 = not found
     (invalid/timeout map here too). */
  int remove_result = peer_book_friend_remove(_peer_book_of(ctx), &target_id,
                                              PEER_BOOK_TIMEOUT_MS);
  if (remove_result != 0) {
    http_response_set_status(response, HTTP_STATUS_NOT_FOUND);
    http_response_end(response);
    return;
  }

  /* Remove from connection_manager if connected */
  connection_manager_remove(&ctx->node->network->conn_mgr, &target_id);

  /* Persist via the debounced dirty flag — same mechanism as the wire handlers. */
  network_mark_peer_state_dirty(ctx->node->network);

  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "removed");
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
}

/* --- GET /friends --- */

static void _friend_list_handler(http_request_t* request, http_response_t* response,
                                  void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  connection_manager_t* mgr = &ctx->node->network->conn_mgr;

  /* Snapshot the friend list through the peer-book actor (deep-copied
     peer_info_t entries, owned by this handler). */
  peer_info_t** friends = NULL;
  size_t friend_count = 0;
  if (peer_book_snapshot_friends(_peer_book_of(ctx), &friends, &friend_count,
                                 PEER_BOOK_TIMEOUT_MS) != 0) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  cJSON* arr = cJSON_CreateArray();
  for (size_t index = 0; index < friend_count; index++) {
    peer_info_t* friend_peer = friends[index];
    peer_connection_t* conn = connection_manager_lookup(mgr, &friend_peer->node_id);
    bool connected = (conn != NULL && conn->connected);
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "node_id", friend_peer->node_id.str);
    cJSON_AddBoolToObject(entry, "connected", connected);
    cJSON_AddItemToArray(arr, entry);
  }
  peer_book_free_peer_info_array(friends, friend_count);

  char* json_str = cJSON_Print(arr);
  cJSON_Delete(arr);
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

/* --- Bootstrap endpoint body decode helper --- */

/* Extract the "endpoint" string from a JSON body of the form
   {"endpoint": "host:port or [ipv6]:port"}. The body buffer is not
   NUL-terminated, so it is size-checked and copied to a stack buffer
   before cJSON_Parse. On success endpoint_out holds the NUL-terminated
   endpoint; returns -1 on any missing/invalid input. */
static int _decode_bootstrap_body(http_request_t* request, char* endpoint_out,
                                  size_t endpoint_len) {
  if (request->body == NULL || request->body->data == NULL ||
      request->body->size == 0 || request->body->size >= endpoint_len) {
    return -1;
  }

  memcpy(endpoint_out, request->body->data, request->body->size);
  endpoint_out[request->body->size] = '\0';

  cJSON* json = cJSON_Parse(endpoint_out);
  if (json == NULL) return -1;

  int result = -1;
  cJSON* endpoint = cJSON_GetObjectItem(json, "endpoint");
  if (cJSON_IsString(endpoint) && endpoint->valuestring != NULL &&
      endpoint->valuestring[0] != '\0') {
    size_t length = strlen(endpoint->valuestring);
    if (length < endpoint_len) {
      memcpy(endpoint_out, endpoint->valuestring, length + 1);
      result = 0;
    }
  }

  cJSON_Delete(json);
  return result;
}

/* --- POST /bootstrap --- */

static void _bootstrap_add_handler(http_request_t* request, http_response_t* response,
                                    void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  char endpoint[512];
  if (_decode_bootstrap_body(request, endpoint, sizeof(endpoint)) != 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  int add_result = peer_book_bootstrap_add(_peer_book_of(ctx), endpoint,
                                           PEER_BOOK_TIMEOUT_MS);
  if (add_result != 0) {
    if (add_result == -2) {
      /* Already a bootstrap peer (config or managed list) */
      cJSON* json = cJSON_CreateObject();
      cJSON_AddStringToObject(json, "status", "already_bootstrap");
      char* json_str = cJSON_Print(json);
      cJSON_Delete(json);
      if (json_str == NULL) {
        http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        http_response_end(response);
        return;
      }
      http_response_set_status(response, HTTP_STATUS_CONFLICT);
      http_response_set_header(response, "Content-Type", "application/json");
      http_response_write(response, json_str, strlen(json_str));
      http_response_end(response);
      free(json_str);
    } else if (add_result == -1) {
      /* Invalid endpoint or OOM */
      http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
      http_response_end(response);
    } else {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_end(response);
    }
    return;
  }

  /* The debounced save timer persists the peer store; no synchronous save. */
  network_mark_peer_state_dirty(ctx->node->network);

  /* Fire-and-forget connect; a failure here is not an HTTP error — the
     connect loop retries once the peer state heals. */
  char host[256];
  uint16_t port = 0;
  if (endpoint_parse(endpoint, host, sizeof(host), &port) == 0) {
    network_connect_peer(ctx->node->network, host, port);
  }

  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "added");
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

/* --- DELETE /bootstrap --- */

static void _bootstrap_remove_handler(http_request_t* request, http_response_t* response,
                                       void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  /* Body-based (not path params): endpoints contain ':' and '[]' characters
     unsuited to path parameters. */
  char endpoint[512];
  if (_decode_bootstrap_body(request, endpoint, sizeof(endpoint)) != 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  int remove_result = peer_book_bootstrap_remove(_peer_book_of(ctx), endpoint,
                                                 PEER_BOOK_TIMEOUT_MS);
  if (remove_result == -1) {
    /* Not found in either list */
    http_response_set_status(response, HTTP_STATUS_NOT_FOUND);
    http_response_end(response);
    return;
  }
  if (remove_result == -2) {
    /* Config-seeded entries are immutable at runtime */
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "config_immutable");
    char* json_str = cJSON_Print(json);
    cJSON_Delete(json);
    if (json_str == NULL) {
      http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
      http_response_end(response);
      return;
    }
    http_response_set_status(response, HTTP_STATUS_CONFLICT);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, json_str, strlen(json_str));
    http_response_end(response);
    free(json_str);
    return;
  }
  if (remove_result != 0) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  /* The debounced save timer persists the peer store; no synchronous save. */
  network_mark_peer_state_dirty(ctx->node->network);

  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "removed");
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

/* --- GET /bootstrap --- */

static void _bootstrap_list_handler(http_request_t* request, http_response_t* response,
                                     void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  /* Snapshot the bootstrap lists through the peer-book actor (owned heap
     endpoint strings for both lists). */
  char** config_endpoints = NULL;
  size_t config_count = 0;
  peer_info_t** config_infos = NULL;
  uint8_t* config_pinned = NULL;
  size_t config_info_count = 0;
  char** managed_endpoints = NULL;
  size_t managed_count = 0;
  if (peer_book_snapshot_bootstrap(_peer_book_of(ctx), &config_endpoints,
                                   &config_count, &config_infos,
                                   &config_pinned, &config_info_count,
                                   &managed_endpoints, &managed_count,
                                   PEER_BOOK_TIMEOUT_MS) != 0) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  cJSON* json = cJSON_CreateObject();
  cJSON* config_array = cJSON_AddArrayToObject(json, "config");
  cJSON* managed_array = cJSON_AddArrayToObject(json, "managed");
  if (config_array == NULL || managed_array == NULL) {
    peer_book_free_peer_info_array(config_infos, config_info_count);
    free(config_pinned);
    peer_book_free_string_array(config_endpoints, config_count);
    peer_book_free_string_array(managed_endpoints, managed_count);
    cJSON_Delete(json);
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  /* Config-seeded entries first, then operator-added managed entries. */
  for (size_t index = 0; index < config_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (endpoint_parse(config_endpoints[index], host, sizeof(host), &port) != 0) {
      continue;
    }
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "host", host);
    cJSON_AddNumberToObject(entry, "port", (double)port);
    cJSON_AddItemToArray(config_array, entry);
  }

  for (size_t index = 0; index < managed_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (endpoint_parse(managed_endpoints[index], host, sizeof(host), &port) != 0) {
      continue;
    }
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "host", host);
    cJSON_AddNumberToObject(entry, "port", (double)port);
    cJSON_AddItemToArray(managed_array, entry);
  }
  peer_book_free_peer_info_array(config_infos, config_info_count);
  free(config_pinned);
  peer_book_free_string_array(config_endpoints, config_count);
  peer_book_free_string_array(managed_endpoints, managed_count);

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
  http_server_post_with_data(server, "/bootstrap", _bootstrap_add_handler, ctx, NULL);
  http_server_delete_with_data(server, "/bootstrap", _bootstrap_remove_handler, ctx, NULL);
  http_server_get_with_data(server, "/bootstrap", _bootstrap_list_handler, ctx, NULL);
}
