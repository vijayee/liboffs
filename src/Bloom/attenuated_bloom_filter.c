//
// Created by victor on 5/14/25.
//

#include "attenuated_bloom_filter.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

attenuated_bloom_filter_t* attenuated_bloom_filter_create(uint32_t levels, size_t size,
                                                           uint32_t hash_count,
                                                           float omega, uint32_t fp_bits) {
  if (levels == 0) levels = 4;
  attenuated_bloom_filter_t* abf = get_clear_memory(sizeof(attenuated_bloom_filter_t));
  abf->level_count = levels;
  abf->levels = get_clear_memory(levels * sizeof(elastic_bloom_filter_t*));
  for (uint32_t index = 0; index < levels; index++) {
    abf->levels[index] = elastic_bloom_filter_create(size, hash_count, omega, fp_bits);
  }
  return abf;
}

void attenuated_bloom_filter_destroy(attenuated_bloom_filter_t* abf) {
  if (abf == NULL) return;
  for (uint32_t index = 0; index < abf->level_count; index++) {
    elastic_bloom_filter_destroy(abf->levels[index]);
  }
  free(abf->levels);
  free(abf);
}

bool attenuated_bloom_filter_subscribe(attenuated_bloom_filter_t* abf,
                                       const uint8_t* topic, size_t topic_len) {
  if (abf == NULL || topic == NULL) return false;
  return elastic_bloom_filter_add(abf->levels[0], topic, topic_len);
}

bool attenuated_bloom_filter_unsubscribe(attenuated_bloom_filter_t* abf,
                                         const uint8_t* topic, size_t topic_len) {
  if (abf == NULL || topic == NULL) return false;
  return elastic_bloom_filter_remove(abf->levels[0], topic, topic_len);
}

bool attenuated_bloom_filter_check(const attenuated_bloom_filter_t* abf,
                                   const uint8_t* topic, size_t topic_len,
                                   uint32_t* out_hops) {
  if (abf == NULL || topic == NULL) return false;
  for (uint32_t level = 0; level < abf->level_count; level++) {
    if (elastic_bloom_filter_contains(abf->levels[level], topic, topic_len)) {
      if (out_hops != NULL) *out_hops = level;
      return true;
    }
  }
  return false;
}

int attenuated_bloom_filter_merge(attenuated_bloom_filter_t* dest,
                                   const attenuated_bloom_filter_t* src) {
  if (dest == NULL || src == NULL) return -1;
  if (dest->level_count != src->level_count) return -1;
  // Merge src level i into dest level i+1 (shifted by one hop)
  for (uint32_t level = 0; level < dest->level_count; level++) {
    uint32_t src_level = (level > 0) ? level - 1 : 0;
    if (level == 0) {
      // Dest level 0 is local only — don't merge remote subscriptions
      continue;
    }
    elastic_bloom_filter_merge(dest->levels[level], src->levels[src_level]);
  }
  return 0;
}

elastic_bloom_filter_t* attenuated_bloom_filter_get_level(attenuated_bloom_filter_t* abf, uint32_t level) {
  if (abf == NULL || level >= abf->level_count) return NULL;
  return abf->levels[level];
}

uint32_t attenuated_bloom_filter_level_count(const attenuated_bloom_filter_t* abf) {
  return abf == NULL ? 0 : abf->level_count;
}

cbor_item_t* attenuated_bloom_filter_encode(const attenuated_bloom_filter_t* abf) {
  if (abf == NULL) return NULL;
  // [level_count, [level_0_ebf, level_1_ebf, ...]]
  cbor_item_t* root = cbor_new_definite_array(2);
  (void)cbor_array_push(root, cbor_build_uint32(abf->level_count));

  cbor_item_t* levels_arr = cbor_new_definite_array((int)abf->level_count);
  for (uint32_t index = 0; index < abf->level_count; index++) {
    cbor_item_t* level_encoded = elastic_bloom_filter_encode(abf->levels[index]);
    if (level_encoded != NULL) {
      (void)cbor_array_push(levels_arr, level_encoded);
    }
  }
  (void)cbor_array_push(root, levels_arr);
  return root;
}

attenuated_bloom_filter_t* attenuated_bloom_filter_decode(cbor_item_t* item) {
  if (item == NULL || !cbor_isa_array(item)) return NULL;
  // [level_count, [level_0_ebf, level_1_ebf, ...]]
  if (cbor_array_size(item) < 2) return NULL;

  cbor_item_t* count_item = cbor_array_get(item, 0);
  uint32_t level_count = cbor_get_uint32(count_item);
  cbor_decref(&count_item);

  cbor_item_t* levels_arr = cbor_array_get(item, 1);
  if (!cbor_isa_array(levels_arr)) { cbor_decref(&levels_arr); return NULL; }

  size_t actual_levels = cbor_array_size(levels_arr);
  if (actual_levels < level_count) level_count = (uint32_t)actual_levels;

  attenuated_bloom_filter_t* abf = get_clear_memory(sizeof(attenuated_bloom_filter_t));
  if (abf == NULL) { cbor_decref(&levels_arr); return NULL; }
  abf->level_count = level_count;
  abf->levels = get_clear_memory(level_count * sizeof(elastic_bloom_filter_t*));
  if (abf->levels == NULL) {
    free(abf);
    cbor_decref(&levels_arr);
    return NULL;
  }

  for (uint32_t index = 0; index < level_count; index++) {
    cbor_item_t* level_item = cbor_array_get(levels_arr, (size_t)index);
    abf->levels[index] = elastic_bloom_filter_decode(level_item);
    cbor_decref(&level_item);
    if (abf->levels[index] == NULL) {
      // If any level fails to decode, clean up and return NULL
      for (uint32_t cleanup = 0; cleanup < index; cleanup++) {
        elastic_bloom_filter_destroy(abf->levels[cleanup]);
      }
      free(abf->levels);
      free(abf);
      cbor_decref(&levels_arr);
      return NULL;
    }
  }
  cbor_decref(&levels_arr);
  return abf;
}