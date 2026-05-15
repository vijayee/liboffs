//
// Created by victor on 5/14/25.
//

#include "gossip.h"
#include "../Util/allocator.h"
#include <string.h>

// --- Gossip exchange ---

gossip_t* gossip_create(uint64_t query_id, net_node_t* target, uint32_t timeout_ms) {
  if (target == NULL) return NULL;
  gossip_t* gossip = get_clear_memory(sizeof(gossip_t));
  gossip->query_id = query_id;
  gossip->state = GOSSIP_STATE_INIT;
  gossip->target = target;
  gossip->start_time_ms = 0;  // Set by caller with monotonic clock
  gossip->timeout_ms = timeout_ms;
  return gossip;
}

void gossip_destroy(gossip_t* gossip) {
  if (gossip == NULL) return;
  // Target is owned by ring table — not freed here
  free(gossip);
}

bool gossip_is_expired(const gossip_t* gossip, uint64_t now_ms) {
  if (gossip == NULL) return true;
  return now_ms > (gossip->start_time_ms + gossip->timeout_ms);
}

// --- Scheduler ---

void gossip_scheduler_init(gossip_scheduler_t* sched,
                           uint32_t init_interval_s,
                           uint32_t num_init_intervals,
                           uint32_t steady_state_interval_s) {
  if (sched == NULL) return;
  sched->init_interval_s = init_interval_s > 0 ? init_interval_s : GOSSIP_INIT_INTERVAL_S;
  sched->num_init_intervals = num_init_intervals > 0 ? num_init_intervals : GOSSIP_INIT_COUNT;
  sched->steady_state_interval_s = steady_state_interval_s > 0
                                       ? steady_state_interval_s
                                       : GOSSIP_STEADY_INTERVAL_S;
  sched->interval_idx = 0;
  sched->is_initial_phase = true;
  sched->last_gossip_ms = 0;
  sched->should_gossip = false;
}

void gossip_scheduler_tick(gossip_scheduler_t* sched, uint64_t now_ms) {
  if (sched == NULL) return;
  sched->should_gossip = false;

  uint64_t interval_ms;
  if (sched->is_initial_phase) {
    interval_ms = (uint64_t)sched->init_interval_s * 1000;
  } else {
    interval_ms = (uint64_t)sched->steady_state_interval_s * 1000;
  }

  if (sched->last_gossip_ms == 0 || now_ms >= sched->last_gossip_ms + interval_ms) {
    sched->should_gossip = true;
    sched->last_gossip_ms = now_ms;

    if (sched->is_initial_phase) {
      sched->interval_idx++;
      if (sched->interval_idx >= sched->num_init_intervals) {
        sched->is_initial_phase = false;
      }
    }
  }
}

// --- Handle ---

void gossip_handle_init(gossip_handle_t* handle,
                        uint32_t init_interval_s,
                        uint32_t num_init_intervals,
                        uint32_t steady_state_interval_s,
                        uint32_t timeout_ms) {
  if (handle == NULL) return;
  gossip_scheduler_init(&handle->scheduler, init_interval_s,
                        num_init_intervals, steady_state_interval_s);
  query_table_init(&handle->query_table, QUERY_TABLE_DEFAULT_CAPACITY);
  vec_init(&handle->active);
  handle->next_query_id = 1;
  handle->timeout_ms = timeout_ms > 0 ? timeout_ms : GOSSIP_TIMEOUT_MS;
  handle->running = false;
}

void gossip_handle_deinit(gossip_handle_t* handle) {
  if (handle == NULL) return;
  for (int index = 0; index < handle->active.length; index++) {
    gossip_destroy(handle->active.data[index]);
  }
  vec_deinit(&handle->active);
  query_table_deinit(&handle->query_table);
}

uint64_t gossip_handle_next_query_id(gossip_handle_t* handle) {
  if (handle == NULL) return 0;
  return handle->next_query_id++;
}

void gossip_handle_expire_queries(gossip_handle_t* handle, uint64_t now_ms) {
  if (handle == NULL) return;

  // Expire timed-out queries from the query table
  query_table_expire(&handle->query_table, now_ms);

  // Also expire active gossip exchanges
  int write_index = 0;
  for (int index = 0; index < handle->active.length; index++) {
    gossip_t* gossip = handle->active.data[index];
    if (gossip_is_expired(gossip, now_ms)) {
      gossip_destroy(gossip);
    } else {
      handle->active.data[write_index++] = gossip;
    }
  }
  handle->active.length = write_index;
}