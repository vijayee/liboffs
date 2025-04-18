//
// Created by victor on 4/8/25.
//

#ifndef OFFS_WORK_H
#define OFFS_WORK_H
#include "priority.h"
#include "../RefCounter/refcounter.h"
#include "../RefCounter/refcounter.p.h"
typedef struct {
  refcounter_t refcounter;
  priority_t priority;
  void* ctx;
  void (* execute)(void*);
} work_t;

work_t* work_create(priority_t priority, void* ctx, void (* execute)(void*));
void work_execute(work_t* work);
void work_destroy(work_t* work);
#endif //OFFS_WORK_H
