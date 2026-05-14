//
// Created by victor on 5/14/25.
//

#ifndef OFFS_RATE_LIMIT_H
#define OFFS_RATE_LIMIT_H

#include "node_id.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// RPC type enumeration for per-type rate limiting
typedef enum rpc_type_e {
  RPC_TYPE_FIND_BLOCK = 0,
  RPC_TYPE_STORE_BLOCK = 1,
  RPC_TYPE_SEEKING_BLOCKS = 2,
  RPC_TYPE_PING_CAPACITY = 3,
  RPC_TYPE_PING = 4,
  RPC_TYPE_COUNT = 5
} rpc_type_e;

// Token bucket configuration per RPC type
typedef struct token_bucket_config_t {
  float base_rate;    // tokens per second (sustained)
  float max_rate;     // tokens per second (burst ceiling)
  float burst_size;   // maximum tokens that can accumulate
  float cost;        // cost per request (1.0 for most, 0.1 for ping)
} token_bucket_config_t;

// Default rate configurations from Network Design spec
extern const token_bucket_config_t RATE_LIMIT_DEFAULTS[RPC_TYPE_COUNT];

// Token bucket state
typedef struct token_bucket_t {
  float tokens;           // current token count
  uint64_t last_refill;   // last refill time in ms
  uint64_t total_accepted;
  uint64_t total_rejected;
} token_bucket_t;

// Per-peer rate limits
typedef struct peer_rate_limits_t {
  node_id_t peer_id;
  float weight;            // Hebbian weight for this peer
  token_bucket_t buckets[RPC_TYPE_COUNT];
} peer_rate_limits_t;

// Rate limit table — flat array, no locks (actor model)
typedef struct rate_limit_table_t {
  peer_rate_limits_t* entries;
  size_t capacity;
  size_t count;
} rate_limit_table_t;

// Initialize/deinitialize rate limit table
void rate_limit_table_init(rate_limit_table_t* table, size_t capacity);
void rate_limit_table_deinit(rate_limit_table_t* table);

// Look up or create per-peer rate limits
peer_rate_limits_t* rate_limit_table_get(rate_limit_table_t* table, const node_id_t* peer_id);

// Remove a peer's rate limits
int rate_limit_table_remove(rate_limit_table_t* table, const node_id_t* peer_id);

// Refill tokens based on elapsed time
void token_bucket_refill(token_bucket_t* bucket, const token_bucket_config_t* config,
                         uint64_t now_ms);

// Check if a request is allowed (consumes tokens if allowed)
// Returns true if allowed, false if rate limited
bool rate_limit_check(rate_limit_table_t* table, const node_id_t* peer_id,
                      rpc_type_e type, uint64_t now_ms);

// Get retry_after_ms for a rate-limited request
uint32_t rate_limit_retry_after(rate_limit_table_t* table, const node_id_t* peer_id,
                                rpc_type_e type, uint64_t now_ms);

// Apply capacity multiplier to rate limits
// If type is StoreBlock and capacity >= 0.80 → multiplier = 0.05
// If 0.50-0.80 → linear taper
// Otherwise 1.0
float rate_limit_capacity_multiplier(float capacity, rpc_type_e type);

#endif // OFFS_RATE_LIMIT_H