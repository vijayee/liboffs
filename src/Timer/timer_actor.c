//
// Created by victor on 5/6/25.
//

#include "timer_actor.h"
#include "../Actor/message.h"
#include "../Util/allocator.h"
#include "../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include <internal/timer.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static void _destroy_stack_init(timer_actor_t* ta) {
  ta->destroy_lock = platform_mutex_create();
  ta->destroy_stack = NULL;
  ta->loop_lock = platform_mutex_create();
}

/* The destroy_stack is retained as a teardown-time scratch list. Pool workers
   now destroy timers inline under loop_lock (no longer deferring to the
   timer thread), so the destroy_stack is empty by the time _destroy_stack_drain
   runs. The lock is still held while popping nodes so the drain can coexist
   with a future caller that pushes from another thread. */
static void _destroy_stack_drain(timer_actor_t* ta) {
  platform_mutex_lock(ta->destroy_lock);
  timer_destroy_node_t* node = ta->destroy_stack;
  ta->destroy_stack = NULL;
  platform_mutex_unlock(ta->destroy_lock);
  while (node != NULL) {
    timer_destroy_node_t* next = node->next;
    pd_timer_destroy(node->timer);
    free(node->user_data);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(timer_actor_t* ta) {
  _destroy_stack_drain(ta);
  platform_mutex_destroy(ta->destroy_lock);
  platform_mutex_destroy(ta->loop_lock);
}

static bool _timer_actor_track(timer_actor_t* ta, pd_timer_t* timer) {
  if (ta->active_timer_count >= ta->active_timer_capacity) {
    size_t new_cap = ta->active_timer_capacity == 0 ? 8 : ta->active_timer_capacity * 2;
    pd_timer_t** new_arr = realloc(ta->active_timers, new_cap * sizeof(pd_timer_t*));
    if (new_arr == NULL) return false;
    ta->active_timers = new_arr;
    ta->active_timer_capacity = new_cap;
  }
  ta->active_timers[ta->active_timer_count++] = timer;
  return true;
}

/* Remove a timer from the tracked set. Returns true if the timer was found and
   removed, false if it was already gone (e.g. cancelled by a concurrent
   TIMER_CANCEL, or torn down by _timer_actor_destroy_all_tracked). Callers
   must use the return value to decide whether it is safe to stop/destroy the
   timer pointer — operating on an untracked (already-freed) timer would be a
   use-after-free. */
static bool _timer_actor_untrack(timer_actor_t* ta, pd_timer_t* timer) {
  for (size_t i = 0; i < ta->active_timer_count; i++) {
    if (ta->active_timers[i] == timer) {
      ta->active_timers[i] = ta->active_timers[ta->active_timer_count - 1];
      ta->active_timer_count--;
      return true;
    }
  }
  return false;
}

/* Destroy every tracked timer synchronously under loop_lock while the loop
   thread is still alive. pd_timer_destroy drains the IOCP via
   iocp_drain_sync, which posts a sync completion and waits for the loop
   thread to process it; if the loop thread has already been joined the drain
   hits its 5s timeout per timer, which is the source of the per-test teardown
   stall. Performing this teardown before joining the loop thread keeps the
   drain fast.

   Holding loop_lock serializes this against a concurrent TIMER_SET /
   TIMER_CANCEL dispatch on a pool worker (which takes the same lock). Callers
   must first ensure no dispatch is in flight: timer_actor_stop and
   timer_actor_destroy set ACTOR_FLAG_DESTROY up front (which makes actor_run
   skip new dispatches), and the pool is either joined (test teardown calls
   scheduler_pool_stop first) or drained via scheduler_pool_wait_for_idle. A
   TIMER_CANCEL that already passed actor_run's destroy-flag check and is
   waiting on loop_lock is handled safely: by the time it acquires the lock,
   _timer_actor_untrack has already removed every timer, so its
   _timer_actor_untrack lookup returns false and it skips the freed pointer
   rather than touching it. */
static void _timer_actor_destroy_all_tracked(timer_actor_t* timer_actor) {
  platform_mutex_lock(timer_actor->loop_lock);
  for (size_t i = 0; i < timer_actor->active_timer_count; i++) {
    pd_timer_t* timer = timer_actor->active_timers[i];
    if (timer == NULL) continue;
    void* user_data = timer->user_data;
    pd_timer_stop(timer);
    pd_timer_destroy(timer);
    free(user_data);
    timer_actor->active_timers[i] = NULL;
  }
  timer_actor->active_timer_count = 0;
  platform_mutex_unlock(timer_actor->loop_lock);
}

static debounce_entry_t* _timer_actor_find_debounce(timer_actor_t* ta,
                                                      actor_t* target,
                                                      uint32_t completion_type) {
  for (size_t i = 0; i < MAX_DEBOUNCE_KEYS; i++) {
    if (ta->debounce_map[i].target == target &&
        ta->debounce_map[i].completion_type == completion_type) {
      return &ta->debounce_map[i];
    }
  }
  return NULL;
}

static debounce_entry_t* _timer_actor_alloc_debounce(timer_actor_t* ta,
                                                       actor_t* target,
                                                       uint32_t completion_type) {
  /* Check for an existing entry first. */
  debounce_entry_t* existing = _timer_actor_find_debounce(ta, target, completion_type);
  if (existing != NULL) {
    return existing;
  }
  /* Find an empty slot. */
  for (size_t i = 0; i < MAX_DEBOUNCE_KEYS; i++) {
    if (ta->debounce_map[i].target == NULL) {
      return &ta->debounce_map[i];
    }
  }
  return NULL;
}

static void _timer_completion_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  (void)events;
  timer_completion_payload_t* completion = (timer_completion_payload_t*)user_data;
  /* Allocate a fresh copy for each firing — actor_send takes ownership and
     frees via payload_destroy. The original completion stays alive for
     repeating timers until pd_timer_destroy is called. */
  timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
  *copy = *completion;
  message_t msg = {0};
  msg.type = TIMER_COMPLETION;
  msg.payload = copy;
  msg.payload_destroy = free;
  actor_send(&completion->timer_actor->actor, &msg);
}

static void _timer_actor_dispatch(void* state, message_t* msg) {
  timer_actor_t* timer_actor = (timer_actor_t*)state;

  switch (msg->type) {
    case TIMER_SET: {
      timer_set_payload_t* payload = (timer_set_payload_t*)msg->payload;
      timer_completion_payload_t* completion = get_clear_memory(sizeof(timer_completion_payload_t));
      completion->timer_id = 0;
      completion->target = payload->target;
      completion->completion_type = payload->completion_type;
      completion->timer_actor = timer_actor;
      /* Hold loop_lock around pd_timer_create/pd_timer_start so a
         concurrent TIMER_CANCEL on a different scheduler worker cannot
         race the watcher list mutation. The two operations must be
         serialized against the loop's watcher list. */
      platform_mutex_lock(timer_actor->loop_lock);
      pd_timer_t* timer = pd_timer_create(
          timer_actor->loop, payload->timeout_ms, payload->interval_ms,
          _timer_completion_callback, completion);
      if (timer != NULL) {
        pd_timer_start(timer);
        if (_timer_actor_track(timer_actor, timer)) {
          completion->timer_id = (uint64_t)(uintptr_t)timer;
        } else {
          /* Tracking failed (OOM) — stop and destroy the timer to avoid leak. */
          pd_timer_stop(timer);
          pd_timer_destroy(timer);
          free(completion);
          completion = NULL;
        }
      } else {
        free(completion);
        completion = NULL;
      }
      platform_mutex_unlock(timer_actor->loop_lock);
      if (payload->out_timer_id != NULL) {
        atomic_store(payload->out_timer_id,
                     completion != NULL ? completion->timer_id : 0);
      }
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
    case TIMER_CANCEL: {
      timer_cancel_payload_t* payload = (timer_cancel_payload_t*)msg->payload;
      if (payload->timer_id != 0) {
        pd_timer_t* timer = (pd_timer_t*)(uintptr_t)payload->timer_id;
        /* Synchronously stop+destroy under loop_lock. This is the
           dispatch side of the same lock a concurrent TIMER_SET on
           another scheduler worker takes. Destroying inline (under the
           lock) is safe because pd_timer_stop is synchronous on IOCP
           (DeleteTimerQueueTimer(INVALID_HANDLE_VALUE) waits for the
           thread-pool callback to finish) and pd_timer_destroy drains
           the IOCP via iocp_drain_sync before freeing the placeholder
           watcher. Deferring destroy to the timer thread would reopen
           the window where a fast TIMER_CANCEL→TIMER_SET sequence
           races on the loop's watcher array.

           _timer_actor_untrack returns false if the timer was already
           removed (a duplicate/late TIMER_CANCEL, or teardown via
           _timer_actor_destroy_all_tracked); in that case the pointer is
           already freed and must not be touched. */
        platform_mutex_lock(timer_actor->loop_lock);
        if (_timer_actor_untrack(timer_actor, timer)) {
          void* user_data = timer->user_data;
          pd_timer_stop(timer);
          pd_timer_destroy(timer);
          free(user_data);
        }
        platform_mutex_unlock(timer_actor->loop_lock);
      }
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
    case TIMER_DEBOUNCE: {
      timer_debounce_payload_t* payload = (timer_debounce_payload_t*)msg->payload;
      debounce_entry_t* entry = _timer_actor_find_debounce(
          timer_actor, payload->target, payload->completion_type);
      platform_mutex_lock(timer_actor->loop_lock);
      if (entry != NULL && entry->timer != NULL) {
        /* Cancel the existing debounce timer for this (target, type) pair. */
        pd_timer_stop(entry->timer);
        void* old_user_data = entry->timer->user_data;
        _timer_actor_untrack(timer_actor, entry->timer);
        pd_timer_destroy(entry->timer);
        free(old_user_data);
        entry->timer = NULL;
        entry->completion_payload = NULL;
      }
      /* Allocate a slot for this debounce key. */
      if (entry == NULL) {
        entry = _timer_actor_alloc_debounce(timer_actor, payload->target,
                                             payload->completion_type);
      }
      timer_completion_payload_t* completion = get_clear_memory(sizeof(timer_completion_payload_t));
      completion->timer_id = 0;
      completion->target = payload->target;
      completion->completion_type = payload->completion_type;
      completion->timer_actor = timer_actor;
      pd_timer_t* timer = pd_timer_create(
          timer_actor->loop, payload->timeout_ms, payload->interval_ms,
          _timer_completion_callback, completion);
      if (timer != NULL && entry != NULL) {
        pd_timer_start(timer);
        if (_timer_actor_track(timer_actor, timer)) {
          completion->timer_id = (uint64_t)(uintptr_t)timer;
          entry->target = payload->target;
          entry->completion_type = payload->completion_type;
          entry->timer = timer;
          entry->completion_payload = completion;
        } else {
          /* Tracking failed (OOM) — stop and destroy the timer. */
          pd_timer_stop(timer);
          pd_timer_destroy(timer);
          free(completion);
          /* Clear the debounce entry since the timer wasn't set up. */
          if (entry->timer == NULL) {
            entry->target = NULL;
            entry->completion_type = 0;
          }
        }
      } else {
        if (timer != NULL) {
          pd_timer_stop(timer);
          pd_timer_destroy(timer);
          free(completion);
        } else {
          free(completion);
        }
      }
      platform_mutex_unlock(timer_actor->loop_lock);
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
    case TIMER_DEBOUNCE_FLUSH: {
      timer_debounce_payload_t* payload = (timer_debounce_payload_t*)msg->payload;
      debounce_entry_t* entry = _timer_actor_find_debounce(
          timer_actor, payload->target, payload->completion_type);
      platform_mutex_lock(timer_actor->loop_lock);
      if (entry != NULL && entry->timer != NULL) {
        /* Cancel the pending debounce timer. */
        pd_timer_stop(entry->timer);
        void* old_user_data = entry->timer->user_data;
        _timer_actor_untrack(timer_actor, entry->timer);
        pd_timer_destroy(entry->timer);
        free(old_user_data);
        /* Clear the debounce entry. */
        entry->target = NULL;
        entry->completion_type = 0;
        entry->timer = NULL;
        entry->completion_payload = NULL;
        /* Immediately dispatch the completion message to the target actor
           via two-hop through the timer_actor. */
        timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
        copy->target = payload->target;
        copy->completion_type = payload->completion_type;
        copy->timer_id = 0;
        copy->timer_actor = timer_actor;
        message_t dispatch_msg = {0};
        dispatch_msg.type = TIMER_COMPLETION;
        dispatch_msg.payload = copy;
        dispatch_msg.payload_destroy = free;
        platform_mutex_unlock(timer_actor->loop_lock);
        actor_send(&timer_actor->actor, &dispatch_msg);
      } else {
        platform_mutex_unlock(timer_actor->loop_lock);
      }
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
    case TIMER_COMPLETION: {
      timer_completion_payload_t* completion = (timer_completion_payload_t*)msg->payload;
      /* Allocate a fresh copy of the completion to forward to the target
       * actor. The original payload is owned by the dispatch and will be
       * freed by actor_run's normal post-dispatch cleanup; we simply
       * consume (set msg->payload = NULL) to indicate ownership transfer
       * of the original to the runner. */
      timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
      *copy = *completion;
      message_t target_msg = {0};
      target_msg.type = copy->completion_type;
      target_msg.payload = copy;
      target_msg.payload_destroy = free;
      actor_send(copy->target, &target_msg);
      msg->payload = NULL;
      msg->payload_destroy = NULL;
      break;
    }
    default:
      break;
  }
}

static void* _timer_actor_thread(void* arg) {
  platform_thread_setup_stack();
  timer_actor_t* timer_actor = (timer_actor_t*)arg;

  while (atomic_load(&timer_actor->running)) {
    int result = pd_loop_run_once(timer_actor->loop, 100);
    if (result < 0) {
      break;
    }
  }

  return NULL;
}

timer_actor_t* timer_actor_create(scheduler_pool_t* pool) {
  timer_actor_t* timer_actor = get_clear_memory(sizeof(timer_actor_t));
  actor_init(&timer_actor->actor, timer_actor, _timer_actor_dispatch, pool);
  timer_actor->loop = pd_loop_create(NULL);
  if (timer_actor->loop == NULL) {
    free(timer_actor);
    return NULL;
  }
  _destroy_stack_init(timer_actor);
  atomic_store(&timer_actor->running, 1);
  timer_actor->thread = platform_thread_create(_timer_actor_thread, timer_actor);
  return timer_actor;
}

void timer_actor_stop(timer_actor_t* timer_actor) {
  if (timer_actor == NULL) return;
  if (!atomic_load(&timer_actor->running)) return;

  /* Mark the actor as destroyed so scheduler workers drop pending messages
     (e.g., TIMER_SET requests from network actors). Without this, processing
     a TIMER_SET message would call pd_timer_create/pd_timer_start on a loop
     whose thread has exited, which may hang or misbehave. */
  uint8_t flags = atomic_load(&timer_actor->actor.flags);
  while (!atomic_compare_exchange_strong(&timer_actor->actor.flags, &flags,
                                           flags | ACTOR_FLAG_DESTROY)) {
  }

  /* Destroy tracked timers while the loop thread is still alive so
     pd_timer_destroy's IOCP drain_sync completes promptly; doing this after
     joining the loop thread (the old order) left the drain with no live loop
     to process the posted sync, hitting a 5s timeout per timer. */
  _timer_actor_destroy_all_tracked(timer_actor);

  atomic_store(&timer_actor->running, 0);
  pd_loop_async_send(timer_actor->loop, NULL);
  platform_thread_join(timer_actor->thread);
  timer_actor->thread = NULL;
}

void timer_actor_destroy(timer_actor_t* timer_actor) {
  if (timer_actor == NULL) {
    return;
  }
  scheduler_pool_t* pool = timer_actor->actor.pool;
  actor_destroy(&timer_actor->actor);
  if (pool != NULL && !atomic_load(&pool->terminate)) {
    scheduler_pool_wait_for_idle(pool);
  }
  /* Destroy tracked timers while the loop thread is still alive so
     pd_timer_destroy's IOCP drain_sync completes promptly. The old code
     joined the loop thread first and then destroyed the timers, leaving
     drain_sync with no live loop to process the posted sync — a 5s timeout
     per leftover timer. When timer_actor_stop ran first it only stopped
     (not destroyed) the timers, so this destroy path still owned the
     destroy and still stalled; destroying here before the join fixes both
     the standalone-destroy and stop-then-destroy paths. The helper is a
     no-op if timer_actor_stop already destroyed the tracked timers. */
  _timer_actor_destroy_all_tracked(timer_actor);
  if (atomic_load(&timer_actor->running)) {
    atomic_store(&timer_actor->running, 0);
    pd_loop_async_send(timer_actor->loop, NULL);
    platform_thread_join(timer_actor->thread);
    timer_actor->thread = NULL;
  }
  free(timer_actor->active_timers);
  _destroy_stack_destroy(timer_actor);
  pd_loop_stop(timer_actor->loop);
  pd_loop_destroy(timer_actor->loop);
  free(timer_actor);
}

uint64_t timer_actor_set(timer_actor_t* timer_actor, uint64_t timeout_ms,
                         uint64_t interval_ms, actor_t* target,
                         uint32_t completion_type,
                         ATOMIC(uint64_t)* out_timer_id) {
  timer_set_payload_t* payload = get_clear_memory(sizeof(timer_set_payload_t));
  payload->timeout_ms = timeout_ms;
  payload->interval_ms = interval_ms;
  payload->target = target;
  payload->completion_type = completion_type;
  payload->timer_id = 0;
  payload->out_timer_id = out_timer_id;

  message_t msg = {0};
  msg.type = TIMER_SET;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  if (atomic_load(&timer_actor->running)) {
    pd_loop_async_send(timer_actor->loop, NULL);
  }

  /* The timer_id is filled in by the dispatch on the scheduler worker.
     Since actor_send is async, the out_timer_id will be 0 until the
     dispatch runs. Caller can read it after scheduler_pool_wait_for_idle.
     Returns 0 — use out_timer_id to get the actual timer_id. */
  return 0;
}

void timer_actor_cancel(timer_actor_t* timer_actor, uint64_t timer_id) {
  timer_cancel_payload_t* payload = get_clear_memory(sizeof(timer_cancel_payload_t));
  payload->timer_id = timer_id;

  message_t msg = {0};
  msg.type = TIMER_CANCEL;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  if (atomic_load(&timer_actor->running)) {
    pd_loop_async_send(timer_actor->loop, NULL);
  }
}

uint64_t timer_actor_debounce(timer_actor_t* timer_actor,
                              uint64_t timeout_ms, uint64_t interval_ms,
                              actor_t* target, uint32_t completion_type) {
  timer_debounce_payload_t* payload = get_clear_memory(sizeof(timer_debounce_payload_t));
  payload->timeout_ms = timeout_ms;
  payload->interval_ms = interval_ms;
  payload->target = target;
  payload->completion_type = completion_type;

  message_t msg = {0};
  msg.type = TIMER_DEBOUNCE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  if (atomic_load(&timer_actor->running)) {
    pd_loop_async_send(timer_actor->loop, NULL);
  }

  return 0;
}

void timer_actor_debounce_flush(timer_actor_t* timer_actor,
                                actor_t* target, uint32_t completion_type) {
  timer_debounce_payload_t* payload = get_clear_memory(sizeof(timer_debounce_payload_t));
  payload->target = target;
  payload->completion_type = completion_type;
  payload->timeout_ms = 0;
  payload->interval_ms = 0;

  message_t msg = {0};
  msg.type = TIMER_DEBOUNCE_FLUSH;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  if (atomic_load(&timer_actor->running)) {
    pd_loop_async_send(timer_actor->loop, NULL);
  }
}
