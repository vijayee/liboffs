//
// Created by victor on 5/26/25.
//

#include "health_handler.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* Rate limit type names (RPC_TYPE_COUNT = 5) */
static const char* rate_limit_names[] = {
  "find_block",
  "store_block",
  "seeking_blocks",
  "ping_capacity",
  "ping"
};

/* RPC type names (PEER_RPC_TYPE_COUNT = 20, index 0 unused) */
static const char* rpc_names[] = {
  NULL,
  "ping",
  "ping_response",
  "ping_capacity",
  "ping_capacity_response",
  "ping_block",
  "ping_block_response",
  "find_block",
  "find_block_response",
  "find_node",
  "find_node_response",
  "store_block",
  "store_block_response",
  "seeking_blocks",
  "seeking_blocks_response",
  "rank_block",
  "recall_block",
  "recall_accept",
  "recall_decline",
  "rate_limited",
  "salutation"
};

health_data_t health_data_collect(const health_context_t* ctx) {
  health_data_t data;
  memset(&data, 0, sizeof(data));

  /* Compute uptime from start_time_ms if available */
  if (ctx != NULL && ctx->start_time_ms != NULL) {
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_usec / 1000;
    uint64_t elapsed = now_ms - *ctx->start_time_ms;
    data.uptime_seconds = elapsed / 1000;
  }

  /* Determine status: draining > starting > ok */
  if (ctx != NULL && ctx->draining != NULL && *ctx->draining) {
    data.status = "draining";
  } else if (ctx != NULL && ctx->running != NULL && !*ctx->running) {
    data.status = "starting";
  } else {
    data.status = "ok";
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

size_t health_data_to_json(const health_data_t* data, char* buf, size_t buf_size) {
  size_t offset = 0;

  /* Macro to append formatted text, returns early if buffer exhausted */
#define APPEND(fmt, ...) do {                                 \
    if (offset >= buf_size) return 0;                         \
    int written = snprintf(buf + offset, buf_size - offset,   \
                           fmt, ##__VA_ARGS__);               \
    if (written < 0 || (size_t)written >= buf_size - offset) { \
      return 0;                                                \
    }                                                          \
    offset += (size_t)written;                                  \
  } while (0)

  APPEND("{\n");
  APPEND("  \"status\": \"%s\",\n", data->status);
  APPEND("  \"uptime_seconds\": %lu,\n", (unsigned long)data->uptime_seconds);

  if (data->node_id_str[0] != '\0') {
    APPEND("  \"node_id\": \"%s\",\n", data->node_id_str);
  }

  APPEND("  \"peer_count\": %zu,\n", data->peer_count);
  APPEND("  \"total_connections\": %zu,\n", data->total_connections);
  APPEND("  \"avg_hebbian_weight\": %.6f,\n", (double)data->avg_hebbian_weight);

  /* Block cache stats */
  APPEND("  \"block_cache\": {\n");
  APPEND("    \"current_bytes\": %zu,\n", data->block_cache_current_bytes);
  APPEND("    \"max_bytes\": %zu,\n", data->block_cache_max_bytes);
  APPEND("    \"block_count\": %zu\n", data->block_cache_block_count);
  APPEND("  },\n");

  /* Rate limit stats */
  APPEND("  \"rate_limits\": [\n");
  for (size_t i = 0; i < 5; i++) {
    const char* comma = (i < 4) ? "," : "";
    APPEND("    {\n");
    APPEND("      \"type\": \"%s\",\n", rate_limit_names[i]);
    APPEND("      \"accepted\": %lu,\n", (unsigned long)data->rate_limit_accepted[i]);
    APPEND("      \"rejected\": %lu,\n", (unsigned long)data->rate_limit_rejected[i]);
    APPEND("      \"avg_tokens\": %.4f,\n", (double)data->avg_rate_limit_tokens[i]);
    APPEND("      \"effective_rate\": %.4f\n", (double)data->effective_rate[i]);
    APPEND("    }%s\n", comma);
  }
  APPEND("  ],\n");

  /* RPC call stats (non-zero entries only) */
  APPEND("  \"rpc_calls\": [\n");
  int first_rpc = 1;
  for (size_t i = 1; i <= 20; i++) {
    if (data->total_rpc_calls[i] > 0) {
      if (!first_rpc) {
        APPEND(",\n");
      }
      APPEND("    {\n");
      APPEND("      \"name\": \"%s\",\n", rpc_names[i]);
      APPEND("      \"count\": %lu\n", (unsigned long)data->total_rpc_calls[i]);
      APPEND("    }");
      first_rpc = 0;
    }
  }
  if (first_rpc) {
    /* No non-zero entries — emit empty array on its own line */
    APPEND("\n");
  } else {
    APPEND("\n");
  }
  APPEND("  ]\n");

  APPEND("}\n");

#undef APPEND

  return offset;
}
