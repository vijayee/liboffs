//
// Created by victor on 5/14/25.
//

#include "wire.h"
#include "../Util/allocator.h"
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

int wire_extract_sender_id(cbor_item_t* item, node_id_t* sender_id) {
  if (item == NULL || sender_id == NULL) return -1;
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int result = _node_id_decode(sender, sender_id);
  cbor_decref(&sender);
  return result;
}

// --- Ping ---

cbor_item_t* wire_ping_encode(const wire_ping_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(5);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 5) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* ts = cbor_array_get(item, 4);
  msg->timestamp = cbor_get_uint64(ts);
  cbor_decref(&ts);
  return 0;
}

// --- PingResponse ---

cbor_item_t* wire_ping_response_encode(const wire_ping_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(7);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 7) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* echo = cbor_array_get(item, 4);
  msg->echo_time = cbor_get_uint64(echo);
  cbor_decref(&echo);
  cbor_item_t* cap = cbor_array_get(item, 5);
  msg->capacity = (float)cbor_float_get_float8(cap);
  cbor_decref(&cap);
  cbor_item_t* phase = cbor_array_get(item, 6);
  msg->phase = (node_phase_e)cbor_get_uint8(phase);
  cbor_decref(&phase);
  return 0;
}

// --- PingBlock ---

cbor_item_t* wire_ping_block_encode(const wire_ping_block_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_BLOCK);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 4) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_BLOCK) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_arr = cbor_array_get(item, 2);
  if (cbor_array_size(id_arr) < 2) { cbor_decref(&id_arr); return -1; }
  cbor_item_t* id_hi = cbor_array_get(id_arr, 0);
  cbor_item_t* id_lo = cbor_array_get(id_arr, 1);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_decref(&id_arr);
  cbor_item_t* hash_item = cbor_array_get(item, 3);
  if (cbor_bytestring_length(hash_item) != 32) { cbor_decref(&hash_item); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash_item), 32);
  cbor_decref(&hash_item);
  return 0;
}

// --- PingBlockResponse ---

cbor_item_t* wire_ping_block_response_encode(const wire_ping_block_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(7);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_BLOCK_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 7) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_BLOCK_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* exists = cbor_array_get(item, 4);
  msg->exists = cbor_get_uint8(exists);
  cbor_decref(&exists);
  cbor_item_t* fib = cbor_array_get(item, 5);
  msg->fib = cbor_get_uint32(fib);
  cbor_decref(&fib);
  cbor_item_t* healthy = cbor_array_get(item, 6);
  msg->healthy = cbor_get_uint8(healthy);
  cbor_decref(&healthy);
  return 0;
}

// --- FindNode ---

cbor_item_t* wire_find_node_encode(const wire_find_node_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(5);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_FIND_NODE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 5) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_FIND_NODE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* target = cbor_array_get(item, 4);
  int rc = _node_id_decode(target, &msg->target_id);
  cbor_decref(&target);
  return rc;
}

// --- FindNodeResponse ---

cbor_item_t* wire_find_node_response_encode(const wire_find_node_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(5);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_FIND_NODE_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 5) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_FIND_NODE_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* nodes = cbor_array_get(item, 4);
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
  cbor_item_t* array = cbor_new_definite_array(5);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RECALL_BLOCK);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 5) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RECALL_BLOCK) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* hash_item = cbor_array_get(item, 4);
  if (cbor_bytestring_length(hash_item) != 32) { cbor_decref(&hash_item); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash_item), 32);
  cbor_decref(&hash_item);
  return 0;
}

// --- RecallAccept / RecallDecline ---

cbor_item_t* wire_recall_accept_encode(const wire_recall_accept_t* msg) {
  int has_block_data = msg->block_data != NULL && msg->block_data_len > 0;
  int num_elements = has_block_data ? 8 : 5;
  cbor_item_t* array = cbor_new_definite_array(num_elements);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RECALL_ACCEPT);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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

  if (has_block_data) {
    item = cbor_build_bytestring(msg->block_data, msg->block_data_len);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);

    item = cbor_build_uint64(msg->block_data_len);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);

    item = cbor_build_uint32(msg->block_fib);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }

  return array;
}

