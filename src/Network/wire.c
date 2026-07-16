//
// Created by victor on 5/14/25.
//

#include "wire.h"
#include "find_block.h"
#include "../Util/allocator.h"
#include <string.h>

_Static_assert(WIRE_MAX_VISITED_BLOOM == FIND_BLOCK_MAX_VISITED_BLOOM,
               "wire and find_block bloom sizes must match");
_Static_assert(CLOSEST_NODES_MAX_VISITED == WIRE_MAX_VISITED_BLOOM,
               "closest_nodes and wire bloom sizes must match");

// --- Helper functions ---

// Type-guarded extractors. Each fetches array[index], checks the type,
// extracts the value, decrefs the item, and returns 0 on success / -1 on
// type mismatch. These prevent a type-confused nested item from causing a
// wild-pointer dereference in a typed getter (cbor_get_uint8 on a string,
// cbor_bytestring_handle on an int, etc.). The top-level array is already
// guarded (cbor_isa_array + cbor_array_size) but nested items from
// cbor_array_get are not — these helpers add the per-item check. See
// audit #13.

static int _array_get_uint8(cbor_item_t* array, size_t index, uint8_t* out) {
  cbor_item_t* item = cbor_array_get(array, index);
  int rc = -1;
  if (cbor_isa_uint(item)) { *out = cbor_get_uint8(item); rc = 0; }
  cbor_decref(&item);
  return rc;
}

static int _array_get_uint16(cbor_item_t* array, size_t index, uint16_t* out) {
  cbor_item_t* item = cbor_array_get(array, index);
  int rc = -1;
  if (cbor_isa_uint(item)) { *out = cbor_get_uint16(item); rc = 0; }
  cbor_decref(&item);
  return rc;
}

static int _array_get_uint32(cbor_item_t* array, size_t index, uint32_t* out) {
  cbor_item_t* item = cbor_array_get(array, index);
  int rc = -1;
  if (cbor_isa_uint(item)) { *out = cbor_get_uint32(item); rc = 0; }
  cbor_decref(&item);
  return rc;
}

static int _array_get_int(cbor_item_t* array, size_t index, uint64_t* out) {
  cbor_item_t* item = cbor_array_get(array, index);
  int rc = -1;
  if (cbor_isa_uint(item)) { *out = cbor_get_int(item); rc = 0; }
  cbor_decref(&item);
  return rc;
}

static int _array_get_float8(cbor_item_t* array, size_t index, double* out) {
  cbor_item_t* item = cbor_array_get(array, index);
  int rc = -1;
  if (cbor_isa_float_ctrl(item)) { *out = cbor_float_get_float(item); rc = 0; }
  cbor_decref(&item);
  return rc;
}

