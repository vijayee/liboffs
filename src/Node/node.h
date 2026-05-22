//
// Created by victor on 5/14/25.
//

#ifndef OFFS_NODE_H
#define OFFS_NODE_H

#include "../Configuration/config.h"
#include "../Network/authority.h"
#include "../Network/network.h"
#include "../Scheduler/scheduler.h"
#include "../Timer/timer_actor.h"
#include "../Util/atomic_compat.h"
#include <stdint.h>
#include <stddef.h>

#define OFFS_ERROR_DRAINING -2

typedef struct block_cache_t block_cache_t;
typedef struct http_server_t http_server_t;

typedef struct offs_node_t {
  config_t* config;
  authority_t* authority;
  network_t* network;
  block_cache_t* block_cache;
  http_server_t* http_server;
  scheduler_pool_t* scheduler;
  timer_actor_t* timer;
  ATOMIC(uint8_t) running;
} offs_node_t;

offs_node_t* offs_node_create(config_t* config, authority_t* authority);
int offs_node_start(offs_node_t* node);
void offs_node_stop(offs_node_t* node);
void offs_node_stop_http(offs_node_t* node);
void offs_node_stop_network(offs_node_t* node);
void offs_node_destroy(offs_node_t* node);

#endif // OFFS_NODE_H