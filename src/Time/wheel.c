//
// Created by victor on 6/17/25.
//
#include "wheel.h"
#include "../Workers/work.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"


void timing_wheel_on_tick();
void timing_wheel_on_abort_tick();
void timing_wheel_worker_execute(void* ctx);
void timing_wheel_worker_abort(void* ctx);
timer_st* timer_list_remove_by_id(timer_list_t* list, size_t timerId);
timer_st* timer_list_remove(timer_list_t* list, timer_list_node_t* node);
timer_list_t* timing_wheel_maintenance(timing_wheel_t* wheel);
void timing_wheel_fire_expired(timing_wheel_t* wheel, timer_list_t* expired);

timer_list_t*  timer_list_create() {
  timer_list_t* list = get_clear_memory(sizeof(timer_list_t*));
  list->first = NULL;
  list->last = NULL;
  return list;
}

void timer_list_destroy(timer_list_t* list) {
  timer_list_node_t* current = list->first;
  timer_list_node_t* next = NULL;
  while (current != NULL ) {
    next = current->next;
    free(current->timer);
    free(current);
    current = next;
  }
}

void timer_list_enqueue(timer_list_t* list, timer_st* timer) {
  timer_list_node_t* node = get_clear_memory(sizeof(timer_list_t ));
  node->timer = timer;
  node->previous = NULL;
  node->next = NULL;
  if ((list->last == NULL) && (list->first == NULL)) {
    list->first = node;
    list->last = node;
  } else {
    node->previous = list->last;
    list->last->next= node;
    list->last = node;
  }
}


timer_st* timer_list_dequeue(timer_list_t* list) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  } else {
    timer_list_node_t* node = list->first;
    list->first = node->next;
    if (node->next != NULL) {
      list->first->previous = NULL;
    }
    if (list->last == node) {
      list->last = NULL;
    }
    timer_st* timer = node->timer;
    free(node);
    return timer;
  }
}

timer_st* timer_list_remove_by_id(timer_list_t* list, size_t timerId) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  }
  timer_list_node_t* current = list->first;
  timer_list_node_t* previous = NULL;
  while (current != NULL) {
    if (current->timer->timerId == timerId) {
      if ((previous != NULL) && (current->next != NULL)) {
        previous->next = current->next;
        current->next->previous = previous;
      } if (previous != NULL) {
        previous->next= NULL;
      } else {
        list->first = NULL;
        list->last = NULL;
      }
      timer_st* timer = current->timer;
      free(current);
      return timer;
    } else {
      previous = current;
      current = current->next;
    }
  }
  return NULL;
}

timer_st* timer_list_remove(timer_list_t* list, timer_list_node_t* node) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  }
  if (list->last == node) {
    list->last = node->previous;
  }
  if (list->first == node) {
    list->first = node->next;
  }
  if (node->previous != NULL) {
    node->previous->next = node->next;
  }
  if (node->next != NULL) {
    node->next->previous = node->previous;
  }
  timer_st* timer = node->timer;
  free(node);
  return timer;
}

timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool) {
  timing_wheel_t* wheel = get_clear_memory(sizeof(timing_wheel_t));
  refcounter_init((refcounter_t*) wheel);
  platform_lock_init(&wheel->lock);
  wheel->pool = pool;
  wheel->interval = interval;
  wheel->position = slot_count - 1;
  wheel->slots = get_clear_memory(sizeof(slots_t));
  vec_init(wheel->slots);
  vec_reserve(wheel->slots, slot_count);
  for (size_t i = 0; i < slot_count; i++) {
    vec_push(wheel->slots, timer_list_create());
  }
  hashmap_init(&wheel->timers, (void*)hash_uint64, (void*)compare_uint64);
  hashmap_set_key_alloc_funcs(&wheel->timers, duplicate_uint64, (void*)free);
  return wheel;
}

void timing_wheel_destroy(timing_wheel_t* wheel) {
  refcounter_dereference((refcounter_t*) wheel);
  if (refcounter_count((refcounter_t*) wheel)) {
    hashmap_cleanup(&wheel->timers);
    for (size_t i = 0; i < wheel->slots->length; i++) {
      timer_list_t* list = wheel->slots->data[i];
      timer_list_destroy(list);
    }
    vec_deinit(wheel->slots);
    //free(&wheel->slots);
    platform_lock_destroy(&wheel->lock);
    free(wheel);
  }
}

void timing_wheel_run(timing_wheel_t* wheel) {
  wheel->stopped = 0;
  priority_t priority = {0};
  work_t* work = work_create(priority, (void*) wheel, timing_wheel_worker_execute, timing_wheel_worker_abort);
  work_pool_enqueue(wheel->pool, work);
}

