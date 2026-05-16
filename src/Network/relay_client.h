//
// Created by victor on 5/16/26.
//

#ifndef OFFS_RELAY_CLIENT_H
#define OFFS_RELAY_CLIENT_H

#include "../Actor/actor.h"
#include "../Network/network.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "../Util/threadding.h"
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
#else
  void* msquic;
  void* registration;
  void* configuration;
  void* connection;
  void* stream;
#endif

  stream_framer_t* framer;
  uint32_t local_endpoint_id;
  ATOMIC(uint8_t) connected;

  pd_loop_t* loop;
  PLATFORMTHREADTYPE thread;
  ATOMIC(uint8_t) running;

  PLATFORMLOCKTYPE(destroy_lock);
  relay_client_destroy_node_t* destroy_head;

  struct sockaddr_storage relay_addr;
  char* cert_path;
  char* key_path;
} relay_client_t;

relay_client_t* relay_client_create(network_t* network, scheduler_pool_t* pool);
void relay_client_destroy(relay_client_t* client);
int relay_client_connect(relay_client_t* client, const char* host, uint16_t port);
void relay_client_disconnect(relay_client_t* client);
void relay_client_dispatch(void* state, message_t* msg);

#endif // OFFS_RELAY_CLIENT_H