//
// Created by victor on 5/15/25.
//

#ifndef OFFS_CONNECTION_MANAGER_H
#define OFFS_CONNECTION_MANAGER_H

#include "peer_connection.h"
#include "hebbian_config.h"
#include "node_id.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/socket.h>

typedef struct connection_manager_t {
  peer_connection_t** peers;
  size_t peer_count;
  size_t peer_capacity;
  hebbian_config_t hebbian;
  size_t max_connections;
} connection_manager_t;

void connection_manager_init(connection_manager_t* mgr, size_t initial_capacity,
                             const hebbian_config_t* hebbian_config);
void connection_manager_deinit(connection_manager_t* mgr);

peer_connection_t* connection_manager_add(connection_manager_t* mgr,
                                          const node_id_t* remote_id,
                                          const struct sockaddr_storage* peer_addr,
                                          scheduler_pool_t* pool);
int connection_manager_remove(connection_manager_t* mgr, const node_id_t* remote_id);
peer_connection_t* connection_manager_lookup(const connection_manager_t* mgr,
                                             const node_id_t* remote_id);

// Gravity well search: find peers whose EABF matches topic at lowest level
// Returns array of peer_connection_t* sorted by gravity well strength (lowest level first)
// Caller must free the returned array with free()
peer_connection_t** connection_manager_get_peers_for_topic(
    const connection_manager_t* mgr,
    const uint8_t* topic, size_t topic_len,
    size_t* out_count);

// Apply Hebbian decay to all peers, remove those below drop_threshold
// Returns number of peers removed
size_t connection_manager_decay_tick(connection_manager_t* mgr);

// Collect metrics from all peers into snapshot array
// Returns number of peers written (min of peer_count and max_count)
size_t connection_manager_collect_metrics(const connection_manager_t* mgr,
                                          peer_metrics_snapshot_t* snapshots,
                                          size_t max_count);

#endif // OFFS_CONNECTION_MANAGER_H