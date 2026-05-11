//
// Created by victor on 5/6/25.
//

#ifndef OFFS_TIMER_ACTOR_H
#define OFFS_TIMER_ACTOR_H

#include "../Actor/actor.h"
#include "../Util/threadding.h"
#include "../Util/atomic_compat.h"
#include <poll-dancer/types.h>
#include <stdint.h>

#define MAX_DEBOUNCE_KEYS 16

typedef struct debounce_entry_t {
  actor_t* target;
  uint32_t completion_type;
  pd_timer_t* timer;
  void* completion_payload;
} debounce_entry_t;

typedef struct timer_actor_t {
  actor_t actor;
  pd_loop_t* loop;
  PLATFORMTHREADTYPE thread;
  ATOMIC(uint8_t) running;
  /* Active timers tracked for cleanup on destroy. */
  pd_timer_t** active_timers;
  size_t active_timer_count;
  size_t active_timer_capacity;
  /* Debounce map: tracks one timer per (target, completion_type) pair,
     cancelling the previous timer when a new debounce arrives. */
  debounce_entry_t debounce_map[MAX_DEBOUNCE_KEYS];
} timer_actor_t;

typedef struct {
  uint64_t timer_id;
  uint64_t timeout_ms;
  uint64_t interval_ms;
  actor_t* target;
  uint32_t completion_type;
} timer_set_payload_t;

typedef struct {
  uint64_t timer_id;
} timer_cancel_payload_t;

typedef struct {
  uint64_t timeout_ms;
  uint64_t interval_ms;
  actor_t* target;
  uint32_t completion_type;
} timer_debounce_payload_t;

typedef struct {
  uint64_t timer_id;
  actor_t* target;
  uint32_t completion_type;
} timer_completion_payload_t;

timer_actor_t* timer_actor_create(void);
void timer_actor_destroy(timer_actor_t* timer_actor);
uint64_t timer_actor_set(timer_actor_t* timer_actor, uint64_t timeout_ms,
                         uint64_t interval_ms, actor_t* target,
                         uint32_t completion_type);
void timer_actor_cancel(timer_actor_t* timer_actor, uint64_t timer_id);
uint64_t timer_actor_debounce(timer_actor_t* timer_actor,
                              uint64_t timeout_ms, uint64_t interval_ms,
                              actor_t* target, uint32_t completion_type);

#endif // OFFS_TIMER_ACTOR_H
