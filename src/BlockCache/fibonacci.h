//
// Created by victor on 4/28/25.
//

#ifndef OFFS_FIBONACCI_H
#define OFFS_FIBONACCI_H
#include <stdint.h>
#include <cbor.h>
uint32_t fibonacci(uint32_t num);

typedef struct {
  uint32_t fib;
  uint32_t count;
  uint32_t threshold;
} fibonacci_hit_counter_t;

uint32_t fibonacci(uint32_t num);
fibonacci_hit_counter_t fibonacci_hit_counter_create();
fibonacci_hit_counter_t fibonacci_hit_counter_from(uint64_t fib, uint64_t count);
uint8_t fibonacci_hit_counter_increment(fibonacci_hit_counter_t* counter);
uint8_t fibonacci_hit_counter_decrement(fibonacci_hit_counter_t* counter);
int fibonacci_hit_counter_compare(fibonacci_hit_counter_t* counter1, fibonacci_hit_counter_t* counter2);
cbor_item_t* fibonacci_hit_counter_to_cbor(fibonacci_hit_counter_t* counter);
fibonacci_hit_counter_t cbor_to_fibonacci_hit_counter(cbor_item_t* cbor);
#endif //OFFS_FIBONACCI_H