// Fetch array[index] as a bytestring. On success, *out_data points into the
// item's storage (valid until decref) and *out_len is the length. Returns
// 0 on success, -1 on type mismatch. Caller must cbor_decref(&item) — but
// since we return the item handle, the caller decrefs after memcpy.
static cbor_item_t* _array_get_bytestring(cbor_item_t* array, size_t index,
                                          const uint8_t** out_data,
                                          size_t* out_len) {
  cbor_item_t* item = cbor_array_get(array, index);
  if (!cbor_isa_bytestring(item)) {
    cbor_decref(&item);
    *out_data = NULL;
    *out_len = 0;
    return NULL;
  }
  *out_data = cbor_bytestring_handle(item);
  *out_len = cbor_bytestring_length(item);
  return item;  // caller must cbor_decref
}

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
  uint8_t type = 0;
  if (cbor_isa_uint(type_item)) {
    type = (uint8_t)cbor_get_uint8(type_item);
  }
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 5) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_PING) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_int(item, 4, &msg->timestamp) != 0) return -1;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 7) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_PING_RESPONSE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_int(item, 4, &msg->echo_time) != 0) return -1;
  double cap_val;
  if (_array_get_float8(item, 5, &cap_val) != 0) return -1;
  msg->capacity = (float)cap_val;
  uint8_t phase_byte;
  if (_array_get_uint8(item, 6, &phase_byte) != 0) return -1;
  msg->phase = (node_phase_e)phase_byte;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_PING_BLOCK) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  cbor_item_t* id_arr = cbor_array_get(item, 2);
  int id_rc = -1;
  if (cbor_isa_array(id_arr) && cbor_array_size(id_arr) >= 2) {
    uint64_t id_hi, id_lo;
    if (_array_get_int(id_arr, 0, &id_hi) == 0 &&
        _array_get_int(id_arr, 1, &id_lo) == 0) {
      msg->message_id = (id_hi << 32) | id_lo;
      id_rc = 0;
    }
  }
  cbor_decref(&id_arr);
  if (id_rc != 0) return id_rc;
  const uint8_t* hash_data; size_t hash_len;
  cbor_item_t* hash_item = _array_get_bytestring(item, 3, &hash_data, &hash_len);
  if (hash_item == NULL || hash_len != 32) {
    if (hash_item != NULL) cbor_decref(&hash_item);
    return -1;
  }
  memcpy(msg->block_hash, hash_data, 32);
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 7) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_PING_BLOCK_RESPONSE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_uint8(item, 4, &msg->exists) != 0) return -1;
  if (_array_get_uint32(item, 5, &msg->fib) != 0) return -1;
  if (_array_get_uint8(item, 6, &msg->healthy) != 0) return -1;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 5) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_FIND_NODE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 5) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_FIND_NODE_RESPONSE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* nodes = cbor_array_get(item, 4);
  size_t count = 0;
  if (cbor_isa_array(nodes)) {
    count = cbor_array_size(nodes);
    if (count > 8) count = 8;
  }
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
  cbor_item_t* array = cbor_new_definite_array(10);
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

  item = cbor_build_uint8(msg->path_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_rank_block_decode(cbor_item_t* item, wire_rank_block_t* msg) {
  if (!cbor_isa_array(item)) return -1;
  size_t array_size = cbor_array_size(item);

  // Minimum 6 elements for backward compatibility (old format without visited_bloom/path)
  if (array_size < 6) return -1;

  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_RANK_BLOCK) return -1;
  const uint8_t* hash_data; size_t hash_len;
  cbor_item_t* hash_item = _array_get_bytestring(item, 1, &hash_data, &hash_len);
  if (hash_item == NULL || hash_len != 32) {
    if (hash_item != NULL) cbor_decref(&hash_item);
    return -1;
  }
  memcpy(msg->block_hash, hash_data, 32);
  cbor_decref(&hash_item);
  if (_array_get_uint32(item, 2, &msg->fib) != 0) return -1;
  if (_array_get_uint32(item, 3, &msg->count) != 0) return -1;
  cbor_item_t* origin = cbor_array_get(item, 4);
  int rc = _node_id_decode(origin, &msg->origin);
  cbor_decref(&origin);
  if (rc != 0) return rc;
  if (_array_get_uint8(item, 5, &msg->hop_count) != 0) return -1;

  if (array_size >= 10) {
    // New format with visited_bloom, visited_count, path, path_len
    const uint8_t* visited_data; size_t visited_len;
    cbor_item_t* visited = _array_get_bytestring(item, 6, &visited_data, &visited_len);
    if (visited == NULL || visited_len != WIRE_MAX_VISITED_BLOOM) {
      if (visited != NULL) cbor_decref(&visited);
      return -1;
    }
    memcpy(msg->visited_bloom, visited_data, WIRE_MAX_VISITED_BLOOM);
    cbor_decref(&visited);

    uint16_t vcount;
    if (_array_get_uint16(item, 7, &vcount) != 0) return -1;
    msg->visited_count = vcount;

    cbor_item_t* path_arr = cbor_array_get(item, 8);
    size_t path_len = 0;
    if (cbor_isa_array(path_arr)) {
      path_len = cbor_array_size(path_arr);
      if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
    }
    msg->path_len = (uint8_t)path_len;
    for (size_t index = 0; index < path_len; index++) {
      cbor_item_t* node_item = cbor_array_get(path_arr, index);
      _node_id_decode(node_item, &msg->path[index]);
      cbor_decref(&node_item);
    }
    cbor_decref(&path_arr);

    uint8_t wire_path_len;
    if (_array_get_uint8(item, 9, &wire_path_len) != 0) return -1;
    if (wire_path_len < msg->path_len) msg->path_len = wire_path_len;
  } else {
    // Old format: zero-fill visited_bloom and path
    memset(msg->visited_bloom, 0, WIRE_MAX_VISITED_BLOOM);
    msg->visited_count = 0;
    memset(msg->path, 0, sizeof(msg->path));
    msg->path_len = 0;
  }

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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 5) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_RECALL_BLOCK) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  const uint8_t* hash_data; size_t hash_len;
  cbor_item_t* hash_item = _array_get_bytestring(item, 4, &hash_data, &hash_len);
  if (hash_item == NULL || hash_len != 32) {
    if (hash_item != NULL) cbor_decref(&hash_item);
    return -1;
  }
  memcpy(msg->block_hash, hash_data, 32);
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
  if (!cbor_isa_array(item)) return -1;
  size_t array_size = cbor_array_size(item);
  if (array_size < 5) return -1;
  msg->block_data = NULL;
  msg->block_data_len = 0;
  msg->block_fib = 0;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_RECALL_ACCEPT) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  const uint8_t* hash_data; size_t hash_len;
  cbor_item_t* hash_item = _array_get_bytestring(item, 4, &hash_data, &hash_len);
  if (hash_item == NULL || hash_len != 32) {
    if (hash_item != NULL) cbor_decref(&hash_item);
    return -1;
  }
  memcpy(msg->block_hash, hash_data, 32);
  cbor_decref(&hash_item);
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
    /* Discard the attacker-controlled declared length at index 6; the
       bytestring length is the single source of truth. See audit #1. */
    cbor_item_t* data_len = cbor_array_get(item, 6);
    cbor_decref(&data_len);
    if (_array_get_uint32(item, 7, &msg->block_fib) != 0) return -1;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 5) return -1;
  memset(msg, 0, sizeof(*msg));
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_RECALL_DECLINE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 7) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_RATE_LIMITED) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_uint8(item, 4, &msg->type) != 0) return -1;
  if (_array_get_uint32(item, 5, &msg->retry_after_ms) != 0) return -1;
  double limit_val;
  if (_array_get_float8(item, 6, &limit_val) != 0) return -1;
  msg->current_limit = (float)limit_val;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  msg->public_key = NULL;
  msg->public_key_len = 0;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_SALUTATION) return -1;
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
  uint64_t declared_len;
  if (_array_get_int(item, 3, &declared_len) != 0) return -1;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 6) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_PING_CAPACITY) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* source = cbor_array_get(item, 3);
  int rc = _node_id_decode(source, &msg->source);
  cbor_decref(&source);
  if (rc != 0) return rc;
  double cap_val;
  if (_array_get_float8(item, 4, &cap_val) != 0) return -1;
  msg->capacity = (float)cap_val;
  uint8_t phase_byte;
  if (_array_get_uint8(item, 5, &phase_byte) != 0) return -1;
  msg->phase = (node_phase_e)phase_byte;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 6) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_PING_CAPACITY_RESPONSE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  double cap_val;
  if (_array_get_float8(item, 4, &cap_val) != 0) return -1;
  msg->capacity = (float)cap_val;
  uint8_t phase_byte;
  if (_array_get_uint8(item, 5, &phase_byte) != 0) return -1;
  msg->phase = (node_phase_e)phase_byte;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 10) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_FIND_BLOCK) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  const uint8_t* hash_data; size_t hash_len;
  cbor_item_t* hash = _array_get_bytestring(item, 3, &hash_data, &hash_len);
  if (hash == NULL || hash_len != 32) {
    if (hash != NULL) cbor_decref(&hash);
    return -1;
  }
  memcpy(msg->block_hash, hash_data, 32);
  cbor_decref(&hash);
  if (_array_get_uint8(item, 4, &msg->ttl) != 0) return -1;
  const uint8_t* visited_data; size_t visited_len;
  cbor_item_t* visited = _array_get_bytestring(item, 5, &visited_data, &visited_len);
  if (visited == NULL || visited_len != WIRE_MAX_VISITED_BLOOM) {
    if (visited != NULL) cbor_decref(&visited);
    return -1;
  }
  memcpy(msg->visited_bloom, visited_data, WIRE_MAX_VISITED_BLOOM);
  cbor_decref(&visited);
  uint16_t vcount;
  if (_array_get_uint16(item, 6, &vcount) != 0) return -1;
  msg->visited_count = vcount;
  cbor_item_t* path_arr = cbor_array_get(item, 7);
  size_t path_len = 0;
  if (cbor_isa_array(path_arr)) {
    path_len = cbor_array_size(path_arr);
    if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  }
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  if (_array_get_int(item, 8, &msg->start_time) != 0) return -1;
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
  if (!cbor_isa_array(item)) return -1;
  size_t array_size = cbor_array_size(item);
  if (array_size < 9) return -1;
  msg->block_data = NULL;
  msg->block_data_len = 0;
  msg->block_fib = 0;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_FIND_BLOCK_RESPONSE) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  const uint8_t* hash_data; size_t hash_len;
  cbor_item_t* hash = _array_get_bytestring(item, 3, &hash_data, &hash_len);
  if (hash == NULL || hash_len != 32) {
    if (hash != NULL) cbor_decref(&hash);
    return -1;
  }
  memcpy(msg->block_hash, hash_data, 32);
  cbor_decref(&hash);
  if (_array_get_uint8(item, 4, &msg->found) != 0) return -1;
  cbor_item_t* holder = cbor_array_get(item, 5);
  int rc = _node_id_decode(holder, &msg->holder);
  cbor_decref(&holder);
  if (rc != 0) return rc;
  if (_array_get_uint32(item, 6, &msg->fib) != 0) return -1;
  cbor_item_t* path_arr = cbor_array_get(item, 7);
  size_t path_len = 0;
  if (cbor_isa_array(path_arr)) {
    path_len = cbor_array_size(path_arr);
    if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  }
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  uint64_t latency_val;
  if (_array_get_int(item, 8, &latency_val) != 0) return -1;
  msg->latency_ms = latency_val;
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
    /* The wire format also carries an explicit block_data_len at array index 10
       for symmetry with the encoder, but it is attacker-controlled and must NOT
       override the bytestring length we already used to size the buffer (a
       mismatch caused a heap over-read; see docs/liboffs-audit-report.md #1).
       Consume the item to release the refcount, then discard the value. */
    cbor_item_t* data_len = cbor_array_get(item, 10);
    cbor_decref(&data_len);
    if (_array_get_uint32(item, 11, &msg->block_fib) != 0) return -1;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 14) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_STORE_BLOCK) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  const uint8_t* hash_data; size_t hash_len;
  cbor_item_t* hash = _array_get_bytestring(item, 3, &hash_data, &hash_len);
  if (hash == NULL || hash_len != 32) {
    if (hash != NULL) cbor_decref(&hash);
    return -1;
  }
  memcpy(msg->block_hash, hash_data, 32);
  cbor_decref(&hash);
  if (_array_get_uint32(item, 4, &msg->block_size) != 0) return -1;
  if (_array_get_uint32(item, 5, &msg->block_fib) != 0) return -1;
  if (_array_get_uint8(item, 6, &msg->replicas_needed) != 0) return -1;
  if (_array_get_uint8(item, 7, &msg->max_hops) != 0) return -1;
  const uint8_t* visited_data; size_t visited_len;
  cbor_item_t* visited = _array_get_bytestring(item, 8, &visited_data, &visited_len);
  if (visited == NULL || visited_len != WIRE_MAX_VISITED_BLOOM) {
    if (visited != NULL) cbor_decref(&visited);
    return -1;
  }
  memcpy(msg->visited_bloom, visited_data, WIRE_MAX_VISITED_BLOOM);
  cbor_decref(&visited);
  uint16_t vcount;
  if (_array_get_uint16(item, 9, &vcount) != 0) return -1;
  msg->visited_count = vcount;
  cbor_item_t* path_arr = cbor_array_get(item, 10);
  size_t path_len = 0;
  if (cbor_isa_array(path_arr)) {
    path_len = cbor_array_size(path_arr);
    if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  }
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  if (_array_get_int(item, 11, &msg->start_time) != 0) return -1;
  if (_array_get_uint8(item, 12, &msg->carry_data) != 0) return -1;
  cbor_item_t* data = cbor_array_get(item, 13);
  if (cbor_isa_bytestring(data) && cbor_bytestring_length(data) > 0) {
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 9) return -1;
  memset(msg, 0, sizeof(*msg));
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_STORE_BLOCK_RESPONSE) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_uint8(item, 3, &msg->accepted) != 0) return -1;
  cbor_item_t* holder = cbor_array_get(item, 4);
  int rc = _node_id_decode(holder, &msg->holder);
  cbor_decref(&holder);
  if (rc != 0) return rc;
  if (_array_get_uint8(item, 5, &msg->replicas_remaining) != 0) return -1;
  cbor_item_t* path_arr = cbor_array_get(item, 6);
  size_t path_len = 0;
  if (cbor_isa_array(path_arr)) {
    path_len = cbor_array_size(path_arr);
    if (path_len > WIRE_MAX_PATH) path_len = WIRE_MAX_PATH;
  }
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  uint64_t latency_val;
  if (_array_get_int(item, 7, &latency_val) != 0) return -1;
  msg->latency_ms = latency_val;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 6) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_SEEKING_BLOCKS) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  double cap_val;
  if (_array_get_float8(item, 4, &cap_val) != 0) return -1;
  msg->capacity = (float)cap_val;
  cbor_item_t* excludes = cbor_array_get(item, 5);
  size_t count = 0;
  if (cbor_isa_array(excludes)) {
    count = cbor_array_size(excludes);
    if (count > WIRE_MAX_OFFERS) count = WIRE_MAX_OFFERS;
  }
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 5) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_SEEKING_BLOCKS_RESPONSE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* offers_arr = cbor_array_get(item, 4);
  size_t count = 0;
  if (cbor_isa_array(offers_arr)) {
    count = cbor_array_size(offers_arr);
    if (count > WIRE_MAX_OFFERS) count = WIRE_MAX_OFFERS;
  }
  msg->offer_count = 0;
  for (size_t index = 0; index < count && msg->offer_count < WIRE_MAX_OFFERS; index++) {
    cbor_item_t* offer = cbor_array_get(offers_arr, index);
    if (!cbor_isa_array(offer) || cbor_array_size(offer) < 3) {
      cbor_decref(&offer);
      continue;
    }
    cbor_item_t* hash = cbor_array_get(offer, 0);
    if (cbor_isa_bytestring(hash) && cbor_bytestring_length(hash) == 32) {
      memcpy(msg->offers[msg->offer_count].hash, cbor_bytestring_handle(hash), 32);
    }
    cbor_decref(&hash);
    uint32_t fib_val;
    if (_array_get_uint32(offer, 1, &fib_val) == 0) {
      msg->offers[msg->offer_count].fib = fib_val;
    }
    uint32_t size_val;
    if (_array_get_uint32(offer, 2, &size_val) == 0) {
      msg->offers[msg->offer_count].size = size_val;
    }
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  msg->payload = NULL;
  msg->payload_len = 0;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_RELAY_SEND) return -1;
  if (_array_get_uint32(item, 1, &msg->src_endpoint_id) != 0) return -1;
  if (_array_get_uint32(item, 2, &msg->dest_endpoint_id) != 0) return -1;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  msg->payload = NULL;
  msg->payload_len = 0;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_RELAY_RECEIVED) return -1;
  if (_array_get_uint32(item, 1, &msg->src_endpoint_id) != 0) return -1;
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