int wire_recall_accept_decode(cbor_item_t* item, wire_recall_accept_t* msg) {
  size_t array_size = cbor_array_size(item);
  if (array_size < 5) return -1;
  msg->block_data = NULL;
  msg->block_data_len = 0;
  msg->block_fib = 0;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RECALL_ACCEPT) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* hash = cbor_array_get(item, 4);
  if (cbor_bytestring_length(hash) != 32) { cbor_decref(&hash); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash), 32);
  cbor_decref(&hash);
  if (array_size >= 8) {
    cbor_item_t* data = cbor_array_get(item, 5);
    if (cbor_isa_bytestring(data) && cbor_bytestring_length(data) > 0) {
      msg->block_data_len = cbor_bytestring_length(data);
      msg->block_data = get_clear_memory(msg->block_data_len);
      if (msg->block_data != NULL) {
        memcpy(msg->block_data, cbor_bytestring_handle(data), msg->block_data_len);
      }
    }
    cbor_decref(&data);
    cbor_item_t* data_len = cbor_array_get(item, 6);
    msg->block_data_len = (size_t)cbor_get_uint64(data_len);
    cbor_decref(&data_len);
    cbor_item_t* bfib = cbor_array_get(item, 7);
    msg->block_fib = cbor_get_uint32(bfib);
    cbor_decref(&bfib);
  }
  return 0;
}

cbor_item_t* wire_recall_decline_encode(const wire_recall_decline_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(5);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RECALL_DECLINE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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

int wire_recall_decline_decode(cbor_item_t* item, wire_recall_decline_t* msg) {
  if (cbor_array_size(item) < 5) return -1;
  memset(msg, 0, sizeof(*msg));
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RECALL_DECLINE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* hash = cbor_array_get(item, 4);
  if (cbor_isa_bytestring(hash) && cbor_bytestring_length(hash) == 32) {
    memcpy(msg->block_hash, cbor_bytestring_handle(hash), 32);
  }
  cbor_decref(&hash);
  return 0;
}

// --- RateLimited ---

cbor_item_t* wire_rate_limited_encode(const wire_rate_limited_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(7);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RATE_LIMITED);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
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
  if (cbor_array_size(item) < 7) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RATE_LIMITED) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* rpc_type = cbor_array_get(item, 4);
  msg->type = cbor_get_uint8(rpc_type);
  cbor_decref(&rpc_type);
  cbor_item_t* retry = cbor_array_get(item, 5);
  msg->retry_after_ms = cbor_get_uint32(retry);
  cbor_decref(&retry);
  cbor_item_t* limit = cbor_array_get(item, 6);
  msg->current_limit = (float)cbor_float_get_float8(limit);
  cbor_decref(&limit);
  return 0;
}

// --- Salutation ---

cbor_item_t* wire_salutation_encode(const wire_salutation_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_SALUTATION);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->public_key != NULL && msg->public_key_len > 0) {
    item = cbor_build_bytestring(msg->public_key, msg->public_key_len);
  } else {
    item = cbor_build_bytestring((const unsigned char*)"", 0);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->public_key_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_salutation_decode(cbor_item_t* item, wire_salutation_t* msg) {
  if (cbor_array_size(item) < 4) return -1;
  msg->public_key = NULL;
  msg->public_key_len = 0;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_SALUTATION) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* key_data = cbor_array_get(item, 2);
  if (cbor_isa_bytestring(key_data) && cbor_bytestring_length(key_data) > 0) {
    msg->public_key_len = cbor_bytestring_length(key_data);
    msg->public_key = get_clear_memory(msg->public_key_len);
    if (msg->public_key != NULL) {
      memcpy(msg->public_key, cbor_bytestring_handle(key_data), msg->public_key_len);
    }
  }
  cbor_decref(&key_data);
  cbor_item_t* key_len = cbor_array_get(item, 3);
  size_t declared_len = (size_t)cbor_get_uint64(key_len);
  cbor_decref(&key_len);
  // If declared length disagrees with actual bytestring length, trust the bytestring
  if (msg->public_key_len != 0 && declared_len != msg->public_key_len) {
    free(msg->public_key);
    msg->public_key = NULL;
    msg->public_key_len = 0;
    return -1;
  }
  return 0;
}

