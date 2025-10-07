//
// Created by victor on 9/16/25.
//

#include "config.h"
config_t config_default() {
  config_t config;
  config.index_bucket_size = 25;
  config.index_wait = 5;
  config.index_max_wait = 5000;
  config.section_size = 78;
  config.section_cache_count = 15;
  config.section_wait = 5;
  config.section_max_wait = 5000;
  config.cache_size = 50;
  config.max_tuple_size = 5;
  config.min_tuple_size  = 2;
  config.lru_size = 30;
  config.descriptor_pad = 32;
  return config;
}