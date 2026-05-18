//
// Created by victor on 5/14/25.
//

#include "store_block.h"
#include "eabf.h"
#include "ring_set.h"
#include "net_node.h"
#include "find_block.h"
#include "connection_manager.h"
#include "peer_connection.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

// --- SHOULD-ACCEPT logic ---
// Accept with probability = 1.0 - capacity/0.80 when capacity < 0.80
// Reject if already stored, no room, or in exhale phase (capacity >= 0.80)

bool store_block_should_accept(float local_capacity, node_phase_e local_phase,
                                const uint8_t* block_hash, uint32_t block_size) {
  // In exhale phase (capacity >= 0.80), decline
  if (local_phase == NODE_PHASE_EXHALE) return false;

  // At or above threshold, decline
  if (local_capacity >= STORE_BLOCK_EXHALE_CAPACITY_THRESHOLD) return false;

  // Accept probability = 1.0 - capacity/0.80
  // At capacity=0: probability=1.0 (always accept)
  // At capacity=0.40: probability=0.5
  // At capacity=0.80: probability=0.0 (never accept)
  float accept_probability = 1.0f - (local_capacity / STORE_BLOCK_EXHALE_CAPACITY_THRESHOLD);

  // Larger blocks are slightly less likely to be accepted to preserve space
  // Scale down by factor of 1.0 / (1.0 + block_size / 1MB)
  if (block_size > 0) {
    float size_factor = 1.0f / (1.0f + (float)block_size / 1048576.0f);
    accept_probability *= size_factor;
  }

  if (block_hash != NULL) {
    // Higher FIB blocks get a slight priority boost
    // This is a placeholder until block_cache lookup is wired up
  }

  if (accept_probability < 0.0f) accept_probability = 0.0f;
  if (accept_probability > 1.0f) accept_probability = 1.0f;

  float roll = (float)rand() / (float)RAND_MAX;
  return roll < accept_probability;
}

// --- Compute composite storage score ---
// S(j) = w_{self→j} × (1 - capacity_j) × 1/(1 + latency/β) × availability_j

static float compute_storage_score(const net_node_t* node, uint32_t latency_beta_us) {
  if (node == NULL) return 0.0f;

  float weight = node->weight;
  if (weight < FIND_BLOCK_MIN_WEIGHT) weight = FIND_BLOCK_MIN_WEIGHT;

  float capacity_factor = 1.0f - node->capacity;
  if (capacity_factor < 0.0f) capacity_factor = 0.0f;

  float latency_factor = 1.0f / (1.0f + (float)node->latency_ms * 1000.0f / (float)latency_beta_us);

  float availability = node->availability;

  return weight * capacity_factor * latency_factor * availability;
}

// --- StoreBlock execute ---