// --- RelayChallenge ---

cbor_item_t* wire_relay_challenge_encode(const wire_relay_challenge_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* entry;

  entry = cbor_build_uint8(WIRE_RELAY_CHALLENGE);
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  entry = _node_id_encode(&msg->challenger_id);
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  entry = cbor_build_uint32(msg->challenger_endpoint_id);
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  entry = cbor_build_bytestring(msg->nonce, 32);
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  return array;
}

int wire_relay_challenge_decode(cbor_item_t* item, wire_relay_challenge_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != WIRE_RELAY_CHALLENGE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* challenger = cbor_array_get(item, 1);
  int challenger_rc = _node_id_decode(challenger, &msg->challenger_id);
  cbor_decref(&challenger);
  if (challenger_rc != 0) return challenger_rc;

  cbor_item_t* endpoint = cbor_array_get(item, 2);
  if (!cbor_isa_uint(endpoint)) {
    cbor_decref(&endpoint);
    return -1;
  }
  msg->challenger_endpoint_id = cbor_get_uint32(endpoint);
  cbor_decref(&endpoint);

  cbor_item_t* nonce = cbor_array_get(item, 3);
  int nonce_rc = -1;
  if (cbor_isa_bytestring(nonce) && cbor_bytestring_length(nonce) == 32) {
    memcpy(msg->nonce, cbor_bytestring_handle(nonce), 32);
    nonce_rc = 0;
  }
  cbor_decref(&nonce);
  return nonce_rc;
}

