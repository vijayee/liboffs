//
// Created by victor on 5/14/25.
//

#include "net_node.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdio.h>

#define LATENCY_EWMA_ALPHA 0.1f
#define AVAILABILITY_EWMA_ALPHA 0.1f

net_node_t* net_node_create(const node_id_t* id, uint32_t addr, uint16_t port) {
  net_node_t* node = get_clear_memory(sizeof(net_node_t));
  if (id != NULL) {
    memcpy(&node->id, id, sizeof(node_id_t));
  } else {
    node_id_clear(&node->id);
  }
  node->addr = addr;
  node->port = port;
  node->latency_ms = 0.0f;
  node->weight = 0.0f;
  node->capacity = 0.0f;
  node->phase = NODE_PHASE_NEUTRAL;
  node->availability = 0.5f;
  node->consecutive_fails = 0;
  node->last_gossip_time = 0;
  node->relay_verified = false;
  node->nat_type = NAT_TYPE_UNKNOWN;
  node->last_seen_ms = 0;
  node->bad_blocks_received = 0;
  return node;
}

static const uint8_t _v4_mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

void net_node_set_platform_addr(net_node_t* node, const platform_address_t* addr) {
  if (node == NULL || addr == NULL) return;
  switch (addr->family) {
    case PLATFORM_AF_INET:
      /* Legacy u32 addr convention is host byte order (see network.c ring
         insertion); platform_address_t stores network byte order, so swap. */
      node->addr_family = PLATFORM_AF_INET;
      node->addr = ((uint32_t)(addr->inet.addr & 0xFF) << 24) |
                   ((addr->inet.addr & 0xFF00u) << 8) |
                   ((addr->inet.addr >> 8) & 0xFF00u) |
                   ((addr->inet.addr >> 24) & 0xFFu);
      memset(node->addr6, 0, sizeof(node->addr6));
      break;
    case PLATFORM_AF_INET6:
      if (memcmp(addr->inet6.addr, _v4_mapped_prefix, 12) == 0) {
        /* v4-mapped collapses to the u32 fast-path. */
        platform_address_t v4;
        memset(&v4, 0, sizeof(v4));
        v4.family = PLATFORM_AF_INET;
        v4.inet.addr = ((uint32_t)addr->inet6.addr[12] << 24) |
                       ((uint32_t)addr->inet6.addr[13] << 16) |
                       ((uint32_t)addr->inet6.addr[14] << 8) |
                       (uint32_t)addr->inet6.addr[15];
        net_node_set_platform_addr(node, &v4);
      } else {
        node->addr_family = PLATFORM_AF_INET6;
        node->addr = 0;
        memcpy(node->addr6, addr->inet6.addr, 16);
      }
      break;
    default:
      break; /* unsupported family — leave the node addressless */
  }
}

void net_node_get_platform_addr(const net_node_t* node, platform_address_t* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  if (node == NULL) return;
  if (node->addr_family == PLATFORM_AF_INET6) {
    out->family = PLATFORM_AF_INET6;
    memcpy(out->inet6.addr, node->addr6, 16);
    return;
  }
  out->family = PLATFORM_AF_INET;
  out->inet.addr = ((node->addr & 0xFFu) << 24) |
                   ((node->addr & 0xFF00u) << 8) |
                   ((node->addr >> 8) & 0xFF00u) |
                   ((node->addr >> 24) & 0xFFu);
}

void net_node_set_rendv_platform(net_node_t* node, const platform_address_t* addr) {
  if (node == NULL || addr == NULL) return;
  if (addr->family == PLATFORM_AF_INET6 &&
      memcmp(addr->inet6.addr, _v4_mapped_prefix, 12) != 0) {
    node->rendv_family = PLATFORM_AF_INET6;
    memcpy(node->rendv6, addr->inet6.addr, 16);
  } else {
    node->rendv_family = PLATFORM_AF_INET;
    /* Rendezvous u32 follows the same host-byte-order convention as addr. */
    if (addr->family == PLATFORM_AF_INET) {
      node->rendv_addr = ((uint32_t)(addr->inet.addr & 0xFF) << 24) |
                         ((addr->inet.addr & 0xFF00u) << 8) |
                         ((addr->inet.addr >> 8) & 0xFF00u) |
                         ((addr->inet.addr >> 24) & 0xFFu);
    }
  }
}

