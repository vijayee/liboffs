//
// Created by victor on 5/15/25.
//

#include "peer_connection.h"
#include "stream_framer.h"
#include "rate_limit.h"
#include "../Util/allocator.h"
#include <string.h>
#include <time.h>

peer_connection_t* peer_connection_create(const node_id_t* remote_id,
                                          const struct sockaddr_storage* peer_addr,
                                          float initial_weight,
                                          scheduler_pool_t* pool) {
  peer_connection_t* peer = get_clear_memory(sizeof(peer_connection_t));
  if (peer == NULL) {
    return NULL;
  }

  if (remote_id != NULL) {
    memcpy(&peer->remote_node_id, remote_id, sizeof(node_id_t));
  }

  if (peer_addr != NULL) {
    memcpy(&peer->peer_addr, peer_addr, sizeof(struct sockaddr_storage));
  }

  peer->hebbian_weight = initial_weight;
  peer->conn_state = CONN_STATE_RELAY;
  peer->direct_path.active = 0;
  peer->relay_path.active = 0;
  peer->peer_nat_type = NAT_TYPE_UNKNOWN;
  peer->direct_attempts = 0;
  peer->connected = true;
  peer->connected_at_ms = (int64_t)time(NULL) * 1000;
  peer->is_friend = false;

  peer->eabf = eabf_create(&peer->remote_node_id);
  if (peer->eabf == NULL) {
    free(peer);
    return NULL;
  }

  timing_wheel_init(&peer->eabf_wheel, 64, 60000);

#ifdef HAS_MSQUIC
  peer->quic_connection = NULL;
  peer->quic_stream = NULL;
  peer->framer = stream_framer_create();
  if (peer->framer == NULL) {
    eabf_destroy(peer->eabf);
    free(peer);
    return NULL;
  }
#endif

  if (pool != NULL) {
    actor_init(&peer->actor, peer, peer_connection_dispatch, pool);
  } else {
    peer->actor.state = peer;
    peer->actor.dispatch = peer_connection_dispatch;
  }

  return peer;
}

void peer_connection_destroy(peer_connection_t* peer) {
  if (peer == NULL) {
    return;
  }

#ifdef HAS_MSQUIC
  if (peer->framer != NULL) {
    stream_framer_destroy(peer->framer);
  }
  /* quic_connection and quic_stream handles are owned by the QUIC listener,
   * NOT by peer_connection — do NOT close/free them here. */
#endif

  if (peer->eabf != NULL) {
    eabf_destroy(peer->eabf);
  }

  timing_wheel_deinit(&peer->eabf_wheel);

  if (peer->actor.pool != NULL) {
    actor_destroy(&peer->actor);
  }

  free(peer);
}

void peer_connection_dispatch(void* state, message_t* msg) {
  peer_connection_t* peer = (peer_connection_t*)state;
  if (peer == NULL || msg == NULL) {
    return;
  }

  switch (msg->type) {
    case PEER_UPDATE_HEBBIAN: {
      if (msg->payload != NULL) {
        float delta = *(float*)msg->payload;
        peer_hebbian_update(peer, delta);
      }
      break;
    }
    case PEER_EABF_TICK: {
      peer_eabf_tick(peer);
      break;
    }
    case PEER_GET_METRICS: {
      if (msg->payload != NULL) {
        peer_metrics_snapshot_t* snapshot = (peer_metrics_snapshot_t*)msg->payload;
        peer_get_metrics(peer, NULL, snapshot);
      }
      break;
    }
    case PEER_CLOSE: {
      peer->connected = false;
      break;
    }
    case CONN_STATE_DIRECT_CONNECTED: {
      conn_state_on_direct_connected(peer);
      break;
    }
    case CONN_STATE_DIRECT_FAILED: {
      conn_state_on_direct_failed(peer);
      break;
    }
    case CONN_STATE_TRY_DIRECT: {
      conn_state_upgrade_to_direct(peer);
      break;
    }
    default:
      break;
  }

  if (msg->payload_destroy != NULL) {
    msg->payload_destroy(msg->payload);
  }
}

bool peer_eabf_subscribe(peer_connection_t* peer, const uint8_t* topic, size_t topic_len) {
  if (peer == NULL || peer->eabf == NULL || topic == NULL) {
    return false;
  }
  return eabf_subscribe(peer->eabf, topic, topic_len);
}

