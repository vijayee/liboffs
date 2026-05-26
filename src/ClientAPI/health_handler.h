//
// Created by victor on 5/26/25.
//

#ifndef OFFS_HEALTH_HANDLER_H
#define OFFS_HEALTH_HANDLER_H

#include <stdint.h>
#include <stddef.h>
#include "../Network/topology_metrics.h"
#include "../Network/node_id.h"
#include "../BlockCache/block_cache.h"

typedef struct health_context_t {
  topology_metrics_t* topology_metrics;
  block_cache_t*      block_cache;
  node_id_t*          node_id;
  uint64_t*           start_time_ms;
  uint8_t*            running;
  uint8_t*            draining;
} health_context_t;

typedef struct health_data_t {
  const char* status;
  uint64_t    uptime_seconds;
  char        node_id_str[64];
  size_t      peer_count;
  size_t      total_connections;
  float       avg_hebbian_weight;
  size_t      block_cache_current_bytes;
  size_t      block_cache_max_bytes;
  size_t      block_cache_block_count;
  uint64_t    rate_limit_accepted[5];
  uint64_t    rate_limit_rejected[5];
  float       avg_rate_limit_tokens[5];
  float       effective_rate[5];
  uint64_t    total_rpc_calls[20];
} health_data_t;

health_data_t health_data_collect(const health_context_t* ctx);
size_t health_data_to_json(const health_data_t* data, char* buf, size_t buf_size);

#endif
