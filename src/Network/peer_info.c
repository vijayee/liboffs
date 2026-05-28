//
// Created by victor on 5/27/26.
//

#include "peer_info.h"
#include "../Util/allocator.h"
#include "../Util/base58.h"
#include <string.h>
#include <stdlib.h>

// CBOR keys for peer_info map
#define PI_KEY_NODE_ID    1
#define PI_KEY_PUBLIC_KEY 2
#define PI_KEY_ADDRESSES  3
#define PA_KEY_TYPE       1
#define PA_KEY_HOST       2
#define PA_KEY_PORT       3
#define PA_KEY_RELAY_ID   4

void peer_address_destroy(peer_address_t* addr) {
  if (addr == NULL) return;
  if (addr->host != NULL) {
    free(addr->host);
    addr->host = NULL;
  }
}

void peer_info_destroy(peer_info_t* info) {
  if (info == NULL) return;
  if (info->public_key != NULL) {
    free(info->public_key);
    info->public_key = NULL;
  }
  info->public_key_len = 0;
  if (info->addresses != NULL) {
    for (size_t index = 0; index < info->address_count; index++) {
      peer_address_destroy(&info->addresses[index]);
    }
    free(info->addresses);
    info->addresses = NULL;
  }
  info->address_count = 0;
}

cbor_item_t* peer_info_encode(const peer_info_t* info) {
  if (info == NULL) return NULL;
  if (info->public_key == NULL) return NULL;

  cbor_item_t* map = cbor_new_definite_map(3);

  // node_id (bstr)
  cbor_item_t* key = cbor_build_uint8(PI_KEY_NODE_ID);
  cbor_item_t* val = cbor_build_bytestring(info->node_id.hash, NODE_ID_HASH_SIZE);
  (void)cbor_map_add(map, (struct cbor_pair){.key = key, .value = val});
  cbor_decref(&key);
  cbor_decref(&val);

  // public_key (bstr)
  key = cbor_build_uint8(PI_KEY_PUBLIC_KEY);
  val = cbor_build_bytestring(info->public_key, info->public_key_len);
  (void)cbor_map_add(map, (struct cbor_pair){.key = key, .value = val});
  cbor_decref(&key);
  cbor_decref(&val);

  // addresses (array of maps)
  key = cbor_build_uint8(PI_KEY_ADDRESSES);
  cbor_item_t* addr_array = cbor_new_definite_array(info->address_count);
  for (size_t index = 0; index < info->address_count; index++) {
    peer_address_t* addr = &info->addresses[index];
    cbor_item_t* addr_map = cbor_new_definite_map(4);

    cbor_item_t* ak = cbor_build_uint8(PA_KEY_TYPE);
    cbor_item_t* av = cbor_build_uint8((uint8_t)addr->type);
    (void)cbor_map_add(addr_map, (struct cbor_pair){.key = ak, .value = av});
    cbor_decref(&ak);
    cbor_decref(&av);

    ak = cbor_build_uint8(PA_KEY_HOST);
    av = cbor_build_string(addr->host);
    (void)cbor_map_add(addr_map, (struct cbor_pair){.key = ak, .value = av});
    cbor_decref(&ak);
    cbor_decref(&av);

    ak = cbor_build_uint8(PA_KEY_PORT);
    av = cbor_build_uint16(addr->port);
    (void)cbor_map_add(addr_map, (struct cbor_pair){.key = ak, .value = av});
    cbor_decref(&ak);
    cbor_decref(&av);

    ak = cbor_build_uint8(PA_KEY_RELAY_ID);
    av = cbor_build_uint32(addr->relay_id);
    (void)cbor_map_add(addr_map, (struct cbor_pair){.key = ak, .value = av});
    cbor_decref(&ak);
    cbor_decref(&av);

    (void)cbor_array_push(addr_array, addr_map);
    cbor_decref(&addr_map);
  }
  (void)cbor_map_add(map, (struct cbor_pair){.key = key, .value = addr_array});
  cbor_decref(&key);
  cbor_decref(&addr_array);

  return map;
}

static int _decode_address(cbor_item_t* item, peer_address_t* addr) {
  if (!cbor_isa_map(item) || cbor_map_size(item) < 2) return -1;

  for (size_t index = 0; index < cbor_map_size(item); index++) {
    cbor_item_t* key = cbor_map_handle(item)[index].key;
    cbor_item_t* val = cbor_map_handle(item)[index].value;

    if (!cbor_isa_uint(key)) continue;
    uint8_t key_val = cbor_get_uint8(key);

    switch (key_val) {
      case PA_KEY_TYPE:
        if (cbor_isa_uint(val)) addr->type = (peer_addr_type_e)cbor_get_uint8(val);
        break;
      case PA_KEY_HOST:
        if (cbor_isa_string(val)) {
          addr->host = get_clear_memory(cbor_string_length(val) + 1);
          if (addr->host != NULL) {
            memcpy(addr->host, cbor_string_handle(val), cbor_string_length(val));
          }
        }
        break;
      case PA_KEY_PORT:
        if (cbor_isa_uint(val)) addr->port = (uint16_t)cbor_get_uint16(val);
        break;
      case PA_KEY_RELAY_ID:
        if (cbor_isa_uint(val)) addr->relay_id = cbor_get_uint32(val);
        break;
    }
  }
  return (addr->host != NULL) ? 0 : -1;
}

