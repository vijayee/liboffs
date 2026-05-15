//
// Created by victor on 5/15/25.
//

#ifndef OFFS_TOPOLOGY_METRICS_H
#define OFFS_TOPOLOGY_METRICS_H

#include "../Actor/actor.h"
#include "peer_connection.h"
#include "node_id.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct ring_topology_entry_t {
  node_id_t node_id;
  uint32_t ring_level;
  double rtt_ms;
  float capacity;
  bool is_active_connection;
} ring_topology_entry_t;

typedef struct topology_metrics_t {
  actor_t actor;

  peer_metrics_snapshot_t* peer_snapshots;
  size_t peer_snapshot_count;
  size_t peer_snapshot_capacity;

  ring_topology_entry_t* ring_entries;
  size_t ring_entry_count;
  size_t ring_entry_capacity;

  size_t total_connections;
  float avg_hebbian_weight;
  uint64_t total_rpc_calls[PEER_RPC_TYPE_COUNT];

  uint64_t collect_interval_ms;
} topology_metrics_t;

topology_metrics_t* topology_metrics_create(scheduler_pool_t* pool);
void topology_metrics_destroy(topology_metrics_t* metrics);
void topology_metrics_dispatch(void* state, message_t* msg);

void topology_metrics_update_peers(topology_metrics_t* metrics,
                                    const peer_metrics_snapshot_t* snapshots,
                                    size_t count);
void topology_metrics_update_rings(topology_metrics_t* metrics,
                                    const ring_topology_entry_t* entries,
                                    size_t count);

#endif