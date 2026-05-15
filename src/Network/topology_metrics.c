#include "topology_metrics.h"
#include "../Util/allocator.h"
#include <string.h>

topology_metrics_t* topology_metrics_create(scheduler_pool_t* pool) {
  topology_metrics_t* metrics = get_clear_memory(sizeof(topology_metrics_t));
  if (metrics == NULL) return NULL;

  metrics->peer_snapshot_capacity = 16;
  metrics->peer_snapshots = get_clear_memory(metrics->peer_snapshot_capacity * sizeof(peer_metrics_snapshot_t));
  metrics->peer_snapshot_count = 0;

  metrics->ring_entry_capacity = 64;
  metrics->ring_entries = get_clear_memory(metrics->ring_entry_capacity * sizeof(ring_topology_entry_t));
  metrics->ring_entry_count = 0;

  metrics->collect_interval_ms = 300000;  // 5 minutes
  metrics->total_connections = 0;
  metrics->avg_hebbian_weight = 0.0f;
  memset(metrics->total_rpc_calls, 0, sizeof(metrics->total_rpc_calls));

  if (pool != NULL) {
    actor_init(&metrics->actor, metrics, topology_metrics_dispatch, pool);
  }
  return metrics;
}

void topology_metrics_destroy(topology_metrics_t* metrics) {
  if (metrics == NULL) return;
  free(metrics->peer_snapshots);
  free(metrics->ring_entries);
  if (metrics->actor.pool != NULL) {
    actor_destroy(&metrics->actor);
  }
  free(metrics);
}

void topology_metrics_dispatch(void* state, message_t* msg) {
  if (state == NULL || msg == NULL) return;
  topology_metrics_t* metrics = (topology_metrics_t*)state;

  switch (msg->type) {
    case TOPOLOGY_METRICS_UPDATE: {
      // Peer/ring updates arrive via direct function calls
      // (topology_metrics_update_peers, topology_metrics_update_rings).
      // This message type is reserved for async push-based updates.
      break;
    }
    default:
      break;
  }

  if (msg->payload != NULL && msg->payload_destroy != NULL) {
    msg->payload_destroy(msg->payload);
  }
}

void topology_metrics_update_peers(topology_metrics_t* metrics,
                                    const peer_metrics_snapshot_t* snapshots,
                                    size_t count) {
  if (metrics == NULL || snapshots == NULL) return;
  if (count > metrics->peer_snapshot_capacity) {
    size_t new_capacity = count * 2;
    peer_metrics_snapshot_t* new_snapshots = get_clear_memory(new_capacity * sizeof(peer_metrics_snapshot_t));
    if (new_snapshots == NULL) return;
    free(metrics->peer_snapshots);
    metrics->peer_snapshots = new_snapshots;
    metrics->peer_snapshot_capacity = new_capacity;
  }
  memcpy(metrics->peer_snapshots, snapshots, count * sizeof(peer_metrics_snapshot_t));
  metrics->peer_snapshot_count = count;

  metrics->total_connections = 0;
  float total_weight = 0.0f;
  memset(metrics->total_rpc_calls, 0, sizeof(metrics->total_rpc_calls));
  for (size_t index = 0; index < count; index++) {
    if (snapshots[index].connected) {
      metrics->total_connections++;
    }
    total_weight += snapshots[index].hebbian_weight;
    for (size_t rpc = 0; rpc < PEER_RPC_TYPE_COUNT; rpc++) {
      metrics->total_rpc_calls[rpc] += snapshots[index].rpc_count[rpc];
    }
  }
  metrics->avg_hebbian_weight = count > 0 ? total_weight / (float)count : 0.0f;
}

void topology_metrics_update_rings(topology_metrics_t* metrics,
                                    const ring_topology_entry_t* entries,
                                    size_t count) {
  if (metrics == NULL || entries == NULL) return;
  if (count > metrics->ring_entry_capacity) {
    size_t new_capacity = count * 2;
    ring_topology_entry_t* new_entries = get_clear_memory(new_capacity * sizeof(ring_topology_entry_t));
    if (new_entries == NULL) return;
    free(metrics->ring_entries);
    metrics->ring_entries = new_entries;
    metrics->ring_entry_capacity = new_capacity;
  }
  memcpy(metrics->ring_entries, entries, count * sizeof(ring_topology_entry_t));
  metrics->ring_entry_count = count;
}