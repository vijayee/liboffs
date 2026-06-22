//
// Created by victor on 5/27/26.
//

#include "peer_handlers.h"
#include "../Network/peer_info.h"
#include "../Network/node_id.h"
#include "../Util/base58.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

/* CBOR map keys for peer list entries */
#define PEER_LIST_KEY_NODE_ID    1
#define PEER_LIST_KEY_CONNECTED  2
#define PEER_LIST_KEY_IS_FRIEND  3
#define PEER_LIST_KEY_RTT_MS     4

void peer_handle_info_request(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  (void)frame; /* no payload */

  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  authority_t* auth = ctx->authority;
  if (auth->public_key == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "No local public key configured");
    return;
  }

  /* Build local peer_info_t from authority fields */
  peer_info_t local_info;
  memset(&local_info, 0, sizeof(local_info));
  local_info.node_id = auth->local_id;
  local_info.public_key = auth->public_key;       /* borrow — authority owns it */
  local_info.public_key_len = auth->public_key_len;
  local_info.addresses = NULL;
  local_info.address_count = 0;

  /* Encode to CBOR and serialize to bytes */
  cbor_item_t* cbor_map = peer_info_encode(&local_info);
  if (cbor_map == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Failed to encode peer info");
    return;
  }

  size_t serialized_len = cbor_serialized_size(cbor_map);
  uint8_t* serialized = get_clear_memory(serialized_len);
  if (serialized == NULL) {
    cbor_decref(&cbor_map);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Memory allocation failed");
    return;
  }

  size_t bytes_serialized = cbor_serialize(cbor_map, serialized, serialized_len);
  cbor_decref(&cbor_map);

  if (bytes_serialized == 0) {
    free(serialized);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "CBOR serialization failed");
    return;
  }

  /* Build and send response */
  client_api_peer_info_response_t response;
  memset(&response, 0, sizeof(response));
  response.format = 0; /* raw CBOR */
  response.data = serialized;
  response.data_size = bytes_serialized;

  cbor_item_t* out_frame = client_api_peer_info_response_encode(&response);
  free(serialized);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_connect(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_peer_connect_t msg;
  if (client_api_peer_connect_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid peer connect message");
    return;
  }

  peer_info_t remote_info;
  memset(&remote_info, 0, sizeof(remote_info));
  int decode_ok = -1;

  if (msg.format == 0) {
    /* CBOR bytes — load and decode */
    struct cbor_load_result load_result;
    cbor_item_t* decoded = cbor_load(msg.data, msg.data_size, &load_result);
    if (decoded != NULL && load_result.error.code == CBOR_ERR_NONE) {
      decode_ok = peer_info_decode(decoded, &remote_info);
      cbor_decref(&decoded);
    }
  } else if (msg.format == 1) {
    /* Base58 text — interpret as null-terminated string */
    char* b58_str = get_clear_memory(msg.data_size + 1);
    if (b58_str != NULL) {
      memcpy(b58_str, msg.data, msg.data_size);
      b58_str[msg.data_size] = '\0';
      decode_ok = peer_info_from_base58(b58_str, &remote_info);
      free(b58_str);
    }
  }

  client_api_peer_connect_destroy(&msg);

  if (decode_ok != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Failed to decode peer info");
    return;
  }

  /* Find first direct address and connect */
  int connected = 0;
  for (size_t index = 0; index < remote_info.address_count; index++) {
    if (remote_info.addresses[index].type == PEER_ADDR_DIRECT) {
      int rc = network_connect_peer(ctx->network,
                                    remote_info.addresses[index].host,
                                    remote_info.addresses[index].port);
      if (rc == 0) {
        connected = 1;
      }
      break; /* only try the first direct address */
    }
  }

  peer_info_destroy(&remote_info);

  client_api_peer_connect_result_t result;
  memset(&result, 0, sizeof(result));
  result.status = connected ? CLIENT_API_STATUS_OK : CLIENT_API_STATUS_BAD_REQUEST;

  cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_list_request(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  (void)frame; /* no payload */

  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  connection_manager_t* mgr = &ctx->network->conn_mgr;

  /* Count non-NULL peers so the definite array has exact size */
  size_t valid_count = 0;
  for (size_t index = 0; index < mgr->peer_count; index++) {
    if (mgr->peers[index] != NULL) valid_count++;
  }

  cbor_item_t* peers_array = cbor_new_definite_array(valid_count);

  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_t* peer = mgr->peers[index];
    if (peer == NULL) continue;

    cbor_item_t* peer_map = cbor_new_definite_map(4);

    /* node_id (bstr) */
    cbor_item_t* key = cbor_build_uint8(PEER_LIST_KEY_NODE_ID);
    cbor_item_t* val = cbor_build_bytestring(peer->remote_node_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_map_add(peer_map, (struct cbor_pair){.key = key, .value = val});
    cbor_decref(&key);
    cbor_decref(&val);

    /* connected (uint) */
    key = cbor_build_uint8(PEER_LIST_KEY_CONNECTED);
    val = cbor_build_uint8(peer->connected ? 1 : 0);
    (void)cbor_map_add(peer_map, (struct cbor_pair){.key = key, .value = val});
    cbor_decref(&key);
    cbor_decref(&val);

    /* is_friend (uint) */
    key = cbor_build_uint8(PEER_LIST_KEY_IS_FRIEND);
    val = cbor_build_uint8(peer->is_friend ? 1 : 0);
    (void)cbor_map_add(peer_map, (struct cbor_pair){.key = key, .value = val});
    cbor_decref(&key);
    cbor_decref(&val);

    /* rtt_ms (float) */
    key = cbor_build_uint8(PEER_LIST_KEY_RTT_MS);
    val = cbor_build_float8(peer->rtt_ewma);
    (void)cbor_map_add(peer_map, (struct cbor_pair){.key = key, .value = val});
    cbor_decref(&key);
    cbor_decref(&val);

    (void)cbor_array_push(peers_array, peer_map);
    cbor_decref(&peer_map);
  }

  client_api_peer_list_response_t response;
  memset(&response, 0, sizeof(response));
  response.peers = peers_array;

  cbor_item_t* out_frame = client_api_peer_list_response_encode(&response);
  client_api_peer_list_response_destroy(&response);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_friend_add(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_friend_add_t msg;
  if (client_api_friend_add_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid friend add message");
    return;
  }

  /* Allocate heap storage first so we can decode directly into it */
  peer_info_t* new_friend = get_clear_memory(sizeof(peer_info_t));
  if (new_friend == NULL) {
    client_api_friend_add_destroy(&msg);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Memory allocation failed");
    return;
  }

  int decode_ok = -1;

  if (msg.format == 0) {
    /* CBOR bytes */
    struct cbor_load_result load_result;
    cbor_item_t* decoded = cbor_load(msg.data, msg.data_size, &load_result);
    if (decoded != NULL && load_result.error.code == CBOR_ERR_NONE) {
      decode_ok = peer_info_decode(decoded, new_friend);
      cbor_decref(&decoded);
    }
  } else if (msg.format == 1) {
    /* Base58 text */
    char* b58_str = get_clear_memory(msg.data_size + 1);
    if (b58_str != NULL) {
      memcpy(b58_str, msg.data, msg.data_size);
      b58_str[msg.data_size] = '\0';
      decode_ok = peer_info_from_base58(b58_str, new_friend);
      free(b58_str);
    }
  }

  client_api_friend_add_destroy(&msg);

  if (decode_ok != 0) {
    peer_info_destroy(new_friend);
    free(new_friend);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Failed to decode friend peer info");
    return;
  }

  /* Append to authority->friend_peers */
  authority_t* auth = ctx->authority;
  size_t new_count = auth->friend_peer_count + 1;
  peer_info_t** expanded = realloc(auth->friend_peers, new_count * sizeof(peer_info_t*));
  if (expanded == NULL) {
    peer_info_destroy(new_friend);
    free(new_friend);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Failed to expand friend list");
    return;
  }
  auth->friend_peers = expanded;
  auth->friend_peers[auth->friend_peer_count] = new_friend;
  auth->friend_peer_count = new_count;

  /* Try to connect to the first direct address */
  int connected = 0;
  for (size_t index = 0; index < new_friend->address_count; index++) {
    if (new_friend->addresses[index].type == PEER_ADDR_DIRECT) {
      int rc = network_connect_peer(ctx->network,
                                    new_friend->addresses[index].host,
                                    new_friend->addresses[index].port);
      if (rc == 0) {
        connected = 1;
      }
      break;
    }
  }

  /* Report whether the best-effort connect to the first direct address
     succeeded, mirroring peer_handle_connect. The friend is added to the
     authority list either way; this only surfaces the connect outcome so the
     caller can retry or report. */
  client_api_peer_connect_result_t result;
  memset(&result, 0, sizeof(result));
  result.status = connected ? CLIENT_API_STATUS_OK : CLIENT_API_STATUS_BAD_REQUEST;

  cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_friend_remove(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_friend_remove_t msg;
  if (client_api_friend_remove_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid friend remove message");
    return;
  }

  /* Build node_id_t from hash bytes */
  node_id_t target_id;
  node_id_clear(&target_id);
  size_t copy_len = msg.node_id_len;
  if (copy_len > NODE_ID_HASH_SIZE) {
    copy_len = NODE_ID_HASH_SIZE;
  }
  memcpy(target_id.hash, msg.node_id, copy_len);
  /* Derive string representation */
  base58_encode(target_id.hash, NODE_ID_HASH_SIZE, target_id.str, NODE_ID_STRING_SIZE);

  client_api_friend_remove_destroy(&msg);

  /* Find and remove from authority->friend_peers */
  authority_t* auth = ctx->authority;
  size_t found_index = 0;
  int found = 0;
  for (size_t index = 0; index < auth->friend_peer_count; index++) {
    if (node_id_equals(&auth->friend_peers[index]->node_id, &target_id)) {
      found_index = index;
      found = 1;
      break;
    }
  }

  if (found) {
    peer_info_destroy(auth->friend_peers[found_index]);
    free(auth->friend_peers[found_index]);

    /* Shift remaining entries down */
    for (size_t index = found_index; index + 1 < auth->friend_peer_count; index++) {
      auth->friend_peers[index] = auth->friend_peers[index + 1];
    }
    auth->friend_peer_count--;
  }

  /* Also remove from connection_manager */
  connection_manager_remove(&ctx->network->conn_mgr, &target_id);

  client_api_peer_connect_result_t result;
  memset(&result, 0, sizeof(result));
  result.status = CLIENT_API_STATUS_OK;

  cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_friend_list_request(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  (void)frame; /* no payload */

  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  authority_t* auth = ctx->authority;

  /* Count non-NULL friend entries */
  size_t valid_count = 0;
  for (size_t index = 0; index < auth->friend_peer_count; index++) {
    if (auth->friend_peers[index] != NULL) valid_count++;
  }

  cbor_item_t* friends_array = cbor_new_definite_array(valid_count);

  for (size_t index = 0; index < auth->friend_peer_count; index++) {
    peer_info_t* friend_info = auth->friend_peers[index];
    if (friend_info == NULL) continue;

    /* Encode peer_info as CBOR and serialize to bytes */
    cbor_item_t* cbor_map = peer_info_encode(friend_info);
    if (cbor_map == NULL) continue;

    size_t serialized_len = cbor_serialized_size(cbor_map);
    uint8_t* serialized = get_clear_memory(serialized_len);
    if (serialized == NULL) {
      cbor_decref(&cbor_map);
      continue;
    }

    size_t bytes_serialized = cbor_serialize(cbor_map, serialized, serialized_len);
    cbor_decref(&cbor_map);

    if (bytes_serialized == 0) {
      free(serialized);
      continue;
    }

    /* Push serialized bytes as a CBOR bytestring into the array */
    cbor_item_t* bstr_item = cbor_build_bytestring(serialized, bytes_serialized);
    free(serialized);

    if (bstr_item != NULL) {
      (void)cbor_array_push(friends_array, bstr_item);
      cbor_decref(&bstr_item);
    }
  }

  client_api_friend_list_response_t response;
  memset(&response, 0, sizeof(response));
  response.friends = friends_array;

  cbor_item_t* out_frame = client_api_friend_list_response_encode(&response);
  client_api_friend_list_response_destroy(&response);
  ctx->send_frame(ctx->conn, out_frame);
}
