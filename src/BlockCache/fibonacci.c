//
// Created by victor on 4/28/25.
//
#include "fibonacci.h"

uint32_t fibonacci(uint32_t num) {
  if((num == 1) || (num == 0)) {
    return num;
  } else {
    uint32_t current = 0;
    uint32_t parent = 1;
    uint32_t grandparent = 0;
    for (int32_t i = 2; i < (num + 1); i++) {
      current = parent + grandparent;
      grandparent = parent;
      parent = current;
    }
    return current;
  }
}

fibonacci_hit_counter_t fibonacci_hit_counter_create() {
  fibonacci_hit_counter_t counter = {0};
  counter.threshold = fibonacci(counter.fib);
  return counter;
}

fibonacci_hit_counter_t fibonacci_hit_counter_from(uint64_t fib, uint64_t count) {
  fibonacci_hit_counter_t counter = {0};
  counter.fib = fib;
  counter.count = count;
  counter.threshold = fibonacci(counter.fib);
  return counter;
}

uint8_t fibonacci_hit_counter_increment(fibonacci_hit_counter_t* counter) {
  counter->count += 1;
  if (counter->count > counter->threshold) {
    counter->fib += 1;
    counter->count = 0;
    counter->threshold = fibonacci(counter->fib);
    return 1;
  } else{
    return 0;
  }
}
uint8_t fibonacci_hit_counter_decrement(fibonacci_hit_counter_t* counter) {
  if (counter->fib == 0) {
    if (counter->count == 0) {
      return 0;
    } else {
      counter->count -= 1;
      return 0;
    }
  } else if (counter->count == 0) {
    counter->fib -= 1;
    counter->threshold = fibonacci(counter->fib);
    counter->count = counter->threshold;
    return 1;
  } else {
    counter->count -= 1;;
    return 0;
  }
}
int fibonacci_hit_counter_compare(fibonacci_hit_counter_t* counter1, fibonacci_hit_counter_t* counter2) {
  if (counter1->fib > counter2->fib ) {
    return 1;
  } else if (counter1->fib < counter2->fib) {
    return -1;
  } else if (counter1->count > counter2->count) {
    return 1;
  } else if (counter1->count < counter2->count) {
    return -1;
  } else {
    return 0;
  }
}