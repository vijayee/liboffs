//
// Created by victor on 5/14/25.
//

#ifndef OFFS_BLOOM_FILTER_H
#define OFFS_BLOOM_FILTER_H

#include "bitset.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct bloom_filter_t {
  bitset_t* bits;
  size_t size;          // number of bits
  uint32_t hash_count;  // k hash functions
  uint64_t seed_a;
  uint64_t seed_b;
  size_t count;         // items inserted
} bloom_filter_t;

bloom_filter_t* bloom_filter_create(size_t size, uint32_t hash_count);
void bloom_filter_destroy(bloom_filter_t* filter);

bool bloom_filter_add(bloom_filter_t* filter, const uint8_t* data, size_t len);
bool bloom_filter_contains(const bloom_filter_t* filter, const uint8_t* data, size_t len);

size_t bloom_filter_count(const bloom_filter_t* filter);
size_t bloom_filter_size(const bloom_filter_t* filter);
void bloom_filter_reset(bloom_filter_t* filter);

void bloom_filter_optimal_size(size_t expected_items, double false_positive_rate,
                               size_t* out_size, uint32_t* out_hash_count);

#endif // OFFS_BLOOM_FILTER_H