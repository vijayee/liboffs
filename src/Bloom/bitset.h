//
// Created by victor on 5/14/25.
//

#ifndef OFFS_BITSET_H
#define OFFS_BITSET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct bitset_t {
  uint8_t* data;
  size_t size;  // size in bytes
} bitset_t;

bitset_t* bitset_create(size_t byte_size);
void bitset_destroy(bitset_t* set);

bool bitset_get(const bitset_t* set, size_t bit_index);
bool bitset_set(bitset_t* set, size_t bit_index, bool value);

int bitset_compare(const bitset_t* left, const bitset_t* right);
bool bitset_eq(const bitset_t* left, const bitset_t* right);

bitset_t* bitset_xor(const bitset_t* left, const bitset_t* right);
bitset_t* bitset_and(const bitset_t* left, const bitset_t* right);
bitset_t* bitset_or(const bitset_t* left, const bitset_t* right);
bitset_t* bitset_not(const bitset_t* set);

size_t bitset_size(const bitset_t* set);
size_t bitset_bit_count(const bitset_t* set);
void bitset_compact(bitset_t* set);

#endif // OFFS_BITSET_H