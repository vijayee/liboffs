//
// Created by victor on 5/14/25.
//

#ifndef OFFS_RESPIRATION_H
#define OFFS_RESPIRATION_H

#include "node_id.h"
#include <stdint.h>
#include <stdbool.h>

// Respiration constants from Network Design spec
#define RESPIRATION_INHALE_THRESHOLD 0.50f
#define RESPIRATION_EXHALE_THRESHOLD 0.80f
#define RESPIRATION_ALPHA 2.0f
#define RESPIRATION_SEEK_MAX_NEIGHBORS 3
// Candidates verified per exhale cycle. The verify/store exchanges ride the
// wire RPC rate limiter (FIND_BLOCK: base_rate 5/s * small-network 3x
// multiplier, 50/s burst ceiling, ~60-token burst budget) and the whole
// cycle must resolve inside RESPIRATION_WATCHDOG_TIMEOUT_MS and inside the
// wanted-list request deadline — so a full-cache exhale (tens of thousands
// of candidates) makes progress in rate-limit-sized batches instead of
// one atomic flood. Keep the batch under the burst budget so a batch is
// answered before the 1s-class request deadline sweeps it as not-found.
#define RESPIRATION_EXHALE_BATCH 40

// Seek throttle: compute interval in ms for a given capacity
// tau(c) = tau_min + (tau_max - tau_min) * (c / 0.50)^alpha  for c in [0, 0.50)
// tau(c) = infinity for c >= 0.50
uint64_t respiration_seek_interval(float capacity,
                                   uint32_t tau_min_ms,
                                   uint32_t tau_max_ms);

// Check if node should inhale (capacity < 50%)
bool respiration_should_inhale(float capacity);

// Check if node should exhale (capacity >= 80%)
bool respiration_should_exhale(float capacity);

// Compute how many blocks to shed to reach 50% capacity
uint32_t respiration_blocks_to_free(float capacity, uint32_t total_blocks, uint32_t block_size);

#endif // OFFS_RESPIRATION_H