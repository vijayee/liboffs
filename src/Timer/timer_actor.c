//
// Created by victor on 5/6/25.
//

#include "timer_actor.h"
#include "../Actor/message.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include <poll-dancer/poll-dancer.h>
#include <internal/timer.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static void _timer_actor_track(timer_actor_t* ta, pd_timer_t* timer) {
  if (ta->active_timer_count >= ta->active_timer_capacity) {
    size_t new_cap = ta->active_timer_capacity == 0 ? 8 : ta->active_timer_capacity * 2;
    pd_timer_t** new_arr = realloc(ta->active_timers, new_cap * sizeof(pd_timer_t*));
    if (new_arr == NULL) return;
    ta->active_timers = new_arr;
    ta->active_timer_capacity = new_cap;
  }
  ta->active_timers[ta->active_timer_count++] = timer;
}

static void _timer_actor_untrack(timer_actor_t* ta, pd_timer_t* timer) {
  for (size_t i = 0; i < ta->active_timer_count; i++) {
    if (ta->active_timers[i] == timer) {
      ta->active_timers[i] = ta->active_timers[ta->active_timer_count - 1];
      ta->active_timer_count--;
      return;
    }
  }
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
  message_t msg;
  msg.type = copy->completion_type;
  msg.payload = copy;
  msg.payload_destroy = free;
  actor_send(copy->target, &msg);
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
      pd_timer_t* timer = pd_timer_create(
          timer_actor->loop, payload->timeout_ms, payload->interval_ms,
          _timer_completion_callback, completion);
      if (timer != NULL) {
        pd_timer_start(timer);
        completion->timer_id = (uint64_t)(uintptr_t)timer;
        _timer_actor_track(timer_actor, timer);
      }
      if (msg->payload_destroy != NULL) {
        msg->payload_destroy(msg->payload);
      }
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
    case TIMER_CANCEL: {
      timer_cancel_payload_t* payload = (timer_cancel_payload_t*)msg->payload;
      if (payload->timer_id != 0) {
        pd_timer_t* timer = (pd_timer_t*)(uintptr_t)payload->timer_id;
        pd_timer_stop(timer);
        /* The completion payload (user_data) was allocated in TIMER_SET/
           TIMER_DEBOUNCE and must be freed when the timer is cancelled. */
        void* user_data = timer->user_data;
        _timer_actor_untrack(timer_actor, timer);
        pd_timer_destroy(timer);
        free(user_data);
      }
      if (msg->payload_destroy != NULL) {
        msg->payload_destroy(msg->payload);
      }
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
    case TIMER_DEBOUNCE: {
      timer_debounce_payload_t* payload = (timer_debounce_payload_t*)msg->payload;
      debounce_entry_t* entry = _timer_actor_find_debounce(
          timer_actor, payload->target, payload->completion_type);
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
      pd_timer_t* timer = pd_timer_create(
          timer_actor->loop, payload->timeout_ms, payload->interval_ms,
          _timer_completion_callback, completion);
      if (timer != NULL && entry != NULL) {
        pd_timer_start(timer);
        completion->timer_id = (uint64_t)(uintptr_t)timer;
        _timer_actor_track(timer_actor, timer);
        entry->target = payload->target;
        entry->completion_type = payload->completion_type;
        entry->timer = timer;
        entry->completion_payload = completion;
      }
      if (msg->payload_destroy != NULL) {
        msg->payload_destroy(msg->payload);
      }
      msg->payload_destroy = NULL;
      msg->payload = NULL;
      break;
    }
    default:
      break;
  }
}

static void* _timer_actor_thread(void* arg) {
  platform_setup_thread_stack();
  timer_actor_t* timer_actor = (timer_actor_t*)arg;

  while (atomic_load(&timer_actor->running)) {
    /* Process any pending actor messages before waiting for events */
    actor_run(&timer_actor->actor, ACTOR_BATCH_SIZE);

    /* Run one iteration of the event loop with a short timeout.
       pd_loop_async_send from other threads wakes this up early. */
    int result = pd_loop_run_once(timer_actor->loop, 100);
    if (result < 0) {
      break;
    }
  }

  return NULL;
}

timer_actor_t* timer_actor_create(void) {
  timer_actor_t* timer_actor = get_clear_memory(sizeof(timer_actor_t));
  actor_init(&timer_actor->actor, timer_actor, _timer_actor_dispatch);
  timer_actor->loop = pd_loop_create(NULL);
  if (timer_actor->loop == NULL) {
    free(timer_actor);
    return NULL;
  }
  atomic_store(&timer_actor->running, 1);
#ifdef _WIN32
  timer_actor->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)_timer_actor_thread,
                                     timer_actor, 0, NULL);
#else
  pthread_create(&timer_actor->thread, NULL, _timer_actor_thread, timer_actor);
#endif
  return timer_actor;
}

void timer_actor_destroy(timer_actor_t* timer_actor) {
  if (timer_actor == NULL) {
    return;
  }
  atomic_store(&timer_actor->running, 0);
  /* Wake the loop so it notices the stop flag */
  pd_loop_async_send(timer_actor->loop, timer_actor);
  pd_loop_stop(timer_actor->loop);
#ifdef _WIN32
  WaitForSingleObject(timer_actor->thread, INFINITE);
#else
  pthread_join(timer_actor->thread, NULL);
#endif
  /* Stop and destroy all active timers, freeing their completion payloads. */
  for (size_t i = 0; i < timer_actor->active_timer_count; i++) {
    pd_timer_t* timer = timer_actor->active_timers[i];
    if (timer != NULL) {
      void* user_data = timer->user_data;
      pd_timer_stop(timer);
      pd_timer_destroy(timer);
      free(user_data);
    }
  }
  free(timer_actor->active_timers);
  pd_loop_destroy(timer_actor->loop);
  actor_destroy(&timer_actor->actor);
  free(timer_actor);
}

uint64_t timer_actor_set(timer_actor_t* timer_actor, uint64_t timeout_ms,
                         uint64_t interval_ms, actor_t* target,
                         uint32_t completion_type) {
  timer_set_payload_t* payload = get_clear_memory(sizeof(timer_set_payload_t));
  payload->timeout_ms = timeout_ms;
  payload->interval_ms = interval_ms;
  payload->target = target;
  payload->completion_type = completion_type;
  payload->timer_id = 0;

  message_t msg;
  msg.type = TIMER_SET;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  pd_loop_async_send(timer_actor->loop, timer_actor);

  /* The timer_id is filled in by the dispatch on the timer thread.
     Since actor_send is async, we return 0 here. Callers should use
     TIMER_DEBOUNCE for auto-cancelling repeating timers. */
  return 0;
}

void timer_actor_cancel(timer_actor_t* timer_actor, uint64_t timer_id) {
  timer_cancel_payload_t* payload = get_clear_memory(sizeof(timer_cancel_payload_t));
  payload->timer_id = timer_id;

  message_t msg;
  msg.type = TIMER_CANCEL;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  pd_loop_async_send(timer_actor->loop, timer_actor);
}

uint64_t timer_actor_debounce(timer_actor_t* timer_actor,
                              uint64_t timeout_ms, uint64_t interval_ms,
                              actor_t* target, uint32_t completion_type) {
  timer_debounce_payload_t* payload = get_clear_memory(sizeof(timer_debounce_payload_t));
  payload->timeout_ms = timeout_ms;
  payload->interval_ms = interval_ms;
  payload->target = target;
  payload->completion_type = completion_type;

  message_t msg;
  msg.type = TIMER_DEBOUNCE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  pd_loop_async_send(timer_actor->loop, timer_actor);

  return 0;
}