//
// Created by victor on 5/20/26.
//
#ifndef OFFS_TCP_TRANSPORT_H
#define OFFS_TCP_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "tcp_connection.h"

typedef vec_t(tcp_connection_t*) vec_tcp_connection_t;

typedef struct tcp_transport_destroy_node_t {
  pd_watcher_t* watcher;
  struct tcp_transport_destroy_node_t* next;
} tcp_transport_destroy_node_t;

typedef struct tcp_transport_t {
  actor_t actor;
  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  platform_socket_t* listen_sock;
  pd_watcher_t* listen_watcher;
  vec_tcp_connection_t connections;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  size_t max_connections;
  ATOMIC(size_t) active_connections;
  platform_mutex_t* destroy_lock;
  tcp_transport_destroy_node_t* destroy_head;
  char* host;
  uint16_t port;
  SSL_CTX* ssl_ctx;
  char* api_key_hash;
} tcp_transport_t;

tcp_transport_t* tcp_transport_create(scheduler_pool_t* pool,
                                       block_cache_t* bc,
                                       ofd_cache_t* ofd_cache,
                                       tuple_cache_t* tc,
                                       const char* host,
                                       uint16_t port,
                                       const char* cert_path,
                                       const char* key_path,
                                       const char* api_key_hash);
void tcp_transport_destroy(tcp_transport_t* transport);
void tcp_transport_start(tcp_transport_t* transport);
void tcp_transport_stop(tcp_transport_t* transport);
void tcp_transport_set_max_connections(tcp_transport_t* transport, size_t max_connections);

#endif // OFFS_TCP_TRANSPORT_H