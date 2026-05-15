//
// Created by victor on 5/14/25.
//

#include "query.h"
#include "../Util/allocator.h"
#include <string.h>

// --- Query lifecycle ---

query_t* query_create(uint64_t query_id, query_type_e type, uint32_t timeout_ms) {
  query_t* query = get_clear_memory(sizeof(query_t));
  query->query_id = query_id;
  query->type = type;
  query->status = QUERY_STATUS_INIT;
  query->timeout_ms = timeout_ms;
  // start_time_ms is set by the caller (network actor) using a monotonic clock
  query->source = NULL;
  query->num_targets = 0;
  query->num_measured = 0;
  return query;
}

void query_destroy(query_t* query) {
  if (query == NULL) return;
  // Source and targets are owned by the network actor's node pool — not freed here
  free(query);
}

int query_add_target(query_t* query, net_node_t* node) {
  if (query == NULL || node == NULL) return -1;
  if (query->num_targets >= QUERY_MAX_TARGETS) return -1;
  query->targets[query->num_targets++] = node;
  return 0;
}

int query_set_latency(query_t* query, size_t target_idx, uint32_t latency_us) {
  if (query == NULL) return -1;
  if (target_idx >= query->num_targets) return -1;
  query->latencies_us[target_idx] = latency_us;
  query->num_measured++;
  return 0;
}

net_node_t* query_get_closest(query_t* query) {
  if (query == NULL || query->num_targets == 0) return NULL;

  size_t closest_idx = 0;
  uint32_t closest_latency = query->latencies_us[0];

  for (size_t index = 1; index < query->num_targets; index++) {
    if (query->latencies_us[index] < closest_latency) {
      closest_latency = query->latencies_us[index];
      closest_idx = index;
    }
  }
  return query->targets[closest_idx];
}

bool query_is_expired(const query_t* query, uint64_t now_ms) {
  if (query == NULL) return true;
  return now_ms > (query->start_time_ms + query->timeout_ms);
}

bool query_is_finished(const query_t* query) {
  if (query == NULL) return true;
  return query->status == QUERY_STATUS_FINISHED ||
         query->status == QUERY_STATUS_FAILED;
}

int query_finish(query_t* query) {
  if (query == NULL) return -1;
  query->status = QUERY_STATUS_FINISHED;
  return 0;
}

int query_fail(query_t* query) {
  if (query == NULL) return -1;
  query->status = QUERY_STATUS_FAILED;
  return 0;
}

// --- Query table ---

void query_table_init(query_table_t* table, size_t capacity) {
  if (capacity == 0) capacity = QUERY_TABLE_DEFAULT_CAPACITY;
  table->entries = get_clear_memory(capacity * sizeof(query_t*));
  table->capacity = capacity;
  table->count = 0;
}

void query_table_deinit(query_table_t* table) {
  if (table == NULL) return;
  for (size_t index = 0; index < table->count; index++) {
    query_destroy(table->entries[index]);
  }
  free(table->entries);
  table->entries = NULL;
  table->capacity = 0;
  table->count = 0;
}

int query_table_insert(query_table_t* table, query_t* query) {
  if (table == NULL || query == NULL) return -1;

  // Grow if needed
  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity * 2;
    query_t** new_entries = get_clear_memory(new_capacity * sizeof(query_t*));
    if (new_entries == NULL) return -1;
    memcpy(new_entries, table->entries, table->count * sizeof(query_t*));
    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
  }

  table->entries[table->count++] = query;
  return 0;
}

query_t* query_table_lookup(const query_table_t* table, uint64_t query_id) {
  if (table == NULL) return NULL;
  for (size_t index = 0; index < table->count; index++) {
    if (table->entries[index]->query_id == query_id) {
      return table->entries[index];
    }
  }
  return NULL;
}

int query_table_remove(query_table_t* table, uint64_t query_id) {
  if (table == NULL) return -1;
  for (size_t index = 0; index < table->count; index++) {
    if (table->entries[index]->query_id == query_id) {
      query_destroy(table->entries[index]);
      // Compact: shift remaining entries down
      for (size_t shift = index; shift < table->count - 1; shift++) {
        table->entries[shift] = table->entries[shift + 1];
      }
      table->count--;
      return 0;
    }
  }
  return -1;
}

size_t query_table_expire(query_table_t* table, uint64_t now_ms) {
  if (table == NULL) return 0;
  size_t write_index = 0;
  size_t expired_count = 0;
  for (size_t read_index = 0; read_index < table->count; read_index++) {
    if (query_is_expired(table->entries[read_index], now_ms)) {
      query_destroy(table->entries[read_index]);
      expired_count++;
    } else {
      if (write_index != read_index) {
        table->entries[write_index] = table->entries[read_index];
      }
      write_index++;
    }
  }
  table->count = write_index;
  return expired_count;
}