//
// Created by victor on 5/14/25.
//

#include "ring.h"
#include "../Util/allocator.h"
#include <string.h>

ring_t* ring_create(uint32_t latency_min_us, uint32_t latency_max_us) {
  ring_t* ring = get_clear_memory(sizeof(ring_t));
  ring->latency_min_us = latency_min_us;
  ring->latency_max_us = latency_max_us;
  ring->frozen = false;
  vec_init(&ring->primary);
  vec_init(&ring->secondary);
  return ring;
}

void ring_destroy(ring_t* ring) {
  if (ring == NULL) return;
  vec_deinit(&ring->primary);
  vec_deinit(&ring->secondary);
  free(ring);
}

int ring_insert_primary(ring_t* ring, net_node_t* node, size_t max_primary) {
  if (ring == NULL || node == NULL) return -1;
  if (ring->frozen) return -1;
  if (ring->primary.length < (int)max_primary) {
    vec_push(&ring->primary, node);
    return 0;
  }
  return -1;
}

int ring_insert_secondary(ring_t* ring, net_node_t* node, size_t max_secondary) {
  if (ring == NULL || node == NULL) return -1;
  if (ring->frozen) return -1;
  if (ring->secondary.length < (int)max_secondary) {
    vec_push(&ring->secondary, node);
    return 0;
  }
  return -1;
}

net_node_t* ring_find_by_id(ring_t* ring, const node_id_t* id) {
  if (ring == NULL || id == NULL) return NULL;
  for (int index = 0; index < ring->primary.length; index++) {
    if (net_node_matches_id(ring->primary.data[index], id)) {
      return ring->primary.data[index];
    }
  }
  for (int index = 0; index < ring->secondary.length; index++) {
    if (net_node_matches_id(ring->secondary.data[index], id)) {
      return ring->secondary.data[index];
    }
  }
  return NULL;
}

bool ring_erase(ring_t* ring, const node_id_t* id) {
  if (ring == NULL || id == NULL) return false;
  for (int index = 0; index < ring->primary.length; index++) {
    if (net_node_matches_id(ring->primary.data[index], id)) {
      vec_splice(&ring->primary, index, 1);
      return true;
    }
  }
  for (int index = 0; index < ring->secondary.length; index++) {
    if (net_node_matches_id(ring->secondary.data[index], id)) {
      vec_splice(&ring->secondary, index, 1);
      return true;
    }
  }
  return false;
}