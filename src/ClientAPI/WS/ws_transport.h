//
// Created by victor on 5/20/26.
//
#ifndef OFFS_WS_TRANSPORT_H
#define OFFS_WS_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../BlockCache/block_cache.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "ws_connection.h"
#include "../health_handler.h"

typedef vec_t(ws_connection_t*) vec_ws_connection_t;

typedef struct ws_transport_destroy_node_t {
  pd_watcher_t* watcher;
  struct ws_transport_destroy_node_t* next;
} ws_transport_destroy_node_t;

typedef struct ws_transport_t {
  actor_t actor;
  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  platform_socket_t* listen_sock;
  pd_watcher_t* listen_watcher;
  vec_ws_connection_t connections;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  size_t max_connections;
  ATOMIC(size_t) active_connections;
  platform_mutex_t* destroy_lock;
  ws_transport_destroy_node_t* destroy_head;
  char* host;
  uint16_t port;
  SSL_CTX* ssl_ctx;
  char* api_key_hash;
  health_context_t* health_ctx;
} ws_transport_t;

ws_transport_t* ws_transport_create(scheduler_pool_t* pool,
                                     block_cache_t* bc,
                                     ofd_cache_t* ofd_cache,
                                     tuple_cache_t* tc,
                                     const char* host,
                                     uint16_t port,
                                     const char* cert_path,
                                     const char* key_path,
                                     size_t max_connections,
                                     const char* api_key_hash,
                                     health_context_t* health_ctx);
void ws_transport_destroy(ws_transport_t* transport);
void ws_transport_start(ws_transport_t* transport);
void ws_transport_stop(ws_transport_t* transport);

#endif // OFFS_WS_TRANSPORT_H
