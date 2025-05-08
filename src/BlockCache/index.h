//
// Created by victor on 4/28/25.
//

#ifndef OFFS_INDEX_H
#define OFFS_INDEX_H
#include "../RefCounter/refcounter.h"
#include "fibonacci.h"
#include <stdint.h>
#include "../Buffer/buffer.h"
#include "../Util/threadding.h"
#include <hashmap.h>
#include "../Util/vec.h"

uint8_t get_bit(buffer_t* buffer, size_t index);

typedef struct {
  refcounter_t refcounter;
  PlATFORMLOCKTYPE(lock);
  fibonacci_hit_counter_t counter;
  buffer_t* hash;
  size_t section_id;
  size_t section_index;
  uint64_t ejection_date;
} index_entry_t;

index_entry_t* index_entry_create(buffer_t* hash);
index_entry_t* index_entry_from(buffer_t* hash, size_t section_id, size_t section_index, uint64_t ejection_date, fibonacci_hit_counter_t counter);
void index_entry_destroy(index_entry_t* entry);
int index_entry_increment(index_entry_t* entry);
void index_entry_set_ejection_date(index_entry_t* entry, uint64_t ejection_date);

typedef vec_t(index_entry_t*) index_entry_vec_t;
typedef struct index_node_t index_node_t;
struct index_node_t {
  refcounter_t refcounter;
  index_entry_vec_t* bucket;
  index_node_t* left;
  index_node_t* right;
};

index_node_t* index_node_create(size_t bucket_size);
void index_node_destroy(index_node_t* node);

typedef struct {
  refcounter_t refcounter;
  index_node_t* root;
  size_t bucket_size;
  HASHMAP(uint32_t, index_entry_vec_t) ranks;
} index_t;

index_t* index_create(size_t bucket_size);
void index_add(index_t* index, index_entry_t* entry);
index_entry_t* index_get(index_t* index, buffer_t* hash);
void index_remove(index_t* index, buffer_t* hash);
void index_destroy(index_t* index);
#endif //OFFS_INDEX_H