store_block_result_e store_block_execute(
    eabf_table_t* eabf_table,
    connection_manager_t* conn_mgr,
    ring_set_t* rings,
    const node_id_t* local_id,
    float local_capacity,
    node_phase_e local_phase,
    const store_block_state_t* state,
    net_node_t** next_hops,
    size_t* next_hop_count) {
  if (state == NULL || local_id == NULL || next_hops == NULL || next_hop_count == NULL) {
    return STORE_BLOCK_DECLINED;
  }

  *next_hop_count = 0;

  // Step 1: Check SHOULD-ACCEPT
  if (store_block_should_accept(local_capacity, local_phase, state->block_hash, state->block_size)) {
    return STORE_BLOCK_ACCEPTED;
  }

  // Step 2: If max_hops == 0, decline
  if (state->max_hops == 0) {
    return STORE_BLOCK_MAX_HOPS_REACHED;
  }

  // Step 2a: Check connection manager gravity wells first (connected peers)
  if (conn_mgr != NULL) {
    size_t match_count = 0;
    peer_connection_t** matches = connection_manager_get_peers_for_topic(
        conn_mgr, state->block_hash, 32, &match_count);
    if (matches != NULL && match_count > 0) {
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
          net_node_t* peer_node = ring_set_find_by_id(rings, &peer->remote_node_id);
          if (peer_node != NULL && !(peer_node->flags & NET_NODE_FLAG_RENDEZVOUS)) {
            next_hops[0] = peer_node;
            *next_hop_count = 1;
            free(matches);
            return STORE_BLOCK_FORWARDING;
          }
        }
      }
      free(matches);
    }
  }

  // Step 2.5: Check EABF gravity wells for directed walk
  net_node_t* gravity_candidate = NULL;
  float gravity_best_weight = FIND_BLOCK_MIN_WEIGHT;
  uint32_t gravity_best_level = UINT32_MAX;

  if (eabf_table != NULL) {
  for (size_t index = 0; index < eabf_table->count; index++) {
    eabf_entry_t* entry = &eabf_table->entries[index];
    eabf_t* eabf = entry->eabf;
    if (eabf == NULL) continue;

    uint32_t hops = 0;
    bool found = eabf_check(eabf, state->block_hash, 32, &hops);
    if (!found) continue;

    net_node_t* peer_node = ring_set_find_by_id(rings, &entry->peer_id);
    if (peer_node == NULL) continue;
    if (peer_node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;
    if (peer_node->phase == NODE_PHASE_EXHALE) continue;
    if (peer_node->consecutive_fails >= STORE_BLOCK_MAX_FAILS) continue;

    // Skip peers in the path
    bool in_path = false;
    for (uint8_t path_index = 0; path_index < state->path_len; path_index++) {
      if (node_id_equals(&state->path[path_index], &peer_node->id)) {
        in_path = true;
        break;
      }
    }
    if (in_path) continue;

    if (find_block_is_visited(state->visited_bloom, state->visited_count, peer_node->id.hash)) {
      continue;
    }

    float weight = peer_node->weight;
    if (weight < FIND_BLOCK_MIN_WEIGHT) weight = FIND_BLOCK_MIN_WEIGHT;

    if (hops < gravity_best_level ||
        (hops == gravity_best_level && weight > gravity_best_weight)) {
      gravity_candidate = peer_node;
      gravity_best_weight = weight;
      gravity_best_level = hops;
    }
  }

  if (gravity_candidate != NULL) {
    next_hops[0] = gravity_candidate;
    *next_hop_count = 1;
    return STORE_BLOCK_FORWARDING;
  }
  } // end if (eabf_table != NULL)

  // Step 3: If max_hops == 0, decline (redundant after EABF check, but kept for clarity)
  if (state->max_hops == 0) {
    return STORE_BLOCK_MAX_HOPS_REACHED;
  }

  // Step 4-5: Declined — gather candidates from rings
  // Skip: exhale-phase peers, peers in visited bloom, peers with consecutive_fails >= MAX_FAILS,
  //       peers with weight < MIN_WEIGHT

  net_node_t* candidates[RING_K * RING_MAX_RINGS];
  float candidate_scores[RING_K * RING_MAX_RINGS];
  size_t candidate_count = 0;

  for (size_t ring_index = 0; ring_index < rings->ring_count; ring_index++) {
    ring_t* ring = &rings->rings[ring_index];
    for (int node_index = 0; node_index < ring->primary.length; node_index++) {
      net_node_t* node = ring->primary.data[node_index];
      if (node == NULL) continue;

      // Skip rendezvous-only nodes
      if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;

      // Skip exhale-phase peers
      if (node->phase == NODE_PHASE_EXHALE) continue;

      // Skip peers with too many consecutive failures
      if (node->consecutive_fails >= STORE_BLOCK_MAX_FAILS) continue;

      // Skip peers in the path
      bool in_path = false;
      for (uint8_t path_index = 0; path_index < state->path_len; path_index++) {
        if (node_id_equals(&state->path[path_index], &node->id)) {
          in_path = true;
          break;
        }
      }
      if (in_path) continue;

      // Skip peers already in visited bloom
      if (find_block_is_visited(state->visited_bloom, state->visited_count, node->id.hash)) {
        continue;
      }

      // Compute composite storage score
      float score = compute_storage_score(node, RING_BETA_MS * 1000);
      if (score < FIND_BLOCK_MIN_WEIGHT) continue;

      if (candidate_count < RING_K * RING_MAX_RINGS) {
        candidates[candidate_count] = node;
        candidate_scores[candidate_count] = score;
        candidate_count++;
      }
    }
  }

  // Also check secondary ring members
  for (size_t ring_index = 0; ring_index < rings->ring_count; ring_index++) {
    ring_t* ring = &rings->rings[ring_index];
    for (int node_index = 0; node_index < ring->secondary.length; node_index++) {
      net_node_t* node = ring->secondary.data[node_index];
      if (node == NULL) continue;

      if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;
      if (node->phase == NODE_PHASE_EXHALE) continue;
      if (node->consecutive_fails >= STORE_BLOCK_MAX_FAILS) continue;

      bool in_path = false;
      for (uint8_t path_index = 0; path_index < state->path_len; path_index++) {
        if (node_id_equals(&state->path[path_index], &node->id)) {
          in_path = true;
          break;
        }
      }
      if (in_path) continue;

      if (find_block_is_visited(state->visited_bloom, state->visited_count, node->id.hash)) {
        continue;
      }

      float score = compute_storage_score(node, RING_BETA_MS * 1000);
      if (score < FIND_BLOCK_MIN_WEIGHT) continue;

      if (candidate_count < RING_K * RING_MAX_RINGS) {
        candidates[candidate_count] = node;
        candidate_scores[candidate_count] = score;
        candidate_count++;
      }
    }
  }

  // Step 6: Roulette-wheel select FORWARD_FANOUT candidates
  if (candidate_count > 0) {
    *next_hop_count = find_block_roulette_wheel_select(
        candidates, candidate_scores, candidate_count,
        STORE_BLOCK_FORWARD_FANOUT, next_hops);
    if (*next_hop_count > 0) {
      return STORE_BLOCK_FORWARDING;
    }
  }

  return STORE_BLOCK_DECLINED;
}