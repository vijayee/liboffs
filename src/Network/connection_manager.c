//
// Created by victor on 5/15/25.
//

#include "connection_manager.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdlib.h>

void connection_manager_init(connection_manager_t* mgr, size_t initial_capacity,
                             const hebbian_config_t* hebbian_config) {
  if (mgr == NULL) {
    return;
  }

  if (initial_capacity == 0) {
    initial_capacity = 4;
  }

  mgr->peers = get_clear_memory(initial_capacity * sizeof(peer_connection_t*));
  mgr->peer_count = 0;
  mgr->peer_capacity = initial_capacity;

  if (hebbian_config != NULL) {
    memcpy(&mgr->hebbian, hebbian_config, sizeof(hebbian_config_t));
  } else {
    hebbian_config_init(&mgr->hebbian);
  }

  mgr->max_connections = 128;
}

void connection_manager_deinit(connection_manager_t* mgr) {
  if (mgr == NULL) {
    return;
  }

  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_destroy(mgr->peers[index]);
  }

  free(mgr->peers);
  mgr->peers = NULL;
  mgr->peer_count = 0;
  mgr->peer_capacity = 0;
}

peer_connection_t* connection_manager_add(connection_manager_t* mgr,
                                           const node_id_t* remote_id,
                                           const struct sockaddr_storage* peer_addr,
                                           scheduler_pool_t* pool) {
  if (mgr == NULL || remote_id == NULL) {
    return NULL;
  }

  // Check if peer already exists
  peer_connection_t* existing = connection_manager_lookup(mgr, remote_id);
  if (existing != NULL) {
    return existing;
  }

  // Check max connections
  if (mgr->peer_count >= mgr->max_connections) {
    return NULL;
  }

  // Grow array if needed
  if (mgr->peer_count >= mgr->peer_capacity) {
    size_t new_capacity = mgr->peer_capacity * 2;
    peer_connection_t** new_peers = get_clear_memory(new_capacity * sizeof(peer_connection_t*));
    if (new_peers == NULL) {
      return NULL;
    }
    memcpy(new_peers, mgr->peers, mgr->peer_count * sizeof(peer_connection_t*));
    free(mgr->peers);
    mgr->peers = new_peers;
    mgr->peer_capacity = new_capacity;
  }

  peer_connection_t* peer = peer_connection_create(remote_id, peer_addr,
                                                    mgr->hebbian.initial_weight, pool);
  if (peer == NULL) {
    return NULL;
  }

  mgr->peers[mgr->peer_count] = peer;
  mgr->peer_count++;

  return peer;
}

int connection_manager_remove(connection_manager_t* mgr, const node_id_t* remote_id) {
  if (mgr == NULL || remote_id == NULL) {
    return -1;
  }

  for (size_t index = 0; index < mgr->peer_count; index++) {
    if (node_id_equals(&mgr->peers[index]->remote_node_id, remote_id)) {
      peer_connection_destroy(mgr->peers[index]);
      // Compact array by shifting remaining elements
      memmove(&mgr->peers[index], &mgr->peers[index + 1],
              (mgr->peer_count - index - 1) * sizeof(peer_connection_t*));
      mgr->peer_count--;
      return 0;
    }
  }

  return -1;
}

peer_connection_t* connection_manager_lookup(const connection_manager_t* mgr,
                                             const node_id_t* remote_id) {
  if (mgr == NULL || remote_id == NULL) {
    return NULL;
  }

  for (size_t index = 0; index < mgr->peer_count; index++) {
    if (node_id_equals(&mgr->peers[index]->remote_node_id, remote_id)) {
      return mgr->peers[index];
    }
  }

  return NULL;
}

peer_connection_t* connection_manager_lookup_by_quic(const connection_manager_t* mgr,
                                                      const void* quic_connection) {
  if (mgr == NULL || quic_connection == NULL) {
    return NULL;
  }

#ifdef HAS_MSQUIC
  for (size_t index = 0; index < mgr->peer_count; index++) {
    if (mgr->peers[index]->quic_connection == quic_connection) {
      return mgr->peers[index];
    }
  }
#else
  (void)mgr;
  (void)quic_connection;
#endif

  return NULL;
}

// Helper for qsort: compare peers by minimum hop level (ascending)
typedef struct peer_match_t {
  peer_connection_t* peer;
  uint32_t min_level;
} peer_match_t;

static int compare_peer_match(const void* left, const void* right) {
  const peer_match_t* match_left = (const peer_match_t*)left;
  const peer_match_t* match_right = (const peer_match_t*)right;
  if (match_left->min_level < match_right->min_level) {
    return -1;
  }
  if (match_left->min_level > match_right->min_level) {
    return 1;
  }
  return 0;
}

peer_connection_t** connection_manager_get_peers_for_topic(
    const connection_manager_t* mgr,
    const uint8_t* topic, size_t topic_len,
    size_t* out_count) {
  if (out_count == NULL) {
    return NULL;
  }
  *out_count = 0;

  if (mgr == NULL || topic == NULL || topic_len == 0) {
    return NULL;
  }

  // Collect matching peers with their minimum hop level
  peer_match_t* matches = get_clear_memory(mgr->peer_count * sizeof(peer_match_t));
  if (matches == NULL) {
    return NULL;
  }

  size_t match_count = 0;
  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_t* peer = mgr->peers[index];
    if (!peer->connected) {
      continue;
    }

    uint32_t hops = 0;
    if (peer_eabf_check(peer, topic, topic_len, &hops)) {
      matches[match_count].peer = peer;
      matches[match_count].min_level = hops;
      match_count++;
    }
  }

  if (match_count == 0) {
    free(matches);
    return NULL;
  }

  // Sort by minimum level ascending
  qsort(matches, match_count, sizeof(peer_match_t), compare_peer_match);

  // Build result array
  peer_connection_t** result = get_clear_memory(match_count * sizeof(peer_connection_t*));
  if (result == NULL) {
    free(matches);
    return NULL;
  }

  for (size_t index = 0; index < match_count; index++) {
    result[index] = matches[index].peer;
  }

  free(matches);
  *out_count = match_count;
  return result;
}

size_t connection_manager_decay_tick(connection_manager_t* mgr) {
  if (mgr == NULL) {
    return 0;
  }

  size_t removed_count = 0;
  size_t index = 0;

  while (index < mgr->peer_count) {
    peer_connection_t* peer = mgr->peers[index];
    peer_hebbian_decay(peer, mgr->hebbian.decay_rate);

    if (peer->hebbian_weight < mgr->hebbian.drop_threshold) {
      peer_connection_destroy(peer);
      memmove(&mgr->peers[index], &mgr->peers[index + 1],
              (mgr->peer_count - index - 1) * sizeof(peer_connection_t*));
      mgr->peer_count--;
      removed_count++;
    } else {
      index++;
    }
  }

  return removed_count;
}

size_t connection_manager_collect_metrics(const connection_manager_t* mgr,
                                          peer_metrics_snapshot_t* snapshots,
                                          size_t max_count) {
  if (mgr == NULL || snapshots == NULL) {
    return 0;
  }

  size_t count = mgr->peer_count < max_count ? mgr->peer_count : max_count;
  for (size_t index = 0; index < count; index++) {
    peer_get_metrics(mgr->peers[index], NULL, &snapshots[index]);
  }

  return count;
}

size_t connection_manager_count(const connection_manager_t* mgr) {
  if (mgr == NULL) return 0;
  return mgr->peer_count;
}