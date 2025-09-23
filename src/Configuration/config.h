//
// Created by victor on 9/16/25.
//

#ifndef OFFS_CONFIG_H
#define OFFS_CONFIG_H
#include <stddef.h>
typedef struct {
  size_t index_bucket_size;
  size_t index_wait;
  size_t index_max_wait;
  size_t section_size;
  size_t section_cache_count;
  size_t section_wait;
  size_t section_max_wait;
  size_t cache_size;
  size_t max_tuple_size;
  size_t min_tuple_size;
  size_t lru_size;
  size_t descriptor_pad;
} config_t;
config_t default_config();
#endif //OFFS_CONFIG_H
