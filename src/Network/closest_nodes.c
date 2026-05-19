//
// Created by victor on 5/18/25.
//

#include "closest_nodes.h"
#include "find_block.h"
#include "eabf.h"
#include "net_node.h"
#include "connection_manager.h"
#include "peer_connection.h"
#include "latency_cache.h"
#include <xxh3.h>
#include <string.h>
#include <stdlib.h>

// --- Visited bloom filter helpers ---
// Same XXH3 double-hashing pattern as find_block.c with 3 hash functions.
// Operates directly on the byte array (256 bytes = 2048 bits).

#define VISITED_BLOOM_HASH_COUNT 3
#define VISITED_BLOOM_BITS (CLOSEST_NODES_MAX_VISITED * 8)
#define VISITED_SEED_A 0x4F464653ULL  // "OFFS"
#define VISITED_SEED_B 0x46534E4F55444E53ULL  // "FSOUNDS"

static void visited_bloom_set(uint8_t* bloom, const uint8_t* data, size_t len) {
  uint64_t hash_a = XXH3_64bits_withSeed(data, len, VISITED_SEED_A);
  uint64_t hash_b = XXH3_64bits_withSeed(data, len, VISITED_SEED_B);

  for (uint32_t index = 0; index < VISITED_BLOOM_HASH_COUNT; index++) {
    uint64_t combined = hash_a + (uint64_t)index * hash_b;
    size_t bit_index = (size_t)(combined % VISITED_BLOOM_BITS);
    size_t byte_index = bit_index / 8;
    uint8_t bit_offset = (uint8_t)(bit_index % 8);
    bloom[byte_index] |= (1u << bit_offset);
  }
}

static bool visited_bloom_check(const uint8_t* bloom, const uint8_t* data, size_t len) {
  uint64_t hash_a = XXH3_64bits_withSeed(data, len, VISITED_SEED_A);
  uint64_t hash_b = XXH3_64bits_withSeed(data, len, VISITED_SEED_B);

  for (uint32_t index = 0; index < VISITED_BLOOM_HASH_COUNT; index++) {
    uint64_t combined = hash_a + (uint64_t)index * hash_b;
    size_t bit_index = (size_t)(combined % VISITED_BLOOM_BITS);
    size_t byte_index = bit_index / 8;
    uint8_t bit_offset = (uint8_t)(bit_index % 8);
    if (!(bloom[byte_index] & (1u << bit_offset))) {
      return false;
    }
  }
  return true;
}

void closest_nodes_add_visited(uint8_t* visited_bloom, uint16_t* visited_count,
                               const uint8_t* hash) {
  if (visited_bloom == NULL || visited_count == NULL || hash == NULL) return;
  visited_bloom_set(visited_bloom, hash, NODE_ID_HASH_SIZE);
  (*visited_count)++;
}

bool closest_nodes_is_visited(const uint8_t* visited_bloom, uint16_t visited_count,
                              const uint8_t* hash) {
  if (visited_bloom == NULL || hash == NULL || visited_count == 0) return false;
  return visited_bloom_check(visited_bloom, hash, NODE_ID_HASH_SIZE);
}

// --- Beta convergence check ---

bool closest_nodes_beta_converged(uint32_t current_latency_us,
                                  uint32_t best_latency_us,
                                  uint16_t beta_numerator,
                                  uint16_t beta_denominator) {
  // If best latency is 0, we are at the target — converged
  if (best_latency_us == 0) return true;
  // Safety: denominator must be non-zero
  if (beta_denominator == 0) return false;
  // Convergence: current * denominator <= best * numerator
  // Use 64-bit arithmetic to avoid overflow
  uint64_t left = (uint64_t)current_latency_us * (uint64_t)beta_denominator;
  uint64_t right = (uint64_t)best_latency_us * (uint64_t)beta_numerator;
  return left <= right;
}

// --- Ring sample selection ---

