//
// Created by victor on 4/28/25.
//

#ifndef OFFS_INDEX_H
#define OFFS_INDEX_H
#include "../RefCounter/refcounter.h"
#include "fibonacci.h"
#include <stdint.h>
#include "../Buffer/buffer.h"
#include <hashmap.h>
#include "../Util/vec.h"
#include <cbor.h>
#include "wal.h"
uint8_t get_bit(buffer_t* buffer, size_t index);

typedef struct {
  refcounter_t refcounter;
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
cbor_item_t* index_entry_to_cbor(index_entry_t* entry);
index_entry_t* cbor_to_index_entry(cbor_item_t* cbor);

typedef vec_t(index_entry_t*) index_entry_vec_t;
typedef struct index_node_t index_node_t;
struct index_node_t {
  refcounter_t refcounter;
  index_entry_vec_t* bucket;
  index_node_t* left;
  index_node_t* right;
};

index_node_t* index_node_create(size_t bucket_size);
index_node_t* index_node_create_from_leaves(index_node_t* left, index_node_t* right);
void index_node_to_array(index_node_t* node, index_entry_vec_t* entries);
void index_node_destroy(index_node_t* node);
size_t index_node_count(index_node_t* node);
cbor_item_t* index_node_to_cbor(index_node_t* node);
index_node_t* cbor_to_index_node(cbor_item_t* cbor, size_t bucket_size);

typedef HASHMAP(uint32_t, index_entry_vec_t) rank_map_t;
typedef struct {
  refcounter_t refcounter;
  index_node_t* root;
  size_t bucket_size;
  rank_map_t ranks;
  char* location;
  char* current_file;
  char* last_file;
  char* parent_location;
  size_t next_id;
  wal_t* wal;
  uint8_t is_rebuilding;
  uint64_t wait;
  uint64_t max_wait;
  size_t max_snapshots;
  size_t max_wals;
} index_t;

index_t* index_create(size_t bucket_size, char* location, uint64_t wait, uint64_t max_wait, size_t max_snapshots, size_t max_wals, int* error_code);
index_t* index_create_from(size_t bucket_size, index_node_t* root, char* location, uint64_t wait, uint64_t max_wait, size_t max_snapshots, size_t max_wals);
size_t index_count(index_t* index);
void index_add(index_t* index, index_entry_t* entry);
index_entry_t* index_get(index_t* index, buffer_t* hash);
index_entry_t* index_find(index_t* index, buffer_t* hash);
index_entry_t* index_peek(index_t* index, buffer_t* hash);
void index_increment(index_t* index, index_entry_t* entry);
void index_remove(index_t* index, buffer_t* hash);
void index_destroy(index_t* index);
index_entry_vec_t* index_to_array(index_t* index);
cbor_item_t* index_to_cbor(index_t* index);
index_t* cbor_to_index(cbor_item_t* cbor, char* location, uint64_t wait, uint64_t max_wait, size_t max_snapshots, size_t max_wals);
void index_set_entry_ejection(index_t* index, index_entry_t* entry, uint64_t date);
void index_debounce(index_t* index);
int index_sync(index_t* index);
int _sort_indexes(const void *str1, const void *str2);
int index_to_crc(index_t* index, uint64_t* crc);
index_entry_vec_t* index_entries_by_ejection_date(index_t* index);
#endif //OFFS_INDEX_H
