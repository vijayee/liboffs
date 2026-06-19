//
// Created by victor on 5/20/26.
//
#ifndef OFFS_UNIX_TRANSPORT_H
#define OFFS_UNIX_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "unix_connection.h"
#include "../health_handler.h"
#include "../update_status_handler.h"

typedef vec_t(unix_connection_t*) vec_unix_connection_t;

typedef struct unix_transport_destroy_node_t {
  pd_watcher_t* watcher;
  struct unix_transport_destroy_node_t* next;
} unix_transport_destroy_node_t;

typedef struct unix_transport_t {
  actor_t actor;
  pd_loop_t* loop;
  platform_thread_t* thread;          /* accept thread (pipe) or run thread (AF_UNIX) */
  platform_thread_t* loop_thread;     /* loop thread (pipe only); NULL on AF_UNIX */
  ATOMIC(uint8_t) running;
  ATOMIC(uint8_t) loop_running;
  platform_socket_t* listen_sock;
  pd_watcher_t* listen_watcher;
  vec_unix_connection_t connections;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  size_t max_connections;
  ATOMIC(size_t) active_connections;
  platform_mutex_t* destroy_lock;
  unix_transport_destroy_node_t* destroy_head;
  char* api_key_hash;
  char* socket_path;
  health_context_t* health_ctx;
  update_status_context_t* update_status_ctx;
} unix_transport_t;

unix_transport_t* unix_transport_create(scheduler_pool_t* pool,
                                         block_cache_t* bc,
                                         ofd_cache_t* ofd_cache,
                                         tuple_cache_t* tc,
                                         const char* socket_path,
                                         const char* api_key_hash,
                                         health_context_t* health_ctx);
void unix_transport_destroy(unix_transport_t* transport);
void unix_transport_start(unix_transport_t* transport);
void unix_transport_stop(unix_transport_t* transport);
void unix_transport_set_max_connections(unix_transport_t* transport, size_t max_connections);
void unix_transport_set_update_status_ctx(unix_transport_t* transport,
                                           update_status_context_t* ctx);

#endif // OFFS_UNIX_TRANSPORT_H