size_t closest_nodes_select_ring_samples(
    ring_set_t* rings,
    const node_id_t* exclude_id,
    node_id_t* ring_nodes,
    uint32_t* ring_latencies_us,
    size_t max_samples) {
  if (rings == NULL || ring_nodes == NULL || ring_latencies_us == NULL || max_samples == 0) {
    return 0;
  }

  size_t collected = 0;

  for (size_t ring_index = 0; ring_index < rings->ring_count && collected < max_samples; ring_index++) {
    ring_t* ring = &rings->rings[ring_index];
    net_node_t* best_node = NULL;
    float best_latency = -1.0f;

    // Pick the primary member with the lowest latency, skipping exclude_id
    for (int node_index = 0; node_index < ring->primary.length; node_index++) {
      net_node_t* node = ring->primary.data[node_index];
      if (node == NULL) continue;
      if (exclude_id != NULL && node_id_equals(&node->id, exclude_id)) continue;
      if (best_node == NULL || node->latency_ms < best_latency) {
        best_node = node;
        best_latency = node->latency_ms;
      }
    }

    if (best_node != NULL) {
      ring_nodes[collected] = best_node->id;
      ring_latencies_us[collected] = (uint32_t)(best_node->latency_ms * 1000.0f);
      collected++;
    }
  }

  return collected;
}

// --- Closest-N execute ---