void wire_relay_challenge_destroy(wire_relay_challenge_t* msg) {
  if (msg == NULL) return;
  // challenge has no allocations; provide a hook for symmetry with the response destroy
  free(msg);
}

// --- RelayChallengeResponse ---

cbor_item_t* wire_relay_challenge_response_encode(const wire_relay_challenge_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(5);
  cbor_item_t* entry;

  entry = cbor_build_uint8(WIRE_RELAY_CHALLENGE_RESPONSE);
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  entry = _node_id_encode(&msg->responder_id);
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  entry = cbor_build_bytestring(msg->nonce, 32);
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  if (msg->public_key != NULL && msg->public_key_len > 0) {
    entry = cbor_build_bytestring(msg->public_key, msg->public_key_len);
  } else {
    entry = cbor_build_bytestring((const unsigned char*)"", 0);
  }
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  if (msg->signature != NULL && msg->signature_len > 0) {
    entry = cbor_build_bytestring(msg->signature, msg->signature_len);
  } else {
    entry = cbor_build_bytestring((const unsigned char*)"", 0);
  }
  (void)cbor_array_push(array, entry);
  cbor_decref(&entry);

  return array;
}

int wire_relay_challenge_response_decode(cbor_item_t* item, wire_relay_challenge_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 5) return -1;
  msg->public_key = NULL;
  msg->public_key_len = 0;
  msg->signature = NULL;
  msg->signature_len = 0;

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != WIRE_RELAY_CHALLENGE_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* responder = cbor_array_get(item, 1);
  int responder_rc = _node_id_decode(responder, &msg->responder_id);
  cbor_decref(&responder);
  if (responder_rc != 0) return responder_rc;

  cbor_item_t* nonce = cbor_array_get(item, 2);
  int nonce_rc = -1;
  if (cbor_isa_bytestring(nonce) && cbor_bytestring_length(nonce) == 32) {
    memcpy(msg->nonce, cbor_bytestring_handle(nonce), 32);
    nonce_rc = 0;
  }
  cbor_decref(&nonce);
  if (nonce_rc != 0) return nonce_rc;

  cbor_item_t* key_data = cbor_array_get(item, 3);
  if (!cbor_isa_bytestring(key_data)) {
    cbor_decref(&key_data);
    return -1;
  }
  if (cbor_bytestring_length(key_data) > 0) {
    msg->public_key_len = cbor_bytestring_length(key_data);
    msg->public_key = get_clear_memory(msg->public_key_len);
    if (msg->public_key != NULL) {
      memcpy(msg->public_key, cbor_bytestring_handle(key_data), msg->public_key_len);
    } else {
      msg->public_key_len = 0;
      cbor_decref(&key_data);
      return -1;
    }
  }
  cbor_decref(&key_data);

  cbor_item_t* sig_data = cbor_array_get(item, 4);
  if (!cbor_isa_bytestring(sig_data)) {
    cbor_decref(&sig_data);
    return -1;
  }
  if (cbor_bytestring_length(sig_data) > 0) {
    msg->signature_len = cbor_bytestring_length(sig_data);
    msg->signature = get_clear_memory(msg->signature_len);
    if (msg->signature != NULL) {
      memcpy(msg->signature, cbor_bytestring_handle(sig_data), msg->signature_len);
    } else {
      msg->signature_len = 0;
      cbor_decref(&sig_data);
      return -1;
    }
  }
  cbor_decref(&sig_data);

  return 0;
}

