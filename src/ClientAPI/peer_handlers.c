//
// Created by victor on 5/27/26.
//

#include "peer_handlers.h"
#include "../Network/peer_book.h"
#include "../Network/peer_info.h"
#include "../Network/node_id.h"
#include "../Network/endpoint.h"
#include "../Util/base58.h"
#include "../Util/allocator.h"
#include "../QR/qr.h"
#include <stdlib.h>
#include <string.h>

/* Peer-list entry map keys live in client_api_wire.h
   (CLIENT_API_PEER_LIST_KEY_*); shared with the C client binding. */

int peer_info_from_payload(uint8_t format, const uint8_t* data,
                           size_t data_size, peer_info_t* info) {
  if (info == NULL) return -1;

  if (format == 0) {
    /* CBOR bytes */
    struct cbor_load_result load_result;
    cbor_item_t* decoded = cbor_load(data, data_size, &load_result);
    if (decoded == NULL || load_result.error.code != CBOR_ERR_NONE) {
      if (decoded != NULL) cbor_decref(&decoded);
      return -1;
    }
    int rc = peer_info_decode(decoded, info);
    cbor_decref(&decoded);
    return rc;
  }

  if (format == 1) {
    /* Base58 text */
    char* b58_str = get_clear_memory(data_size + 1);
    if (b58_str == NULL) return -1;
    memcpy(b58_str, data, data_size);
    b58_str[data_size] = '\0';
    int rc = peer_info_from_base58(b58_str, info);
    free(b58_str);
    return rc;
  }

  if (format == 2) {
    /* PPM QR image → payload bytes → CBOR peer_info */
    size_t payload_len = 0;
    uint8_t* payload = qr_decode_from_ppm(data, data_size, &payload_len);
    if (payload == NULL) return -1;
    struct cbor_load_result load_result;
    cbor_item_t* decoded = cbor_load(payload, payload_len, &load_result);
    free(payload);
    if (decoded == NULL || load_result.error.code != CBOR_ERR_NONE) {
      if (decoded != NULL) cbor_decref(&decoded);
      return -1;
    }
    int rc = peer_info_decode(decoded, info);
    cbor_decref(&decoded);
    return rc;
  }

  return -1;
}

