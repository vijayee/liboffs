//
// Created by victor on 5/14/25.
//

#include "find_block.h"
#include "eabf.h"
#include "ring_set.h"
#include "net_node.h"
#include "connection_manager.h"
#include "peer_connection.h"
#include "../Util/allocator.h"
#include <xxh3.h>
#include <string.h>
#include <stdlib.h>

// --- Visited bloom filter helpers ---
// The visited bloom is a fixed 256-byte (2048-bit) bloom filter using
// XXH3 double-hashing with 3 hash functions. This avoids allocating a
// bloom_filter_t struct — we operate directly on the byte array.

#define VISITED_BLOOM_HASH_COUNT 3
#define VISITED_BLOOM_BITS (FIND_BLOCK_MAX_VISITED_BLOOM * 8)
#define VISITED_SEED_A 0x4F464653ULL  // "OFFS"
#define VISITED_SEED_B 0x46534E4F55444E53ULL  // "FSOUNDS"

static void visited_bloom_set(uint8_t* bloom, const uint8_t* data, size_t len) {
  uint64_t hash_a = XXH3_64bits_withSeed(data, len, VISITED_SEED_A);
  uint64_t hash_b = XXH3_64bits_withSeed(data, len, VISITED_SEED_B);

  for (uint32_t index = 0; index < VISITED_BLOOM_HASH_COUNT; index++) {
    // Double hashing: h(i) = (hash_a + i * hash_b) % bits
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

void find_block_add_visited(uint8_t* visited_bloom, uint16_t* visited_count,
                           const uint8_t* block_hash) {
  if (visited_bloom == NULL || visited_count == NULL || block_hash == NULL) return;
  visited_bloom_set(visited_bloom, block_hash, 32);
  (*visited_count)++;
}

bool find_block_is_visited(const uint8_t* visited_bloom, uint16_t visited_count,
                          const uint8_t* block_hash) {
  if (visited_bloom == NULL || block_hash == NULL || visited_count == 0) return false;
  return visited_bloom_check(visited_bloom, block_hash, 32);
}

// --- Roulette-wheel selection helper ---
// Select up to max_candidates from candidates[] weighted by weight[].
// Returns the number of selected candidates. No duplicates.

size_t find_block_roulette_wheel_select(net_node_t** candidates, float* weights,
                                         size_t candidate_count, size_t max_candidates,
                                         net_node_t** selected) {
  if (candidate_count == 0 || max_candidates == 0) return 0;

  size_t selected_count = 0;

  // Compute total weight
  float total_weight = 0.0f;
  for (size_t index = 0; index < candidate_count; index++) {
    total_weight += weights[index];
  }

  if (total_weight <= 0.0f) {
    // All weights zero — fall back to sequential selection
    size_t limit = max_candidates < candidate_count ? max_candidates : candidate_count;
    for (size_t index = 0; index < limit; index++) {
      selected[index] = candidates[index];
      selected_count++;
    }
    return selected_count;
  }

  // Track which candidates have been selected (to avoid duplicates)
  bool* already_selected = get_clear_memory(candidate_count * sizeof(bool));
  if (already_selected == NULL) {
    size_t limit = max_candidates < candidate_count ? max_candidates : candidate_count;
    for (size_t index = 0; index < limit; index++) {
      selected[index] = candidates[index];
      selected_count++;
    }
    return selected_count;
  }

  float remaining_weight = total_weight;

  for (size_t selection = 0; selection < max_candidates && remaining_weight > 0.0f; selection++) {
    // Spin the wheel
    float spin = (float)rand() / (float)RAND_MAX * remaining_weight;
    float cumulative = 0.0f;

    for (size_t index = 0; index < candidate_count; index++) {
      if (already_selected[index]) continue;
      cumulative += weights[index];
      if (spin <= cumulative) {
        selected[selected_count++] = candidates[index];
        already_selected[index] = true;
        remaining_weight -= weights[index];
        break;
      }
    }
  }

  free(already_selected);
  return selected_count;
}

// --- FindBlock execute ---

find_block_result_e find_block_execute(
    eabf_table_t* eabf_table,
    eabf_ttl_table_t* eabf_ttl,
    connection_manager_t* conn_mgr,
    ring_set_t* rings,
    const node_id_t* local_id,
    const find_block_state_t* state,
    net_node_t** next_hops,
    size_t* next_hop_count) {
  if (state == NULL || local_id == NULL || next_hops == NULL || next_hop_count == NULL) {
    return FIND_BLOCK_NOT_FOUND;
  }

  // eabf_ttl is used by the caller to set timer-based TTL on EABF subscriptions
  // after find_block_execute returns FIND_BLOCK_TTL_EXPIRED
  (void)eabf_ttl;

  *next_hop_count = 0;

  // Step 1: Check local index — caller checks block_cache before invoking this.
  // If the block is local, respond FOUND directly without calling find_block_execute.

  // Step 2: If TTL expired (ttl == 0), this is a terminal node.
  // We should insert the block_hash into our EABFs as a negative-information
  // gravity well for future queries, but that's done by the caller after
  // this function returns. Return TTL_EXPIRED so the caller knows to
  // subscribe the hash in local EABFs.
  if (state->ttl == 0) {
    return FIND_BLOCK_TTL_EXPIRED;
  }

  // Step 3: Check connection manager gravity wells first (connected peers)
  // Prefer connected peers — they have live QUIC streams for immediate forwarding
  if (conn_mgr != NULL) {
    size_t match_count = 0;
    peer_connection_t** matches = connection_manager_get_peers_for_topic(
        conn_mgr, state->block_hash, 32, &match_count);
    if (matches != NULL && match_count > 0) {
      // Find the first matching peer not already in the path
      for (size_t match_index = 0; match_index < match_count; match_index++) {
        peer_connection_t* peer = matches[match_index];
        bool in_path = false;
        for (uint8_t path_index = 0; path_index < state->path_len; path_index++) {
          if (node_id_equals(&state->path[path_index], &peer->remote_node_id)) {
            in_path = true;
            break;
          }
        }
        if (!in_path) {
          // Skip peers already in visited bloom
          if (find_block_is_visited(state->visited_bloom, state->visited_count,
                                    peer->remote_node_id.hash)) {
            continue;
          }
          // Look up the peer node in ring table for net_node_t
          net_node_t* peer_node = ring_set_find_by_id(rings, &peer->remote_node_id);
          if (peer_node != NULL && !(peer_node->flags & NET_NODE_FLAG_RENDEZVOUS)) {
            next_hops[0] = peer_node;
            *next_hop_count = 1;
            free(matches);
            return FIND_BLOCK_FORWARDING;
          }
        }
      }
      free(matches);
    }
  }

  // Step 4: Check EABF gravity wells — directed walk (ring table peers)
  // For each peer's EABF, check if block_hash appears at some level L.
  // Prefer the peer with the lowest L (strongest gravity well) and highest weight.
  // Skip peers already in the path (cycle detection).

  net_node_t* gravity_candidate = NULL;
  float gravity_best_weight = FIND_BLOCK_MIN_WEIGHT;
  uint32_t gravity_best_level = UINT32_MAX;

  if (eabf_table != NULL) {
  for (size_t index = 0; index < eabf_table->count; index++) {
    eabf_entry_t* entry = &eabf_table->entries[index];
    eabf_t* eabf = entry->eabf;
    if (eabf == NULL) continue;

    // Check if this peer's EABF has the block_hash
    uint32_t hops = 0;
    bool found = eabf_check(eabf, state->block_hash, 32, &hops);
    if (!found) continue;

    // Look up the peer node in our ring table
    net_node_t* peer_node = ring_set_find_by_id(rings, &entry->peer_id);
    if (peer_node == NULL) continue;

    // Skip rendezvous-only nodes
    if (peer_node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;

    // Skip nodes already in the path (cycle detection)
    bool peer_in_path = false;
    for (uint8_t path_index = 0; path_index < state->path_len; path_index++) {
      if (node_id_equals(&state->path[path_index], &entry->peer_id)) {
        peer_in_path = true;
        break;
      }
    }
    if (peer_in_path) continue;

    // Skip peers already in visited bloom
    if (find_block_is_visited(state->visited_bloom, state->visited_count,
                              entry->peer_id.hash)) {
      continue;
    }

    // Use the peer's Hebbian weight, with a floor of FIND_BLOCK_MIN_WEIGHT
    float weight = peer_node->weight;
    if (weight < FIND_BLOCK_MIN_WEIGHT) weight = FIND_BLOCK_MIN_WEIGHT;

    // Prefer lower hops (stronger gravity well); break ties by weight
    if (hops < gravity_best_level ||
        (hops == gravity_best_level && weight > gravity_best_weight)) {
      gravity_candidate = peer_node;
      gravity_best_weight = weight;
      gravity_best_level = hops;
    }
  }
  } // end if (eabf_table != NULL)

  if (gravity_candidate != NULL) {
    // Directed walk: forward to the peer with the strongest gravity well
    next_hops[0] = gravity_candidate;
    *next_hop_count = 1;
    return FIND_BLOCK_FORWARDING;
  }

  // Step 5: No gravity well — gather candidates from ring table
  // Walk rings from lowest latency to highest. Collect candidates that:
  //   - Are not in the visited path
  //   - Are not rendezvous-only
  //   - Have weight >= FIND_BLOCK_MIN_WEIGHT

  net_node_t* candidates[RING_K * RING_MAX_RINGS];
  float candidate_weights[RING_K * RING_MAX_RINGS];
  size_t candidate_count = 0;

  for (size_t ring_index = 0; ring_index < rings->ring_count; ring_index++) {
    ring_t* ring = &rings->rings[ring_index];
    for (int node_index = 0; node_index < ring->primary.length; node_index++) {
      net_node_t* node = ring->primary.data[node_index];
      if (node == NULL) continue;

      // Skip rendezvous-only nodes
      if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;

      // Skip nodes already in the path
      bool in_path = false;
      for (uint8_t path_index = 0; path_index < state->path_len; path_index++) {
        if (node_id_equals(&state->path[path_index], &node->id)) {
          in_path = true;
          break;
        }
      }
      if (in_path) continue;

      // Skip nodes already in visited bloom
      if (find_block_is_visited(state->visited_bloom, state->visited_count,
                                node->id.hash)) {
        continue;
      }

      // Skip nodes below minimum weight
      if (node->weight < FIND_BLOCK_MIN_WEIGHT) continue;

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
      for (uint8_t path_index = 0; path_index < state->path_len; path_index++) {
        if (node_id_equals(&state->path[path_index], &node->id)) {
          in_path = true;
          break;
        }
      }
      if (in_path) continue;

      // Skip nodes already in visited bloom
      if (find_block_is_visited(state->visited_bloom, state->visited_count,
                                node->id.hash)) {
        continue;
      }

      if (node->weight < FIND_BLOCK_MIN_WEIGHT) continue;

      if (candidate_count < RING_K * RING_MAX_RINGS) {
        candidates[candidate_count] = node;
        candidate_weights[candidate_count] = node->weight;
        candidate_count++;
      }
    }
  }

  // Step 6: Roulette-wheel select FORWARD_FANOUT candidates
  if (candidate_count > 0) {
    *next_hop_count = find_block_roulette_wheel_select(
        candidates, candidate_weights, candidate_count,
        FIND_BLOCK_FORWARD_FANOUT, next_hops);
    if (*next_hop_count > 0) {
      return FIND_BLOCK_FORWARDING;
    }
  }

  // Step 7: No candidates — same as TTL=0 failure
  return FIND_BLOCK_NOT_FOUND;
}