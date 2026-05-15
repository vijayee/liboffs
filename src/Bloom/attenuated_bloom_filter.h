//
// Created by victor on 5/14/25.
//

#ifndef OFFS_ATTENUATED_BLOOM_FILTER_H
#define OFFS_ATTENUATED_BLOOM_FILTER_H

#include "elastic_bloom_filter.h"
#include <cbor.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct attenuated_bloom_filter_t {
  uint32_t level_count;
  elastic_bloom_filter_t** levels;
} attenuated_bloom_filter_t;

attenuated_bloom_filter_t* attenuated_bloom_filter_create(uint32_t levels, size_t size,
                                                           uint32_t hash_count,
                                                           float omega, uint32_t fp_bits);
void attenuated_bloom_filter_destroy(attenuated_bloom_filter_t* abf);

bool attenuated_bloom_filter_subscribe(attenuated_bloom_filter_t* abf,
                                        const uint8_t* topic, size_t topic_len);
bool attenuated_bloom_filter_unsubscribe(attenuated_bloom_filter_t* abf,
                                          const uint8_t* topic, size_t topic_len);
bool attenuated_bloom_filter_check(const attenuated_bloom_filter_t* abf,
                                   const uint8_t* topic, size_t topic_len,
                                   uint32_t* out_hops);

int attenuated_bloom_filter_merge(attenuated_bloom_filter_t* dest,
                                   const attenuated_bloom_filter_t* src);

elastic_bloom_filter_t* attenuated_bloom_filter_get_level(attenuated_bloom_filter_t* abf, uint32_t level);
uint32_t attenuated_bloom_filter_level_count(const attenuated_bloom_filter_t* abf);

cbor_item_t* attenuated_bloom_filter_encode(const attenuated_bloom_filter_t* abf);
attenuated_bloom_filter_t* attenuated_bloom_filter_decode(cbor_item_t* item);

#endif // OFFS_ATTENUATED_BLOOM_FILTER_H