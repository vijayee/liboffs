//
// Created by victor on 5/14/25.
//

#ifndef OFFS_EABF_H
#define OFFS_EABF_H

#include "../Bloom/attenuated_bloom_filter.h"
#include "node_id.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// EABF configuration constants
#define EABF_LEVELS 4
#define EABF_LEVEL_BITS 4096   // 512 bytes per level
#define EABF_HASH_COUNT 3
#define EABF_OMEGA 0.75f
#define EABF_FP_BITS 8

// TTL constants (in milliseconds)
#define EABF_BASE_TTL_MS 3600000   // 60 minutes
#define EABF_MAINTENANCE_MS 60000  // 60 second sweep

// Per-connection EABF wrapper
typedef struct eabf_t {
  node_id_t peer_id;                    // which peer this EABF is for
  attenuated_bloom_filter_t* filter;    // the multi-level ABF
  float weight;                         // Hebbian w_{self→peer}
} eabf_t;

// EABF table: per-connection collection
typedef struct eabf_entry_t {
  node_id_t peer_id;
  eabf_t* eabf;
} eabf_entry_t;

typedef struct eabf_table_t {
  eabf_entry_t* entries;
  size_t capacity;
  size_t count;
} eabf_table_t;

// TTL tracking entry — pairs a timer ID with the EABF location
typedef struct eabf_ttl_entry_t {
  uint64_t timer_id;
  node_id_t peer_id;
  uint32_t level;
  size_t bucket_index;
  uint32_t fingerprint;
  uint8_t hash[32];           // block hash that was inserted
} eabf_ttl_entry_t;

typedef struct eabf_ttl_table_t {
  eabf_ttl_entry_t* entries;
  size_t capacity;
  size_t count;
} eabf_ttl_table_t;

// --- EABF lifecycle ---

eabf_t* eabf_create(const node_id_t* peer_id);
void eabf_destroy(eabf_t* eabf);

bool eabf_subscribe(eabf_t* eabf, const uint8_t* topic, size_t topic_len);
bool eabf_unsubscribe(eabf_t* eabf, const uint8_t* topic, size_t topic_len);
bool eabf_check(const eabf_t* eabf, const uint8_t* topic, size_t topic_len, uint32_t* out_hops);

int eabf_merge(eabf_t* dest, const eabf_t* src);
elastic_bloom_filter_t* eabf_get_level(eabf_t* eabf, uint32_t level);

// --- EABF table ---

void eabf_table_init(eabf_table_t* table, size_t capacity);
void eabf_table_deinit(eabf_table_t* table);

eabf_t* eabf_table_lookup(const eabf_table_t* table, const node_id_t* peer_id);
eabf_t* eabf_table_insert(eabf_table_t* table, const node_id_t* peer_id);
int eabf_table_remove(eabf_table_t* table, const node_id_t* peer_id);

// --- TTL table ---

void eabf_ttl_table_init(eabf_ttl_table_t* table, size_t capacity);
void eabf_ttl_table_deinit(eabf_ttl_table_t* table);

uint64_t eabf_ttl_table_add(eabf_ttl_table_t* table, const node_id_t* peer_id,
                            uint32_t level, size_t bucket_index, uint32_t fingerprint,
                            const uint8_t* hash, uint64_t timer_id);
int eabf_ttl_table_remove_by_timer(eabf_ttl_table_t* table, uint64_t timer_id,
                                    eabf_ttl_entry_t* out_entry);

// Compute TTL for a given distance level
uint64_t eabf_ttl_for_level(uint32_t level);

#endif // OFFS_EABF_H