void wire_salutation_destroy(wire_salutation_t* msg) {
  if (msg == NULL) return;
  free(msg->public_key);
  free(msg);
}

// --- PingCapacity ---

cbor_item_t* wire_ping_capacity_encode(const wire_ping_capacity_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_CAPACITY);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* source = _node_id_encode(&msg->source);
  (void)cbor_array_push(array, source);
  cbor_decref(&source);

  item = cbor_new_float8();
  cbor_set_float8(item, (double)msg->capacity);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->phase);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_ping_capacity_decode(cbor_item_t* item, wire_ping_capacity_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_CAPACITY) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* source = cbor_array_get(item, 3);
  int rc = _node_id_decode(source, &msg->source);
  cbor_decref(&source);
  if (rc != 0) return rc;
  cbor_item_t* cap = cbor_array_get(item, 4);
  msg->capacity = (float)cbor_float_get_float8(cap);
  cbor_decref(&cap);
  cbor_item_t* phase = cbor_array_get(item, 5);
  msg->phase = (node_phase_e)cbor_get_uint8(phase);
  cbor_decref(&phase);
  return 0;
}

// --- PingCapacityResponse ---

cbor_item_t* wire_ping_capacity_response_encode(const wire_ping_capacity_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_PING_CAPACITY_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
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

int wire_ping_capacity_response_decode(cbor_item_t* item, wire_ping_capacity_response_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_PING_CAPACITY_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* cap = cbor_array_get(item, 4);
  msg->capacity = (float)cbor_float_get_float8(cap);
  cbor_decref(&cap);
  cbor_item_t* phase = cbor_array_get(item, 5);
  msg->phase = (node_phase_e)cbor_get_uint8(phase);
  cbor_decref(&phase);
  return 0;
}

// --- FindBlock ---

cbor_item_t* wire_find_block_encode(const wire_find_block_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(10);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_FIND_BLOCK);
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

  item = cbor_build_uint8(msg->ttl);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->visited_bloom, WIRE_MAX_VISITED_BLOOM);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->visited_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* path = cbor_new_definite_array(msg->path_len);
  for (uint8_t index = 0; index < msg->path_len; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->path[index]);
    (void)cbor_array_push(path, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, path);
  cbor_decref(&path);

  item = cbor_build_uint64(msg->start_time);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* source = _node_id_encode(&msg->original_source);
  (void)cbor_array_push(array, source);
  cbor_decref(&source);

  return array;
}

int wire_find_block_decode(cbor_item_t* item, wire_find_block_t* msg) {
  if (cbor_array_size(item) < 10) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_FIND_BLOCK) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* hash = cbor_array_get(item, 3);
  if (cbor_bytestring_length(hash) != 32) { cbor_decref(&hash); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash), 32);
  cbor_decref(&hash);
  cbor_item_t* ttl = cbor_array_get(item, 4);
  msg->ttl = cbor_get_uint8(ttl);
  cbor_decref(&ttl);
  cbor_item_t* visited = cbor_array_get(item, 5);
  if (cbor_bytestring_length(visited) != WIRE_MAX_VISITED_BLOOM) { cbor_decref(&visited); return -1; }
  memcpy(msg->visited_bloom, cbor_bytestring_handle(visited), WIRE_MAX_VISITED_BLOOM);
  cbor_decref(&visited);
  cbor_item_t* vcount = cbor_array_get(item, 6);
  msg->visited_count = (uint16_t)cbor_get_uint16(vcount);
  cbor_decref(&vcount);
  cbor_item_t* path_arr = cbor_array_get(item, 7);
  size_t path_len = cbor_array_size(path_arr);
  if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  cbor_item_t* start = cbor_array_get(item, 8);
  msg->start_time = cbor_get_uint64(start);
  cbor_decref(&start);
  cbor_item_t* source = cbor_array_get(item, 9);
  int rc = _node_id_decode(source, &msg->original_source);
  cbor_decref(&source);
  return rc;
}

