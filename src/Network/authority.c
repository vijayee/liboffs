//
// Created by victor on 5/14/25.
//

#include "authority.h"
#include "network.h"
#include "hebbian.h"
#include "ring_set.h"
#include "net_node.h"
#include "pem_key.h"
#include "respiration.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/base58.h"
#include <string.h>
#include <stdio.h>
#include <cbor.h>

// Peer store format version
#define PEER_STORE_VERSION 1

// --- Lifecycle ---

authority_t* authority_create(config_t* config) {
  if (config == NULL) return NULL;
  authority_t* authority = get_clear_memory(sizeof(authority_t));
  if (authority == NULL) return NULL;
  authority->config = config;
  authority->max_peers = 64;
  authority->max_inflight = 256;
  return authority;
}

void authority_destroy(authority_t* authority) {
  if (authority == NULL) return;
  if (authority->peer_store_path != NULL) {
    free(authority->peer_store_path);
  }
  if (authority->bootstrap_peers != NULL) {
    for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
      free(authority->bootstrap_peers[index]);
    }
    free(authority->bootstrap_peers);
  }
  if (authority->persisted_peers != NULL) {
    for (size_t index = 0; index < authority->persisted_peer_count; index++) {
      free(authority->persisted_peers[index]);
    }
    free(authority->persisted_peers);
  }
  if (authority->ca_cert_path != NULL) free(authority->ca_cert_path);
  if (authority->node_cert_path != NULL) free(authority->node_cert_path);
  if (authority->node_key_path != NULL) free(authority->node_key_path);
  if (authority->relay_url != NULL) free(authority->relay_url);
  if (authority->public_key != NULL) free(authority->public_key);
  free(authority);
}

// --- Local ID initialization ---

int authority_init_local_id(authority_t* authority) {
  if (authority == NULL) return -1;

  // If local_id is already set (e.g., from persistence), just regenerate .str
  if (!node_id_is_null(&authority->local_id)) {
    base58_encode(authority->local_id.hash, NODE_ID_HASH_SIZE,
                  authority->local_id.str, NODE_ID_STRING_SIZE);
    return 0;
  }

  // Derive node_id from certificate public key and cache the key
  if (authority->node_cert_path != NULL) {
    size_t key_len = 0;
    uint8_t* public_key = pem_extract_public_key(authority->node_cert_path, &key_len);
    if (public_key != NULL && key_len > 0) {
      int rc = node_id_from_public_key(public_key, key_len, &authority->local_id);
      if (rc == 0) {
        authority->public_key = public_key;
        authority->public_key_len = key_len;
        return 0;
      }
      free(public_key);
    }
    log_error("authority_init_local_id: failed to derive node_id from cert, generating random");
  }

  // No cert or extraction failed — generate a random node_id
  authority->public_key = NULL;
  authority->public_key_len = 0;
  node_id_generate(&authority->local_id);
  return 0;
}

// --- Config-only save/load ---

int authority_save(const authority_t* authority) {
  if (authority == NULL || authority->peer_store_path == NULL) {
    return -1;
  }
  // Config-only save is a no-op for now — config comes from offs_config_t
  // The actual persistence is in authority_save_peers which includes runtime state
  return 0;
}

int authority_load(authority_t* authority) {
  if (authority == NULL || authority->peer_store_path == NULL) {
    return -1;
  }
  // Config-only load is a no-op — runtime state is loaded by authority_load_peers
  return 0;
}

// --- Runtime peer state persistence ---

// CBOR structure:
// {
//   "v": uint64 (version),
//   "local_id": bytes[32],
//   "hebbian": [
//     [bytes[32] peer_id, float weight],
//     ...
//   ],
//   "peers": [
//     [bytes[32] id, uint32 addr, uint16 port, float latency_ms,
//      float weight, float capacity, uint8 phase, float availability],
//     ...
//   ]
// }

