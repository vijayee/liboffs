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
#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include <poll-dancer/poll-dancer.h>
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

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
  void* stream;                   /* HQUIC persistent bidirectional stream handle */
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

// Payload for QUIC_LISTENER_SEND_SALUTATION messages
typedef struct quic_salutation_payload_t {
  void* stream;  // HQUIC stream handle to send salutation on
} quic_salutation_payload_t;

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
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;

  // Destroy stack for deferred watcher cleanup
  platform_mutex_t* destroy_lock;
  struct quic_destroy_node_t* destroy_head;

  // Active connection tracking for graceful shutdown
#ifdef HAS_MSQUIC
  HQUIC* connections;
  size_t connection_count;
  size_t connection_capacity;
  platform_mutex_t* conn_lock;
  void* peer_verify;  // peer_verify_ctx_t* — NULL if no CA cert loaded
#else
  void** connections;
  size_t connection_count;
  size_t connection_capacity;
  platform_mutex_t* conn_lock;
  void* peer_verify;
#endif
} quic_listener_t;

quic_listener_t* quic_listener_create(network_t* network, scheduler_pool_t* pool);
void quic_listener_destroy(quic_listener_t* listener);
int quic_listener_start(quic_listener_t* listener, const char* host, uint16_t port);
void quic_listener_stop(quic_listener_t* listener);
int quic_listener_connect(quic_listener_t* listener, const char* host, uint16_t port);

void quic_listener_dispatch(void* state, message_t* msg);

// Test-only accessors for the connection tracking array. Defined only in
// debug builds (NDEBUG unset) so they cannot be relied on in production.
// Used by test_quic_integration.cpp ConnTrackAddIsIdempotent to verify
// _conn_track_add is idempotent without spinning up a full msquic
// registration. See docs/liboffs-audit-report.md #4.
#ifndef NDEBUG
void quic_listener__conn_track_init_for_test(quic_listener_t* listener);
void quic_listener__conn_track_destroy_for_test(quic_listener_t* listener);
void quic_listener__conn_track_add_for_test(quic_listener_t* listener,
                                              HQUIC connection);
size_t quic_listener__conn_track_count_for_test(quic_listener_t* listener);
#endif

#endif // OFFS_QUIC_LISTENER_H