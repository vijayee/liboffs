//
// Created by victor on 5/7/26.
//

#include "tuple.h"
#include "../Util/allocator.h"
#include <string.h>

tuple_t* tuple_create(size_t capacity) {
  tuple_t* tuple = get_clear_memory(sizeof(tuple_t));
  tuple->capacity = capacity;
  tuple->count = 0;
  tuple->hashes = get_clear_memory(sizeof(buffer_t*) * capacity);
  refcounter_init((refcounter_t*) tuple);
  return tuple;
}

void tuple_destroy(tuple_t* tuple) {
  refcounter_dereference((refcounter_t*) tuple);
  if (refcounter_count((refcounter_t*) tuple) == 0) {
    for (size_t i = 0; i < tuple->count; i++) {
      if (tuple->hashes[i] != NULL) {
        DESTROY(tuple->hashes[i], buffer);
      }
    }
    free(tuple->hashes);
    refcounter_destroy_lock((refcounter_t*) tuple);
    free(tuple);
  }
}

buffer_t* tuple_get(tuple_t* tuple, size_t index) {
  if (index >= tuple->count) {
    return NULL;
  }
  return tuple->hashes[index];
}

void tuple_set(tuple_t* tuple, size_t index, buffer_t* hash) {
  if (index >= tuple->capacity) {
    return;
  }
  if (tuple->hashes[index] != NULL) {
    DESTROY(tuple->hashes[index], buffer);
  }
  REFERENCE(hash, buffer_t);
  if (index >= tuple->count) {
    tuple->count = index + 1;
  }
  tuple->hashes[index] = hash;
}

void tuple_push(tuple_t* tuple, buffer_t* hash) {
  if (tuple->count >= tuple->capacity) {
    return;
  }
  REFERENCE(hash, buffer_t);
  tuple->hashes[tuple->count++] = hash;
}

buffer_t* tuple_shift(tuple_t* tuple) {
  if (tuple->count == 0) {
    return NULL;
  }
  buffer_t* first = tuple->hashes[0];
  refcounter_dereference((refcounter_t*) first);
  tuple->count--;
  memmove(tuple->hashes, tuple->hashes + 1, tuple->count * sizeof(buffer_t*));
  tuple->hashes[tuple->count] = NULL;
  return first;
}

size_t tuple_size(tuple_t* tuple) {
  return tuple->count;
}

uint64_t tuple_hash(tuple_t* tuple) {
  uint64_t hash = 5381;
  for (size_t i = 0; i < tuple->count; i++) {
    if (tuple->hashes[i] != NULL) {
      buffer_t* buf = tuple->hashes[i];
      for (size_t j = 0; j < buf->size; j++) {
        hash = ((hash << 5) + hash) + (uint64_t)buf->data[j];
      }
    }
  }
  return hash;
}

uint8_t tuple_equals(tuple_t* left, tuple_t* right) {
  if (left->count != right->count) {
    return 0;
  }
  for (size_t i = 0; i < left->count; i++) {
    if (left->hashes[i] == NULL || right->hashes[i] == NULL) {
      if (left->hashes[i] != right->hashes[i]) {
        return 0;
      }
      continue;
    }
    if (left->hashes[i]->size != right->hashes[i]->size) {
      return 0;
    }
    if (memcmp(left->hashes[i]->data, right->hashes[i]->data, left->hashes[i]->size) != 0) {
      return 0;
    }
  }
  return 1;
}