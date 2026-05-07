//
// Created by victor on 5/7/26.
//

#ifndef OFFS_TUPLE_H
#define OFFS_TUPLE_H

#include "../Buffer/buffer.h"
#include "../RefCounter/refcounter.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  refcounter_t refcounter;
  buffer_t** hashes;
  size_t count;
  size_t capacity;
} tuple_t;

tuple_t* tuple_create(size_t capacity);
void tuple_destroy(tuple_t* tuple);

buffer_t* tuple_get(tuple_t* tuple, size_t index);
void tuple_set(tuple_t* tuple, size_t index, buffer_t* hash);
void tuple_push(tuple_t* tuple, buffer_t* hash);
buffer_t* tuple_shift(tuple_t* tuple);
size_t tuple_size(tuple_t* tuple);
uint64_t tuple_hash(tuple_t* tuple);
uint8_t tuple_equals(tuple_t* a, tuple_t* b);

#endif //OFFS_TUPLE_H