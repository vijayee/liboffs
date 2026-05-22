//
// Created by victor on 5/14/25.
//

#include "eabf.h"
#include "../Util/allocator.h"
#include <string.h>

// --- EABF lifecycle ---

eabf_t* eabf_create(const node_id_t* peer_id) {
  if (peer_id == NULL) return NULL;
  eabf_t* eabf = get_clear_memory(sizeof(eabf_t));
  memcpy(&eabf->peer_id, peer_id, sizeof(node_id_t));
  eabf->filter = attenuated_bloom_filter_create(
      EABF_LEVELS, EABF_LEVEL_BITS, EABF_HASH_COUNT, EABF_OMEGA, EABF_FP_BITS);
  if (eabf->filter == NULL) {
    free(eabf);
    return NULL;
  }
  eabf->weight = 0.0f;
  return eabf;
}

void eabf_destroy(eabf_t* eabf) {
  if (eabf == NULL) return;
  attenuated_bloom_filter_destroy(eabf->filter);
  free(eabf);
}

bool eabf_subscribe(eabf_t* eabf, const uint8_t* topic, size_t topic_len) {
  if (eabf == NULL) return false;
  return attenuated_bloom_filter_subscribe(eabf->filter, topic, topic_len);
}

bool eabf_unsubscribe(eabf_t* eabf, const uint8_t* topic, size_t topic_len) {
  if (eabf == NULL) return false;
  return attenuated_bloom_filter_unsubscribe(eabf->filter, topic, topic_len);
}

bool eabf_check(const eabf_t* eabf, const uint8_t* topic, size_t topic_len, uint32_t* out_hops) {
  if (eabf == NULL) return false;
  return attenuated_bloom_filter_check(eabf->filter, topic, topic_len, out_hops);
}

int eabf_merge(eabf_t* dest, const eabf_t* src) {
  if (dest == NULL || src == NULL) return -1;
  return attenuated_bloom_filter_merge(dest->filter, src->filter);
}

elastic_bloom_filter_t* eabf_get_level(eabf_t* eabf, uint32_t level) {
  if (eabf == NULL) return NULL;
  return attenuated_bloom_filter_get_level(eabf->filter, level);
}

// --- EABF table ---

void eabf_table_init(eabf_table_t* table, size_t capacity,
                     uint64_t base_ttl_ms, uint64_t maintenance_ms) {
  if (capacity == 0) capacity = 16;
  table->entries = get_clear_memory(capacity * sizeof(eabf_entry_t));
  table->capacity = capacity;
  table->count = 0;
  table->base_ttl_ms = base_ttl_ms;
  table->maintenance_ms = maintenance_ms;
}

void eabf_table_deinit(eabf_table_t* table) {
  if (table == NULL) return;
  for (size_t index = 0; index < table->count; index++) {
    eabf_destroy(table->entries[index].eabf);
  }
  free(table->entries);
  table->entries = NULL;
  table->capacity = 0;
  table->count = 0;
}

eabf_t* eabf_table_lookup(const eabf_table_t* table, const node_id_t* peer_id) {
  if (table == NULL || peer_id == NULL) return NULL;
  for (size_t index = 0; index < table->count; index++) {
    if (node_id_equals(&table->entries[index].peer_id, peer_id)) {
      return table->entries[index].eabf;
    }
  }
  return NULL;
}

eabf_t* eabf_table_insert(eabf_table_t* table, const node_id_t* peer_id) {
  if (table == NULL || peer_id == NULL) return NULL;
  // Check if already exists
  eabf_t* existing = eabf_table_lookup(table, peer_id);
  if (existing != NULL) return existing;

  // Grow if needed
  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity * 2;
    eabf_entry_t* new_entries = get_clear_memory(new_capacity * sizeof(eabf_entry_t));
    if (new_entries == NULL) return NULL;
    memcpy(new_entries, table->entries, table->count * sizeof(eabf_entry_t));
    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
  }

  eabf_t* eabf = eabf_create(peer_id);
  if (eabf == NULL) return NULL;

  memcpy(&table->entries[table->count].peer_id, peer_id, sizeof(node_id_t));
  table->entries[table->count].eabf = eabf;
  table->count++;
  return eabf;
}

int eabf_table_remove(eabf_table_t* table, const node_id_t* peer_id) {
  if (table == NULL || peer_id == NULL) return -1;
  for (size_t index = 0; index < table->count; index++) {
    if (node_id_equals(&table->entries[index].peer_id, peer_id)) {
      eabf_destroy(table->entries[index].eabf);
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

// --- TTL table ---

void eabf_ttl_table_init(eabf_ttl_table_t* table, size_t capacity) {
  if (capacity == 0) capacity = 64;
  table->entries = get_clear_memory(capacity * sizeof(eabf_ttl_entry_t));
  table->capacity = capacity;
  table->count = 0;
}

void eabf_ttl_table_deinit(eabf_ttl_table_t* table) {
  if (table == NULL) return;
  free(table->entries);
  table->entries = NULL;
  table->capacity = 0;
  table->count = 0;
}

uint64_t eabf_ttl_table_add(eabf_ttl_table_t* table, const node_id_t* peer_id,
                            uint32_t level, size_t bucket_index, uint32_t fingerprint,
                            const uint8_t* hash, uint64_t timer_id) {
  if (table == NULL || peer_id == NULL) return 0;

  // Grow if needed
  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity * 2;
    eabf_ttl_entry_t* new_entries = get_clear_memory(new_capacity * sizeof(eabf_ttl_entry_t));
    if (new_entries == NULL) return 0;
    memcpy(new_entries, table->entries, table->count * sizeof(eabf_ttl_entry_t));
    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
  }

  eabf_ttl_entry_t* entry = &table->entries[table->count++];
  memcpy(&entry->peer_id, peer_id, sizeof(node_id_t));
  entry->level = level;
  entry->bucket_index = bucket_index;
  entry->fingerprint = fingerprint;
  if (hash != NULL) {
    memcpy(entry->hash, hash, 32);
  }
  entry->timer_id = timer_id;
  return timer_id;
}

int eabf_ttl_table_remove_by_timer(eabf_ttl_table_t* table, uint64_t timer_id,
                                    eabf_ttl_entry_t* out_entry) {
  if (table == NULL) return -1;
  for (size_t index = 0; index < table->count; index++) {
    if (table->entries[index].timer_id == timer_id) {
      if (out_entry != NULL) {
        *out_entry = table->entries[index];
      }
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

// Compute TTL for a given distance level
// Level 0: 60 min, Level 1: 40 min, Level 2: 30 min, Level 3: 24 min
uint64_t eabf_ttl_for_level(uint32_t level, uint64_t base_ttl_ms) {
  if (level >= EABF_LEVELS) return base_ttl_ms / 4;
  double divisor = 1.0 + (double)level * 0.5;
  return (uint64_t)((double)base_ttl_ms / divisor);
}