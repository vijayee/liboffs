//
// Created by victor on 5/14/25.
//

#ifndef OFFS_HEBBIAN_H
#define OFFS_HEBBIAN_H

#include "node_id.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Hebbian learning constants from Network Design spec
#define HEBBIAN_GAMMA_0 0.1f
#define HEBBIAN_ETA_FEEDBACK 0.25f
#define HEBBIAN_ETA_SYMMETRY 0.05f
#define HEBBIAN_MAX_SEARCH_TIME_MS 60000
#define HEBBIAN_MIN_WEIGHT 0.01f
#define HEBBIAN_INITIAL_WEIGHT 0.1f

// Weight multipliers for different RPC outcomes
#define HEBBIAN_FIND_BLOCK_MULTIPLIER 1.0f
#define HEBBIAN_STORE_BLOCK_EXHALE_MULTIPLIER 1.0f
#define HEBBIAN_STORE_BLOCK_RECALL_MULTIPLIER 2.0f
#define HEBBIAN_PUSH_DECLINED_MULTIPLIER -0.5f

// Hebbian weight entry — one per directed edge (self → peer)
typedef struct hebbian_weight_t {
  node_id_t peer_id;
  float weight;
} hebbian_weight_t;

// Hebbian weight table — flat array, no locks (actor model)
typedef struct hebbian_table_t {
  hebbian_weight_t* entries;
  size_t capacity;
  size_t count;
  size_t max_count;     /* Cap; evict lowest-weight entry when reached. 0 = no cap. See audit #14. */
  float decay_factor;
} hebbian_table_t;

// Initialize/deinitialize weight table
void hebbian_table_init(hebbian_table_t* table, size_t capacity, float decay_factor);
void hebbian_table_deinit(hebbian_table_t* table);

// Set the max-count cap. When the table reaches max_count, the lowest-weight
// entry is evicted on insert. 0 disables the cap (unbounded — old behavior).
void hebbian_table_set_max_count(hebbian_table_t* table, size_t max_count);

// Look up weight for a peer (returns HEBBIAN_MIN_WEIGHT if not found)
float hebbian_table_get(const hebbian_table_t* table, const node_id_t* peer_id);

// Set weight for a peer (creates entry if not found)
void hebbian_table_set(hebbian_table_t* table, const node_id_t* peer_id, float weight);

// Apply frequency rule: w_{requester→holder} += delta_w
void hebbian_frequency(hebbian_table_t* table, const node_id_t* holder, float delta_w);

// Apply feedback rule along path:
// For i = 1 to path_len-2: w_{path[i]→path[i+1]} += eta_f × delta_w
void hebbian_feedback(hebbian_table_t* table, const node_id_t* path, uint8_t path_len,
                      float delta_w);

// Apply symmetry rule along path:
// For i = 0 to path_len-2: w_{path[i+1]→path[i]} += eta_s × delta_w
void hebbian_symmetry(hebbian_table_t* table, const node_id_t* path, uint8_t path_len,
                      float delta_w);

// Apply decay: multiply all weights by table->decay_factor
void hebbian_decay(hebbian_table_t* table);

// Remove a peer's weight entry
int hebbian_table_remove(hebbian_table_t* table, const node_id_t* peer_id);

// Compute delta_w from response latency
// delta_w = gamma_0 × max(0, 1 - latency/MAX_SEARCH_TIME) × quality × multiplier
float hebbian_compute_delta(uint64_t latency_ms, float multiplier);

// Apply all three rules for a successful RPC response along a path
void hebbian_apply_success(hebbian_table_t* table, const node_id_t* path, uint8_t path_len,
                           uint64_t latency_ms, float multiplier);

// Query the weight table — returns a heap-allocated array of (node_id, weight)
// pairs and sets *out_count. Caller must free() the returned array.
// Returns NULL on error.
hebbian_weight_t* hebbian_table_query(const hebbian_table_t* table, size_t* out_count);

#endif // OFFS_HEBBIAN_H