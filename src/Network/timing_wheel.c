//
// Created by victor on 5/15/25.
//

#include "timing_wheel.h"
#include "../Util/allocator.h"
#include <string.h>

void timing_wheel_init(timing_wheel_t* wheel, size_t slot_count, uint64_t slot_duration_ms) {
  wheel->slots = get_clear_memory(slot_count * sizeof(timing_wheel_slot_t));
  wheel->slot_count = slot_count;
  wheel->slot_duration_ms = slot_duration_ms;
  wheel->current_slot = 0;
  wheel->next_id = 1;
  wheel->count = 0;
}

void timing_wheel_deinit(timing_wheel_t* wheel) {
  if (wheel->slots == NULL) {
    return;
  }
  for (size_t slot_index = 0; slot_index < wheel->slot_count; slot_index++) {
    timing_wheel_entry_t* entry = wheel->slots[slot_index].head;
    while (entry != NULL) {
      timing_wheel_entry_t* next = entry->next;
      free(entry);
      entry = next;
    }
    wheel->slots[slot_index].head = NULL;
  }
  free(wheel->slots);
  wheel->slots = NULL;
  wheel->slot_count = 0;
  wheel->count = 0;
}

uint64_t timing_wheel_add(timing_wheel_t* wheel, const node_id_t* peer_id,
                          uint32_t level, size_t bucket_index, uint32_t fingerprint,
                          const uint8_t* block_hash) {
  timing_wheel_entry_t* entry = get_clear_memory(sizeof(timing_wheel_entry_t));
  entry->id = wheel->next_id++;
  entry->peer_id = *peer_id;
  entry->level = level;
  entry->bucket_index = bucket_index;
  entry->fingerprint = fingerprint;
  if (block_hash != NULL) {
    memcpy(entry->block_hash, block_hash, 32);
  }

  uint64_t ttl_ms = timing_wheel_ttl_for_level(level, wheel->slot_duration_ms * wheel->slot_count);
  size_t slots_ahead = (size_t)(ttl_ms / wheel->slot_duration_ms);
  if (slots_ahead == 0) {
    slots_ahead = 1;
  }
  if (slots_ahead > wheel->slot_count - 1) {
    slots_ahead = wheel->slot_count - 1;
  }
  size_t target_slot = (wheel->current_slot + slots_ahead) % wheel->slot_count;

  entry->next = wheel->slots[target_slot].head;
  wheel->slots[target_slot].head = entry;
  wheel->count++;
  return entry->id;
}

uint64_t timing_wheel_refresh(timing_wheel_t* wheel, uint64_t old_id,
                              const node_id_t* peer_id,
                              uint32_t level, size_t bucket_index, uint32_t fingerprint,
                              const uint8_t* block_hash) {
  timing_wheel_remove(wheel, old_id);
  return timing_wheel_add(wheel, peer_id, level, bucket_index, fingerprint, block_hash);
}

int timing_wheel_remove(timing_wheel_t* wheel, uint64_t id) {
  for (size_t slot_index = 0; slot_index < wheel->slot_count; slot_index++) {
    timing_wheel_entry_t** indirect = &wheel->slots[slot_index].head;
    while (*indirect != NULL) {
      if ((*indirect)->id == id) {
        timing_wheel_entry_t* removed = *indirect;
        *indirect = removed->next;
        free(removed);
        wheel->count--;
        return 0;
      }
      indirect = &(*indirect)->next;
    }
  }
  return -1;
}

timing_wheel_entry_t* timing_wheel_advance(timing_wheel_t* wheel, size_t num_slots,
                                           size_t* out_count) {
  size_t capacity = 16;
  size_t result_count = 0;
  timing_wheel_entry_t* results = get_clear_memory(capacity * sizeof(timing_wheel_entry_t));

  for (size_t step = 0; step < num_slots; step++) {
    wheel->current_slot = (wheel->current_slot + 1) % wheel->slot_count;
    timing_wheel_entry_t* entry = wheel->slots[wheel->current_slot].head;
    while (entry != NULL) {
      if (result_count >= capacity) {
        capacity *= 2;
        timing_wheel_entry_t* new_results = get_clear_memory(capacity * sizeof(timing_wheel_entry_t));
        memcpy(new_results, results, result_count * sizeof(timing_wheel_entry_t));
        free(results);
        results = new_results;
      }
      results[result_count] = *entry;
      result_count++;
      wheel->count--;
      timing_wheel_entry_t* freed = entry;
      entry = entry->next;
      free(freed);
    }
    wheel->slots[wheel->current_slot].head = NULL;
  }

  *out_count = result_count;
  return results;
}

uint64_t timing_wheel_ttl_for_level(uint32_t level, uint64_t base_ttl_ms) {
  if (level == 0) {
    return base_ttl_ms;
  }
  if (level == 1) {
    return (uint64_t)(base_ttl_ms / 1.5);
  }
  if (level == 2) {
    return base_ttl_ms / 2;
  }
  // Level 3+: TTL / (1 + level * 0.5)
  double divisor = 1.0 + (double)level * 0.5;
  return (uint64_t)(base_ttl_ms / divisor);
}