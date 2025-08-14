//
// Created by victor on 8/4/25.
//

#ifndef OFFS_SECTIONS_H
#define OFFS_SECTIONS_H
#include <stddef.h>
#include <hashmap.h>
#include "../Buffer/buffer.h"
#include "section.h"
typedef struct sections_lru_node_t sections_lru_node_t;
struct sections_lru_node_t {
  section_t* value;
  size_t* next;
  size_t* previous;
};

typedef HASHMAP(size_t, sections_lru_node_t) section_cache_t;

typedef struct {
  section_cache_t cache;
  size_t* first;
  size_t* last;
  size_t size;
} sections_lru_cache_t;

sections_lru_cache_t* sections_lru_cache_create(size_t size);
void sections_lru_cache_destroy(sections_lru_cache_t* lru);
section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id);
void  sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id);
void sections_lru_cache_put(sections_lru_cache_t* lru, section_t* section);
uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id);
#endif //OFFS_SECTIONS_H
