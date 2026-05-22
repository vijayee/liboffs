//
// Created by victor on 5/14/25.
//

#ifndef OFFS_GOSSIP_H
#define OFFS_GOSSIP_H

#include "net_node.h"
#include "query.h"
#include "../Util/vec.h"
#include <stdint.h>
#include <stdbool.h>

// Two-phase gossip timing constants
// (tunable parameters now set via config_t at startup)

// Gossip states
typedef enum gossip_state_e {
  GOSSIP_STATE_INIT,
  GOSSIP_STATE_ACTIVE,
  GOSSIP_STATE_WAIT_REPLY
} gossip_state_e;

// Single gossip exchange with a target node
typedef struct gossip_t {
  uint64_t query_id;
  gossip_state_e state;
  net_node_t* target;
  uint64_t start_time_ms;
  uint32_t timeout_ms;
} gossip_t;

// Two-phase scheduler: aggressive init, relaxed steady-state
// Tick is driven by timer actor messages — no threads, no locks
typedef struct gossip_scheduler_t {
  uint32_t init_interval_s;
  uint32_t num_init_intervals;
  uint32_t steady_state_interval_s;
  uint32_t interval_idx;
  bool is_initial_phase;
  uint64_t last_gossip_ms;
  bool should_gossip;
} gossip_scheduler_t;

// Top-level gossip manager — owned by network actor
typedef struct gossip_handle_t {
  gossip_scheduler_t scheduler;
  query_table_t query_table;
  vec_t(gossip_t*) active;
  uint64_t next_query_id;
  uint32_t timeout_ms;
  bool running;
} gossip_handle_t;

// --- Gossip exchange ---

gossip_t* gossip_create(uint64_t query_id, net_node_t* target, uint32_t timeout_ms);
void gossip_destroy(gossip_t* gossip);

bool gossip_is_expired(const gossip_t* gossip, uint64_t now_ms);

// --- Scheduler ---

void gossip_scheduler_init(gossip_scheduler_t* sched,
                           uint32_t init_interval_s,
                           uint32_t num_init_intervals,
                           uint32_t steady_state_interval_s);
void gossip_scheduler_tick(gossip_scheduler_t* sched, uint64_t now_ms);

// --- Handle ---

void gossip_handle_init(gossip_handle_t* handle,
                        uint32_t init_interval_s,
                        uint32_t num_init_intervals,
                        uint32_t steady_state_interval_s,
                        uint32_t timeout_ms);
void gossip_handle_deinit(gossip_handle_t* handle);

uint64_t gossip_handle_next_query_id(gossip_handle_t* handle);
void gossip_handle_expire_queries(gossip_handle_t* handle, uint64_t now_ms);

#endif // OFFS_GOSSIP_H