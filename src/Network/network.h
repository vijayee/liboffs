//
// Created by victor on 5/14/25.
//

#ifndef OFFS_NETWORK_H
#define OFFS_NETWORK_H

#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Timer/timer_actor.h"
#include "authority.h"
#include "gossip.h"
#include "ring_set.h"
#include "latency_cache.h"
#include "eabf.h"
#include "hebbian.h"
#include "hebbian_config.h"
#include "connection_manager.h"
#include "rate_limit.h"
#include "node_id.h"
#include <stdint.h>
#include <stddef.h>

typedef struct block_cache_t block_cache_t;

typedef struct network_t {
  actor_t actor;
  authority_t* authority;
  block_cache_t* block_cache;
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  gossip_handle_t gossip;
  ring_set_t* rings;
  latency_cache_t* latency_cache;
  eabf_table_t eabf_table;
  eabf_ttl_table_t eabf_ttl;
  hebbian_table_t hebbian;
  rate_limit_table_t rate_limits;
  connection_manager_t conn_mgr;
  uint64_t gossip_timer_id;
  uint64_t eabf_maintenance_timer_id;
  uint64_t hebbian_decay_timer_id;
  uint64_t metrics_push_timer_id;
  ATOMIC(uint8_t) running;
} network_t;

network_t* network_create(authority_t* authority, block_cache_t* block_cache,
                          timer_actor_t* timer, scheduler_pool_t* pool);
void network_destroy(network_t* network);
void network_dispatch(void* state, message_t* msg);

#endif // OFFS_NETWORK_H