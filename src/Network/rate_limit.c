//
// Created by victor on 5/14/25.
//

#include "rate_limit.h"
#include "../Util/allocator.h"
#include <string.h>

// Default rate configurations from Network Design spec
const token_bucket_config_t RATE_LIMIT_DEFAULTS[RPC_TYPE_COUNT] = {
  { 5.0f,   50.0f,  20.0f, 1.0f },   // FIND_BLOCK
  { 0.5f,   5.0f,    3.0f, 1.0f },   // STORE_BLOCK
  { 1.0f,  10.0f,    5.0f, 1.0f },   // SEEKING_BLOCKS
  { 10.0f, 10.0f,   10.0f, 0.1f },   // PING_CAPACITY
  { 10.0f, 10.0f,   10.0f, 0.1f },   // PING
};

// --- Table lifecycle ---

void rate_limit_table_init(rate_limit_table_t* table, size_t capacity) {
  if (capacity == 0) capacity = 16;
  table->entries = get_clear_memory(capacity * sizeof(peer_rate_limits_t));
  table->capacity = capacity;
  table->count = 0;
}

void rate_limit_table_deinit(rate_limit_table_t* table) {
  if (table == NULL) return;
  free(table->entries);
  table->entries = NULL;
  table->capacity = 0;
  table->count = 0;
}

// --- Lookup and mutation ---

static peer_rate_limits_t* rate_limit_find(rate_limit_table_t* table, const node_id_t* peer_id) {
  for (size_t index = 0; index < table->count; index++) {
    if (node_id_equals(&table->entries[index].peer_id, peer_id)) {
      return &table->entries[index];
    }
  }
  return NULL;
}

peer_rate_limits_t* rate_limit_table_get(rate_limit_table_t* table, const node_id_t* peer_id) {
  if (table == NULL || peer_id == NULL) return NULL;

  peer_rate_limits_t* entry = rate_limit_find(table, peer_id);
  if (entry != NULL) return entry;

  // Grow if needed
  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity * 2;
    peer_rate_limits_t* new_entries = get_clear_memory(new_capacity * sizeof(peer_rate_limits_t));
    if (new_entries == NULL) return NULL;
    memcpy(new_entries, table->entries, table->count * sizeof(peer_rate_limits_t));
    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
  }

  entry = &table->entries[table->count++];
  memcpy(&entry->peer_id, peer_id, sizeof(node_id_t));
  entry->weight = 0.0f;

  // Initialize all token buckets with default burst size
  for (int type = 0; type < RPC_TYPE_COUNT; type++) {
    entry->buckets[type].tokens = RATE_LIMIT_DEFAULTS[type].burst_size;
    entry->buckets[type].last_refill = 0;
    entry->buckets[type].total_accepted = 0;
    entry->buckets[type].total_rejected = 0;
  }

  return entry;
}

int rate_limit_table_remove(rate_limit_table_t* table, const node_id_t* peer_id) {
  if (table == NULL || peer_id == NULL) return -1;
  for (size_t index = 0; index < table->count; index++) {
    if (node_id_equals(&table->entries[index].peer_id, peer_id)) {
      for (size_t shift = index; shift < table->count - 1; shift++) {
        table->entries[shift] = table->entries[shift + 1];
      }
      table->count--;
      return 0;
    }
  }
  return -1;
}

// --- Token bucket refill ---

void token_bucket_refill(token_bucket_t* bucket, const token_bucket_config_t* config,
                         uint64_t now_ms) {
  if (bucket->last_refill == 0) {
    bucket->last_refill = now_ms;
    return;
  }

  uint64_t elapsed_ms = now_ms - bucket->last_refill;
  if (elapsed_ms == 0) return;

  float elapsed_s = (float)elapsed_ms / 1000.0f;
  float new_tokens = config->max_rate * elapsed_s;
  bucket->tokens += new_tokens;

  if (bucket->tokens > config->burst_size) {
    bucket->tokens = config->burst_size;
  }

  bucket->last_refill = now_ms;
}

// --- Rate limit check ---

bool rate_limit_check(rate_limit_table_t* table, const node_id_t* peer_id,
                      rpc_type_e type, uint64_t now_ms) {
  if (table == NULL || peer_id == NULL) return false;
  if (type < 0 || type >= RPC_TYPE_COUNT) return false;

  peer_rate_limits_t* entry = rate_limit_table_get(table, peer_id);
  if (entry == NULL) return false;

  token_bucket_t* bucket = &entry->buckets[type];
  const token_bucket_config_t* config = &RATE_LIMIT_DEFAULTS[type];

  // Refill tokens based on elapsed time
  token_bucket_refill(bucket, config, now_ms);

  // Check if we have enough tokens
  if (bucket->tokens >= config->cost) {
    bucket->tokens -= config->cost;
    bucket->total_accepted++;
    return true;
  }

  bucket->total_rejected++;
  return false;
}

// --- Retry after ---

uint32_t rate_limit_retry_after(rate_limit_table_t* table, const node_id_t* peer_id,
                                rpc_type_e type, uint64_t now_ms) {
  if (table == NULL || peer_id == NULL) return 0;
  if (type < 0 || type >= RPC_TYPE_COUNT) return 0;
  (void)now_ms;  // Used for refill in check, not needed for retry calc

  peer_rate_limits_t* entry = rate_limit_find(table, peer_id);
  if (entry == NULL) return 0;

  token_bucket_t* bucket = &entry->buckets[type];
  const token_bucket_config_t* config = &RATE_LIMIT_DEFAULTS[type];

  // How long until we have enough tokens?
  float deficit = config->cost - bucket->tokens;
  if (deficit <= 0.0f) return 0;

  float seconds_needed = deficit / config->max_rate;
  uint32_t ms_needed = (uint32_t)(seconds_needed * 1000.0f);
  return ms_needed;
}

// --- Capacity multiplier ---

float rate_limit_capacity_multiplier(float capacity, rpc_type_e type) {
  if (type == RPC_TYPE_STORE_BLOCK) {
    if (capacity >= 0.80f) return 0.05f;
    if (capacity >= 0.50f) {
      // Linear taper from 1.0 at 0.50 to 0.05 at 0.80
      float t = (capacity - 0.50f) / 0.30f;
      return 1.0f - t * 0.95f;
    }
  }
  return 1.0f;
}