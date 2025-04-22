//
// Created by victor on 4/15/25.
//

#ifndef OFFS_QUEUE_H
#define OFFS_QUEUE_H
#include "work.h"


typedef struct work_queue_item_t work_queue_item_t;
struct work_queue_item_t {
  work_t* work;
  work_queue_item_t* next;
  work_queue_item_t* last;
};
typedef struct {
  work_queue_item_t* first;
  work_queue_item_t* last;
} work_queue_t;

work_queue_t* work_queue_init(work_queue_t* queue);
void work_enqueue(work_queue_t* queue, work_t* work);
work_t* work_dequeue(work_queue_t* queue);

#endif //OFFS_QUEUE_H
