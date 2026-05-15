//
// Created by victor on 5/14/25.
//

#include "bitset.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

bitset_t* bitset_create(size_t byte_size) {
  if (byte_size == 0) byte_size = 1;
  bitset_t* set = get_clear_memory(sizeof(bitset_t));
  set->data = get_clear_memory(byte_size);
  set->size = byte_size;
  return set;
}

void bitset_destroy(bitset_t* set) {
  if (set == NULL) return;
  free(set->data);
  free(set);
}

static bool bitset_ensure_capacity(bitset_t* set, size_t bit_index) {
  size_t byte_index = bit_index / 8 + 1;
  if (byte_index <= set->size) return true;
  size_t new_size = byte_index * 2;
  uint8_t* new_data = realloc(set->data, new_size);
  if (new_data == NULL) return false;
  memset(new_data + set->size, 0, new_size - set->size);
  set->data = new_data;
  set->size = new_size;
  return true;
}

bool bitset_get(const bitset_t* set, size_t bit_index) {
  if (set == NULL) return false;
  size_t byte_index = bit_index / 8;
  if (byte_index >= set->size) return false;
  size_t bit_offset = bit_index % 8;
  return (set->data[byte_index] >> bit_offset) & 1;
}

bool bitset_set(bitset_t* set, size_t bit_index, bool value) {
  if (set == NULL) return false;
  if (!bitset_ensure_capacity(set, bit_index)) return false;
  size_t byte_index = bit_index / 8;
  size_t bit_offset = bit_index % 8;
  bool old = (set->data[byte_index] >> bit_offset) & 1;
  if (value) {
    set->data[byte_index] |= (1 << bit_offset);
  } else {
    set->data[byte_index] &= ~(1 << bit_offset);
  }
  return old;
}

int bitset_compare(const bitset_t* left, const bitset_t* right) {
  if (left == NULL && right == NULL) return 0;
  if (left == NULL) return -1;
  if (right == NULL) return 1;
  size_t min_size = left->size < right->size ? left->size : right->size;
  int cmp = memcmp(left->data, right->data, min_size);
  if (cmp != 0) return cmp;
  if (left->size < right->size) return -1;
  if (left->size > right->size) return 1;
  return 0;
}

bool bitset_eq(const bitset_t* left, const bitset_t* right) {
  return bitset_compare(left, right) == 0;
}

typedef uint8_t (*bitwise_op_fn)(uint8_t left, uint8_t right);

static bitset_t* bitset_bitwise_op(const bitset_t* left, const bitset_t* right, bitwise_op_fn op) {
  if (left == NULL || right == NULL) return NULL;
  size_t max_size = left->size > right->size ? left->size : right->size;
  bitset_t* result = bitset_create(max_size);
  for (size_t index = 0; index < max_size; index++) {
    uint8_t left_byte = index < left->size ? left->data[index] : 0;
    uint8_t right_byte = index < right->size ? right->data[index] : 0;
    result->data[index] = op(left_byte, right_byte);
  }
  return result;
}

static uint8_t xor_op(uint8_t left, uint8_t right) { return left ^ right; }
static uint8_t and_op(uint8_t left, uint8_t right) { return left & right; }
static uint8_t or_op(uint8_t left, uint8_t right) { return left | right; }

bitset_t* bitset_xor(const bitset_t* left, const bitset_t* right) {
  return bitset_bitwise_op(left, right, xor_op);
}

bitset_t* bitset_and(const bitset_t* left, const bitset_t* right) {
  return bitset_bitwise_op(left, right, and_op);
}

bitset_t* bitset_or(const bitset_t* left, const bitset_t* right) {
  return bitset_bitwise_op(left, right, or_op);
}

bitset_t* bitset_not(const bitset_t* set) {
  if (set == NULL) return NULL;
  bitset_t* result = bitset_create(set->size);
  for (size_t index = 0; index < set->size; index++) {
    result->data[index] = ~set->data[index];
  }
  return result;
}

size_t bitset_size(const bitset_t* set) {
  return set == NULL ? 0 : set->size;
}

size_t bitset_bit_count(const bitset_t* set) {
  return set == NULL ? 0 : set->size * 8;
}

void bitset_compact(bitset_t* set) {
  if (set == NULL) return;
  size_t new_size = set->size;
  while (new_size > 1 && set->data[new_size - 1] == 0) {
    new_size--;
  }
  if (new_size == set->size) return;
  uint8_t* new_data = realloc(set->data, new_size);
  if (new_data != NULL) {
    set->data = new_data;
    set->size = new_size;
  }
}