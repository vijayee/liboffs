//
// Created by victor on 5/14/25.
//

#include "wire.h"
#include <string.h>

// --- Helper functions ---

static cbor_item_t* _node_id_encode(const node_id_t* id) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* hash = cbor_build_bytestring(id->hash, NODE_ID_HASH_SIZE);
  cbor_item_t* str = cbor_build_bytestring((const unsigned char*)id->str, strlen(id->str));
  (void)cbor_array_push(array, hash);
  (void)cbor_array_push(array, str);
  cbor_decref(&hash);
  cbor_decref(&str);
  return array;
}

static int _node_id_decode(cbor_item_t* item, node_id_t* id) {
  if (cbor_array_size(item) < 2) return -1;
  cbor_item_t* hash_item = cbor_array_get(item, 0);
  cbor_item_t* str_item = cbor_array_get(item, 1);
  if (!cbor_isa_bytestring(hash_item) || !cbor_isa_bytestring(str_item)) {
    cbor_decref(&hash_item);
    cbor_decref(&str_item);
    return -1;
  }
  if (cbor_bytestring_length(hash_item) != NODE_ID_HASH_SIZE) {
    cbor_decref(&hash_item);
    cbor_decref(&str_item);
    return -1;
  }
  memcpy(id->hash, cbor_bytestring_handle(hash_item), NODE_ID_HASH_SIZE);
  size_t str_len = cbor_bytestring_length(str_item);
  size_t copy_len = str_len < NODE_ID_STRING_SIZE - 1 ? str_len : NODE_ID_STRING_SIZE - 1;
  memcpy(id->str, cbor_bytestring_handle(str_item), copy_len);
  id->str[copy_len] = '\0';
  cbor_decref(&hash_item);
  cbor_decref(&str_item);
  return 0;
}

uint8_t wire_get_type(cbor_item_t* item) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 1) return 0;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  uint8_t type = (uint8_t)cbor_get_uint8(type_item);
  cbor_decref(&type_item);
  return type;
}

// --- Ping ---

cbor_item_t* wire_ping_encode(const wire_ping_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->timestamp);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_ping_decode(cbor_item_t* item, wire_ping_t* msg) {
  if (cbor_array_size(item) < 4) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* ts = cbor_array_get(item, 3);
  msg->timestamp = cbor_get_uint64(ts);
  cbor_decref(&ts);
  return 0;
}

// --- PingResponse ---

cbor_item_t* wire_ping_response_encode(const wire_ping_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->echo_time);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_new_float8();
  cbor_set_float8(item, (double)msg->capacity);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->phase);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_ping_response_decode(cbor_item_t* item, wire_ping_response_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* echo = cbor_array_get(item, 3);
  msg->echo_time = cbor_get_uint64(echo);
  cbor_decref(&echo);
  cbor_item_t* cap = cbor_array_get(item, 4);
  msg->capacity = (float)cbor_float_get_float8(cap);
  cbor_decref(&cap);
  cbor_item_t* phase = cbor_array_get(item, 5);
  msg->phase = (node_phase_e)cbor_get_uint8(phase);
  cbor_decref(&phase);
  return 0;
}

// --- PingBlock ---

cbor_item_t* wire_ping_block_encode(const wire_ping_block_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_BLOCK);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* id = cbor_new_definite_array(2);
  cbor_item_t* id_hi = cbor_build_uint64(msg->message_id >> 32);
  cbor_item_t* id_lo = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(id, id_hi);
  (void)cbor_array_push(id, id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  (void)cbor_array_push(array, id);
  cbor_decref(&id);

  item = cbor_build_bytestring(msg->block_hash, 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_ping_block_decode(cbor_item_t* item, wire_ping_block_t* msg) {
  if (cbor_array_size(item) < 3) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_BLOCK) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_arr = cbor_array_get(item, 1);
  if (cbor_array_size(id_arr) < 2) { cbor_decref(&id_arr); return -1; }
  cbor_item_t* id_hi = cbor_array_get(id_arr, 0);
  cbor_item_t* id_lo = cbor_array_get(id_arr, 1);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_decref(&id_arr);
  cbor_item_t* hash_item = cbor_array_get(item, 2);
  if (cbor_bytestring_length(hash_item) != 32) { cbor_decref(&hash_item); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash_item), 32);
  cbor_decref(&hash_item);
  return 0;
}

