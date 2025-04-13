//
// Created by victor on 4/8/25.
//

#ifndef OFFS_PRIORITY_H
#define OFFS_PRIORITY_H
#include <stdint.h>
typedef struct {
  uint64_t time;
  uint64_t count;
} priority_t;
priority_t get_next_priority();
int priority_compare(priority_t* priority1, priority_t* priority2);
#endif //OFFS_PRIORITY_H