// --- FindBlockResponse ---

cbor_item_t* wire_find_block_response_encode(const wire_find_block_response_t* msg) {
  int has_block_data = msg->found && msg->block_data != NULL && msg->block_data_len > 0;
  int num_elements = has_block_data ? 12 : 9;
  cbor_item_t* array = cbor_new_definite_array(num_elements);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_FIND_BLOCK_RESPONSE);
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

  item = cbor_build_uint8(msg->found);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* holder = _node_id_encode(&msg->holder);
  (void)cbor_array_push(array, holder);
  cbor_decref(&holder);

  item = cbor_build_uint32(msg->fib);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* path = cbor_new_definite_array(msg->path_len);
  for (uint8_t index = 0; index < msg->path_len; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->path[index]);
    (void)cbor_array_push(path, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, path);
  cbor_decref(&path);

  item = cbor_build_uint64(msg->latency_ms);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (has_block_data) {
    item = cbor_build_bytestring(msg->block_data, msg->block_data_len);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);

    item = cbor_build_uint64(msg->block_data_len);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);

    item = cbor_build_uint32(msg->block_fib);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }

  return array;
}

int wire_find_block_response_decode(cbor_item_t* item, wire_find_block_response_t* msg) {
  size_t array_size = cbor_array_size(item);
  if (array_size < 9) return -1;
  msg->block_data = NULL;
  msg->block_data_len = 0;
  msg->block_fib = 0;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_FIND_BLOCK_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* hash = cbor_array_get(item, 3);
  if (cbor_bytestring_length(hash) != 32) { cbor_decref(&hash); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash), 32);
  cbor_decref(&hash);
  cbor_item_t* found = cbor_array_get(item, 4);
  msg->found = cbor_get_uint8(found);
  cbor_decref(&found);
  cbor_item_t* holder = cbor_array_get(item, 5);
  int rc = _node_id_decode(holder, &msg->holder);
  cbor_decref(&holder);
  if (rc != 0) return rc;
  cbor_item_t* fib = cbor_array_get(item, 6);
  msg->fib = cbor_get_uint32(fib);
  cbor_decref(&fib);
  cbor_item_t* path_arr = cbor_array_get(item, 7);
  size_t path_len = cbor_array_size(path_arr);
  if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  cbor_item_t* latency = cbor_array_get(item, 8);
  msg->latency_ms = (uint64_t)cbor_get_uint64(latency);
  cbor_decref(&latency);
  if (array_size >= 12) {
    cbor_item_t* data = cbor_array_get(item, 9);
    if (cbor_isa_bytestring(data) && cbor_bytestring_length(data) > 0) {
      msg->block_data_len = cbor_bytestring_length(data);
      msg->block_data = get_clear_memory(msg->block_data_len);
      if (msg->block_data != NULL) {
        memcpy(msg->block_data, cbor_bytestring_handle(data), msg->block_data_len);
      }
    }
    cbor_decref(&data);
    cbor_item_t* data_len = cbor_array_get(item, 10);
    msg->block_data_len = (size_t)cbor_get_uint64(data_len);
    cbor_decref(&data_len);
    cbor_item_t* bfib = cbor_array_get(item, 11);
    msg->block_fib = cbor_get_uint32(bfib);
    cbor_decref(&bfib);
  }
  return 0;
}

