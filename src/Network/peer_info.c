//
// Created by victor on 5/27/26.
//

#include "peer_info.h"
#include "network.h"
#include "relay_client.h"
#include "quic_listener.h"
#include "../Util/allocator.h"
#include "../Util/base58.h"
#include "../Util/log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

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
          size_t raw_count = cbor_array_size(val);
          if (raw_count > PEER_INFO_MAX_ADDRESSES) {
            raw_count = PEER_INFO_MAX_ADDRESSES;
          }
          info->addresses = get_clear_memory(raw_count * sizeof(peer_address_t));
          if (info->addresses != NULL) {
            // Decode each address; on failure the slot stays zeroed and is
            // skipped via the write_index. Decrementing address_count inside
            // a forward-iterating loop (the old approach) skipped the next
            // element every time a decode failed, dropping a valid address.
            // See audit #33.
            size_t write_index = 0;
            for (size_t addr_index = 0; addr_index < raw_count; addr_index++) {
              cbor_item_t* addr_item = cbor_array_get(val, addr_index);
              if (_decode_address(addr_item, &info->addresses[write_index]) == 0) {
                write_index++;
              }
              cbor_decref(&addr_item);
            }
            info->address_count = write_index;
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

/* --- peer_info_from_node helpers (audit #18) --- */

/* Append a candidate address to info->addresses, growing the array via
   get_clear_memory. Returns 0 on success, -1 on failure. On success the
   host string is owned by the address slot. */
static int _peer_info_append_address(peer_info_t* info, peer_addr_type_e type,
                                     const char* host, uint16_t port,
                                     uint32_t relay_id) {
  if (info == NULL || host == NULL) return -1;
  if (info->address_count >= PEER_INFO_MAX_ADDRESSES) return -1;

  size_t new_count = info->address_count + 1;
  peer_address_t* grown = get_clear_memory(new_count * sizeof(peer_address_t));
  if (grown == NULL) return -1;
  if (info->addresses != NULL) {
    memcpy(grown, info->addresses, info->address_count * sizeof(peer_address_t));
    free(info->addresses);
  }
  info->addresses = grown;

  peer_address_t* slot = &info->addresses[info->address_count];
  slot->type = type;
  size_t host_len = strlen(host);
  slot->host = get_clear_memory(host_len + 1);
  if (slot->host == NULL) {
    /* Leave the slot zeroed; don't advance count so the caller sees no
       partial entry. The grown array still holds prior valid slots. */
    return -1;
  }
  memcpy(slot->host, host, host_len);
  slot->host[host_len] = '\0';
  slot->port = port;
  slot->relay_id = relay_id;
  info->address_count = new_count;
  return 0;
}

/* Return true if the IPv4 octets describe a private/link-local address worth
   advertising as a HOST candidate: RFC1918 (10.x, 172.16-31.x, 192.168.x) or
   link-local (169.254.x). Loopback (127.x) is excluded. */
static bool _peer_info_is_lan_ipv4(uint8_t octet0, uint8_t octet1) {
  if (octet0 == 10) return true;
  if (octet0 == 172 && (octet1 >= 16 && octet1 <= 31)) return true;
  if (octet0 == 192 && octet1 == 168) return true;
  if (octet0 == 169 && octet1 == 254) return true;
  return false;
}

/* Convert a host-byte-order IPv4 address to dotted-quad string. */
static void _peer_info_ipv4_to_string(uint32_t addr_host_order, char* out, size_t out_size) {
  uint8_t octet0 = (uint8_t)((addr_host_order >> 24) & 0xFF);
  uint8_t octet1 = (uint8_t)((addr_host_order >> 16) & 0xFF);
  uint8_t octet2 = (uint8_t)((addr_host_order >> 8) & 0xFF);
  uint8_t octet3 = (uint8_t)(addr_host_order & 0xFF);
  snprintf(out, out_size, "%u.%u.%u.%u", octet0, octet1, octet2, octet3);
}

#ifdef _WIN32
static int _peer_info_collect_host_addresses(peer_info_t* info, uint16_t port) {
  ULONG buffer_size = 15000;
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER;
  PIP_ADAPTER_ADDRESSES adapters = (PIP_ADAPTER_ADDRESSES)get_clear_memory(buffer_size);
  if (adapters == NULL) return -1;

  DWORD rc = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &buffer_size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    free(adapters);
    adapters = (PIP_ADAPTER_ADDRESSES)get_clear_memory(buffer_size);
    if (adapters == NULL) return -1;
    rc = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &buffer_size);
  }
  if (rc != NO_ERROR) {
    free(adapters);
    log_warn("peer_info_from_node: GetAdaptersAddresses failed: %lu", (unsigned long)rc);
    return -1;
  }

  int added = 0;
  for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULL; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) continue;
    for (PIP_ADAPTER_UNICAST_ADDRESS_LH addr = adapter->FirstUnicastAddress;
         addr != NULL; addr = addr->Next) {
      SOCKADDR* sock_addr = addr->Address.lpSockaddr;
      if (sock_addr == NULL || sock_addr->sa_family != AF_INET) continue;
      struct sockaddr_in* sin = (struct sockaddr_in*)sock_addr;
      uint32_t ip_host = ntohl(sin->sin_addr.s_addr);
      uint8_t octet0 = (uint8_t)((ip_host >> 24) & 0xFF);
      uint8_t octet1 = (uint8_t)((ip_host >> 16) & 0xFF);
      if (octet0 == 127) continue;  /* loopback */
      if (!_peer_info_is_lan_ipv4(octet0, octet1)) continue;
      char ip_str[INET_ADDRSTRLEN];
      _peer_info_ipv4_to_string(ip_host, ip_str, sizeof(ip_str));
      if (_peer_info_append_address(info, PEER_ADDR_HOST, ip_str, port, 0) == 0) {
        added++;
      }
    }
  }
  free(adapters);
  return added;
}
#else
static int _peer_info_collect_host_addresses(peer_info_t* info, uint16_t port) {
  struct ifaddrs* interfaces = NULL;
  if (getifaddrs(&interfaces) != 0) {
    log_warn("peer_info_from_node: getifaddrs failed");
    return -1;
  }

  int added = 0;
  for (struct ifaddrs* interface = interfaces; interface != NULL;
       interface = interface->ifa_next) {
    if (interface->ifa_addr == NULL) continue;
    if (interface->ifa_addr->sa_family != AF_INET) continue;
    if ((interface->ifa_flags & IFF_UP) == 0) continue;
    if ((interface->ifa_flags & IFF_LOOPBACK) != 0) continue;

    struct sockaddr_in* sin = (struct sockaddr_in*)interface->ifa_addr;
    uint32_t ip_host = ntohl(sin->sin_addr.s_addr);
    uint8_t octet0 = (uint8_t)((ip_host >> 24) & 0xFF);
    uint8_t octet1 = (uint8_t)((ip_host >> 16) & 0xFF);
    if (!_peer_info_is_lan_ipv4(octet0, octet1)) continue;

    char ip_str[INET_ADDRSTRLEN];
    _peer_info_ipv4_to_string(ip_host, ip_str, sizeof(ip_str));
    if (_peer_info_append_address(info, PEER_ADDR_HOST, ip_str, port, 0) == 0) {
      added++;
    }
  }
  freeifaddrs(interfaces);
  return added;
}
#endif

