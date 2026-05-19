//
// Created by victor on 5/18/25.
//

#ifndef OFFS_CLOSEST_NODES_H
#define OFFS_CLOSEST_NODES_H

#include "eabf.h"
#include "ring_set.h"
#include "connection_manager.h"
#include "latency_cache.h"
#include "wire.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Closest-N protocol constants (from Meridian spec)
#define CLOSEST_NODES_MAX_VISITED 256  // bytes = 2048 bits
#define CLOSEST_NODES_MIN_WEIGHT 0.01f

// Closest-N result codes
typedef enum closest_nodes_result_e {
  CLOSEST_NODES_FOUND,
  CLOSEST_NODES_NOT_FOUND,
  CLOSEST_NODES_FORWARDING,
  CLOSEST_NODES_TTL_EXPIRED
} closest_nodes_result_e;

// Execute Closest-N handler logic
// Returns result code and populates next_hops with selected forward targets
closest_nodes_result_e closest_nodes_execute(
    eabf_table_t* eabf_table,
    eabf_ttl_table_t* eabf_ttl,
    connection_manager_t* conn_mgr,
    ring_set_t* rings,
    latency_cache_t* latency_cache,
    const node_id_t* local_id,
    const wire_closest_nodes_t* query,
    net_node_t** next_hops,
    size_t* next_hop_count);

// Select ring samples for a Closest-N response
// Walks rings from lowest latency to highest, picking one primary member per ring
// with the lowest latency, excluding exclude_id. Returns the count collected.
size_t closest_nodes_select_ring_samples(
    ring_set_t* rings,
    const node_id_t* exclude_id,
    node_id_t* ring_nodes,
    uint32_t* ring_latencies_us,
    size_t max_samples);

// Check beta convergence: returns true if current_latency is close enough to best_latency
// Convergence: current_latency * beta_denominator <= best_latency * beta_numerator
bool closest_nodes_beta_converged(uint32_t current_latency_us,
                                   uint32_t best_latency_us,
                                   uint16_t beta_numerator,
                                   uint16_t beta_denominator);

// Add a node_id hash to the visited bloom filter
void closest_nodes_add_visited(uint8_t* visited_bloom, uint16_t* visited_count,
                               const uint8_t* hash);

// Check if a node_id hash is in the visited bloom filter
bool closest_nodes_is_visited(const uint8_t* visited_bloom, uint16_t visited_count,
                              const uint8_t* hash);

#endif // OFFS_CLOSEST_NODES_H