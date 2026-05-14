//
// Created by victor on 5/14/25.
//

#include "hebbian.h"
#include "../Util/allocator.h"
#include <string.h>
#include <math.h>

// --- Table lifecycle ---

void hebbian_table_init(hebbian_table_t* table, size_t capacity) {
  if (capacity == 0) capacity = 16;
  table->entries = get_clear_memory(capacity * sizeof(hebbian_weight_t));
  table->capacity = capacity;
  table->count = 0;
}

void hebbian_table_deinit(hebbian_table_t* table) {
  if (table == NULL) return;
  free(table->entries);
  table->entries = NULL;
  table->capacity = 0;
  table->count = 0;
}

// --- Lookup and mutation ---

static hebbian_weight_t* hebbian_find(hebbian_table_t* table, const node_id_t* peer_id) {
  for (size_t index = 0; index < table->count; index++) {
    if (node_id_equals(&table->entries[index].peer_id, peer_id)) {
      return &table->entries[index];
    }
  }
  return NULL;
}

static hebbian_weight_t* hebbian_insert(hebbian_table_t* table, const node_id_t* peer_id, float weight) {
  // Grow if needed
  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity * 2;
    hebbian_weight_t* new_entries = get_clear_memory(new_capacity * sizeof(hebbian_weight_t));
    if (new_entries == NULL) return NULL;
    memcpy(new_entries, table->entries, table->count * sizeof(hebbian_weight_t));
    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
  }

  hebbian_weight_t* entry = &table->entries[table->count++];
  memcpy(&entry->peer_id, peer_id, sizeof(node_id_t));
  entry->weight = weight;
  return entry;
}

float hebbian_table_get(const hebbian_table_t* table, const node_id_t* peer_id) {
  if (table == NULL || peer_id == NULL) return HEBBIAN_MIN_WEIGHT;
  for (size_t index = 0; index < table->count; index++) {
    if (node_id_equals(&table->entries[index].peer_id, peer_id)) {
      return table->entries[index].weight;
    }
  }
  return HEBBIAN_MIN_WEIGHT;
}

void hebbian_table_set(hebbian_table_t* table, const node_id_t* peer_id, float weight) {
  if (table == NULL || peer_id == NULL) return;
  hebbian_weight_t* entry = hebbian_find(table, peer_id);
  if (entry != NULL) {
    entry->weight = weight;
  } else {
    hebbian_insert(table, peer_id, weight);
  }
}

int hebbian_table_remove(hebbian_table_t* table, const node_id_t* peer_id) {
  if (table == NULL || peer_id == NULL) return -1;
  for (size_t index = 0; index < table->count; index++) {
    if (node_id_equals(&table->entries[index].peer_id, peer_id)) {
      // Compact
      for (size_t shift = index; shift < table->count - 1; shift++) {
        table->entries[shift] = table->entries[shift + 1];
      }
      table->count--;
      return 0;
    }
  }
  return -1;
}

// --- Hebbian learning rules ---

float hebbian_compute_delta(uint64_t latency_ms, float multiplier) {
  float tau = (float)latency_ms;
  float ratio = tau / (float)HEBBIAN_MAX_SEARCH_TIME_MS;
  float quality = 1.0f;  // validated on receipt
  float delta = HEBBIAN_GAMMA_0 * fmaxf(0.0f, 1.0f - ratio) * quality * multiplier;
  return delta;
}

// Frequency rule: w_{requester→holder} += delta_w
void hebbian_frequency(hebbian_table_t* table, const node_id_t* holder, float delta_w) {
  if (table == NULL || holder == NULL) return;
  hebbian_weight_t* entry = hebbian_find(table, holder);
  if (entry != NULL) {
    entry->weight += delta_w;
    if (entry->weight < HEBBIAN_MIN_WEIGHT) entry->weight = HEBBIAN_MIN_WEIGHT;
  } else {
    // Create new connection with initial strength = delta_w
    float initial = fmaxf(delta_w, HEBBIAN_INITIAL_WEIGHT);
    hebbian_insert(table, holder, initial);
  }
}

// Feedback rule: for i = 1 to path_len-2: w_{path[i]→path[i+1]} += eta_f × delta_w
void hebbian_feedback(hebbian_table_t* table, const node_id_t* path, uint8_t path_len,
                      float delta_w) {
  if (table == NULL || path == NULL || path_len < 3) return;

  float feedback_delta = HEBBIAN_ETA_FEEDBACK * delta_w;
  for (uint8_t index = 1; index < path_len - 1; index++) {
    hebbian_frequency(table, &path[index + 1], feedback_delta);
  }
}

// Symmetry rule: for i = 0 to path_len-2: w_{path[i+1]→path[i]} += eta_s × delta_w
void hebbian_symmetry(hebbian_table_t* table, const node_id_t* path, uint8_t path_len,
                      float delta_w) {
  if (table == NULL || path == NULL || path_len < 2) return;

  float symmetry_delta = HEBBIAN_ETA_SYMMETRY * delta_w;
  for (uint8_t index = 0; index < path_len - 1; index++) {
    hebbian_frequency(table, &path[index], symmetry_delta);
  }
}

// Decay: multiply all weights by HEBBIAN_DECAY_FACTOR
void hebbian_decay(hebbian_table_t* table) {
  if (table == NULL) return;
  for (size_t index = 0; index < table->count; index++) {
    table->entries[index].weight *= HEBBIAN_DECAY_FACTOR;
    if (table->entries[index].weight < HEBBIAN_MIN_WEIGHT) {
      table->entries[index].weight = HEBBIAN_MIN_WEIGHT;
    }
  }
}

// Apply all three rules for a successful RPC response along a path
void hebbian_apply_success(hebbian_table_t* table, const node_id_t* path, uint8_t path_len,
                           uint64_t latency_ms, float multiplier) {
  if (table == NULL || path == NULL || path_len == 0) return;

  float delta_w = hebbian_compute_delta(latency_ms, multiplier);

  // Frequency rule: strengthen direct connection to holder
  hebbian_frequency(table, &path[path_len - 1], delta_w);

  // Feedback rule: strengthen intermediate hops
  hebbian_feedback(table, path, path_len, delta_w);

  // Symmetry rule: strengthen reverse direction
  hebbian_symmetry(table, path, path_len, delta_w);
}