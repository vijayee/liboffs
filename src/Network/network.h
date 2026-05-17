//
// Created by victor on 5/14/25.
//

#ifndef OFFS_NETWORK_H
#define OFFS_NETWORK_H

#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Timer/timer_actor.h"
#include "authority.h"
#include "gossip.h"
#include "ring_set.h"
#include "latency_cache.h"
#include "eabf.h"
#include "hebbian.h"
#include "hebbian_config.h"
#include "connection_manager.h"
#include "rate_limit.h"
#include "node_id.h"
#include "wanted_list.h"
#include "conn_state.h"
#include <stdint.h>
#include <stddef.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

typedef struct block_cache_t block_cache_t;
typedef struct relay_client_t relay_client_t;
typedef struct nat_detect_t nat_detect_t;
typedef struct quic_listener_t quic_listener_t;

// Pending QUIC connections awaiting salutation identity handshake
typedef struct pending_quic_t {
  void* quic_connection;           // HQUIC handle
  void* quic_stream;                // HQUIC persistent bidirectional stream handle
  struct sockaddr_storage peer_addr;
  struct pending_quic_t* next;
} pending_quic_t;

typedef struct network_t {
  actor_t actor;
  authority_t* authority;
  block_cache_t* block_cache;
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  gossip_handle_t gossip;
  ring_set_t* rings;
  latency_cache_t* latency_cache;
  eabf_table_t eabf_table;
  eabf_ttl_table_t eabf_ttl;
  wanted_list_t* wanted_list;
  hebbian_table_t hebbian;
  rate_limit_table_t rate_limits;
  topology_metrics_t* topology_metrics;
  connection_manager_t conn_mgr;
  uint64_t gossip_timer_id;
  uint64_t eabf_maintenance_timer_id;
  uint64_t hebbian_decay_timer_id;
  uint64_t metrics_push_timer_id;
  ATOMIC(uint8_t) running;

  relay_client_t* relay;          /* Connected relay client (or NULL) */
  nat_detect_t* nat_detect;       /* NAT detection module */
  nat_type_e local_nat_type;      /* Detected NAT type */

  pending_quic_t* pending_connections;  /* QUIC connections awaiting salutation */

  quic_listener_t* quic_listener;      /* QUIC listener for direct P2P connections */

#ifdef HAS_MSQUIC
  const struct QUIC_API_TABLE* msquic;
#endif
} network_t;

network_t* network_create(authority_t* authority, block_cache_t* block_cache,
                          timer_actor_t* timer, scheduler_pool_t* pool);
void network_destroy(network_t* network);
void network_dispatch(void* state, message_t* msg);
int network_connect_relay(network_t* network, const char* host, uint16_t port);
int network_connect_peer(network_t* network, const char* host, uint16_t port);

#endif // OFFS_NETWORK_H