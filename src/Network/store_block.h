//
// Created by victor on 5/14/25.
//

#ifndef OFFS_STORE_BLOCK_H
#define OFFS_STORE_BLOCK_H

#include "eabf.h"
#include "ring_set.h"
#include "authority.h"
#include <stdint.h>
#include <stdbool.h>

// StoreBlock constants from Network Design spec
#define STORE_BLOCK_FORWARD_FANOUT 3
#define STORE_BLOCK_MAX_HOPS 6
#define STORE_BLOCK_MAX_VISITED_BLOOM 256  // bytes = 2048 bits
#define STORE_BLOCK_MAX_FAILS 3
#define STORE_BLOCK_EXHALE_CAPACITY_THRESHOLD 0.80f
#define STORE_BLOCK_INHALE_CAPACITY_THRESHOLD 0.50f

// StoreBlock result codes
typedef enum store_block_result_e {
  STORE_BLOCK_ACCEPTED,
  STORE_BLOCK_DECLINED,
  STORE_BLOCK_FORWARDING,
  STORE_BLOCK_MAX_HOPS_REACHED
} store_block_result_e;

// StoreBlock state — used internally by the handler
typedef struct store_block_state_t {
  uint64_t message_id;
  uint8_t block_hash[32];
  uint32_t block_size;
  uint32_t block_fib;
  uint8_t replicas_needed;
  uint8_t max_hops;
  uint8_t visited_bloom[STORE_BLOCK_MAX_VISITED_BLOOM];
  uint16_t visited_count;
  node_id_t path[STORE_BLOCK_MAX_HOPS];
  uint8_t path_len;
  uint64_t start_time_ms;
  bool carry_data;
} store_block_state_t;

// Compute SHOULD-ACCEPT for a StoreBlock request
// Returns true if we should accept the block
bool store_block_should_accept(float local_capacity, node_phase_e local_phase,
                                const uint8_t* block_hash, uint32_t block_size);

// Execute StoreBlock handler logic
// Returns result code and populates next_hops for FORWARDING result
store_block_result_e store_block_execute(
    eabf_table_t* eabf_table,
    ring_set_t* rings,
    const node_id_t* local_id,
    float local_capacity,
    node_phase_e local_phase,
    const store_block_state_t* state,
    /* Output: selected next-hop nodes (up to FORWARD_FANOUT) */
    net_node_t** next_hops,
    size_t* next_hop_count);

#endif // OFFS_STORE_BLOCK_H