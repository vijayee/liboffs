//
// Created by victor on 5/14/25.
//

#ifndef OFFS_QUIC_LISTENER_H
#define OFFS_QUIC_LISTENER_H

#include "../Actor/actor.h"
#include "../Network/network.h"
#include "../Network/node_id.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "../Util/threadding.h"
#include "../Util/allocator.h"
#include <poll-dancer/poll-dancer.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

typedef struct quic_connection_t quic_connection_t;

// Payload for NETWORK_QUIC_DATA messages
typedef struct quic_data_payload_t {
  uint8_t* data;
  size_t length;
  struct sockaddr_storage peer_addr;  // Peer address for routing responses
  void* quic_connection;              // HQUIC connection handle for peer lookup
} quic_data_payload_t;

void quic_data_payload_destroy(quic_data_payload_t* payload);

// Payload for NETWORK_QUIC_CONNECTED messages
typedef struct quic_connected_payload_t {
  void* connection;               /* HQUIC connection handle */
  struct sockaddr_storage peer_addr;
} quic_connected_payload_t;

void quic_connected_payload_destroy(quic_connected_payload_t* payload);

// Payload for QUIC_LISTENER_SEND messages
typedef struct quic_send_payload_t {
  node_id_t dest;                  /* Destination node ID */
  uint8_t* data;                   /* Framed CBOR data to send */
  size_t length;                   /* Length of data */
} quic_send_payload_t;

void quic_send_payload_destroy(quic_send_payload_t* payload);

typedef struct quic_listener_t {
  actor_t actor;
  network_t* network;
  scheduler_pool_t* pool;

  // QUIC state (only used when HAS_MSQUIC is defined)
#ifdef HAS_MSQUIC
  const struct QUIC_API_TABLE* msquic;
  HQUIC registration;
  HQUIC configuration;
  HQUIC listener;
#else
  void* msquic;
  void* registration;
  void* configuration;
  void* listener_handle;
#endif

  // I/O thread for poll-dancer timers
  pd_loop_t* loop;
  PLATFORMTHREADTYPE thread;
  ATOMIC(uint8_t) running;

  // Destroy stack for deferred watcher cleanup
  PLATFORMLOCKTYPE(destroy_lock);
  struct quic_destroy_node_t* destroy_head;
} quic_listener_t;

quic_listener_t* quic_listener_create(network_t* network, scheduler_pool_t* pool);
void quic_listener_destroy(quic_listener_t* listener);
int quic_listener_start(quic_listener_t* listener, const char* host, uint16_t port);
void quic_listener_stop(quic_listener_t* listener);

void quic_listener_dispatch(void* state, message_t* msg);

#endif // OFFS_QUIC_LISTENER_H