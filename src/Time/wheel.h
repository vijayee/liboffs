//
// Created by victor on 6/17/25.
//

#ifndef OFFS_WHEEL_H
#define OFFS_WHEEL_H
#include <hashmap.h>
#include "../Util/vec.h"
#include "../RefCounter/refcounter.h"
#include "ticker.h"
#include "../Workers/pool.h"
#include "../Util//threadding.h"
#include <stdint.h>

#define Time_Milliseconds 1000000
#define Time_Seconds 1000000000

typedef struct {
  size_t timerId;
  uint64_t delay;
  void* ctx;
  void (* cb)(void*);
  void (* abort)(void*);
  uint8_t removed;
  int32_t circle;
} timer_st;

typedef struct timer_list_node_t timer_list_node_t;
struct timer_list_node_t {
  timer_st* timer;
  timer_list_node_t* next;
  timer_list_node_t* previous;
};
typedef struct {
  timer_list_node_t* first;
  timer_list_node_t* last;
} timer_list_t;

timer_list_t* timer_list_create();
void timer_list_destroy(timer_list_t* list);
void timer_list_enqueue(timer_list_t* list, timer_st* timer);
timer_st* timer_list_dequeue(timer_list_t* list);

typedef HASHMAP(size_t, timer_st) timer_map_t;
typedef vec_t(timer_list_t*) slots_t;

typedef struct {
  refcounter_t refcounter;
  PlATFORMLOCKTYPE(lock);
  size_t position;
  ticker_t ticker;
  timer_map_t timers;
  slots_t* slots;
  size_t next_id;
  uint64_t interval;
  work_pool_t* pool;
  uint8_t stopped;
  uint8_t simulated;
} timing_wheel_t;

timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool);
void timing_wheel_destroy(timing_wheel_t* wheel);
uint64_t timing_wheel_set_timer(timing_wheel_t* wheel, void* ctx, void (* cb)(void*), void (* abort)(void*), uint64_t delay);
void timing_wheel_cancel_timer(timing_wheel_t* wheel, uint64_t timerId);
void timing_wheel_stop(timing_wheel_t* wheel);
void timing_wheel_run(timing_wheel_t* wheel);

#endif //OFFS_WHEEL_H
