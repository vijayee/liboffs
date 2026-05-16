//
// Created by victor on 5/15/25.
//

#ifndef OFFS_PEER_CONNECTION_H
#define OFFS_PEER_CONNECTION_H

#include "../Actor/actor.h"
#include "eabf.h"
#include "hebbian_config.h"
#include "node_id.h"
#include "timing_wheel.h"
#include "conn_state.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/socket.h>

#define PEER_RPC_TYPE_COUNT 20

typedef struct peer_metrics_snapshot_t {
  node_id_t node_id;
  float hebbian_weight;
  double rtt_ewma_ms;
  uint64_t rpc_count[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_success[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_failure[PEER_RPC_TYPE_COUNT];
  bool connected;
  int64_t connected_at_ms;
} peer_metrics_snapshot_t;

typedef struct peer_connection_t {
  actor_t actor;
  node_id_t remote_node_id;
  struct sockaddr_storage peer_addr;

#ifdef HAS_MSQUIC
  void* quic_connection;           /* HQUIC connection handle (opaque to avoid msquic header dependency) */
  void* quic_stream;               /* HQUIC stream handle */
  struct stream_framer_t* framer;  /* Accumulator for inbound framed messages */
#endif

  eabf_t* eabf;
  timing_wheel_t eabf_wheel;

  float hebbian_weight;

  /* Connection state machine for direct/relay path selection */
  conn_state_e conn_state;
  conn_path_t direct_path;
  conn_path_t relay_path;
  nat_type_e peer_nat_type;
  uint32_t direct_attempts;

  uint64_t rpc_count[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_success[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_failure[PEER_RPC_TYPE_COUNT];

  double last_rtt_ms;
  double rtt_ewma;

  bool connected;
  int64_t connected_at_ms;
} peer_connection_t;

peer_connection_t* peer_connection_create(const node_id_t* remote_id,
                                          const struct sockaddr_storage* peer_addr,
                                          float initial_weight,
                                          scheduler_pool_t* pool);
void peer_connection_destroy(peer_connection_t* peer);
void peer_connection_dispatch(void* state, message_t* msg);

bool peer_eabf_subscribe(peer_connection_t* peer, const uint8_t* topic, size_t topic_len);
bool peer_eabf_check(const peer_connection_t* peer, const uint8_t* topic, size_t topic_len,
                     uint32_t* out_hops);
void peer_eabf_add_with_ttl(peer_connection_t* peer, const uint8_t* block_hash,
                             uint32_t level, size_t bucket_index, uint32_t fingerprint);
void peer_eabf_tick(peer_connection_t* peer);

void peer_hebbian_update(peer_connection_t* peer, float delta);
void peer_hebbian_decay(peer_connection_t* peer, float decay_rate);

void peer_get_metrics(const peer_connection_t* peer, peer_metrics_snapshot_t* snapshot);
void peer_update_rtt(peer_connection_t* peer, double rtt_ms);

#endif // OFFS_PEER_CONNECTION_H