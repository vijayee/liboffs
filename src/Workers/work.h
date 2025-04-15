//
// Created by victor on 4/8/25.
//

#ifndef OFFS_WORK_H
#define OFFS_WORK_H
#include "priority.h"
typedef struct {
  priority_t priority;
  void* ctx;
  void (* run)(void*);
} work_t;
#endif //OFFS_WORK_H
