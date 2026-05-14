//
// Created by victor on 5/14/25.
//

#include "respiration.h"
#include <math.h>

// Seek throttle: compute interval in ms for a given capacity
// tau(c) = tau_min + (tau_max - tau_min) * (c / 0.50)^alpha  for c in [0, 0.50)
// tau(c) = infinity (UINT64_MAX) for c >= 0.50
uint64_t respiration_seek_interval(float capacity) {
  if (capacity >= RESPIRATION_INHALE_THRESHOLD) {
    return UINT64_MAX;  // Don't seek
  }

  float ratio = capacity / RESPIRATION_INHALE_THRESHOLD;
  if (ratio < 0.0f) ratio = 0.0f;
  float scaled = powf(ratio, RESPIRATION_ALPHA);
  float interval = (float)RESPIRATION_TAU_MIN_MS +
                   ((float)RESPIRATION_TAU_MAX_MS - (float)RESPIRATION_TAU_MIN_MS) * scaled;
  return (uint64_t)interval;
}

bool respiration_should_inhale(float capacity) {
  return capacity < RESPIRATION_INHALE_THRESHOLD;
}

bool respiration_should_exhale(float capacity) {
  return capacity >= RESPIRATION_EXHALE_THRESHOLD;
}

// Compute how many blocks to free to reach 50% capacity
uint32_t respiration_blocks_to_free(float capacity, uint32_t total_blocks, uint32_t block_size) {
  (void)block_size;
  if (capacity < RESPIRATION_EXHALE_THRESHOLD) return 0;
  if (total_blocks == 0) return 0;

  // target_count = total * 0.50
  uint32_t target_count = (uint32_t)((float)total_blocks * RESPIRATION_INHALE_THRESHOLD);
  if (total_blocks <= target_count) return 0;

  return total_blocks - target_count;
}