// --- PingBlockResponse ---

cbor_item_t* wire_ping_block_response_encode(const wire_ping_block_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_BLOCK_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->exists);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->fib);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->healthy);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_ping_block_response_decode(cbor_item_t* item, wire_ping_block_response_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_BLOCK_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* exists = cbor_array_get(item, 3);
  msg->exists = cbor_get_uint8(exists);
  cbor_decref(&exists);
  cbor_item_t* fib = cbor_array_get(item, 4);
  msg->fib = cbor_get_uint32(fib);
  cbor_decref(&fib);
  cbor_item_t* healthy = cbor_array_get(item, 5);
  msg->healthy = cbor_get_uint8(healthy);
  cbor_decref(&healthy);
  return 0;
}

// --- FindNode ---

cbor_item_t* wire_find_node_encode(const wire_find_node_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_FIND_NODE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* target = _node_id_encode(&msg->target_id);
  (void)cbor_array_push(array, target);
  cbor_decref(&target);

  return array;
}

int wire_find_node_decode(cbor_item_t* item, wire_find_node_t* msg) {
  if (cbor_array_size(item) < 4) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_FIND_NODE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* target = cbor_array_get(item, 3);
  int rc = _node_id_decode(target, &msg->target_id);
  cbor_decref(&target);
  return rc;
}

// --- FindNodeResponse ---

cbor_item_t* wire_find_node_response_encode(const wire_find_node_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_FIND_NODE_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* nodes = cbor_new_definite_array(msg->closest_count);
  for (uint8_t index = 0; index < msg->closest_count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->closest_nodes[index]);
    (void)cbor_array_push(nodes, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, nodes);
  cbor_decref(&nodes);

  return array;
}

int wire_find_node_response_decode(cbor_item_t* item, wire_find_node_response_t* msg) {
  if (cbor_array_size(item) < 4) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_FIND_NODE_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* nodes = cbor_array_get(item, 3);
  size_t count = cbor_array_size(nodes);
  if (count > 8) count = 8;
  msg->closest_count = (uint8_t)count;
  for (size_t index = 0; index < count; index++) {
    cbor_item_t* node_item = cbor_array_get(nodes, index);
    _node_id_decode(node_item, &msg->closest_nodes[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&nodes);
  return 0;
}

// --- RankBlock ---

cbor_item_t* wire_rank_block_encode(const wire_rank_block_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RANK_BLOCK);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->block_hash, 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->fib);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* origin = _node_id_encode(&msg->origin);
  (void)cbor_array_push(array, origin);
  cbor_decref(&origin);

  item = cbor_build_uint8(msg->hop_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_rank_block_decode(cbor_item_t* item, wire_rank_block_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RANK_BLOCK) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* hash_item = cbor_array_get(item, 1);
  if (cbor_bytestring_length(hash_item) != 32) { cbor_decref(&hash_item); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash_item), 32);
  cbor_decref(&hash_item);
  cbor_item_t* fib = cbor_array_get(item, 2);
  msg->fib = cbor_get_uint32(fib);
  cbor_decref(&fib);
  cbor_item_t* count = cbor_array_get(item, 3);
  msg->count = cbor_get_uint32(count);
  cbor_decref(&count);
  cbor_item_t* origin = cbor_array_get(item, 4);
  int rc = _node_id_decode(origin, &msg->origin);
  cbor_decref(&origin);
  if (rc != 0) return rc;
  cbor_item_t* hop = cbor_array_get(item, 5);
  msg->hop_count = cbor_get_uint8(hop);
  cbor_decref(&hop);
  return 0;
}

// --- RecallBlock ---

