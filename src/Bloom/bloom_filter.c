//
// Created by victor on 5/14/25.
//

#include "bloom_filter.h"
#include "../Util/allocator.h"
#include <xxh3.h>
#include <math.h>
#include <string.h>

bloom_filter_t* bloom_filter_create(size_t size, uint32_t hash_count) {
  if (size == 0) size = 256;
  if (hash_count == 0) hash_count = 3;
  bloom_filter_t* filter = get_clear_memory(sizeof(bloom_filter_t));
  filter->bits = bitset_create((size + 7) / 8);
  filter->size = size;
  filter->hash_count = hash_count;
  filter->seed_a = 1;
  filter->seed_b = 2;
  filter->count = 0;
  return filter;
}

void bloom_filter_destroy(bloom_filter_t* filter) {
  if (filter == NULL) return;
  bitset_destroy(filter->bits);
  free(filter);
}

bool bloom_filter_add(bloom_filter_t* filter, const uint8_t* data, size_t len) {
  if (filter == NULL || data == NULL) return false;
  bool was_new = false;
  for (uint32_t index = 0; index < filter->hash_count; index++) {
    uint64_t hash_a = XXH3_64bits_withSeed(data, len, filter->seed_a);
    uint64_t hash_b = XXH3_64bits_withSeed(data, len, filter->seed_b);
    size_t bit_index = (hash_a + index * hash_b + index * index) % filter->size;
    if (!bitset_get(filter->bits, bit_index)) {
      was_new = true;
    }
    bitset_set(filter->bits, bit_index, true);
  }
  if (was_new) filter->count++;
  return was_new;
}

bool bloom_filter_contains(const bloom_filter_t* filter, const uint8_t* data, size_t len) {
  if (filter == NULL || data == NULL) return false;
  for (uint32_t index = 0; index < filter->hash_count; index++) {
    uint64_t hash_a = XXH3_64bits_withSeed(data, len, filter->seed_a);
    uint64_t hash_b = XXH3_64bits_withSeed(data, len, filter->seed_b);
    size_t bit_index = (hash_a + index * hash_b + index * index) % filter->size;
    if (!bitset_get(filter->bits, bit_index)) return false;
  }
  return true;
}

size_t bloom_filter_count(const bloom_filter_t* filter) {
  return filter == NULL ? 0 : filter->count;
}

size_t bloom_filter_size(const bloom_filter_t* filter) {
  return filter == NULL ? 0 : filter->size;
}

void bloom_filter_reset(bloom_filter_t* filter) {
  if (filter == NULL) return;
  bitset_destroy(filter->bits);
  filter->bits = bitset_create((filter->size + 7) / 8);
  filter->count = 0;
}

void bloom_filter_optimal_size(size_t expected_items, double false_positive_rate,
                               size_t* out_size, uint32_t* out_hash_count) {
  if (expected_items == 0) expected_items = 1;
  if (false_positive_rate <= 0.0 || false_positive_rate >= 1.0) false_positive_rate = 0.01;
  double ln2 = log(2.0);
  double size_bits = -((double)expected_items * log(false_positive_rate)) / (ln2 * ln2);
  *out_size = (size_t)ceil(size_bits);
  if (*out_size == 0) *out_size = 256;
  *out_hash_count = (uint32_t)round(ln2 * size_bits / (double)expected_items);
  if (*out_hash_count == 0) *out_hash_count = 1;
}