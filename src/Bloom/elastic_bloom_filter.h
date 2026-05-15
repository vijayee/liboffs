//
// Created by victor on 5/14/25.
//

#ifndef OFFS_ELASTIC_BLOOM_FILTER_H
#define OFFS_ELASTIC_BLOOM_FILTER_H

#include "bitset.h"
#include <cbor.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EBF_DEFAULT_FP_BITS 8

typedef struct ebf_bucket_entry_t {
  uint32_t fingerprint;
  struct ebf_bucket_entry_t* next;
} ebf_bucket_entry_t;

typedef struct elastic_bloom_filter_t {
  bitset_t* bits;
  size_t size;
  size_t bucket_count;
  ebf_bucket_entry_t** buckets;
  uint32_t hash_count;
  uint64_t seed_a;
  uint64_t seed_b;
  float omega;
  uint32_t fp_bits;
  size_t count;
} elastic_bloom_filter_t;

typedef void (*ebf_entry_cb_t)(void* ctx, size_t bucket_idx, uint32_t fingerprint);

elastic_bloom_filter_t* elastic_bloom_filter_create(size_t size, uint32_t hash_count,
                                                     float omega, uint32_t fp_bits);
void elastic_bloom_filter_destroy(elastic_bloom_filter_t* ebf);

bool elastic_bloom_filter_add(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len);
bool elastic_bloom_filter_contains(const elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len);
bool elastic_bloom_filter_remove(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len);

int elastic_bloom_filter_expand(elastic_bloom_filter_t* ebf);
int elastic_bloom_filter_compress(elastic_bloom_filter_t* ebf);
int elastic_bloom_filter_merge(elastic_bloom_filter_t* dest, const elastic_bloom_filter_t* src);

size_t elastic_bloom_filter_count(const elastic_bloom_filter_t* ebf);
size_t elastic_bloom_filter_size(const elastic_bloom_filter_t* ebf);
float elastic_bloom_filter_ratio(const elastic_bloom_filter_t* ebf);

void elastic_bloom_filter_iterate(const elastic_bloom_filter_t* ebf, ebf_entry_cb_t callback, void* ctx);
int elastic_bloom_filter_bucket_insert(elastic_bloom_filter_t* ebf, size_t bucket_idx, uint32_t fingerprint);

cbor_item_t* elastic_bloom_filter_encode(const elastic_bloom_filter_t* ebf);
elastic_bloom_filter_t* elastic_bloom_filter_decode(cbor_item_t* item);

#endif // OFFS_ELASTIC_BLOOM_FILTER_H