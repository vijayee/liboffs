//
// Created by victor on 5/14/25.
//

#include "latency_cache.h"
#include "../Util/allocator.h"
#include <string.h>

latency_cache_t* latency_cache_create(size_t capacity) {
  latency_cache_t* cache = get_clear_memory(sizeof(latency_cache_t));
  cache->capacity = capacity > 0 ? capacity : LATENCY_CACHE_DEFAULT_SIZE;
  cache->entries = get_clear_memory(sizeof(latency_entry_t) * cache->capacity);
  cache->count = 0;
  return cache;
}

void latency_cache_destroy(latency_cache_t* cache) {
  if (cache == NULL) return;
  free(cache->entries);
  free(cache);
}

int latency_cache_insert(latency_cache_t* cache, const node_id_t* id,
                         uint32_t addr, uint16_t port, float latency_ms,
                         uint64_t now_ms) {
  if (cache == NULL || id == NULL) return -1;

  // Check if entry exists — update in place
  for (size_t index = 0; index < cache->count; index++) {
    if (node_id_equals(&cache->entries[index].id, id)) {
      cache->entries[index].latency_ms = latency_ms;
      cache->entries[index].addr = addr;
      cache->entries[index].port = port;
      cache->entries[index].timestamp_ms = now_ms;
      return 0;
    }
  }

  // Not found — insert new entry
  if (cache->count >= cache->capacity) {
    // Cache full — evict oldest (first entry)
    memmove(&cache->entries[0], &cache->entries[1],
            sizeof(latency_entry_t) * (cache->count - 1));
    cache->count--;
  }

  latency_entry_t* entry = &cache->entries[cache->count];
  memcpy(&entry->id, id, sizeof(node_id_t));
  entry->addr = addr;
  entry->port = port;
  entry->latency_ms = latency_ms;
  // Stamp the entry with the current time so the eviction sweep computes a
  // real age instead of `now - 0` (which would purge every entry on the first
  // sweep). See audit #27.
  entry->timestamp_ms = now_ms;
  cache->count++;
  return 0;
}

int latency_cache_get(const latency_cache_t* cache, const node_id_t* id,
                      float* latency_ms) {
  if (cache == NULL || id == NULL || latency_ms == NULL) return -1;

  for (size_t index = 0; index < cache->count; index++) {
    if (node_id_equals(&cache->entries[index].id, id)) {
      *latency_ms = cache->entries[index].latency_ms;
      return 0;
    }
  }
  return -1;
}

void latency_cache_evict_expired(latency_cache_t* cache, uint64_t now_ms) {
  if (cache == NULL) return;

  size_t write_index = 0;
  for (size_t read_index = 0; read_index < cache->count; read_index++) {
    latency_entry_t* entry = &cache->entries[read_index];
    uint64_t age = now_ms - entry->timestamp_ms;
    if (age < LATENCY_CACHE_TIMEOUT_MS) {
      if (write_index != read_index) {
        cache->entries[write_index] = cache->entries[read_index];
      }
      write_index++;
    }
  }
  cache->count = write_index;
}