//
// Created by victor on 5/14/25.
//

#ifndef OFFS_LATENCY_CACHE_H
#define OFFS_LATENCY_CACHE_H

#include "net_node.h"
#include <stdint.h>
#include <stddef.h>

#define LATENCY_CACHE_DEFAULT_SIZE 1024
#define LATENCY_CACHE_TIMEOUT_MS 5000

typedef struct latency_entry_t {
  node_id_t id;
  uint32_t addr;
  uint16_t port;
  float latency_ms;
  uint64_t timestamp_ms;
} latency_entry_t;

typedef struct latency_cache_t {
  latency_entry_t* entries;
  size_t capacity;
  size_t count;
} latency_cache_t;

latency_cache_t* latency_cache_create(size_t capacity);
void latency_cache_destroy(latency_cache_t* cache);

int latency_cache_insert(latency_cache_t* cache, const node_id_t* id,
                         uint32_t addr, uint16_t port, float latency_ms,
                         uint64_t now_ms);
int latency_cache_get(const latency_cache_t* cache, const node_id_t* id,
                      float* latency_ms);
void latency_cache_evict_expired(latency_cache_t* cache, uint64_t now_ms);

#endif // OFFS_LATENCY_CACHE_H