int net_node_addr_string(const net_node_t* node, bool bracket, char* buf, size_t len) {
  if (node == NULL || buf == NULL || len == 0) return -1;
  if (node->addr == 0 && node->addr_family != PLATFORM_AF_INET6) {
    return -1; /* addressless */
  }
  platform_address_t addr;
  net_node_get_platform_addr(node, &addr);
  char ip[64];
  if (platform_address_to_string(&addr, ip, sizeof(ip)) != 0) return -1;
  if (node->addr_family == PLATFORM_AF_INET6 && bracket) {
    int written = snprintf(buf, len, "[%s]", ip);
    return (written > 0 && (size_t)written < len) ? 0 : -1;
  }
  int written = snprintf(buf, len, "%s", ip);
  return (written > 0 && (size_t)written < len) ? 0 : -1;
}

net_node_t* net_node_create_rendv(const node_id_t* id, uint32_t addr, uint16_t port,
                                  uint32_t rendv_addr, uint16_t rendv_port) {
  net_node_t* node = net_node_create(id, addr, port);
  node->rendv_addr = rendv_addr;
  node->rendv_port = rendv_port;
  node->flags = NET_NODE_FLAG_RENDEZVOUS;
  return node;
}

net_node_t* net_node_create_unidentified(uint32_t addr, uint16_t port) {
  return net_node_create(NULL, addr, port);
}

void net_node_destroy(net_node_t* node) {
  if (node == NULL) return;
  free(node);
}

bool net_node_equals_by_id(const net_node_t* left, const net_node_t* right) {
  if (left == NULL || right == NULL) return false;
  if (node_id_is_null(&left->id) || node_id_is_null(&right->id)) return false;
  return node_id_equals(&left->id, &right->id);
}

bool net_node_id_equals(const node_id_t* left, const node_id_t* right) {
  if (left == NULL || right == NULL) return false;
  if (node_id_is_null(left) || node_id_is_null(right)) return false;
  return node_id_equals(left, right);
}

bool net_node_matches_id(const net_node_t* node, const node_id_t* id) {
  if (node == NULL || id == NULL) return false;
  if (node_id_is_null(&node->id) || node_id_is_null(id)) return false;
  return node_id_equals(&node->id, id);
}

bool net_node_equals_by_addr(const net_node_t* left, const net_node_t* right) {
  if (left == NULL || right == NULL) return false;
  return left->addr == right->addr && left->port == right->port;
}

int net_node_latency_cmp(const void* left, const void* right) {
  const net_node_t* left_node = *(const net_node_t* const*)left;
  const net_node_t* right_node = *(const net_node_t* const*)right;
  if (left_node->latency_ms < right_node->latency_ms) return -1;
  if (left_node->latency_ms > right_node->latency_ms) return 1;
  return 0;
}

void net_node_update_latency(net_node_t* node, float latency_ms) {
  if (node == NULL) return;
  if (node->latency_ms == 0.0f) {
    node->latency_ms = latency_ms;
  } else {
    node->latency_ms = LATENCY_EWMA_ALPHA * latency_ms +
                        (1.0f - LATENCY_EWMA_ALPHA) * node->latency_ms;
  }
}

void net_node_record_success(net_node_t* node) {
  if (node == NULL) return;
  node->consecutive_fails = 0;
  node->availability = AVAILABILITY_EWMA_ALPHA * 1.0f +
                       (1.0f - AVAILABILITY_EWMA_ALPHA) * node->availability;
}

void net_node_record_fail(net_node_t* node) {
  if (node == NULL) return;
  node->consecutive_fails++;
  node->availability = AVAILABILITY_EWMA_ALPHA * 0.0f +
                       (1.0f - AVAILABILITY_EWMA_ALPHA) * node->availability;
}