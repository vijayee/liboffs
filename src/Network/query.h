//
// Created by victor on 5/14/25.
//

#ifndef OFFS_QUERY_H
#define OFFS_QUERY_H

#include "net_node.h"
#include <stdint.h>
#include <stdbool.h>

#define QUERY_MAX_TARGETS 256
#define QUERY_TABLE_DEFAULT_CAPACITY 64

// Query types corresponding to network operations
typedef enum query_type_e {
  QUERY_TYPE_GOSSIP,
  QUERY_TYPE_MEASURE,
  QUERY_TYPE_CLOSEST,
  QUERY_TYPE_CONSTRAINT
} query_type_e;

// Query lifecycle states
typedef enum query_status_e {
  QUERY_STATUS_INIT,
  QUERY_STATUS_WAITING,
  QUERY_STATUS_FINISHED,
  QUERY_STATUS_FAILED
} query_status_e;

// In-flight query tracking a multi-target network operation
typedef struct query_t {
  uint64_t query_id;
  query_type_e type;
  query_status_e status;
  uint64_t start_time_ms;
  uint32_t timeout_ms;
  net_node_t* source;
  net_node_t* targets[QUERY_MAX_TARGETS];
  uint32_t latencies_us[QUERY_MAX_TARGETS];
  size_t num_targets;
  size_t num_measured;
} query_t;

// Flat array table for tracking in-flight queries
// No locks needed — actor-serialized access
typedef struct query_table_t {
  query_t** entries;
  size_t capacity;
  size_t count;
} query_table_t;

// --- Query lifecycle ---

query_t* query_create(uint64_t query_id, query_type_e type, uint32_t timeout_ms);
void query_destroy(query_t* query);

int query_add_target(query_t* query, net_node_t* node);
int query_set_latency(query_t* query, size_t target_idx, uint32_t latency_us);
net_node_t* query_get_closest(query_t* query);

bool query_is_expired(const query_t* query, uint64_t now_ms);
bool query_is_finished(const query_t* query);
int query_finish(query_t* query);
int query_fail(query_t* query);

// --- Query table ---

void query_table_init(query_table_t* table, size_t capacity);
void query_table_deinit(query_table_t* table);

int query_table_insert(query_table_t* table, query_t* query);
query_t* query_table_lookup(const query_table_t* table, uint64_t query_id);
int query_table_remove(query_table_t* table, uint64_t query_id);
size_t query_table_expire(query_table_t* table, uint64_t now_ms);

#endif // OFFS_QUERY_H