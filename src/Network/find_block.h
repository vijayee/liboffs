//
// Created by victor on 5/14/25.
//

#ifndef OFFS_FIND_BLOCK_H
#define OFFS_FIND_BLOCK_H

#include "eabf.h"
#include "ring_set.h"
#include "authority.h"
#include "connection_manager.h"
#include <stdint.h>
#include <stdbool.h>

// FindBlock constants from Network Design spec
#define FIND_BLOCK_FORWARD_FANOUT 3
#define FIND_BLOCK_MAX_PATH 6
#define FIND_BLOCK_MAX_VISITED_BLOOM 256  // bytes = 2048 bits
#define FIND_BLOCK_MIN_WEIGHT 0.01f

// FindBlock result codes
typedef enum find_block_result_e {
  FIND_BLOCK_FOUND,
  FIND_BLOCK_NOT_FOUND,
  FIND_BLOCK_FORWARDING,
  FIND_BLOCK_TTL_EXPIRED
} find_block_result_e;

// FindBlock directed walk state — used internally by the handler
typedef struct find_block_state_t {
  uint64_t message_id;
  uint8_t block_hash[32];
  uint8_t ttl;
  uint8_t visited_bloom[FIND_BLOCK_MAX_VISITED_BLOOM];
  uint16_t visited_count;
  node_id_t path[FIND_BLOCK_MAX_PATH];
  uint8_t path_len;
  uint64_t start_time_ms;
  node_id_t original_source;
} find_block_state_t;

// Execute FindBlock handler logic
// Returns result code and populates response fields
find_block_result_e find_block_execute(
    eabf_table_t* eabf_table,
    eabf_ttl_table_t* eabf_ttl,
    connection_manager_t* conn_mgr,
    ring_set_t* rings,
    const node_id_t* local_id,
    const find_block_state_t* state,
    /* Output: selected next-hop nodes (up to FORWARD_FANOUT) */
    net_node_t** next_hops,
    size_t* next_hop_count);

// Add a block_hash to the visited bloom filter
void find_block_add_visited(uint8_t* visited_bloom, uint16_t* visited_count,
                           const uint8_t* block_hash);

// Check if a block_hash is in the visited bloom filter
bool find_block_is_visited(const uint8_t* visited_bloom, uint16_t visited_count,
                           const uint8_t* block_hash);

// Roulette-wheel selection — used by both FindBlock and StoreBlock
// Select up to max_candidates from candidates[] weighted by weights[].
// Returns the number of selected candidates.
size_t find_block_roulette_wheel_select(net_node_t** candidates, float* weights,
                                         size_t candidate_count, size_t max_candidates,
                                         net_node_t** selected);

#endif // OFFS_FIND_BLOCK_H