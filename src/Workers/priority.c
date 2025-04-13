//
// Created by victor on 4/8/25.
//
#include "priority.h"
#include <time.h>
#include <stdint.h>
priority_t current = {0};

priority_t get_next_priority() {
  if ((current.time == 0) && (current.count == 0)) {
    current.time = (uint64_t) time(NULL);
    return current;
  }

  priority_t next = {0};
  next.time = (uint64_t) time(NULL);
  int cmp = priority_compare(&next, &current);
  if (cmp == 1) {
    current = next;
    return next;
  } else {
    next.count = current.count;
    while (cmp == 0 || cmp == -1) {
      next.count++;
      cmp = priority_compare(&next, &current);
    }
    current = next;
    return next;
  }
}

int priority_compare(priority_t* priority1, priority_t* priority2) {
  if (priority1->time > priority2->time ) {
    return 1;
  } else if (priority1->time < priority2->time) {
    return -1;
  } else if (priority1->count > priority2->count) {
    return 1;
  } else if (priority1->count < priority2->count) {
    return -1;
  } else {
    return 0;
  }
}
