//
// Created by victor on 7/8/25.
//

#ifndef OFFS_DEBOUNCER_H
#define OFFS_DEBOUNCER_H
#include <stdint.h>
#include <time.h>
#include "wheel.h"
#include "../RefCounter/refcounter.h"
#if _WIN32
#include <windows.h>
typedef LARGE_INTEGER timeval_t;
#else
#include <sys/time.h>
typedef struct timeval timeval_t;
#endif


typedef struct {
  refcounter_t refcounter_t;
  uint64_t timerId;
  void* ctx;
  void (* cb)(void*);
  void (* abort)(void*);
  timeval_t interval_start;
  uint64_t wait;
  uint64_t max_wait;
  hierarchical_timing_wheel_t* wheel;
} debouncer_t;

debouncer_t* debouncer_create(hierarchical_timing_wheel_t* wheel, void* ctx, void (* cb)(void*), void (* abort)(void*), uint64_t wait, uint64_t max_wait);
void debouncer_destroy(debouncer_t* bouncer);
void debouncer_debounce(debouncer_t* bouncer);
uint64_t elapsed_time(timeval_t start, timeval_t end);
void get_time(timeval_t* tv);
#endif //OFFS_DEBOUNCER_H