// --- StoreBlock ---

cbor_item_t* wire_store_block_encode(const wire_store_block_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(14);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_STORE_BLOCK);
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

  item = cbor_build_uint32(msg->block_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->block_fib);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->replicas_needed);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->max_hops);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->visited_bloom, WIRE_MAX_VISITED_BLOOM);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->visited_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* path = cbor_new_definite_array(msg->path_len);
  for (uint8_t index = 0; index < msg->path_len; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->path[index]);
    (void)cbor_array_push(path, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, path);
  cbor_decref(&path);

  item = cbor_build_uint64(msg->start_time);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->carry_data);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->carry_data && msg->block_data != NULL && msg->block_data_len > 0) {
    item = cbor_build_bytestring(msg->block_data, msg->block_data_len);
  } else {
    item = cbor_build_bytestring(NULL, 0);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_store_block_decode(cbor_item_t* item, wire_store_block_t* msg) {
  if (cbor_array_size(item) < 14) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_STORE_BLOCK) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* hash = cbor_array_get(item, 3);
  if (cbor_bytestring_length(hash) != 32) { cbor_decref(&hash); return -1; }
  memcpy(msg->block_hash, cbor_bytestring_handle(hash), 32);
  cbor_decref(&hash);
  cbor_item_t* bsize = cbor_array_get(item, 4);
  msg->block_size = cbor_get_uint32(bsize);
  cbor_decref(&bsize);
  cbor_item_t* bfib = cbor_array_get(item, 5);
  msg->block_fib = cbor_get_uint32(bfib);
  cbor_decref(&bfib);
  cbor_item_t* replicas = cbor_array_get(item, 6);
  msg->replicas_needed = cbor_get_uint8(replicas);
  cbor_decref(&replicas);
  cbor_item_t* maxhops = cbor_array_get(item, 7);
  msg->max_hops = cbor_get_uint8(maxhops);
  cbor_decref(&maxhops);
  cbor_item_t* visited = cbor_array_get(item, 8);
  if (cbor_bytestring_length(visited) != WIRE_MAX_VISITED_BLOOM) { cbor_decref(&visited); return -1; }
  memcpy(msg->visited_bloom, cbor_bytestring_handle(visited), WIRE_MAX_VISITED_BLOOM);
  cbor_decref(&visited);
  cbor_item_t* vcount = cbor_array_get(item, 9);
  msg->visited_count = (uint16_t)cbor_get_uint16(vcount);
  cbor_decref(&vcount);
  cbor_item_t* path_arr = cbor_array_get(item, 10);
  size_t path_len = cbor_array_size(path_arr);
  if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  cbor_item_t* start = cbor_array_get(item, 11);
  msg->start_time = cbor_get_uint64(start);
  cbor_decref(&start);
  cbor_item_t* carry = cbor_array_get(item, 12);
  msg->carry_data = cbor_get_uint8(carry);
  cbor_decref(&carry);
  cbor_item_t* data = cbor_array_get(item, 13);
  if (cbor_bytestring_length(data) > 0) {
    msg->block_data_len = cbor_bytestring_length(data);
    msg->block_data = get_clear_memory(msg->block_data_len);
    if (msg->block_data != NULL) {
      memcpy(msg->block_data, cbor_bytestring_handle(data), msg->block_data_len);
    }
  } else {
    msg->block_data = NULL;
    msg->block_data_len = 0;
  }
  cbor_decref(&data);
  return 0;
}

// --- StoreBlockResponse ---