void wire_relay_challenge_response_destroy(wire_relay_challenge_response_t* msg) {
  if (msg == NULL) return;
  free(msg->public_key);
  free(msg->signature);
  free(msg);
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_ADDR_REQUEST) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
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
  if (!cbor_isa_array(item) || cbor_array_size(item) < 6) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_ADDR_RESPONSE) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_uint32(item, 3, &msg->endpoint_id) != 0) return -1;
  if (_array_get_uint32(item, 4, &msg->reflexive_addr) != 0) return -1;
  uint16_t port_val;
  if (_array_get_uint16(item, 5, &port_val) != 0) return -1;
  msg->reflexive_port = port_val;
  return 0;
}

// --- Gossip ---

cbor_item_t* wire_gossip_encode(const wire_gossip_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(8);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_GOSSIP);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* sender = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, sender);
  cbor_decref(&sender);

  item = cbor_build_uint32(msg->rendezvous_addr);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->rendezvous_port);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->target_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* targets = cbor_new_definite_array(msg->target_count);
  for (uint8_t index = 0; index < msg->target_count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->targets[index]);
    (void)cbor_array_push(targets, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, targets);
  cbor_decref(&targets);

  return array;
}

int wire_gossip_decode(cbor_item_t* item, wire_gossip_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 8) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_GOSSIP) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* sender = cbor_array_get(item, 3);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  if (_array_get_uint32(item, 4, &msg->rendezvous_addr) != 0) return -1;
  uint16_t port_val;
  if (_array_get_uint16(item, 5, &port_val) != 0) return -1;
  msg->rendezvous_port = port_val;
  if (_array_get_uint8(item, 6, &msg->target_count) != 0) return -1;
  if (msg->target_count > RING_MAX_RINGS) msg->target_count = RING_MAX_RINGS;
  cbor_item_t* targets_arr = cbor_array_get(item, 7);
  size_t arr_len = 0;
  if (cbor_isa_array(targets_arr)) {
    arr_len = cbor_array_size(targets_arr);
  }
  size_t decode_count = arr_len < msg->target_count ? arr_len : msg->target_count;
  for (size_t index = 0; index < decode_count; index++) {
    cbor_item_t* node_item = cbor_array_get(targets_arr, index);
    _node_id_decode(node_item, &msg->targets[index]);
    cbor_decref(&node_item);
  }
  msg->target_count = (uint8_t)decode_count;
  cbor_decref(&targets_arr);
  return 0;
}

