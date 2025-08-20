//
// Created by victor on 8/4/25.
//

#ifndef OFFS_SECTIONS_H
#define OFFS_SECTIONS_H
#include <stddef.h>
#include <hashmap.h>
#include "../Buffer/buffer.h"
#include "section.h"
#include "../Time/debouncer.h"
#include "../Util/threadding.h"
#include <cbor.h>
typedef struct sections_lru_node_t sections_lru_node_t;
struct sections_lru_node_t {
  section_t* value;
  sections_lru_node_t* next;
  sections_lru_node_t* previous;
};

typedef HASHMAP(size_t, sections_lru_node_t) section_cache_t;

typedef struct {
  section_cache_t cache;
  sections_lru_node_t* first;
  sections_lru_node_t* last;
  size_t size;
} sections_lru_cache_t;

sections_lru_cache_t* sections_lru_cache_create(size_t size);
void sections_lru_cache_destroy(sections_lru_cache_t* lru);
section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id);
void  sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id);
void sections_lru_cache_put(sections_lru_cache_t* lru, section_t* section);
uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id);

typedef struct round_robin_node_t round_robin_node_t;
struct round_robin_node_t {
  size_t id;
  round_robin_node_t* next;
  round_robin_node_t* previous;
};

typedef struct {
  PlATFORMLOCKTYPE(lock);
  debouncer_t* debouncer;
  char* path;
  size_t size;
  round_robin_node_t* first;
  round_robin_node_t* last;
} round_robin_t;

round_robin_t* round_robin_create(char* robin_path, hierarchical_timing_wheel_t* wheel);
void round_robin_destroy(round_robin_t* robin);
void round_robin_add(round_robin_t* robin, size_t id);
size_t round_robin_next(round_robin_t* robin);
void round_robin_remove(round_robin_t* robin, size_t id);
cbor_item_t* round_robin_to_cbor(round_robin_t* robin);
round_robin_t* cbor_to_round_robin(cbor_item_t* cbor, char* robin_path, hierarchical_timing_wheel_t* wheel);

#endif //OFFS_SECTIONS_H
