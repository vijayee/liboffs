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
   synchronous timer_actor_cancel, or torn down by
   _timer_actor_destroy_all_tracked). Callers must use the return value to
   decide whether it is safe to stop/destroy the timer pointer — operating on
   an untracked (already-freed) timer would be a use-after-free. */
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

   Holding loop_lock serializes this against a concurrent synchronous
   timer_actor_set / timer_actor_cancel on another thread, or a
   TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatch on a pool worker (all of
   which take the same lock). Callers must first ensure no set/cancel or
   dispatch is in flight: timer_actor_stop and timer_actor_destroy set
   ACTOR_FLAG_DESTROY up front (which makes actor_run skip new dispatches),
   and the pool is either joined (test teardown calls scheduler_pool_stop
   first) or drained via scheduler_pool_wait_for_idle. A synchronous cancel
   (or debounce dispatch) that is waiting on loop_lock is handled safely: by
   the time it acquires the lock, _timer_actor_untrack has already removed
   every timer, so its _timer_actor_untrack lookup returns false and it skips
   the freed pointer rather than touching it. */
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
      /* payload (timer_debounce_payload_t) is read-only to this dispatch —
         the fields are copied into the timer's completion payload above.
         Let actor_run free it via msg->payload_destroy (free). The previous
         CONSUME here (NULLing msg fields) leaked the payload because the
         dispatch never freed it. */
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
        /* Build the completion to forward to the target. Send it ONE-hop
           directly to the target instead of two-hop through the timer_actor:
           the entry was just cleared above, so the TIMER_COMPLETION
           dispatch's F8 re-check (which only forwards when the target is
           still in the debounce_map / active_timers) would otherwise drop
           the completion. The cancel+clear happened under loop_lock, so the
           timer can no longer fire and enqueue a competing completion.
           actor_send to a destroyed target drops the payload safely; a
           freed target is the destroyer's responsibility (the destroyer
           calls timer_actor_cancel_target before freeing, which removes
           the entry, making this flush a no-op). */
        timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
        copy->target = payload->target;
        copy->completion_type = payload->completion_type;
        copy->timer_id = 0;
        copy->timer_actor = timer_actor;
        message_t target_msg = {0};
        target_msg.type = copy->completion_type;
        target_msg.payload = copy;
        target_msg.payload_destroy = free;
        platform_mutex_unlock(timer_actor->loop_lock);
        actor_send(copy->target, &target_msg);
      } else {
        platform_mutex_unlock(timer_actor->loop_lock);
      }
      /* payload (timer_debounce_payload_t) fields are copied into the
         completion copy above. Let actor_run free the original via
         msg->payload_destroy. */
      break;
    }
    case TIMER_COMPLETION: {
      timer_completion_payload_t* completion = (timer_completion_payload_t*)msg->payload;
      /* F8 re-check: before actor_send-ing to completion->target, confirm
         the target is still tracked in the debounce_map or active_timers.
         timer_actor_cancel_target removes the target from both BEFORE the
         target is freed; if the target isn't tracked, this completion is
         stale (the target was canceled/freed) and must be dropped without
         dereferencing completion->target. The lookup compares pointer VALUES
         (no dereference), so a freed target's stale pointer value is safe
         to compare. actor_send would otherwise read freed flags -> UAF.

         Both debounce timers (TIMER_DEBOUNCE dispatch) and direct
         timer_actor_set timers are tracked in active_timers, so checking
         both the debounce_map and active_timers covers every completion
         source. The completion payload carried in msg->payload is owned by
         actor_run (payload_destroy = free) and is freed regardless of
         whether we drop or forward, so the drop path leaks nothing. */
      platform_mutex_lock(timer_actor->loop_lock);
      bool target_tracked = false;
      for (size_t index = 0; index < MAX_DEBOUNCE_KEYS; index++) {
        if (timer_actor->debounce_map[index].target == completion->target) {
          target_tracked = true;
          break;
        }
      }
      if (!target_tracked) {
        for (size_t index = 0; index < timer_actor->active_timer_count;
             index++) {
          pd_timer_t* tracked_timer = timer_actor->active_timers[index];
          if (tracked_timer == NULL) {
            continue;
          }
          timer_completion_payload_t* tracked_completion =
              (timer_completion_payload_t*)tracked_timer->user_data;
          if (tracked_completion != NULL &&
              tracked_completion->target == completion->target) {
            target_tracked = true;
            break;
          }
        }
      }
      platform_mutex_unlock(timer_actor->loop_lock);
      if (!target_tracked) {
        /* Drop the stale completion — the target was canceled/freed. The
           payload is freed by actor_run via msg->payload_destroy. */
        break;
      }
      /* Allocate a fresh copy of the completion to forward to the target
       * actor. The original payload's fields are copied into `copy` and the
       * original is then freed by actor_run's post-dispatch cleanup via
       * msg->payload_destroy (free). */
      timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
      *copy = *completion;
      message_t target_msg = {0};
      target_msg.type = copy->completion_type;
      target_msg.payload = copy;
      target_msg.payload_destroy = free;
      actor_send(copy->target, &target_msg);
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
  timer_actor->loop_lock = platform_mutex_create();
  atomic_store(&timer_actor->running, 1);
  timer_actor->thread = platform_thread_create(_timer_actor_thread, timer_actor);
  return timer_actor;
}