cbor_item_t* wire_store_block_response_encode(const wire_store_block_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(9);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_STORE_BLOCK_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->accepted);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* holder = _node_id_encode(&msg->holder);
  (void)cbor_array_push(array, holder);
  cbor_decref(&holder);

  item = cbor_build_uint8(msg->replicas_remaining);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* path = cbor_new_definite_array(msg->path_len);
  for (uint8_t index = 0; index < msg->path_len; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->path[index]);
    (void)cbor_array_push(path, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, path);
  cbor_decref(&path);

  item = cbor_build_uint64(msg->latency_ms);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->block_hash, 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_store_block_response_decode(cbor_item_t* item, wire_store_block_response_t* msg) {
  if (cbor_array_size(item) < 9) return -1;
  memset(msg, 0, sizeof(*msg));
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_STORE_BLOCK_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* accepted = cbor_array_get(item, 3);
  msg->accepted = cbor_get_uint8(accepted);
  cbor_decref(&accepted);
  cbor_item_t* holder = cbor_array_get(item, 4);
  int rc = _node_id_decode(holder, &msg->holder);
  cbor_decref(&holder);
  if (rc != 0) return rc;
  cbor_item_t* replicas = cbor_array_get(item, 5);
  msg->replicas_remaining = cbor_get_uint8(replicas);
  cbor_decref(&replicas);
  cbor_item_t* path_arr = cbor_array_get(item, 6);
  size_t path_len = cbor_array_size(path_arr);
  if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  cbor_item_t* latency = cbor_array_get(item, 7);
  msg->latency_ms = (uint64_t)cbor_get_uint64(latency);
  cbor_decref(&latency);
  cbor_item_t* hash = cbor_array_get(item, 8);
  if (cbor_isa_bytestring(hash) && cbor_bytestring_length(hash) == 32) {
    memcpy(msg->block_hash, cbor_bytestring_handle(hash), 32);
  }
  cbor_decref(&hash);
  return 0;
}

// --- SeekingBlocks ---

cbor_item_t* wire_seeking_blocks_encode(const wire_seeking_blocks_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_SEEKING_BLOCKS);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_new_float8();
  cbor_set_float8(item, (double)msg->capacity);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* excludes = cbor_new_definite_array(msg->exclude_count);
  for (size_t index = 0; index < msg->exclude_count; index++) {
    cbor_item_t* hash = cbor_build_bytestring(msg->exclude_hashes[index], 32);
    (void)cbor_array_push(excludes, hash);
    cbor_decref(&hash);
  }
  (void)cbor_array_push(array, excludes);
  cbor_decref(&excludes);

  return array;
}

int wire_seeking_blocks_decode(cbor_item_t* item, wire_seeking_blocks_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_SEEKING_BLOCKS) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* cap = cbor_array_get(item, 4);
  msg->capacity = (float)cbor_float_get_float8(cap);
  cbor_decref(&cap);
  cbor_item_t* excludes = cbor_array_get(item, 5);
  size_t count = cbor_array_size(excludes);
  if (count > WIRE_MAX_OFFERS) count = WIRE_MAX_OFFERS;
  msg->exclude_count = 0;
  msg->exclude_hashes = NULL;
  if (count > 0) {
    msg->exclude_hashes = get_clear_memory(count * sizeof(uint8_t*));
    if (msg->exclude_hashes == NULL) { cbor_decref(&excludes); return -1; }
    for (size_t index = 0; index < count; index++) {
      cbor_item_t* hash_item = cbor_array_get(excludes, index);
      if (cbor_isa_bytestring(hash_item) && cbor_bytestring_length(hash_item) == 32) {
        msg->exclude_hashes[msg->exclude_count] = get_clear_memory(32);
        if (msg->exclude_hashes[msg->exclude_count] != NULL) {
          memcpy(msg->exclude_hashes[msg->exclude_count], cbor_bytestring_handle(hash_item), 32);
          msg->exclude_count++;
        }
      }
      cbor_decref(&hash_item);
    }
  }
  cbor_decref(&excludes);
  return 0;
}

// --- SeekingBlocksResponse ---