cbor_item_t* wire_recall_block_encode(const wire_recall_block_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RECALL_BLOCK);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->block_hash, 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_recall_block_decode(cbor_item_t* item, wire_recall_block_t* msg) {
  if (cbor_array_size(item) < 4) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RECALL_BLOCK) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* hash_item = cbor_array_get(item, 3);
  if (cbor_bytestring_length(hash_item) != 32) { cbor_decref(&hash_item); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash_item), 32);
  cbor_decref(&hash_item);
  return 0;
}

// --- RecallAccept / RecallDecline ---

cbor_item_t* wire_recall_accept_encode(const wire_recall_accept_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RECALL_ACCEPT);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_recall_accept_decode(cbor_item_t* item, wire_recall_accept_t* msg) {
  if (cbor_array_size(item) < 3) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RECALL_ACCEPT) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  return 0;
}

cbor_item_t* wire_recall_decline_encode(const wire_recall_decline_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RECALL_DECLINE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_recall_decline_decode(cbor_item_t* item, wire_recall_decline_t* msg) {
  if (cbor_array_size(item) < 3) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RECALL_DECLINE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  return 0;
}

// --- RateLimited ---

cbor_item_t* wire_rate_limited_encode(const wire_rate_limited_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RATE_LIMITED);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->type);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->retry_after_ms);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_new_float8();
  cbor_set_float8(item, (double)msg->current_limit);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_rate_limited_decode(cbor_item_t* item, wire_rate_limited_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RATE_LIMITED) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* rpc_type = cbor_array_get(item, 3);
  msg->type = cbor_get_uint8(rpc_type);
  cbor_decref(&rpc_type);
  cbor_item_t* retry = cbor_array_get(item, 4);
  msg->retry_after_ms = cbor_get_uint32(retry);
  cbor_decref(&retry);
  cbor_item_t* limit = cbor_array_get(item, 5);
  msg->current_limit = (float)cbor_float_get_float8(limit);
  cbor_decref(&limit);
  return 0;
}

// Stub encode/decode for remaining complex message types
// (FindBlock, FindBlockResponse, StoreBlock, StoreBlockResponse,
//  PingCapacity, PingCapacityResponse, SeekingBlocks, SeekingBlocksResponse)
// These will be implemented in subsequent tickets as their handlers are built.

cbor_item_t* wire_ping_capacity_encode(const wire_ping_capacity_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_ping_capacity_decode(cbor_item_t* item, wire_ping_capacity_t* msg) {
  (void)item; (void)msg;
  return -1;
}
cbor_item_t* wire_ping_capacity_response_encode(const wire_ping_capacity_response_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_ping_capacity_response_decode(cbor_item_t* item, wire_ping_capacity_response_t* msg) {
  (void)item; (void)msg;
  return -1;
}
cbor_item_t* wire_find_block_encode(const wire_find_block_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_find_block_decode(cbor_item_t* item, wire_find_block_t* msg) {
  (void)item; (void)msg;
  return -1;
}
cbor_item_t* wire_find_block_response_encode(const wire_find_block_response_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_find_block_response_decode(cbor_item_t* item, wire_find_block_response_t* msg) {
  (void)item; (void)msg;
  return -1;
}
cbor_item_t* wire_store_block_encode(const wire_store_block_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_store_block_decode(cbor_item_t* item, wire_store_block_t* msg) {
  (void)item; (void)msg;
  return -1;
}
cbor_item_t* wire_store_block_response_encode(const wire_store_block_response_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_store_block_response_decode(cbor_item_t* item, wire_store_block_response_t* msg) {
  (void)item; (void)msg;
  return -1;
}
cbor_item_t* wire_seeking_blocks_encode(const wire_seeking_blocks_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_seeking_blocks_decode(cbor_item_t* item, wire_seeking_blocks_t* msg) {
  (void)item; (void)msg;
  return -1;
}
cbor_item_t* wire_seeking_blocks_response_encode(const wire_seeking_blocks_response_t* msg) {
  (void)msg;
  return cbor_new_definite_array(0);
}
int wire_seeking_blocks_response_decode(cbor_item_t* item, wire_seeking_blocks_response_t* msg) {
  (void)item; (void)msg;
  return -1;
}