void peer_handle_info_request(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  uint8_t format = 0;
  if (client_api_peer_info_request_decode(frame, &format) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST,
                    "Invalid peer info request");
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
  /* Populate candidate addresses: include LAN (HOST) candidates only for
     authenticated friends (privacy — never broadcast internal IPs to
     arbitrary peers or in DHT gossip). SRFLX + RELAY candidates are always
     safe to share. See audit #18. */
  if (peer_info_from_node(&local_info, ctx->network,
                          ctx->is_authenticated != 0) != 0) {
    local_info.public_key = NULL;  /* borrowed — don't let destroy free it */
    peer_info_destroy(&local_info);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Failed to populate local addresses");
    return;
  }

  /* Encode to CBOR and serialize to bytes */
  cbor_item_t* cbor_map = peer_info_encode(&local_info);
  if (cbor_map == NULL) {
    /* public_key is borrowed from authority — NULL it so peer_info_destroy
       only frees the addresses we allocated in peer_info_from_node. */
    local_info.public_key = NULL;
    peer_info_destroy(&local_info);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Failed to encode peer info");
    return;
  }
  /* CBOR encode has copied address data into refcounted cbor items; release
     our address array (public_key is still borrowed — NULL before destroy). */
  local_info.public_key = NULL;
  peer_info_destroy(&local_info);

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

  if (format == 2) {
    /* PPM QR image — ownership of the encoded image transfers to the
       response struct; client_api_peer_info_response_destroy frees it. */
    size_t ppm_len = 0;
    uint8_t* ppm = qr_encode_to_ppm(serialized, bytes_serialized, &ppm_len);
    free(serialized);
    if (ppm == NULL) {
      ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                      "QR encoding failed");
      return;
    }
    response.format = 2;
    response.data = ppm;
    response.data_size = ppm_len;
  } else if (format == 1) {
    /* Base58 text — encode the CBOR payload so the format label matches. */
    size_t b58_len = base58_encoded_length(bytes_serialized) + 1;
    char* b58 = get_clear_memory(b58_len);
    int encoded_len = base58_encode(serialized, bytes_serialized, b58, b58_len);
    free(serialized);
    if (encoded_len <= 0) {
      ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                      "Base58 encoding failed");
      return;
    }
    response.format = 1;
    response.data = (uint8_t*)b58;
    response.data_size = (size_t)encoded_len;
  } else {
    response.format = 0;  /* raw CBOR */
    response.data = serialized;
    response.data_size = bytes_serialized;
  }

  cbor_item_t* out_frame = client_api_peer_info_response_encode(&response);
  client_api_peer_info_response_destroy(&response);
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
  int decode_ok = peer_info_from_payload(msg.format, msg.data, msg.data_size,
                                         &remote_info);

  client_api_peer_connect_destroy(&msg);

  if (decode_ok != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Failed to decode peer info");
    return;
  }

  /* Try candidates in priority order: HOST (LAN) -> SRFLX (reflexive) ->
     DIRECT (back-compat) -> RELAY. The first that connects wins. RELAY
     candidates are admitted via the connection manager with
     relay_endpoint_id set; no QUIC connection is needed. See audit #18. */
  int connected = (network_connect_peer_candidates(ctx->network,
                                                   &remote_info.node_id,
                                                   remote_info.addresses,
                                                   remote_info.address_count,
                                                   false) == 0) ? 1 : 0;

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
    cbor_item_t* key = cbor_build_uint8(CLIENT_API_PEER_LIST_KEY_NODE_ID);
    cbor_item_t* val = cbor_build_bytestring(peer->remote_node_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_map_add(peer_map, (struct cbor_pair){.key = key, .value = val});
    cbor_decref(&key);
    cbor_decref(&val);

    /* connected (uint) */
    key = cbor_build_uint8(CLIENT_API_PEER_LIST_KEY_CONNECTED);
    val = cbor_build_uint8(peer->connected ? 1 : 0);
    (void)cbor_map_add(peer_map, (struct cbor_pair){.key = key, .value = val});
    cbor_decref(&key);
    cbor_decref(&val);

    /* is_friend (uint) */
    key = cbor_build_uint8(CLIENT_API_PEER_LIST_KEY_IS_FRIEND);
    val = cbor_build_uint8(peer->is_friend ? 1 : 0);
    (void)cbor_map_add(peer_map, (struct cbor_pair){.key = key, .value = val});
    cbor_decref(&key);
    cbor_decref(&val);

    /* rtt_ms (float) */
    key = cbor_build_uint8(CLIENT_API_PEER_LIST_KEY_RTT_MS);
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

  if (ctx->network == NULL || ctx->network->peer_book == NULL) {
    /* Transports wired without a node borrow (cache-only deployments) have no
       peer-book actor (hence no peering state to mutate); fail cleanly
       instead of dereferencing NULL. */
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Peering unavailable on this transport");
    return;
  }

  client_api_friend_add_t msg;
  if (client_api_friend_add_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid friend add message");
    return;
  }

  /* Decode into a caller-owned peer_info_t: the peer-book actor deep-copies
     it into the list, so this copy stays ours for the best-effort connect
     below and is destroyed here. */
  peer_info_t new_friend;
  memset(&new_friend, 0, sizeof(new_friend));

  int decode_ok = peer_info_from_payload(msg.format, msg.data, msg.data_size,
                                         &new_friend);

  client_api_friend_add_destroy(&msg);

  if (decode_ok != 0) {
    peer_info_destroy(&new_friend);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Failed to decode friend peer info");
    return;
  }

  /* Add through the peer-book actor (round-trip; -1 = invalid/OOM/timeout,
     -2 = already a friend). A timed-out add may still have been applied —
     reported as BAD_REQUEST. */
  int add_result = peer_book_friend_add(ctx->network->peer_book, &new_friend,
                                        PEER_BOOK_TIMEOUT_MS);

  if (add_result != 0 && add_result != -2) {
    peer_info_destroy(&new_friend);
    client_api_peer_connect_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = CLIENT_API_STATUS_BAD_REQUEST;

    cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result);
    ctx->send_frame(ctx->conn, out_frame);
    return;
  }

  /* Persist via the debounced dirty flag — previously only the HTTP handlers
     saved immediately, so a crash before shutdown could lose friend changes. */
  network_mark_peer_state_dirty(ctx->network);

  /* Try candidates in priority order: HOST -> SRFLX -> DIRECT -> RELAY.
     Friend peers are admitted via connection_manager_add_friend so they
     skip Hebbian decay eviction. See audit #18. */
  int connected = (network_connect_peer_candidates(ctx->network,
                                                   &new_friend.node_id,
                                                   new_friend.addresses,
                                                   new_friend.address_count,
                                                   true) == 0) ? 1 : 0;

  peer_info_destroy(&new_friend);

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

  if (ctx->network == NULL || ctx->authority == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Peering unavailable on this transport");
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

  /* Remove through the peer-book actor. 0 = removed, -1 = not found (the
     wire result frame carries no distinction and the call has always been
     idempotent here — both map to OK). -1 after a timeout also lands here;
     the mutation may still have been applied by the actor. */
  (void)peer_book_friend_remove(ctx->network->peer_book, &target_id,
                                PEER_BOOK_TIMEOUT_MS);

  /* Also remove from connection_manager */
  connection_manager_remove(&ctx->network->conn_mgr, &target_id);

  /* Persist via the debounced dirty flag — previously only the HTTP handlers
     saved immediately, so a crash before shutdown could lose friend changes. */
  network_mark_peer_state_dirty(ctx->network);

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

  if (ctx->network == NULL || ctx->network->peer_book == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Peering unavailable on this transport");
    return;
  }

  /* Snapshot the friend list through the peer-book actor: the reply holds
     deep-copied peer_info_t entries owned by this handler. */
  peer_info_t** friends = NULL;
  size_t friend_count = 0;
  if (peer_book_snapshot_friends(ctx->network->peer_book, &friends,
                                 &friend_count,
                                 PEER_BOOK_TIMEOUT_MS) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Friend list snapshot failed");
    return;
  }

  cbor_item_t* friends_array = cbor_new_definite_array(friend_count);

  for (size_t index = 0; index < friend_count; index++) {
    peer_info_t* friend_info = friends[index];
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

  peer_book_free_peer_info_array(friends, friend_count);
  ctx->send_frame(ctx->conn, out_frame);
}

/* Build and push one [host: string, port: uint16, source: uint8] entry into
   the bootstrap list response array. Unparseable endpoints are skipped. */
static void _push_bootstrap_entry(cbor_item_t* entries, const char* endpoint,
                                  uint8_t source) {
  char host[256];
  uint16_t port = 0;
  if (endpoint == NULL) return;
  if (endpoint_parse(endpoint, host, sizeof(host), &port) != 0) return;

  cbor_item_t* entry = cbor_new_definite_array(3);
  cbor_item_t* host_item = cbor_build_string(host);
  cbor_item_t* port_item = cbor_build_uint16(port);
  cbor_item_t* source_item = cbor_build_uint8(source);
  if (entry == NULL || host_item == NULL || port_item == NULL ||
      source_item == NULL) {
    if (host_item != NULL) cbor_decref(&host_item);
    if (port_item != NULL) cbor_decref(&port_item);
    if (source_item != NULL) cbor_decref(&source_item);
    if (entry != NULL) cbor_decref(&entry);
    return;
  }

  (void)cbor_array_push(entry, host_item);
  cbor_decref(&host_item);
  (void)cbor_array_push(entry, port_item);
  cbor_decref(&port_item);
  (void)cbor_array_push(entry, source_item);
  cbor_decref(&source_item);

  (void)cbor_array_push(entries, entry);
  cbor_decref(&entry);
}

void peer_handle_bootstrap_add(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  if (ctx->network == NULL || ctx->network->peer_book == NULL) {
    /* Transports wired without a node borrow (cache-only deployments) have no
       peer-book actor (hence no peering state to mutate); fail cleanly
       instead of dereferencing NULL. */
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Peering unavailable on this transport");
    return;
  }

  client_api_bootstrap_add_t msg;
  if (client_api_bootstrap_add_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST,
                    "Invalid bootstrap add message");
    return;
  }

  /* The peer-book actor parses and re-encodes the endpoint into its own
     storage (authority_bootstrap_add runs on the actor thread now), so the
     decoded string can be freed right after the round-trip. */
  int add_result = peer_book_bootstrap_add(ctx->network->peer_book, msg.endpoint,
                                           PEER_BOOK_TIMEOUT_MS);

  char host[256];
  uint16_t port = 0;
  int parsed = endpoint_parse(msg.endpoint, host, sizeof(host), &port);
  client_api_bootstrap_add_destroy(&msg);

  if (add_result != 0) {
    /* -1 covers both invalid endpoint and allocation failure inside the
       authority (indistinguishable from the caller's side); -2 is a
       duplicate in either list, reported as CONFLICT to match the HTTP
       friend route's already_friend mapping. */
    client_api_peer_connect_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = (add_result == -2) ? CLIENT_API_STATUS_CONFLICT
                                       : CLIENT_API_STATUS_BAD_REQUEST;

    cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result);
    ctx->send_frame(ctx->conn, out_frame);
    return;
  }

  /* Persist via the debounced dirty flag. */
  network_mark_peer_state_dirty(ctx->network);

  /* Fire-and-forget connect to the newly added bootstrap peer. */
  if (parsed == 0) {
    network_connect_peer(ctx->network, host, port);
  }

  client_api_peer_connect_result_t result;
  memset(&result, 0, sizeof(result));
  result.status = CLIENT_API_STATUS_OK;

  cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_bootstrap_remove(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  if (ctx->network == NULL || ctx->authority == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Peering unavailable on this transport");
    return;
  }

  client_api_bootstrap_remove_t msg;
  if (client_api_bootstrap_remove_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST,
                    "Invalid bootstrap remove message");
    return;
  }

  int remove_result = peer_book_bootstrap_remove(ctx->network->peer_book,
                                                 msg.endpoint,
                                                 PEER_BOOK_TIMEOUT_MS);
  client_api_bootstrap_remove_destroy(&msg);

  client_api_peer_connect_result_t result;
  memset(&result, 0, sizeof(result));
  if (remove_result == 0) {
    result.status = CLIENT_API_STATUS_OK;

    /* Persist via the debounced dirty flag. */
    network_mark_peer_state_dirty(ctx->network);
  } else if (remove_result == -2) {
    /* Config-seeded entries are immutable at runtime. The wire result frame
       carries only a status, so the CONFLICT byte is the message. */
    result.status = CLIENT_API_STATUS_CONFLICT;
  } else {
    result.status = CLIENT_API_STATUS_NOT_FOUND;
  }

  cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_bootstrap_list_request(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  (void)frame; /* no payload */

  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  if (ctx->network == NULL || ctx->network->peer_book == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Peering unavailable on this transport");
    return;
  }

  /* Snapshot the bootstrap lists through the peer-book actor: config-seeded
     and operator-managed endpoints arrive as owned heap strings. */
  char** config_endpoints = NULL;
  size_t config_count = 0;
  char** managed_endpoints = NULL;
  size_t managed_count = 0;
  peer_info_t** config_infos = NULL;
  uint8_t* config_pinned = NULL;
  size_t config_info_count = 0;
  if (peer_book_snapshot_bootstrap(ctx->network->peer_book, &config_endpoints,
                                   &config_count, &config_infos,
                                   &config_pinned, &config_info_count,
                                   &managed_endpoints, &managed_count,
                                   PEER_BOOK_TIMEOUT_MS) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Bootstrap list snapshot failed");
    return;
  }

  cbor_item_t* entries = cbor_new_definite_array(config_count + managed_count);
  if (entries == NULL) {
    peer_book_free_peer_info_array(config_infos, config_info_count);
    free(config_pinned);
    peer_book_free_string_array(config_endpoints, config_count);
    peer_book_free_string_array(managed_endpoints, managed_count);
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Memory allocation failed");
    return;
  }

  for (size_t index = 0; index < config_count; index++) {
    _push_bootstrap_entry(entries, config_endpoints[index],
                          CLIENT_API_BOOTSTRAP_SOURCE_CONFIG);
  }
  for (size_t index = 0; index < managed_count; index++) {
    _push_bootstrap_entry(entries, managed_endpoints[index],
                          CLIENT_API_BOOTSTRAP_SOURCE_MANAGED);
  }
  peer_book_free_peer_info_array(config_infos, config_info_count);
  free(config_pinned);
  peer_book_free_string_array(config_endpoints, config_count);
  peer_book_free_string_array(managed_endpoints, managed_count);

  client_api_bootstrap_list_response_t response;
  memset(&response, 0, sizeof(response));
  response.entries = entries;

  cbor_item_t* out_frame = client_api_bootstrap_list_response_encode(&response);
  client_api_bootstrap_list_response_destroy(&response);
  ctx->send_frame(ctx->conn, out_frame);
}
