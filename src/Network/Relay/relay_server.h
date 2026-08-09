//
// Created by victor on 5/16/26.
//

#ifndef OFFS_RELAY_SERVER_H
#define OFFS_RELAY_SERVER_H

#include "../../Actor/actor.h"
#include "../../Scheduler/scheduler.h"
#include "../../Util/atomic_compat.h"
#include "../../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

#define RELAY_MAX_CLIENTS 256

typedef struct relay_client_entry_t {
  uint32_t endpoint_id;
  void* connection;     /* HQUIC connection handle */
  void* stream;         /* HQUIC stream handle */
  struct stream_framer_t* framer;
  uint8_t active;
} relay_client_entry_t;

typedef struct relay_destroy_node_t {
  pd_watcher_t* watcher;
  struct relay_destroy_node_t* next;
} relay_destroy_node_t;

typedef struct relay_server_t {
  actor_t actor;
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
  uint16_t listen_port;
  char* cert_path;
  char* key_path;
  void* peer_verify;  // peer_verify_ctx_t* — NULL if no CA cert loaded
  /* When true, a CA certificate is required and client certs are validated.
   * Default false — no CA validation is performed (NO_CERTIFICATE_VALIDATION).
   * Set to true for production use with a configured CA. See audit #11. */
  bool allow_secure;
  relay_client_entry_t clients[RELAY_MAX_CLIENTS];
  size_t num_clients;
  uint32_t next_endpoint_id;
  scheduler_pool_t* pool;
  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  platform_mutex_t* clients_lock;  /* Protects clients array */
  platform_mutex_t* destroy_lock;  /* Destroy stack lock */
  relay_destroy_node_t* destroy_head;
} relay_server_t;

relay_server_t* relay_server_create(scheduler_pool_t* pool);
void relay_server_destroy(relay_server_t* server);
int relay_server_start(relay_server_t* server, const char* host, uint16_t port);
void relay_server_stop(relay_server_t* server);
void relay_server_dispatch(void* state, message_t* msg);

#endif // OFFS_RELAY_SERVER_H