cbor_item_t* wire_seeking_blocks_response_encode(const wire_seeking_blocks_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(5);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_SEEKING_BLOCKS_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* offers = cbor_new_definite_array(msg->offer_count);
  for (uint8_t index = 0; index < msg->offer_count; index++) {
    cbor_item_t* offer = cbor_new_definite_array(3);
    cbor_item_t* hash = cbor_build_bytestring(msg->offers[index].hash, 32);
    (void)cbor_array_push(offer, hash);
    cbor_decref(&hash);
    cbor_item_t* fib = cbor_build_uint32(msg->offers[index].fib);
    (void)cbor_array_push(offer, fib);
    cbor_decref(&fib);
    cbor_item_t* size = cbor_build_uint32(msg->offers[index].size);
    (void)cbor_array_push(offer, size);
    cbor_decref(&size);
    (void)cbor_array_push(offers, offer);
    cbor_decref(&offer);
  }
  (void)cbor_array_push(array, offers);
  cbor_decref(&offers);

  return array;
}

int wire_seeking_blocks_response_decode(cbor_item_t* item, wire_seeking_blocks_response_t* msg) {
  if (cbor_array_size(item) < 5) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_SEEKING_BLOCKS_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_hi = cbor_array_get(item, 2);
  cbor_item_t* id_lo = cbor_array_get(item, 3);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* offers_arr = cbor_array_get(item, 4);
  size_t count = cbor_array_size(offers_arr);
  if (count > WIRE_MAX_OFFERS) count = WIRE_MAX_OFFERS;
  msg->offer_count = 0;
  for (size_t index = 0; index < count && msg->offer_count < WIRE_MAX_OFFERS; index++) {
    cbor_item_t* offer = cbor_array_get(offers_arr, index);
    if (cbor_array_size(offer) < 3) { cbor_decref(&offer); continue; }
    cbor_item_t* hash = cbor_array_get(offer, 0);
    if (cbor_isa_bytestring(hash) && cbor_bytestring_length(hash) == 32) {
      memcpy(msg->offers[msg->offer_count].hash, cbor_bytestring_handle(hash), 32);
    }
    cbor_decref(&hash);
    cbor_item_t* fib = cbor_array_get(offer, 1);
    msg->offers[msg->offer_count].fib = cbor_get_uint32(fib);
    cbor_decref(&fib);
    cbor_item_t* size = cbor_array_get(offer, 2);
    msg->offers[msg->offer_count].size = cbor_get_uint32(size);
    cbor_decref(&size);
    msg->offer_count++;
    cbor_decref(&offer);
  }
  cbor_decref(&offers_arr);
  return 0;
}

// --- Destroy helpers for wire types with nested allocations ---

void wire_store_block_destroy(wire_store_block_t* msg) {
  if (msg == NULL) return;
  free(msg->block_data);
  free(msg);
}

void wire_find_block_response_destroy(wire_find_block_response_t* msg) {
  if (msg == NULL) return;
  free(msg->block_data);
  free(msg);
}

void wire_recall_accept_destroy(wire_recall_accept_t* msg) {
  if (msg == NULL) return;
  free(msg->block_data);
  free(msg);
}

void wire_seeking_blocks_destroy(wire_seeking_blocks_t* msg) {
  if (msg == NULL) return;
  if (msg->exclude_hashes != NULL) {
    for (size_t index = 0; index < msg->exclude_count; index++) {
      free(msg->exclude_hashes[index]);
    }
    free(msg->exclude_hashes);
  }
  free(msg);
}

// --- RelaySend ---