void timer_actor_stop(timer_actor_t* timer_actor) {
  if (timer_actor == NULL) return;
  if (!atomic_load(&timer_actor->running)) return;

  /* Mark the actor as destroyed so scheduler workers drop pending debounce
     messages. (timer_actor_set / timer_actor_cancel are synchronous and do
     not queue messages.) Without this, processing a debounce message would
     call pd_timer_create/pd_timer_start on a loop whose thread has exited,
     which may hang or misbehave. */
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
  platform_mutex_destroy(timer_actor->loop_lock);
  pd_loop_stop(timer_actor->loop);
  pd_loop_destroy(timer_actor->loop);
  free(timer_actor);
}

uint64_t timer_actor_set(timer_actor_t* timer_actor, uint64_t timeout_ms,
                         uint64_t interval_ms, actor_t* target,
                         uint32_t completion_type,
                         ATOMIC(uint64_t)* out_timer_id) {
  if (timer_actor == NULL) {
    if (out_timer_id != NULL) atomic_store(out_timer_id, 0);
    return 0;
  }

  /* Synchronous set: create, start, and track the timer inline under
     loop_lock, and fill out_timer_id before returning. This bypasses the
     actor message queue for the same reason timer_actor_cancel does: under
     2+ scheduler workers a TIMER_SET message can strand in the timer_actor's
     queue (pushed but never dispatched when scheduler_pool_wait_for_idle
     returns). A stranded SET leaves out_timer_id at 0, so the caller (e.g.
     network_destroy) skips the matching cancel — the timer is then created
     late, on a future pool drain, after the caller has already moved on, and
     is never cancelled. Its pd_timer (and CreateEvent + CreateTimerQueueTimer
     handles) stays tracked until timer_actor_destroy: a linear per-cycle handle
     leak. Creating synchronously makes the id available immediately, so every
     set has a matching synchronous cancel and no timer is orphaned.

     This mirrors the former TIMER_SET dispatch logic and takes the same
     loop_lock a concurrent TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatch on
     a pool worker (or a synchronous cancel on another thread) takes, so the
     watcher-list mutation (pd_timer_create/pd_timer_start) is serialized.
     pd_timer_create/pd_timer_start do not need the timer_actor's loop thread
     to make progress, so holding loop_lock across them cannot deadlock with
     the loop thread (which never takes loop_lock). The completion payload is
     the timer's user_data and is freed when the timer is cancelled or torn
     down via _timer_actor_destroy_all_tracked. */
  timer_completion_payload_t* completion = get_clear_memory(sizeof(timer_completion_payload_t));
  completion->timer_id = 0;
  completion->target = target;
  completion->completion_type = completion_type;
  completion->timer_actor = timer_actor;

  platform_mutex_lock(timer_actor->loop_lock);
  uint64_t id = 0;
  pd_timer_t* timer = pd_timer_create(
      timer_actor->loop, timeout_ms, interval_ms,
      _timer_completion_callback, completion);
  if (timer != NULL) {
    pd_timer_start(timer);
    if (_timer_actor_track(timer_actor, timer)) {
      id = (uint64_t)(uintptr_t)timer;
      completion->timer_id = id;
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

  /* id was captured under loop_lock; publish it after unlock. No other thread
     can cancel this timer before out_timer_id is published (cancel needs the
     id, which only comes from out_timer_id). */
  if (out_timer_id != NULL) {
    atomic_store(out_timer_id, id);
  }
  return id;
}

void timer_actor_cancel(timer_actor_t* timer_actor, uint64_t timer_id) {
  if (timer_actor == NULL || timer_id == 0) {
    return;
  }
  pd_timer_t* timer = (pd_timer_t*)(uintptr_t)timer_id;

  /* Synchronous cancel: untrack, stop, and destroy the timer inline under
     loop_lock, bypassing the actor message queue.

     The queue is a lock-free mailbox drained by the scheduler, and under
     2+ workers a cancel message can strand — be pushed to the timer_actor's
     queue yet never dispatched (the actor is not in any worker's deque when
     scheduler_pool_wait_for_idle returns, so the message sits until
     timer_actor_destroy). A stranded cancel leaves the pd_timer tracked and
     its OS handles (CreateEvent stop_event + CreateTimerQueueTimer) leaked
     until teardown. Cancelling synchronously from the caller destroys the
     timer immediately and releases its handles, with no dependence on
     scheduler delivery.

     This mirrors the stop+destroy sequence _timer_actor_destroy_all_tracked
     uses at teardown, and takes the same lock a concurrent synchronous
     timer_actor_set (or a TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatch on a
     pool worker) takes. loop_lock serializes the watcher-list mutation
     (pd_timer_stop/destroy) against those. pd_timer_stop is
     synchronous on IOCP (DeleteTimerQueueTimer(INVALID_HANDLE_VALUE) waits
     for the thread-pool callback) and pd_timer_destroy drains the IOCP via
     iocp_drain_sync; neither needs the timer_actor's loop thread to take
     loop_lock, so holding loop_lock across them cannot deadlock with the
     loop thread (which never takes loop_lock). _timer_actor_untrack returns
     false if the timer was already removed (a duplicate cancel, or teardown
     via _timer_actor_destroy_all_tracked); in that case the pointer is
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

void timer_actor_cancel_target(timer_actor_t* timer_actor, actor_t* target) {
  if (timer_actor == NULL || target == NULL) {
    return;
  }

  /* Synchronous cancel-by-target: remove every debounce entry and every
     active timer whose completion payload targets `target`, inline under
     loop_lock. This is the F8 fix: a short-lived target actor can be freed
     before an in-flight TIMER_COMPLETION (already enqueued by
     _timer_completion_callback on the pd-loop thread) is processed by the
     timer_actor's dispatch. Without this cancel, the dispatch's
     actor_send(payload->target, ...) would read freed flags -> UAF.

     The destroyer calls cancel_target BEFORE freeing `target`; any
     in-flight completion is then dropped by the TIMER_COMPLETION dispatch
     re-check (the target is no longer in the debounce_map nor in
     active_timers). The re-check compares pointer VALUES (no dereference),
     so a freed target's stale pointer value is safe to compare.

     Takes the same loop_lock as timer_actor_cancel / timer_actor_set / the
     TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatches / teardown, so the
     debounce_map and active_timers mutations are serialized. pd_timer_stop
     and pd_timer_destroy do not need the timer_actor's loop thread to take
     loop_lock, so holding it across them cannot deadlock with the loop
     thread (which never takes loop_lock). _timer_actor_untrack returns
     false if a timer was already removed (a concurrent cancel, or teardown
     via _timer_actor_destroy_all_tracked); in that case the pointer is
     already freed and must not be touched. */
  platform_mutex_lock(timer_actor->loop_lock);

  /* Cancel every debounce entry for this target. Mirrors the per-entry
     cleanup in the TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatches. */
  for (size_t index = 0; index < MAX_DEBOUNCE_KEYS; index++) {
    debounce_entry_t* entry = &timer_actor->debounce_map[index];
    if (entry->target != target) {
      continue;
    }
    if (entry->timer != NULL) {
      pd_timer_stop(entry->timer);
      void* old_user_data = entry->timer->user_data;
      _timer_actor_untrack(timer_actor, entry->timer);
      pd_timer_destroy(entry->timer);
      free(old_user_data);
      entry->timer = NULL;
      entry->completion_payload = NULL;
    }
    entry->target = NULL;
    entry->completion_type = 0;
  }

  /* Cancel any non-debounce timers (from timer_actor_set) whose completion
     payload targets this actor. _timer_actor_untrack does swap-removal, so
     on a match do not advance the index — the swapped-in last element must
     be re-checked at the same slot. A while loop (rather than a for-loop
     with index--) avoids the size_t underflow at index 0. */
  size_t index = 0;
  while (index < timer_actor->active_timer_count) {
    pd_timer_t* timer = timer_actor->active_timers[index];
    if (timer == NULL) {
      index++;
      continue;
    }
    timer_completion_payload_t* completion =
        (timer_completion_payload_t*)timer->user_data;
    if (completion == NULL || completion->target != target) {
      index++;
      continue;
    }
    pd_timer_stop(timer);
    _timer_actor_untrack(timer_actor, timer);
    pd_timer_destroy(timer);
    free(completion);
    /* Don't advance index — the swap brought the previous last element
       into this slot, and it may also target `target`. */
  }

  platform_mutex_unlock(timer_actor->loop_lock);
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
