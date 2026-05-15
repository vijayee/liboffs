//
// Created by victor on 5/14/25.
//

#include "net_node.h"
#include "../Util/allocator.h"
#include <string.h>

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
  return node;
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