cbor_item_t* wire_relay_send_encode(const wire_relay_send_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RELAY_SEND);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->src_endpoint_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->dest_endpoint_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->payload, msg->payload_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_relay_send_decode(cbor_item_t* item, wire_relay_send_t* msg) {
  if (cbor_array_size(item) < 4) return -1;
  msg->payload = NULL;
  msg->payload_len = 0;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RELAY_SEND) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* src = cbor_array_get(item, 1);
  msg->src_endpoint_id = cbor_get_uint32(src);
  cbor_decref(&src);
  cbor_item_t* dest = cbor_array_get(item, 2);
  msg->dest_endpoint_id = cbor_get_uint32(dest);
  cbor_decref(&dest);
  cbor_item_t* data = cbor_array_get(item, 3);
  if (cbor_isa_bytestring(data) && cbor_bytestring_length(data) > 0) {
    msg->payload_len = cbor_bytestring_length(data);
    msg->payload = get_clear_memory(msg->payload_len);
    if (msg->payload != NULL) {
      memcpy(msg->payload, cbor_bytestring_handle(data), msg->payload_len);
    }
  }
  cbor_decref(&data);
  return 0;
}

// --- RelayReceived ---

cbor_item_t* wire_relay_received_encode(const wire_relay_received_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_RELAY_RECEIVED);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->src_endpoint_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->payload, msg->payload_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_relay_received_decode(cbor_item_t* item, wire_relay_received_t* msg) {
  if (cbor_array_size(item) < 3) return -1;
  msg->payload = NULL;
  msg->payload_len = 0;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_RELAY_RECEIVED) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* src = cbor_array_get(item, 1);
  msg->src_endpoint_id = cbor_get_uint32(src);
  cbor_decref(&src);
  cbor_item_t* data = cbor_array_get(item, 2);
  if (cbor_isa_bytestring(data) && cbor_bytestring_length(data) > 0) {
    msg->payload_len = cbor_bytestring_length(data);
    msg->payload = get_clear_memory(msg->payload_len);
    if (msg->payload != NULL) {
      memcpy(msg->payload, cbor_bytestring_handle(data), msg->payload_len);
    }
  }
  cbor_decref(&data);
  return 0;
}

// --- AddrRequest ---

cbor_item_t* wire_addr_request_encode(const wire_addr_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_ADDR_REQUEST);
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

int wire_addr_request_decode(cbor_item_t* item, wire_addr_request_t* msg) {
  if (cbor_array_size(item) < 3) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_ADDR_REQUEST) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  return 0;
}

// --- AddrResponse ---

cbor_item_t* wire_addr_response_encode(const wire_addr_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_ADDR_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->endpoint_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->reflexive_addr);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->reflexive_port);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_addr_response_decode(cbor_item_t* item, wire_addr_response_t* msg) {
  if (cbor_array_size(item) < 6) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (cbor_get_uint8(type_item) != WIRE_ADDR_RESPONSE) { cbor_decref(&type_item); return -1; }
  cbor_decref(&type_item);
  cbor_item_t* id_hi = cbor_array_get(item, 1);
  cbor_item_t* id_lo = cbor_array_get(item, 2);
  msg->message_id = ((uint64_t)cbor_get_uint64(id_hi) << 32) | (uint64_t)cbor_get_uint64(id_lo);
  cbor_decref(&id_hi);
  cbor_decref(&id_lo);
  cbor_item_t* endpoint = cbor_array_get(item, 3);
  msg->endpoint_id = cbor_get_uint32(endpoint);
  cbor_decref(&endpoint);
  cbor_item_t* addr = cbor_array_get(item, 4);
  msg->reflexive_addr = cbor_get_uint32(addr);
  cbor_decref(&addr);
  cbor_item_t* port = cbor_array_get(item, 5);
  msg->reflexive_port = (uint16_t)cbor_get_uint16(port);
  cbor_decref(&port);
  return 0;
}

// --- Destroy helpers for relay wire types ---

void wire_relay_send_destroy(wire_relay_send_t* msg) {
  if (msg == NULL) return;
  free(msg->payload);
  free(msg);
}

void wire_relay_received_destroy(wire_relay_received_t* msg) {
  if (msg == NULL) return;
  free(msg->payload);
  free(msg);
}