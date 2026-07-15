//
// Created by victor on 5/6/25.
//

#ifndef OFFS_TIMER_ACTOR_H
#define OFFS_TIMER_ACTOR_H

#include "../Actor/actor.h"
#include "../Platform/platform.h"
#include "../Scheduler/scheduler.h"
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
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  /* Active timers tracked for cleanup on destroy. */
  pd_timer_t** active_timers;
  size_t active_timer_count;
  size_t active_timer_capacity;
  /* Debounce map: tracks one timer per (target, completion_type) pair,
     cancelling the previous timer when a new debounce arrives. */
  debounce_entry_t debounce_map[MAX_DEBOUNCE_KEYS];
  /* Serializes pd_timer_create/start/stop/destroy (which mutate the loop's
     watcher list) across threads. timer_actor_set and timer_actor_cancel
     run synchronously on the caller's thread; TIMER_DEBOUNCE and
     TIMER_DEBOUNCE_FLUSH dispatch on a scheduler pool worker; and
     _timer_actor_destroy_all_tracked runs at teardown. Without this lock,
     two of these can race on pd_loop_add_watcher / pd_loop_remove_watcher —
     the watcher array's watcher_count is left inconsistent, and the loop
     later iterates a stale (already-freed) pointer. */
  platform_mutex_t* loop_lock;
} timer_actor_t;

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
  timer_actor_t* timer_actor;  // back-reference for two-hop dispatch
} timer_completion_payload_t;

timer_actor_t* timer_actor_create(scheduler_pool_t* pool);
void timer_actor_stop(timer_actor_t* timer_actor);
void timer_actor_destroy(timer_actor_t* timer_actor);
uint64_t timer_actor_set(timer_actor_t* timer_actor, uint64_t timeout_ms,
                         uint64_t interval_ms, actor_t* target,
                         uint32_t completion_type,
                         ATOMIC(uint64_t)* out_timer_id);
void timer_actor_cancel(timer_actor_t* timer_actor, uint64_t timer_id);
/* Cancel all debounce timers for `target` and remove its entries from the
   debounce_map and active_timers. Call this BEFORE freeing `target` — any
   in-flight completion already in the timer_actor's mailbox will be dropped
   by the dispatch (the target is no longer in the map / active_timers, so
   the dispatch won't actor_send to it). The map lookup compares pointer
   VALUES (not dereferencing), so a freed target's stale pointer value is
   safe to compare. See concurrency-pass.md F8. */
void timer_actor_cancel_target(timer_actor_t* timer_actor, actor_t* target);
uint64_t timer_actor_debounce(timer_actor_t* timer_actor,
                              uint64_t timeout_ms, uint64_t interval_ms,
                              actor_t* target, uint32_t completion_type);
void timer_actor_debounce_flush(timer_actor_t* timer_actor,
                                actor_t* target, uint32_t completion_type);

#endif // OFFS_TIMER_ACTOR_H
