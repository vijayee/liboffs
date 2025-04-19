//
// Created by victor on 4/15/25.
//

#ifndef OFFS_QUEUE_H
#define OFFS_QUEUE_H
#include "work.h"
#if _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
typedef struct work_queue_item_t work_queue_item_t;
struct work_queue_item_t {
  work_t* work;
  work_queue_item_t* next;
  work_queue_item_t* last;
};
typedef struct {
#if _WIN32
  CRITICAL_SECTION lock;
#else
  pthread_mutex_t lock;
#endif
  work_queue_item_t* first;
  work_queue_item_t* last;
} work_queue_t;

void work_queue_init();
void work_enqueue(work_t* work);
work_t* work_dequeue();

#endif //OFFS_QUEUE_H
