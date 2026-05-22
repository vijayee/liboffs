//
// Created by victor on 5/16/26.
//

#ifndef OFFS_RELAY_CLIENT_H
#define OFFS_RELAY_CLIENT_H

#include "../Actor/actor.h"
#include "../Network/network.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "../Platform/platform.h"
#include "../Network/stream_framer.h"
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

typedef struct relay_client_destroy_node_t {
  pd_watcher_t* watcher;
  struct relay_client_destroy_node_t* next;
} relay_client_destroy_node_t;

#define RELAY_CLIENT_MAX_RETRIES 5
#define RELAY_CLIENT_RETRY_DELAY_MS 500

typedef struct relay_client_t {
  actor_t actor;
  network_t* network;
  scheduler_pool_t* pool;

#ifdef HAS_MSQUIC
  const struct QUIC_API_TABLE* msquic;
  HQUIC registration;
  HQUIC configuration;
  HQUIC connection;
  HQUIC stream;
  uint8_t owns_registration;  // 1 if we created the registration, 0 if shared
#else
  void* msquic;
  void* registration;
  void* configuration;
  void* connection;
  void* stream;
  uint8_t owns_registration;
#endif

  stream_framer_t* framer;
  uint32_t local_endpoint_id;
  ATOMIC(uint8_t) connected;

  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;

  platform_mutex_t* destroy_lock;
  relay_client_destroy_node_t* destroy_head;

  struct sockaddr_storage relay_addr;
  char* cert_path;
  char* key_path;

  // Retry state: when the QUIC connection fails with UNREACHABLE,
  // we automatically retry up to RELAY_CLIENT_MAX_RETRIES times.
  char* relay_host;
  uint16_t relay_port;
  uint8_t retry_count;
  uint8_t shutdown_pending;  // 1 if we're intentionally shutting down
#ifdef HAS_MSQUIC
  HQUIC shared_registration;  // shared registration from quic_listener (may be NULL)
#else
  void* shared_registration;
#endif
} relay_client_t;

relay_client_t* relay_client_create(network_t* network, scheduler_pool_t* pool);
void relay_client_destroy(relay_client_t* client);
int relay_client_connect(relay_client_t* client, const char* host, uint16_t port,
#ifdef HAS_MSQUIC
                          HQUIC shared_registration
#else
                          void* shared_registration
#endif
                          );
void relay_client_disconnect(relay_client_t* client);
void relay_client_dispatch(void* state, message_t* msg);

// Message types for relay_client actor
#define RELAY_CLIENT_ADDR_REQUEST  1
#define RELAY_CLIENT_SEND          2
#define RELAY_CLIENT_RETRY         3

typedef struct relay_retry_payload_t {
  unsigned long delay_ms;
} relay_retry_payload_t;

#endif // OFFS_RELAY_CLIENT_H