// --- GossipPull ---

cbor_item_t* wire_gossip_pull_encode(const wire_gossip_pull_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(8);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_GOSSIP_PULL);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* sender = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, sender);
  cbor_decref(&sender);

  item = cbor_build_uint32(msg->rendezvous_addr);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->rendezvous_port);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->target_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* targets = cbor_new_definite_array(msg->target_count);
  for (uint8_t index = 0; index < msg->target_count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->targets[index]);
    (void)cbor_array_push(targets, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, targets);
  cbor_decref(&targets);

  return array;
}

int wire_gossip_pull_decode(cbor_item_t* item, wire_gossip_pull_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 8) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_GOSSIP_PULL) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* sender = cbor_array_get(item, 3);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  if (_array_get_uint32(item, 4, &msg->rendezvous_addr) != 0) return -1;
  uint16_t port_val;
  if (_array_get_uint16(item, 5, &port_val) != 0) return -1;
  msg->rendezvous_port = port_val;
  if (_array_get_uint8(item, 6, &msg->target_count) != 0) return -1;
  if (msg->target_count > RING_MAX_RINGS) msg->target_count = RING_MAX_RINGS;
  cbor_item_t* targets_arr = cbor_array_get(item, 7);
  size_t arr_len = 0;
  if (cbor_isa_array(targets_arr)) {
    arr_len = cbor_array_size(targets_arr);
  }
  size_t decode_count = arr_len < msg->target_count ? arr_len : msg->target_count;
  for (size_t index = 0; index < decode_count; index++) {
    cbor_item_t* node_item = cbor_array_get(targets_arr, index);
    _node_id_decode(node_item, &msg->targets[index]);
    cbor_decref(&node_item);
  }
  msg->target_count = (uint8_t)decode_count;
  cbor_decref(&targets_arr);
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

// --- ClosestNodes (Meridian proximity routing) ---

cbor_item_t* wire_closest_nodes_encode(const wire_closest_nodes_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(16);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_CLOSEST_NODES);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id >> 32));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id & 0xFFFFFFFF));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->target_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->beta_numerator);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->beta_denominator);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->ttl);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->visited_bloom, CLOSEST_NODES_MAX_VISITED);
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

  item = cbor_build_uint8(msg->path_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->start_time >> 32));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->start_time & 0xFFFFFFFF));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->original_source);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_closest_nodes_decode(cbor_item_t* item, wire_closest_nodes_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 16) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_CLOSEST_NODES) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* target = cbor_array_get(item, 4);
  int target_rc = _node_id_decode(target, &msg->target_id);
  cbor_decref(&target);
  if (target_rc != 0) return target_rc;
  if (_array_get_uint8(item, 5, &msg->count) != 0) return -1;
  uint16_t beta_num;
  if (_array_get_uint16(item, 6, &beta_num) != 0) return -1;
  msg->beta_numerator = beta_num;
  uint16_t beta_den;
  if (_array_get_uint16(item, 7, &beta_den) != 0) return -1;
  msg->beta_denominator = beta_den;
  if (_array_get_uint8(item, 8, &msg->ttl) != 0) return -1;
  const uint8_t* visited_data; size_t visited_len;
  cbor_item_t* visited = _array_get_bytestring(item, 9, &visited_data, &visited_len);
  if (visited == NULL || visited_len != CLOSEST_NODES_MAX_VISITED) {
    if (visited != NULL) cbor_decref(&visited);
    return -1;
  }
  memcpy(msg->visited_bloom, visited_data, CLOSEST_NODES_MAX_VISITED);
  cbor_decref(&visited);
  uint16_t vcount;
  if (_array_get_uint16(item, 10, &vcount) != 0) return -1;
  msg->visited_count = vcount;
  cbor_item_t* path_arr = cbor_array_get(item, 11);
  size_t path_len = 0;
  if (cbor_isa_array(path_arr)) {
    path_len = cbor_array_size(path_arr);
    if (path_len > CLOSEST_NODES_MAX_PATH) path_len = CLOSEST_NODES_MAX_PATH;
  }
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  uint8_t wire_path_len;
  if (_array_get_uint8(item, 12, &wire_path_len) != 0) return -1;
  if (wire_path_len < msg->path_len) msg->path_len = wire_path_len;
  uint64_t start_hi, start_lo;
  if (_array_get_int(item, 13, &start_hi) != 0) return -1;
  if (_array_get_int(item, 14, &start_lo) != 0) return -1;
  msg->start_time = (start_hi << 32) | start_lo;
  cbor_item_t* source = cbor_array_get(item, 15);
  int source_rc = _node_id_decode(source, &msg->original_source);
  cbor_decref(&source);
  return source_rc;
}

