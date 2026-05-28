//
// Created by victor on 5/26/25.
//

#include "health_handler.h"
#include <cJSON.h>
#include <string.h>
#include <time.h>

/* Rate limit type names (RPC_TYPE_COUNT = 5) */
static const char* rate_limit_names[] = {
  "find_block",
  "store_block",
  "seeking_blocks",
  "ping_capacity",
  "ping"
};

/* RPC type names (PEER_RPC_TYPE_COUNT = 20, entry[i] maps to wire type i+1) */
static const char* rpc_names[] = {
  "ping",                    /* WIRE_PING=1 */
  "ping_response",           /* WIRE_PING_RESPONSE=2 */
  "ping_capacity",           /* WIRE_PING_CAPACITY=3 */
  "ping_capacity_response",  /* WIRE_PING_CAPACITY_RESPONSE=4 */
  "ping_block",              /* WIRE_PING_BLOCK=5 */
  "ping_block_response",     /* WIRE_PING_BLOCK_RESPONSE=6 */
  "find_block",              /* WIRE_FIND_BLOCK=7 */
  "find_block_response",     /* WIRE_FIND_BLOCK_RESPONSE=8 */
  "find_node",               /* WIRE_FIND_NODE=9 */
  "find_node_response",      /* WIRE_FIND_NODE_RESPONSE=10 */
  "store_block",             /* WIRE_STORE_BLOCK=11 */
  "store_block_response",    /* WIRE_STORE_BLOCK_RESPONSE=12 */
  "seeking_blocks",          /* WIRE_SEEKING_BLOCKS=13 */
  "seeking_blocks_response", /* WIRE_SEEKING_BLOCKS_RESPONSE=14 */
  "rank_block",              /* WIRE_RANK_BLOCK=15 */
  "recall_block",            /* WIRE_RECALL_BLOCK=16 */
  "recall_accept",           /* WIRE_RECALL_ACCEPT=17 */
  "recall_decline",          /* WIRE_RECALL_DECLINE=18 */
  "rate_limited",            /* WIRE_RATE_LIMITED=19 */
  "salutation"               /* WIRE_SALUTATION=20 */
};

health_data_t health_data_collect(const health_context_t* ctx) {
  health_data_t data;
  memset(&data, 0, sizeof(data));

  /* Compute uptime from start_time_ms if available */
  if (ctx != NULL && ctx->start_time_ms != NULL) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    uint64_t elapsed = now_ms - *ctx->start_time_ms;
    data.uptime_seconds = elapsed / 1000;
  }

  /* Determine status based on flags */
  if (ctx != NULL && ctx->draining != NULL && *ctx->draining) {
    data.status = "draining";
  } else if (ctx != NULL && ctx->running != NULL) {
    data.status = *ctx->running ? "running" : "stopped";
  } else {
    data.status = "unknown";
  }

  /* Copy node_id string if available */
  if (ctx != NULL && ctx->node_id != NULL) {
    strncpy(data.node_id_str, ctx->node_id->str, sizeof(data.node_id_str) - 1);
    data.node_id_str[sizeof(data.node_id_str) - 1] = '\0';
  }

  /* Copy topology metrics if available */
  if (ctx != NULL && ctx->topology_metrics != NULL) {
    topology_metrics_t* metrics = ctx->topology_metrics;
    data.peer_count = metrics->peer_snapshot_count;
    data.total_connections = metrics->total_connections;
    data.avg_hebbian_weight = metrics->avg_hebbian_weight;
    memcpy(data.rate_limit_accepted, metrics->total_rate_limit_accepted, sizeof(data.rate_limit_accepted));
    memcpy(data.rate_limit_rejected, metrics->total_rate_limit_rejected, sizeof(data.rate_limit_rejected));
    memcpy(data.avg_rate_limit_tokens, metrics->avg_rate_limit_tokens, sizeof(data.avg_rate_limit_tokens));
    memcpy(data.effective_rate, metrics->effective_rate, sizeof(data.effective_rate));
    memcpy(data.total_rpc_calls, metrics->total_rpc_calls, sizeof(data.total_rpc_calls));
  }

  /* Copy block cache stats if available */
  if (ctx != NULL && ctx->block_cache != NULL) {
    block_cache_t* cache = ctx->block_cache;
    data.block_cache_current_bytes = cache->current_bytes;
    data.block_cache_max_bytes = cache->max_capacity_bytes;
    data.block_cache_block_count = block_cache_count(cache);
  }

  return data;
}

cJSON* health_data_to_json(const health_data_t* data) {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddStringToObject(root, "status", data->status);
  cJSON_AddNumberToObject(root, "uptime_seconds", (double)data->uptime_seconds);

  if (data->node_id_str[0] != '\0') {
    cJSON_AddStringToObject(root, "node_id", data->node_id_str);
  }

  cJSON_AddNumberToObject(root, "peer_count", (double)data->peer_count);
  cJSON_AddNumberToObject(root, "total_connections", (double)data->total_connections);
  cJSON_AddNumberToObject(root, "avg_hebbian_weight", (double)data->avg_hebbian_weight);

  /* Block cache stats */
  cJSON* block_cache = cJSON_CreateObject();
  cJSON_AddNumberToObject(block_cache, "current_bytes", (double)data->block_cache_current_bytes);
  cJSON_AddNumberToObject(block_cache, "max_bytes", (double)data->block_cache_max_bytes);
  cJSON_AddNumberToObject(block_cache, "block_count", (double)data->block_cache_block_count);
  cJSON_AddItemToObject(root, "block_cache", block_cache);

  /* Rate limit stats */
  cJSON* rate_limits = cJSON_CreateArray();
  for (size_t i = 0; i < RPC_TYPE_COUNT; i++) {
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "type", rate_limit_names[i]);
    cJSON_AddNumberToObject(entry, "accepted", (double)data->rate_limit_accepted[i]);
    cJSON_AddNumberToObject(entry, "rejected", (double)data->rate_limit_rejected[i]);
    cJSON_AddNumberToObject(entry, "avg_tokens", (double)data->avg_rate_limit_tokens[i]);
    cJSON_AddNumberToObject(entry, "effective_rate", (double)data->effective_rate[i]);
    cJSON_AddItemToArray(rate_limits, entry);
  }
  cJSON_AddItemToObject(root, "rate_limits", rate_limits);

  /* RPC call stats (non-zero entries only) */
  cJSON* rpc_calls = cJSON_CreateArray();
  for (size_t i = 0; i < PEER_RPC_TYPE_COUNT; i++) {
    if (data->total_rpc_calls[i] > 0) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "name", rpc_names[i]);
      cJSON_AddNumberToObject(entry, "count", (double)data->total_rpc_calls[i]);
      cJSON_AddItemToArray(rpc_calls, entry);
    }
  }
  cJSON_AddItemToObject(root, "rpc_calls", rpc_calls);

  return root;
}