bool peer_eabf_check(const peer_connection_t* peer, const uint8_t* topic, size_t topic_len,
                     uint32_t* out_hops) {
  if (peer == NULL || peer->eabf == NULL || topic == NULL) {
    return false;
  }
  return eabf_check(peer->eabf, topic, topic_len, out_hops);
}

void peer_eabf_add_with_ttl(peer_connection_t* peer, const uint8_t* block_hash,
                             uint32_t level, size_t bucket_index, uint32_t fingerprint) {
  if (peer == NULL || block_hash == NULL) {
    return;
  }
  timing_wheel_add(&peer->eabf_wheel, &peer->remote_node_id,
                   level, bucket_index, fingerprint, block_hash);
}

void peer_eabf_tick(peer_connection_t* peer) {
  if (peer == NULL) {
    return;
  }

  size_t expired_count = 0;
  timing_wheel_entry_t* expired = timing_wheel_advance(&peer->eabf_wheel, 1, &expired_count);

  for (size_t index = 0; index < expired_count; index++) {
    elastic_bloom_filter_t* level_ebf = eabf_get_level(peer->eabf, expired[index].level);
    if (level_ebf != NULL) {
      elastic_bloom_filter_remove(level_ebf, expired[index].block_hash, 32);
    }
  }

  free(expired);
}

void peer_hebbian_update(peer_connection_t* peer, float delta) {
  if (peer == NULL) {
    return;
  }
  peer->hebbian_weight += delta;
  if (peer->hebbian_weight < 0.0f) {
    peer->hebbian_weight = 0.0f;
  }
}

void peer_hebbian_decay(peer_connection_t* peer, float decay_rate) {
  if (peer == NULL) {
    return;
  }
  peer->hebbian_weight -= decay_rate;
  if (peer->hebbian_weight < 0.0f) {
    peer->hebbian_weight = 0.0f;
  }
}

void peer_get_metrics(const peer_connection_t* peer,
                      const rate_limit_table_t* rate_limits,
                      peer_metrics_snapshot_t* snapshot) {
  if (peer == NULL || snapshot == NULL) {
    return;
  }
  memcpy(&snapshot->node_id, &peer->remote_node_id, sizeof(node_id_t));
  snapshot->hebbian_weight = peer->hebbian_weight;
  snapshot->rtt_ewma_ms = peer->rtt_ewma;
  memcpy(snapshot->rpc_count, peer->rpc_count, sizeof(peer->rpc_count));
  memcpy(snapshot->rpc_success, peer->rpc_success, sizeof(peer->rpc_success));
  memcpy(snapshot->rpc_failure, peer->rpc_failure, sizeof(peer->rpc_failure));
  snapshot->connected = peer->connected;
  snapshot->connected_at_ms = peer->connected_at_ms;

  // Populate rate limit snapshot
  const peer_rate_limits_t* limits = rate_limit_table_find(rate_limits, &peer->remote_node_id);
  if (limits != NULL) {
    for (int type = 0; type < RPC_TYPE_COUNT; type++) {
      snapshot->rate_limit_tokens[type] = limits->buckets[type].tokens;
      snapshot->rate_limit_accepted[type] = limits->buckets[type].total_accepted;
      snapshot->rate_limit_rejected[type] = limits->buckets[type].total_rejected;
    }
  } else {
    memset(snapshot->rate_limit_tokens, 0, sizeof(snapshot->rate_limit_tokens));
    memset(snapshot->rate_limit_accepted, 0, sizeof(snapshot->rate_limit_accepted));
    memset(snapshot->rate_limit_rejected, 0, sizeof(snapshot->rate_limit_rejected));
  }
  // Populate effective rates (inverse-scaled)
  if (rate_limits != NULL) {
    for (int type = 0; type < RPC_TYPE_COUNT; type++) {
      snapshot->rate_limit_effective_rate[type] = rate_limit_effective_rate(rate_limits, type);
    }
  } else {
    memset(snapshot->rate_limit_effective_rate, 0, sizeof(snapshot->rate_limit_effective_rate));
  }
}

void peer_update_rtt(peer_connection_t* peer, double rtt_ms) {
  if (peer == NULL) {
    return;
  }
  if (peer->rtt_ewma == 0.0) {
    peer->rtt_ewma = rtt_ms;
  } else {
    const double alpha = 0.1;
    peer->rtt_ewma = alpha * rtt_ms + (1.0 - alpha) * peer->rtt_ewma;
  }
  peer->last_rtt_ms = rtt_ms;
}