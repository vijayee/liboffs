//
// Created by victor on 5/15/25.
//

#ifndef OFFS_TIMING_WHEEL_H
#define OFFS_TIMING_WHEEL_H

#include "node_id.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct timing_wheel_entry_t {
  uint64_t id;                      // unique ID for removal/refresh
  node_id_t peer_id;                // which peer's EABF
  uint32_t level;                   // EABF level
  size_t bucket_index;              // bucket in the EBF at that level
  uint32_t fingerprint;             // fingerprint in that bucket
  uint8_t block_hash[32];           // the block hash that was inserted
  struct timing_wheel_entry_t* next; // linked list within slot
} timing_wheel_entry_t;

typedef struct timing_wheel_slot_t {
  timing_wheel_entry_t* head;
} timing_wheel_slot_t;

typedef struct timing_wheel_t {
  timing_wheel_slot_t* slots;
  size_t slot_count;
  uint64_t slot_duration_ms;       // duration of each slot in ms
  size_t current_slot;              // current position in the wheel
  uint64_t next_id;                 // monotonically increasing ID counter
  size_t count;                     // total entries across all slots
} timing_wheel_t;

void timing_wheel_init(timing_wheel_t* wheel, size_t slot_count, uint64_t slot_duration_ms);
void timing_wheel_deinit(timing_wheel_t* wheel);

uint64_t timing_wheel_add(timing_wheel_t* wheel, const node_id_t* peer_id,
                          uint32_t level, size_t bucket_index, uint32_t fingerprint,
                          const uint8_t* block_hash);

uint64_t timing_wheel_refresh(timing_wheel_t* wheel, uint64_t old_id,
                              const node_id_t* peer_id,
                              uint32_t level, size_t bucket_index, uint32_t fingerprint,
                              const uint8_t* block_hash);

int timing_wheel_remove(timing_wheel_t* wheel, uint64_t id);

// Advance the wheel by num_slots positions. Returns array of expired entries.
// Caller must free the returned array.
timing_wheel_entry_t* timing_wheel_advance(timing_wheel_t* wheel, size_t num_slots,
                                           size_t* out_count);

uint64_t timing_wheel_ttl_for_level(uint32_t level, uint64_t base_ttl_ms);

#endif // OFFS_TIMING_WHEEL_H