int peer_info_from_node(peer_info_t* info, const struct network_t* network,
                        bool include_lan) {
  if (info == NULL || network == NULL) return -1;

  /* HOST candidates (LAN addresses). Gated on include_lan for privacy —
     never broadcast internal IPs to arbitrary peers or in DHT gossip. */
  if (include_lan && network->quic_listener != NULL) {
    uint16_t listen_port = network->quic_listener->listen_port;
    if (listen_port > 0) {
      (void)_peer_info_collect_host_addresses(info, listen_port);
    }
  }

  /* SRFLX candidate: the relay-learned reflexive address. The relay emits
     the address in host byte order (see relay_server.c), so we convert via
     _peer_info_ipv4_to_string rather than inet_ntop. */
  if (network->relay != NULL && network->relay->reflexive_port != 0 &&
      network->relay->reflexive_addr != 0) {
    char ip_str[INET_ADDRSTRLEN];
    _peer_info_ipv4_to_string(network->relay->reflexive_addr,
                              ip_str, sizeof(ip_str));
    if (_peer_info_append_address(info, PEER_ADDR_SRFLX, ip_str,
                                  network->relay->reflexive_port, 0) != 0) {
      log_warn("peer_info_from_node: failed to append SRFLX candidate");
    }
  }

  /* RELAY candidate: the relay endpoint. The relay_id carries this node's
     local_endpoint_id so peers can route via the relay. */
  if (network->relay != NULL && network->relay->local_endpoint_id != 0 &&
      network->relay->relay_host != NULL && network->relay->relay_port != 0) {
    if (_peer_info_append_address(info, PEER_ADDR_RELAY,
                                  network->relay->relay_host,
                                  network->relay->relay_port,
                                  network->relay->local_endpoint_id) != 0) {
      log_warn("peer_info_from_node: failed to append RELAY candidate");
    }
  }

  return 0;
}