int peer_info_decode(cbor_item_t* item, peer_info_t* info) {
  if (item == NULL || info == NULL) return -1;
  if (!cbor_isa_map(item)) return -1;

  memset(info, 0, sizeof(peer_info_t));

  for (size_t index = 0; index < cbor_map_size(item); index++) {
    cbor_item_t* key = cbor_map_handle(item)[index].key;
    cbor_item_t* val = cbor_map_handle(item)[index].value;

    if (!cbor_isa_uint(key)) continue;
    uint8_t key_val = cbor_get_uint8(key);

    switch (key_val) {
      case PI_KEY_NODE_ID:
        if (cbor_isa_bytestring(val) && cbor_bytestring_length(val) == NODE_ID_HASH_SIZE) {
          memcpy(info->node_id.hash, cbor_bytestring_handle(val), NODE_ID_HASH_SIZE);
          if (base58_encode(info->node_id.hash, NODE_ID_HASH_SIZE,
                           info->node_id.str, NODE_ID_STRING_SIZE) < 0) {
            memset(info->node_id.str, 0, NODE_ID_STRING_SIZE);
          }
        }
        break;
      case PI_KEY_PUBLIC_KEY:
        if (cbor_isa_bytestring(val)) {
          info->public_key_len = cbor_bytestring_length(val);
          info->public_key = get_clear_memory(info->public_key_len);
          if (info->public_key != NULL) {
            memcpy(info->public_key, cbor_bytestring_handle(val), info->public_key_len);
          }
        }
        break;
      case PI_KEY_ADDRESSES:
        if (cbor_isa_array(val)) {
          info->address_count = cbor_array_size(val);
          if (info->address_count > PEER_INFO_MAX_ADDRESSES) {
            info->address_count = PEER_INFO_MAX_ADDRESSES;
          }
          info->addresses = get_clear_memory(info->address_count * sizeof(peer_address_t));
          if (info->addresses != NULL) {
            for (size_t addr_index = 0; addr_index < info->address_count; addr_index++) {
              cbor_item_t* addr_item = cbor_array_get(val, addr_index);
              if (_decode_address(addr_item, &info->addresses[addr_index]) != 0) {
                info->address_count--;
              }
            }
          }
        }
        break;
    }
  }

  if (info->public_key == NULL) {
    if (info->addresses != NULL) {
      for (size_t i = 0; i < info->address_count; i++) {
        peer_address_destroy(&info->addresses[i]);
      }
      free(info->addresses);
      info->addresses = NULL;
    }
    info->address_count = 0;
    return -1;
  }
  return 0;
}

char* peer_info_to_base58(const peer_info_t* info) {
  if (info == NULL) return NULL;

  cbor_item_t* cbor = peer_info_encode(info);
  if (cbor == NULL) return NULL;

  size_t cbor_len = cbor_serialized_size(cbor);
  uint8_t* cbor_bytes = get_clear_memory(cbor_len);
  if (cbor_bytes == NULL) {
    cbor_decref(&cbor);
    return NULL;
  }
  size_t serialized = cbor_serialize(cbor, cbor_bytes, cbor_len);
  if (serialized == 0) {
    cbor_decref(&cbor);
    free(cbor_bytes);
    return NULL;
  }
  cbor_decref(&cbor);

  size_t b58_len = base58_encoded_length(cbor_len);
  char* b58 = get_clear_memory(b58_len + 1);
  if (b58 == NULL) {
    free(cbor_bytes);
    return NULL;
  }
  int rc = base58_encode(cbor_bytes, cbor_len, b58, b58_len + 1);
  free(cbor_bytes);
  if (rc < 0) {
    free(b58);
    return NULL;
  }
  return b58;
}

int peer_info_from_base58(const char* b58, peer_info_t* info) {
  if (b58 == NULL || info == NULL) return -1;

  size_t decoded_len = base58_decoded_length(strlen(b58));
  uint8_t* cbor_bytes = get_clear_memory(decoded_len);
  if (cbor_bytes == NULL) return -1;

  size_t bytes_written = 0;
  if (base58_decode(b58, cbor_bytes, decoded_len, &bytes_written) != 0) {
    free(cbor_bytes);
    return -1;
  }

  struct cbor_load_result load_result;
  cbor_item_t* item = cbor_load(cbor_bytes, bytes_written, &load_result);
  free(cbor_bytes);

  if (item == NULL || load_result.error.code != CBOR_ERR_NONE) {
    if (item != NULL) cbor_decref(&item);
    return -1;
  }

  int rc = peer_info_decode(item, info);
  cbor_decref(&item);
  return rc;
}

bool peer_info_equals(const peer_info_t* left, const peer_info_t* right) {
  if (left == NULL || right == NULL) return false;
  return node_id_equals(&left->node_id, &right->node_id);
}
