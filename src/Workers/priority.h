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
void priority_init();
priority_t priority_get_next();
int priority_compare(priority_t* priority1, priority_t* priority2);
#endif //OFFS_PRIORITY_H