int authority_save_peers(const authority_t* authority, const network_t* network) {
  if (authority == NULL || network == NULL) return -1;
  if (authority->peer_store_path == NULL) return -1;

  // Count hebbian entries and ring nodes for array sizing
  size_t hebbian_count = network->hebbian.count;

  // Count total ring nodes
  size_t peer_count = ring_set_total_nodes(network->rings);

  // Build CBOR structure
  cbor_item_t* root = cbor_new_definite_map(4);

  // Version
  {
    cbor_item_t* key = cbor_build_string("v");
    cbor_item_t* val = cbor_build_uint8(PEER_STORE_VERSION);
    (void)cbor_map_add(root, (struct cbor_pair){ .key = key, .value = val });
    cbor_decref(&key);
    cbor_decref(&val);
  }

  // Local ID
  {
    cbor_item_t* key = cbor_build_string("local_id");
    cbor_item_t* val = cbor_build_bytestring(authority->local_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_map_add(root, (struct cbor_pair){ .key = key, .value = val });
    cbor_decref(&key);
    cbor_decref(&val);
  }

  // Hebbian weights
  cbor_item_t* hebbian_array = cbor_new_definite_array(hebbian_count);
  for (size_t index = 0; index < hebbian_count; index++) {
    const hebbian_weight_t* entry = &network->hebbian.entries[index];

    cbor_item_t* pair = cbor_new_definite_array(2);

    cbor_item_t* id_item = cbor_build_bytestring(entry->peer_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_array_push(pair, id_item);
    cbor_decref(&id_item);

    cbor_item_t* weight_val = cbor_new_float8();
    cbor_set_float8(weight_val, (double)entry->weight);
    (void)cbor_array_push(pair, weight_val);
    cbor_decref(&weight_val);

    (void)cbor_array_push(hebbian_array, pair);
    cbor_decref(&pair);
  }
  {
    cbor_item_t* key = cbor_build_string("hebbian");
    (void)cbor_map_add(root, (struct cbor_pair){ .key = key, .value = hebbian_array });
    cbor_decref(&key);
  }
  cbor_decref(&hebbian_array);

  // Peer data from rings
  cbor_item_t* peers_array = cbor_new_definite_array(peer_count);
  for (size_t ring_idx = 0; ring_idx < network->rings->ring_count; ring_idx++) {
    ring_t* ring = &network->rings->rings[ring_idx];

    for (int node_idx = 0; node_idx < ring->primary.length; node_idx++) {
      net_node_t* node = ring->primary.data[node_idx];
      if (node == NULL) continue;

      cbor_item_t* peer = cbor_new_definite_array(8);

      // id
      cbor_item_t* id_bytes = cbor_build_bytestring(node->id.hash, NODE_ID_HASH_SIZE);
      (void)cbor_array_push(peer, id_bytes);
      cbor_decref(&id_bytes);

      // addr
      cbor_item_t* addr_val = cbor_build_uint32(node->addr);
      (void)cbor_array_push(peer, addr_val);
      cbor_decref(&addr_val);

      // port
      cbor_item_t* port_val = cbor_build_uint16(node->port);
      (void)cbor_array_push(peer, port_val);
      cbor_decref(&port_val);

      // latency_ms
      cbor_item_t* lat = cbor_new_float4();
      cbor_set_float4(lat, node->latency_ms);
      (void)cbor_array_push(peer, lat);
      cbor_decref(&lat);

      // weight
      cbor_item_t* wt = cbor_new_float4();
      cbor_set_float4(wt, node->weight);
      (void)cbor_array_push(peer, wt);
      cbor_decref(&wt);

      // capacity
      cbor_item_t* cap = cbor_new_float4();
      cbor_set_float4(cap, node->capacity);
      (void)cbor_array_push(peer, cap);
      cbor_decref(&cap);

      // phase
      cbor_item_t* phase_val = cbor_build_uint8((uint8_t)node->phase);
      (void)cbor_array_push(peer, phase_val);
      cbor_decref(&phase_val);

      // availability
      cbor_item_t* avail = cbor_new_float4();
      cbor_set_float4(avail, node->availability);
      (void)cbor_array_push(peer, avail);
      cbor_decref(&avail);

      (void)cbor_array_push(peers_array, peer);
      cbor_decref(&peer);
    }
  }
  {
    cbor_item_t* key = cbor_build_string("peers");
    (void)cbor_map_add(root, (struct cbor_pair){ .key = key, .value = peers_array });
    cbor_decref(&key);
  }
  cbor_decref(&peers_array);

  // Serialize to file
  unsigned char* buffer = NULL;
  size_t buffer_size = 0;
  size_t length = cbor_serialize_alloc(root, &buffer, &buffer_size);
  cbor_decref(&root);

  if (length == 0 || buffer == NULL) {
    return -1;
  }

  FILE* file = fopen(authority->peer_store_path, "wb");
  if (file == NULL) {
    free(buffer);
    return -1;
  }

  size_t written = fwrite(buffer, 1, length, file);
  fclose(file);
  free(buffer);

  return (written == length) ? 0 : -1;
}

int authority_load_peers(authority_t* authority, network_t* network) {
  if (authority == NULL || network == NULL) return -1;
  if (authority->peer_store_path == NULL) return -1;

  FILE* file = fopen(authority->peer_store_path, "rb");
  if (file == NULL) return -1;

  // Get file size
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size <= 0) {
    fclose(file);
    return -1;
  }

  unsigned char* buffer = get_clear_memory((size_t)file_size);
  if (buffer == NULL) {
    fclose(file);
    return -1;
  }

  size_t read_bytes = fread(buffer, 1, (size_t)file_size, file);
  fclose(file);

  if (read_bytes == 0) {
    free(buffer);
    return -1;
  }

  // Parse CBOR
  struct cbor_load_result result;
  cbor_item_t* root = cbor_load(buffer, read_bytes, &result);
  free(buffer);

  if (result.error.code != CBOR_ERR_NONE || !cbor_isa_map(root)) {
    if (root != NULL) cbor_decref(&root);
    return -1;
  }

  // Extract version
  size_t map_size = cbor_map_size(root);
  uint8_t version = 0;
  for (size_t index = 0; index < map_size; index++) {
    struct cbor_pair pair = cbor_map_handle(root)[index];
    if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 1 &&
        cbor_string_handle(pair.key)[0] == 'v') {
      if (cbor_isa_uint(pair.value)) {
        version = (uint8_t)cbor_get_uint64(pair.value);
      }
    }
  }

  if (version != PEER_STORE_VERSION) {
    cbor_decref(&root);
    return -1;
  }

  // Extract local_id
  for (size_t index = 0; index < map_size; index++) {
    struct cbor_pair pair = cbor_map_handle(root)[index];
    if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 9 &&
        memcmp(cbor_string_handle(pair.key), "local_id", 9) == 0) {
      if (cbor_isa_bytestring(pair.value) && cbor_bytestring_length(pair.value) == NODE_ID_HASH_SIZE) {
        memcpy(authority->local_id.hash, cbor_bytestring_handle(pair.value), NODE_ID_HASH_SIZE);
        base58_encode(authority->local_id.hash, NODE_ID_HASH_SIZE,
                      authority->local_id.str, NODE_ID_STRING_SIZE);
      }
    }
  }

  // Extract hebbian weights
  for (size_t index = 0; index < map_size; index++) {
    struct cbor_pair pair = cbor_map_handle(root)[index];
    if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 7 &&
        memcmp(cbor_string_handle(pair.key), "hebbian", 7) == 0) {
      if (cbor_isa_array(pair.value)) {
        size_t hebbian_len = cbor_array_size(pair.value);
        for (size_t he_idx = 0; he_idx < hebbian_len; he_idx++) {
          cbor_item_t* entry = cbor_array_get(pair.value, he_idx);
          if (cbor_isa_array(entry) && cbor_array_size(entry) == 2) {
            cbor_item_t* peer_id_item = cbor_array_get(entry, 0);
            cbor_item_t* weight_item = cbor_array_get(entry, 1);

            if (cbor_isa_bytestring(peer_id_item) && cbor_bytestring_length(peer_id_item) == NODE_ID_HASH_SIZE &&
                cbor_is_float(weight_item)) {
              node_id_t peer_id;
              memset(&peer_id, 0, sizeof(peer_id));
              memcpy(peer_id.hash, cbor_bytestring_handle(peer_id_item), NODE_ID_HASH_SIZE);
              float weight = (float)cbor_float_get_float8(weight_item);
              hebbian_table_set(&network->hebbian, &peer_id, weight);
            }
            cbor_decref(&peer_id_item);
            cbor_decref(&weight_item);
          }
          cbor_decref(&entry);
        }
      }
    }
  }

  // Extract peer data and insert into rings
  for (size_t index = 0; index < map_size; index++) {
    struct cbor_pair pair = cbor_map_handle(root)[index];
    if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 5 &&
        memcmp(cbor_string_handle(pair.key), "peers", 5) == 0) {
      if (cbor_isa_array(pair.value)) {
        size_t peers_len = cbor_array_size(pair.value);
        for (size_t peer_idx = 0; peer_idx < peers_len; peer_idx++) {
          cbor_item_t* peer = cbor_array_get(pair.value, peer_idx);

          if (cbor_isa_array(peer) && cbor_array_size(peer) == 8) {
            cbor_item_t* id_item = cbor_array_get(peer, 0);
            cbor_item_t* addr_item = cbor_array_get(peer, 1);
            cbor_item_t* port_item = cbor_array_get(peer, 2);
            cbor_item_t* lat_item = cbor_array_get(peer, 3);
            cbor_item_t* wt_item = cbor_array_get(peer, 4);
            cbor_item_t* cap_item = cbor_array_get(peer, 5);
            cbor_item_t* phase_item = cbor_array_get(peer, 6);
            cbor_item_t* avail_item = cbor_array_get(peer, 7);

            if (cbor_isa_bytestring(id_item) && cbor_bytestring_length(id_item) == NODE_ID_HASH_SIZE) {
              node_id_t peer_id;
              memset(&peer_id, 0, sizeof(peer_id));
              memcpy(peer_id.hash, cbor_bytestring_handle(id_item), NODE_ID_HASH_SIZE);

              uint32_t addr = cbor_isa_uint(addr_item) ? (uint32_t)cbor_get_uint64(addr_item) : 0;
              uint16_t port = cbor_isa_uint(port_item) ? (uint16_t)cbor_get_uint64(port_item) : 0;

              net_node_t* node = net_node_create(&peer_id, addr, port);
              if (node != NULL) {
                if (cbor_is_float(lat_item)) {
                  node->latency_ms = (float)cbor_float_get_float4(lat_item);
                }
                if (cbor_is_float(wt_item)) {
                  node->weight = (float)cbor_float_get_float4(wt_item);
                }
                if (cbor_is_float(cap_item)) {
                  node->capacity = (float)cbor_float_get_float4(cap_item);
                }
                if (cbor_isa_uint(phase_item)) {
                  node->phase = (node_phase_e)cbor_get_uint64(phase_item);
                }
                if (cbor_is_float(avail_item)) {
                  node->availability = (float)cbor_float_get_float4(avail_item);
                }

                uint32_t latency_us = (uint32_t)(node->latency_ms * 1000.0f);
                ring_set_insert(network->rings, node, latency_us);
              }
            }
            cbor_decref(&id_item);
            cbor_decref(&addr_item);
            cbor_decref(&port_item);
            cbor_decref(&lat_item);
            cbor_decref(&wt_item);
            cbor_decref(&cap_item);
            cbor_decref(&phase_item);
            cbor_decref(&avail_item);
          }
          cbor_decref(&peer);
        }
      }
    }
  }

  cbor_decref(&root);

  // Sync hebbian weights into ring nodes
  for (size_t index = 0; index < network->hebbian.count; index++) {
    hebbian_weight_t* weight = &network->hebbian.entries[index];
    net_node_t* node = ring_set_find_by_id(network->rings, &weight->peer_id);
    if (node != NULL) {
      node->weight = weight->weight;
    }
  }

  return 0;
}

void authority_update_capacity(authority_t* authority, float capacity) {
  if (authority == NULL) return;
  ATOMIC_STORE(&authority->capacity, capacity);
}

void authority_update_phase(authority_t* authority, float capacity) {
  if (authority == NULL) return;
  node_phase_e phase;
  if (capacity >= RESPIRATION_EXHALE_THRESHOLD) {
    phase = NODE_PHASE_EXHALE;
  } else if (capacity < RESPIRATION_INHALE_THRESHOLD) {
    phase = NODE_PHASE_INHALE;
  } else {
    phase = NODE_PHASE_NEUTRAL;
  }
  ATOMIC_STORE(&authority->phase, phase);
}