// --- ClosestNodesResponse ---

cbor_item_t* wire_closest_nodes_response_encode(const wire_closest_nodes_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(15);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_CLOSEST_NODES_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id >> 32));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id & 0xFFFFFFFF));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->target_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->found);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->closest);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32(msg->closest_latency_us);
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

  item = cbor_build_uint8(msg->path_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->latency_us >> 32));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->latency_us & 0xFFFFFFFF));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* ring_nodes = cbor_new_definite_array(msg->ring_count);
  for (uint8_t index = 0; index < msg->ring_count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->ring_nodes[index]);
    (void)cbor_array_push(ring_nodes, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, ring_nodes);
  cbor_decref(&ring_nodes);

  cbor_item_t* ring_latencies = cbor_new_definite_array(msg->ring_count);
  for (uint8_t index = 0; index < msg->ring_count; index++) {
    cbor_item_t* latency = cbor_build_uint32(msg->ring_latencies_us[index]);
    (void)cbor_array_push(ring_latencies, latency);
    cbor_decref(&latency);
  }
  (void)cbor_array_push(array, ring_latencies);
  cbor_decref(&ring_latencies);

  item = cbor_build_uint8(msg->ring_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_closest_nodes_response_decode(cbor_item_t* item, wire_closest_nodes_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 15) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_CLOSEST_NODES_RESPONSE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* target = cbor_array_get(item, 4);
  int target_rc = _node_id_decode(target, &msg->target_id);
  cbor_decref(&target);
  if (target_rc != 0) return target_rc;
  if (_array_get_uint8(item, 5, &msg->found) != 0) return -1;
  cbor_item_t* closest = cbor_array_get(item, 6);
  int closest_rc = _node_id_decode(closest, &msg->closest);
  cbor_decref(&closest);
  if (closest_rc != 0) return closest_rc;
  if (_array_get_uint32(item, 7, &msg->closest_latency_us) != 0) return -1;
  cbor_item_t* path_arr = cbor_array_get(item, 8);
  size_t path_len = 0;
  if (cbor_isa_array(path_arr)) {
    path_len = cbor_array_size(path_arr);
    if (path_len > CLOSEST_NODES_MAX_PATH) path_len = CLOSEST_NODES_MAX_PATH;
  }
  msg->path_len = (uint8_t)path_len;
  for (size_t index = 0; index < path_len; index++) {
    cbor_item_t* node_item = cbor_array_get(path_arr, index);
    _node_id_decode(node_item, &msg->path[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&path_arr);
  uint8_t wire_path_len;
  if (_array_get_uint8(item, 9, &wire_path_len) != 0) return -1;
  if (wire_path_len < msg->path_len) msg->path_len = wire_path_len;
  uint64_t lat_hi, lat_lo;
  if (_array_get_int(item, 10, &lat_hi) != 0) return -1;
  if (_array_get_int(item, 11, &lat_lo) != 0) return -1;
  msg->latency_us = (lat_hi << 32) | lat_lo;
  cbor_item_t* ring_nodes_arr = cbor_array_get(item, 12);
  size_t ring_count = 0;
  if (cbor_isa_array(ring_nodes_arr)) {
    ring_count = cbor_array_size(ring_nodes_arr);
    if (ring_count > CLOSEST_NODES_MAX_RING_SAMPLES) ring_count = CLOSEST_NODES_MAX_RING_SAMPLES;
  }
  msg->ring_count = (uint8_t)ring_count;
  for (size_t index = 0; index < ring_count; index++) {
    cbor_item_t* node_item = cbor_array_get(ring_nodes_arr, index);
    _node_id_decode(node_item, &msg->ring_nodes[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&ring_nodes_arr);
  cbor_item_t* ring_latencies_arr = cbor_array_get(item, 13);
  size_t lat_count = 0;
  if (cbor_isa_array(ring_latencies_arr)) {
    lat_count = cbor_array_size(ring_latencies_arr);
    if (lat_count > CLOSEST_NODES_MAX_RING_SAMPLES) lat_count = CLOSEST_NODES_MAX_RING_SAMPLES;
  }
  for (size_t index = 0; index < lat_count; index++) {
    cbor_item_t* lat_item = cbor_array_get(ring_latencies_arr, index);
    if (cbor_isa_uint(lat_item)) {
      msg->ring_latencies_us[index] = cbor_get_uint32(lat_item);
    }
    cbor_decref(&lat_item);
  }
  cbor_decref(&ring_latencies_arr);
  uint8_t wire_ring_count;
  if (_array_get_uint8(item, 14, &wire_ring_count) != 0) return -1;
  if (wire_ring_count < msg->ring_count) msg->ring_count = wire_ring_count;
  return 0;
}

// --- MeasureNodes ---

cbor_item_t* wire_measure_nodes_encode(const wire_measure_nodes_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_MEASURE_NODES);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id >> 32));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id & 0xFFFFFFFF));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->probe_type);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* targets = cbor_new_definite_array(msg->target_count);
  for (uint8_t index = 0; index < msg->target_count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->targets[index]);
    (void)cbor_array_push(targets, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, targets);
  cbor_decref(&targets);

  return array;
}

