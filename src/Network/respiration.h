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