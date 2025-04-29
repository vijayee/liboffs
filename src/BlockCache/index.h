//
// Created by victor on 4/28/25.
//

#ifndef OFFS_INDEX_H
#define OFFS_INDEX_H
#include "../RefCounter/refcounter.h"
#include "fibonacci.h"
#include <stdint.h>
#include "../Buffer/buffer.h"

typedef struct {
  refcounter_t refcounter;
  fibonacci_hit_counter_t counter;
  buffer_t* hash;
  size_t section_id;
  size_t section_index;
  uint64_t ejection_date;
} index_entry_t;
index_entry_t index_entry_create(buffer_t* hash);
index_entry_t index_entry_from(buffer_t* hash, size_t section_id, size_t section_index, uint64_t ejection_date, fibonacci_hit_counter_t counter);

typedef struct index_node_t index_node_t;
struct index_node_t {
  index_entry_t* bucket;
  index_node_t* left;
  index_node_t* right;
};

typedef struct {
  index_node_t root;
  size_t bucket_size;
} index_t;


#endif //OFFS_INDEX_H