int wire_measure_nodes_decode(cbor_item_t* item, wire_measure_nodes_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 6) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_MEASURE_NODES) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_uint8(item, 4, &msg->probe_type) != 0) return -1;
  cbor_item_t* targets_arr = cbor_array_get(item, 5);
  size_t target_count = 0;
  if (cbor_isa_array(targets_arr)) {
    target_count = cbor_array_size(targets_arr);
    if (target_count > MEASURE_NODES_MAX_TARGETS) target_count = MEASURE_NODES_MAX_TARGETS;
  }
  msg->target_count = (uint8_t)target_count;
  for (size_t index = 0; index < target_count; index++) {
    cbor_item_t* node_item = cbor_array_get(targets_arr, index);
    _node_id_decode(node_item, &msg->targets[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&targets_arr);
  return 0;
}

// --- MeasureNodesResponse ---

cbor_item_t* wire_measure_nodes_response_encode(const wire_measure_nodes_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(7);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_MEASURE_NODES_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id >> 32));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id & 0xFFFFFFFF));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->target_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* targets = cbor_new_definite_array(msg->target_count);
  for (uint8_t index = 0; index < msg->target_count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->targets[index]);
    (void)cbor_array_push(targets, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, targets);
  cbor_decref(&targets);

  cbor_item_t* latencies = cbor_new_definite_array(msg->target_count);
  for (uint8_t index = 0; index < msg->target_count; index++) {
    cbor_item_t* latency = cbor_build_uint32(msg->latencies_us[index]);
    (void)cbor_array_push(latencies, latency);
    cbor_decref(&latency);
  }
  (void)cbor_array_push(array, latencies);
  cbor_decref(&latencies);

  return array;
}

int wire_measure_nodes_response_decode(cbor_item_t* item, wire_measure_nodes_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 7) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_MEASURE_NODES_RESPONSE) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  if (_array_get_uint8(item, 4, &msg->target_count) != 0) return -1;
  if (msg->target_count > MEASURE_NODES_MAX_TARGETS) msg->target_count = MEASURE_NODES_MAX_TARGETS;
  cbor_item_t* targets_arr = cbor_array_get(item, 5);
  size_t targets_count = 0;
  if (cbor_isa_array(targets_arr)) {
    targets_count = cbor_array_size(targets_arr);
    if (targets_count > MEASURE_NODES_MAX_TARGETS) targets_count = MEASURE_NODES_MAX_TARGETS;
  }
  if (targets_count < msg->target_count) msg->target_count = (uint8_t)targets_count;
  for (size_t index = 0; index < msg->target_count; index++) {
    cbor_item_t* node_item = cbor_array_get(targets_arr, index);
    _node_id_decode(node_item, &msg->targets[index]);
    cbor_decref(&node_item);
  }
  cbor_decref(&targets_arr);
  cbor_item_t* latencies_arr = cbor_array_get(item, 6);
  size_t lat_count = 0;
  if (cbor_isa_array(latencies_arr)) {
    lat_count = cbor_array_size(latencies_arr);
    if (lat_count > MEASURE_NODES_MAX_TARGETS) lat_count = MEASURE_NODES_MAX_TARGETS;
  }
  for (size_t index = 0; index < lat_count; index++) {
    cbor_item_t* lat_item = cbor_array_get(latencies_arr, index);
    if (cbor_isa_uint(lat_item)) {
      msg->latencies_us[index] = cbor_get_uint32(lat_item);
    }
    cbor_decref(&lat_item);
  }
  cbor_decref(&latencies_arr);
  return 0;
}

// --- ClosestNodesProgress ---

cbor_item_t* wire_closest_nodes_progress_encode(const wire_closest_nodes_progress_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_CLOSEST_NODES_PROGRESS);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id >> 32));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint32((uint32_t)(msg->message_id & 0xFFFFFFFF));
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _node_id_encode(&msg->target_id);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->hop_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_closest_nodes_progress_decode(cbor_item_t* item, wire_closest_nodes_progress_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 6) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_CLOSEST_NODES_PROGRESS) return -1;
  cbor_item_t* sender = cbor_array_get(item, 1);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 2, &id_hi) != 0) return -1;
  if (_array_get_int(item, 3, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* target = cbor_array_get(item, 4);
  int target_rc = _node_id_decode(target, &msg->target_id);
  cbor_decref(&target);
  if (target_rc != 0) return target_rc;
  if (_array_get_uint8(item, 5, &msg->hop_count) != 0) return -1;
  return 0;
}