void timing_wheel_on_tick(void* ctx) {
  timing_wheel_t* wheel = (timing_wheel_t*) ctx;
  if (wheel->stopped == 1) {
    return;
  }
  timer_list_t* expired = NULL;
  platform_lock(&wheel->lock);
  wheel->position = (wheel->position + 1) % wheel->slots->length;
  expired = timing_wheel_maintenance(wheel);
  platform_unlock(&wheel->lock);
  timing_wheel_run(wheel);
  if(expired != NULL) {
    timing_wheel_fire_expired(wheel, expired);
  }
}

void timing_wheel_fire_expired(timing_wheel_t* wheel, timer_list_t* expired) {
  timer_st* current = timer_list_dequeue(expired);
  while (current != NULL) {
    priority_t priority = {0};
    work_t* work = work_create(priority, current->ctx, current->cb, current->abort);
    work_pool_enqueue(wheel->pool, work);
    free(current);
    current= timer_list_dequeue(expired);
  }
  timer_list_destroy(expired);
}

timer_list_t* timing_wheel_maintenance(timing_wheel_t* wheel) {
  timer_list_t* list = wheel->slots->data[wheel->position];
  timer_list_t* expired = NULL;
  timer_list_node_t* current = list->first;
  while (current != NULL) {
    timer_list_node_t* next = NULL;
    timer_st* timer = current->timer;
    if (timer->removed) {
      next = current->next;
      timer_list_remove(list, current);
      current = next;
      continue;
    } else if (timer->circle > 0) {
      timer->circle--;
      current = current->next;
      continue;
    } /*else if (timer->diff > 0) {
      next = current->next;
      timer = current->timer;
      timer_list_remove(list, current);
      size_t position = (wheel->position + timer->diff) % wheel->slots->length;
      timer_list_enqueue(wheel->slots->data[position], timer);
      timer->diff = 0;
      current= next;
      continue;
    }*/
    if (expired == NULL) {
      expired = timer_list_create();
    }
    timer_list_enqueue(expired, timer);
    next = current-> next;
    timer_list_remove(list, current);
    hashmap_remove(&wheel->timers, &timer->timerId);
    current = next;
  }
  return expired;
}

void timing_wheel_worker_execute(void* ctx) {
  timing_wheel_t* wheel = (timing_wheel_t*) ctx;
  ticker_t ticker = {0};
  ticker.ctx = ctx;
  ticker.cb = timing_wheel_on_tick;
  ticker_start(ticker, wheel->simulated == 1 ? 0 : wheel->interval);
}

void timing_wheel_worker_abort(void* ctx) {
  timing_wheel_t* wheel = (timing_wheel_t*) ctx;
  platform_lock(&wheel->lock);
  wheel->stopped = 1;
  for(size_t i = 0; i < wheel->slots->length; i++) {
    timer_list_t* list = wheel->slots->data[i];
    timer_st* current = timer_list_dequeue(list);
    while (current != NULL) {
      current->abort(current->ctx);
      hashmap_remove(&wheel->timers, &current->timerId);
      free(current);
    }
  }
  platform_unlock(&wheel->lock);
}

uint64_t timing_wheel_set_timer(timing_wheel_t* wheel, void* ctx, void (* cb)(void*), void (* abort)(void*), uint64_t delay) {
  timer_st* timer = get_clear_memory(sizeof(timer_st));
  timer->delay = delay;
  timer->cb = cb;
  timer->abort = abort;
  timer->ctx = ctx;
  size_t timerId;
  platform_lock(&wheel->lock);
  timerId = wheel->next_id++;
  timer->timerId = timerId;
  if (delay < wheel->interval) {
    timer->delay = wheel->interval;
  }
  size_t steps = delay / wheel->interval;
  size_t position = (wheel->position + steps) % wheel->slots->length;
  timer->circle = (steps - 1) / wheel->slots->length;
  timer_list_t* list = wheel->slots->data[position];
  timer_list_enqueue(list, timer);
  hashmap_put(&wheel->timers, &timer->timerId, timer);
  platform_unlock(&wheel->lock);
  return timerId;
}

void timing_wheel_cancel_timer(timing_wheel_t* wheel, uint64_t timerId) {
  platform_lock(&wheel->lock);
  timer_st* timer = hashmap_get(&wheel->timers, &timerId);
  if (timer != NULL) {
    timer->removed = 1;
    hashmap_remove(&wheel->timers, &timerId);
  }
  platform_unlock(&wheel->lock);
}