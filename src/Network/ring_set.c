//
// Created by victor on 5/14/25.
//

#include "ring_set.h"
#include "../Util/allocator.h"
#include <math.h>
#include <string.h>

ring_set_t* ring_set_create(size_t primary_size, size_t secondary_size,
                            int32_t exponent_base) {
  ring_set_t* set = get_clear_memory(sizeof(ring_set_t));
  set->primary_ring_size = primary_size > 0 ? primary_size : RING_K;
  set->secondary_ring_size = secondary_size > 0 ? secondary_size : RING_M;
  set->exponent_base = exponent_base > 0 ? exponent_base : RING_ALPHA;

  // Compute ring count based on max expected latency
  // With base=2 and beta=5ms, ring i covers [5*2^(i-1), 5*2^i) us
  // Ring 0 covers [0, 5000) us
  // For 10 rings, max latency is ~5*2^10 = 5120ms
  set->ring_count = RING_MAX_RINGS;

  for (size_t index = 0; index < set->ring_count; index++) {
    uint32_t min_us, max_us;
    if (index == 0) {
      min_us = 0;
      max_us = RING_BETA_MS * 1000;
    } else {
      min_us = (uint32_t)(RING_BETA_MS * 1000.0 * pow((double)set->exponent_base, (double)(index - 1)));
      max_us = (uint32_t)(RING_BETA_MS * 1000.0 * pow((double)set->exponent_base, (double)index));
    }
    set->rings[index].latency_min_us = min_us;
    set->rings[index].latency_max_us = max_us;
    set->rings[index].frozen = false;
    vec_init(&set->rings[index].primary);
    vec_init(&set->rings[index].secondary);
  }

  return set;
}

void ring_set_destroy(ring_set_t* set) {
  if (set == NULL) return;
  for (size_t index = 0; index < set->ring_count; index++) {
    vec_deinit(&set->rings[index].primary);
    vec_deinit(&set->rings[index].secondary);
  }
  free(set);
}

int ring_set_get_ring_index(const ring_set_t* set, uint32_t latency_us) {
  if (set == NULL || set->exponent_base <= 0) return 0;
  int ring = (int)floor(log((double)latency_us) / log((double)set->exponent_base));
  if (ring < 0) ring = 0;
  if (ring >= (int)set->ring_count) ring = (int)set->ring_count - 1;
  return ring;
}

ring_t* ring_set_get_ring(ring_set_t* set, uint32_t latency_us) {
  if (set == NULL) return NULL;
  int index = ring_set_get_ring_index(set, latency_us);
  return &set->rings[index];
}

int ring_set_insert(ring_set_t* set, net_node_t* node, uint32_t latency_us) {
  if (set == NULL || node == NULL) return -1;
  int ring_index = ring_set_get_ring_index(set, latency_us);
  ring_t* ring = &set->rings[ring_index];

  // Try primary first
  if (ring_insert_primary(ring, node, set->primary_ring_size) == 0) {
    return 0;
  }
  // Overflow to secondary
  if (ring_insert_secondary(ring, node, set->secondary_ring_size) == 0) {
    return 0;
  }
  // Both full
  return -1;
}

net_node_t* ring_set_find_by_id(ring_set_t* set, const node_id_t* id) {
  if (set == NULL || id == NULL) return NULL;
  for (size_t index = 0; index < set->ring_count; index++) {
    net_node_t* found = ring_find_by_id(&set->rings[index], id);
    if (found != NULL) return found;
  }
  return NULL;
}

net_node_t* ring_set_find_closest(ring_set_t* set) {
  if (set == NULL) return NULL;
  // Return first primary node in lowest-latency non-empty ring
  for (size_t index = 0; index < set->ring_count; index++) {
    if (set->rings[index].primary.length > 0) {
      return set->rings[index].primary.data[0];
    }
  }
  return NULL;
}

bool ring_set_erase(ring_set_t* set, const node_id_t* id) {
  if (set == NULL || id == NULL) return false;
  for (size_t index = 0; index < set->ring_count; index++) {
    if (ring_erase(&set->rings[index], id)) {
      return true;
    }
  }
  return false;
}

bool ring_set_eligible_for_replacement(const ring_set_t* set, int ring_index) {
  if (set == NULL || ring_index < 0 || ring_index >= (int)set->ring_count) return false;
  const ring_t* ring = &set->rings[ring_index];
  if (ring->frozen) return false;
  if (ring->primary.length < (int)set->primary_ring_size) return false;
  if (ring->secondary.length == 0) return false;
  // Check that there are more than primary_ring_size non-rendezvous nodes
  int non_rendv_count = 0;
  for (int index = 0; index < ring->primary.length; index++) {
    if (!(ring->primary.data[index]->flags & NET_NODE_FLAG_RENDEZVOUS)) {
      non_rendv_count++;
    }
  }
  for (int index = 0; index < ring->secondary.length; index++) {
    if (!(ring->secondary.data[index]->flags & NET_NODE_FLAG_RENDEZVOUS)) {
      non_rendv_count++;
    }
  }
  return non_rendv_count > (int)set->primary_ring_size;
}

net_node_t* ring_set_promote_secondary(ring_set_t* set, int ring_index) {
  if (set == NULL || ring_index < 0 || ring_index >= (int)set->ring_count) return NULL;
  ring_t* ring = &set->rings[ring_index];
  if (ring->secondary.length == 0) return NULL;
  net_node_t* promoted = ring->secondary.data[0];
  vec_splice(&ring->secondary, 0, 1);
  vec_push(&ring->primary, promoted);
  return promoted;
}

size_t ring_set_total_nodes(const ring_set_t* set) {
  if (set == NULL) return 0;
  size_t total = 0;
  for (size_t index = 0; index < set->ring_count; index++) {
    total += (size_t)set->rings[index].primary.length;
    total += (size_t)set->rings[index].secondary.length;
  }
  return total;
}

void ring_set_clear_nodes(ring_set_t* set) {
  if (set == NULL) return;
  for (size_t index = 0; index < set->ring_count; index++) {
    for (int node_idx = 0; node_idx < set->rings[index].primary.length; node_idx++) {
      net_node_destroy(set->rings[index].primary.data[node_idx]);
    }
    for (int node_idx = 0; node_idx < set->rings[index].secondary.length; node_idx++) {
      net_node_destroy(set->rings[index].secondary.data[node_idx]);
    }
    set->rings[index].primary.length = 0;
    set->rings[index].secondary.length = 0;
  }
}