closest_nodes_result_e closest_nodes_execute(
    eabf_table_t* eabf_table,
    eabf_ttl_table_t* eabf_ttl,
    connection_manager_t* conn_mgr,
    ring_set_t* rings,
    latency_cache_t* latency_cache,
    const node_id_t* local_id,
    const wire_closest_nodes_t* query,
    net_node_t** next_hops,
    size_t* next_hop_count) {
  if (query == NULL || local_id == NULL || next_hops == NULL || next_hop_count == NULL) {
    return CLOSEST_NODES_NOT_FOUND;
  }

  // eabf_ttl is used by the caller to set timer-based TTL on EABF subscriptions
  // after closest_nodes_execute returns CLOSEST_NODES_TTL_EXPIRED
  (void)eabf_ttl;

  *next_hop_count = 0;

  // Step 1: Self-check — if we are the target, report FOUND with latency 0
  if (node_id_equals(local_id, &query->target_id)) {
    return CLOSEST_NODES_FOUND;
  }

  // Step 2: Latency cache + beta convergence check
  if (latency_cache != NULL) {
    float cached_latency_ms = 0.0f;
    int found = latency_cache_get(latency_cache, &query->target_id, &cached_latency_ms);
    if (found == 0 && cached_latency_ms > 0.0f) {
      uint32_t latency_us = (uint32_t)(cached_latency_ms * 1000.0f);
      if (closest_nodes_beta_converged(latency_us, latency_us,
                                       query->beta_numerator, query->beta_denominator)) {
        return CLOSEST_NODES_FOUND;
      }
    }
  }

  // Step 3: TTL check
  if (query->ttl == 0) {
    return CLOSEST_NODES_TTL_EXPIRED;
  }

  // Step 4: Connection manager gravity wells (connected peers)
  if (conn_mgr != NULL) {
    size_t match_count = 0;
    peer_connection_t** matches = connection_manager_get_peers_for_topic(
        conn_mgr, query->target_id.hash, NODE_ID_HASH_SIZE, &match_count);
    if (matches != NULL && match_count > 0) {
      for (size_t match_index = 0; match_index < match_count; match_index++) {
        peer_connection_t* peer = matches[match_index];

        // Skip nodes in the path
        bool in_path = false;
        for (uint8_t path_index = 0; path_index < query->path_len; path_index++) {
          if (node_id_equals(&query->path[path_index], &peer->remote_node_id)) {
            in_path = true;
            break;
          }
        }
        if (in_path) continue;

        // Skip nodes in visited bloom
        if (closest_nodes_is_visited(query->visited_bloom, query->visited_count,
                                     peer->remote_node_id.hash)) {
          continue;
        }

        net_node_t* peer_node = ring_set_find_by_id(rings, &peer->remote_node_id);
        if (peer_node != NULL && !(peer_node->flags & NET_NODE_FLAG_RENDEZVOUS)) {
          next_hops[0] = peer_node;
          *next_hop_count = 1;
          free(matches);
          return CLOSEST_NODES_FORWARDING;
        }
      }
      free(matches);
    }
  }

  // Step 5: EABF gravity wells — directed walk (ring table peers)
  net_node_t* gravity_candidate = NULL;
  float gravity_best_weight = CLOSEST_NODES_MIN_WEIGHT;
  uint32_t gravity_best_level = UINT32_MAX;

  if (eabf_table != NULL) {
    for (size_t index = 0; index < eabf_table->count; index++) {
      eabf_entry_t* entry = &eabf_table->entries[index];
      eabf_t* eabf = entry->eabf;
      if (eabf == NULL) continue;

      uint32_t hops = 0;
      bool eabf_found = eabf_check(eabf, query->target_id.hash, NODE_ID_HASH_SIZE, &hops);
      if (!eabf_found) continue;

      net_node_t* peer_node = ring_set_find_by_id(rings, &entry->peer_id);
      if (peer_node == NULL) continue;

      // Skip rendezvous-only nodes
      if (peer_node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;

      // Skip nodes in the path
      bool peer_in_path = false;
      for (uint8_t path_index = 0; path_index < query->path_len; path_index++) {
        if (node_id_equals(&query->path[path_index], &entry->peer_id)) {
          peer_in_path = true;
          break;
        }
      }
      if (peer_in_path) continue;

      // Skip nodes in visited bloom
      if (closest_nodes_is_visited(query->visited_bloom, query->visited_count,
                                   entry->peer_id.hash)) {
        continue;
      }

      float weight = peer_node->weight;
      if (weight < CLOSEST_NODES_MIN_WEIGHT) weight = CLOSEST_NODES_MIN_WEIGHT;

      // Prefer lower hops (stronger gravity well); break ties by weight
      if (hops < gravity_best_level ||
          (hops == gravity_best_level && weight > gravity_best_weight)) {
        gravity_candidate = peer_node;
        gravity_best_weight = weight;
        gravity_best_level = hops;
      }
    }
  }

  if (gravity_candidate != NULL) {
    next_hops[0] = gravity_candidate;
    *next_hop_count = 1;
    return CLOSEST_NODES_FORWARDING;
  }

  // Step 6: Gather ring candidates
  net_node_t* candidates[RING_K * RING_MAX_RINGS];
  float candidate_weights[RING_K * RING_MAX_RINGS];
  size_t candidate_count = 0;

  for (size_t ring_index = 0; ring_index < rings->ring_count; ring_index++) {
    ring_t* ring = &rings->rings[ring_index];
    for (int node_index = 0; node_index < ring->primary.length; node_index++) {
      net_node_t* node = ring->primary.data[node_index];
      if (node == NULL) continue;

      if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;

      bool in_path = false;
      for (uint8_t path_index = 0; path_index < query->path_len; path_index++) {
        if (node_id_equals(&query->path[path_index], &node->id)) {
          in_path = true;
          break;
        }
      }
      if (in_path) continue;

      if (closest_nodes_is_visited(query->visited_bloom, query->visited_count,
                                   node->id.hash)) {
        continue;
      }

      if (node->weight < CLOSEST_NODES_MIN_WEIGHT) continue;

      if (candidate_count < RING_K * RING_MAX_RINGS) {
        candidates[candidate_count] = node;
        candidate_weights[candidate_count] = node->weight;
        candidate_count++;
      }
    }
  }

  // Also check secondary members
  for (size_t ring_index = 0; ring_index < rings->ring_count; ring_index++) {
    ring_t* ring = &rings->rings[ring_index];
    for (int node_index = 0; node_index < ring->secondary.length; node_index++) {
      net_node_t* node = ring->secondary.data[node_index];
      if (node == NULL) continue;

      if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;

      bool in_path = false;
      for (uint8_t path_index = 0; path_index < query->path_len; path_index++) {
        if (node_id_equals(&query->path[path_index], &node->id)) {
          in_path = true;
          break;
        }
      }
      if (in_path) continue;

      if (closest_nodes_is_visited(query->visited_bloom, query->visited_count,
                                   node->id.hash)) {
        continue;
      }

      if (node->weight < CLOSEST_NODES_MIN_WEIGHT) continue;

      if (candidate_count < RING_K * RING_MAX_RINGS) {
        candidates[candidate_count] = node;
        candidate_weights[candidate_count] = node->weight;
        candidate_count++;
      }
    }
  }

  // Step 7: Roulette-wheel select CLOSEST_NODES_FORWARD_FANOUT candidates
  if (candidate_count > 0) {
    *next_hop_count = find_block_roulette_wheel_select(
        candidates, candidate_weights, candidate_count,
        CLOSEST_NODES_FORWARD_FANOUT, next_hops);
    if (*next_hop_count > 0) {
      return CLOSEST_NODES_FORWARDING;
    }
  }

  // Step 8: No candidates found
  return CLOSEST_NODES_NOT_FOUND;
}