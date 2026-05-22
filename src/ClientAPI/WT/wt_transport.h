//
// Created by victor on 5/20/26.
//
#ifndef OFFS_WT_TRANSPORT_H
#define OFFS_WT_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "../../Platform/platform.h"

#ifdef HAS_MSQUIC
#include <msquic.h>
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../BlockCache/block_cache.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../Network/msquic_singleton.h"
#include <poll-dancer/poll-dancer.h>
#include "wt_connection.h"

typedef vec_t(wt_connection_t*) vec_wt_connection_t;

typedef struct wt_transport_destroy_node_t {
  void* item;
  struct wt_transport_destroy_node_t* next;
} wt_transport_destroy_node_t;

typedef struct wt_transport_t {
  actor_t actor;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  ATOMIC(uint8_t) running;
  ATOMIC(uint8_t) listening;
  platform_thread_t* thread;
  pd_loop_t* loop;
  platform_mutex_t* destroy_lock;
  wt_transport_destroy_node_t* destroy_head;

  const struct QUIC_API_TABLE* msquic;
  HQUIC registration;
  HQUIC configuration;
  HQUIC listener;
  char* host;
  uint16_t port;
  char* cert_path;
  char* key_path;
  size_t max_connections;
  ATOMIC(size_t) active_connections;

  platform_mutex_t* conn_lock;
  vec_wt_connection_t connections;
} wt_transport_t;

wt_transport_t* wt_transport_create(scheduler_pool_t* pool,
                                      block_cache_t* bc,
                                      ofd_cache_t* ofd_cache,
                                      tuple_cache_t* tc,
                                      const char* host,
                                      uint16_t port,
                                      const char* cert_path,
                                      const char* key_path,
                                      size_t max_connections);
void wt_transport_destroy(wt_transport_t* transport);
void wt_transport_start(wt_transport_t* transport);
void wt_transport_stop(wt_transport_t* transport);

#endif /* HAS_MSQUIC */
#endif /* OFFS_WT_TRANSPORT_H */