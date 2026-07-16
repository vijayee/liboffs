//
// Created by victor on 5/27/26.
//

#ifndef OFFS_PEER_INFO_H
#define OFFS_PEER_INFO_H

#include "node_id.h"
#include <cbor.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward declaration — network_t is defined in network.h. Avoids a circular
   include (network.h pulls in many headers; peer_info.h stays lightweight). */
struct network_t;

#define PEER_INFO_MAX_ADDRESSES 8

typedef enum {
  PEER_ADDR_DIRECT = 0,  // Direct QUIC connection (back-compat: treated as HOST)
  PEER_ADDR_RELAY   = 1,  // Relay-mediated connection
  PEER_ADDR_HOST    = 2,  // Private/LAN address (RFC1918 or link-local)
  PEER_ADDR_SRFLX   = 3,  // Server-reflexive address (learned from relay)
} peer_addr_type_e;

typedef struct {
  peer_addr_type_e type;
  char* host;
  uint16_t port;
  uint32_t relay_id;  // Endpoint ID on relay server (0 if type==DIRECT)
} peer_address_t;

typedef struct {
  node_id_t node_id;
  uint8_t* public_key;
  size_t public_key_len;
  peer_address_t* addresses;
  size_t address_count;
} peer_info_t;

// Encode peer_info to CBOR item. Caller must cbor_decref() the result.
cbor_item_t* peer_info_encode(const peer_info_t* info);

// Decode peer_info from CBOR item. Fills *info. Returns 0 on success, -1 on error.
// On success, caller must call peer_info_destroy().
int peer_info_decode(cbor_item_t* item, peer_info_t* info);

// Encode peer_info to CBOR, then Base58-encode the CBOR bytes.
// Returns a malloc'd string, or NULL on error. Caller must free().
char* peer_info_to_base58(const peer_info_t* info);

// Decode Base58 string to CBOR bytes, then decode as peer_info.
// Returns 0 on success (caller must peer_info_destroy()), -1 on error.
int peer_info_from_base58(const char* b58, peer_info_t* info);

void peer_address_destroy(peer_address_t* addr);
void peer_info_destroy(peer_info_t* info);

// Check if two peer_info_t refer to the same node (by node_id).
bool peer_info_equals(const peer_info_t* left, const peer_info_t* right);

/* Populate a peer_info_t with this node's candidate addresses:
   - HOST candidates: local LAN addresses (RFC1918/link-local) from interfaces
   - SRFLX candidate: the relay-learned reflexive address (if connected)
   - RELAY candidate: the relay endpoint (host + port + local_endpoint_id)
   Only includes HOST candidates if include_lan is true (privacy gating —
   friends only). The node_id and public_key fields are NOT touched; the
   caller sets them from authority. See audit #18.
   Returns 0 on success, -1 on error. */
int peer_info_from_node(peer_info_t* info, const struct network_t* network,
                        bool include_lan);

#endif // OFFS_PEER_INFO_H
