//
// Created by victor on 8/5/25.
//
#include "sections.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"

void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node);

sections_lru_cache_t* sections_lru_cache_create(size_t size) {
  sections_lru_cache_t* lru = get_clear_memory(sizeof(sections_lru_cache_t));
  lru->size = size;
  lru->first = NULL;
  lru->last = NULL;
  hashmap_init(&lru->cache, (void*) hash_size_t, (void*) compare_size_t);
  hashmap_set_key_alloc_funcs(&lru->cache, duplicate_uint64, (void*)free);
  return lru;
}

void sections_lru_cache_destroy(sections_lru_cache_t* lru) {
  sections_lru_node_t* node;
  hashmap_foreach_data(node, &lru->cache) {
    section_destroy(node->value);
    free(node);
  }
  hashmap_cleanup(&lru->cache);
  free(lru);
}

section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  if (node == NULL) {
    return NULL;
  } else {
    sections_lru_cache_move(lru, node);
    return node->value;
  }
}

void sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  if (node != NULL) {
    if (node->previous == NULL) { // Node is the _start of the list
      if (node->next == NULL) {// List has only one node
        if (lru->first != NULL) {
          lru->first = NULL;
        }
        if (lru->last != NULL) {
          lru->last = NULL;
        }
      } else {
        sections_lru_node_t* next_node = node->next;
        if (next_node->previous != NULL) {
          next_node->previous = NULL;
        }
        lru->first = node->next;
      }
    } else {//Node is not at the start
      sections_lru_node_t* previous_node = node->previous;
      if (node->next == NULL) { // Node is the end of the list
        previous_node->next = NULL;
      } else { // Node is in the middle of the list
        sections_lru_node_t* next_node = node->next;
        next_node->previous = node->previous;
        previous_node->next = node->next;
      }
    }
    hashmap_remove(&lru->cache, &section_id);
    section_destroy(node->value);
    free(node);
  }
}

void sections_lru_cache_put(sections_lru_cache_t* lru, section_t* section) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section->id);
  // Add a new node if none exists already
  if (node == NULL) {
    node = get_clear_memory(sizeof(sections_lru_node_t));
    node->previous = NULL;
    node->next = NULL;
    node->value = refcounter_reference((refcounter_t*) section);
  }
  // Cache Ejection
  if (hashmap_size(&lru->cache) == lru->size) {
    if (lru->last != NULL) {
      sections_lru_node_t* last_node = lru->last;
      if(last_node->previous == NULL) { // We are at the end and its equal to the start
        lru->last = NULL;
        if (lru->first != NULL) {
          lru->first = NULL;
        }
      } else {
        sections_lru_node_t* new_last_node = last_node->previous;
        if (new_last_node->next != NULL) {
          new_last_node->next = NULL;
        }
        lru->last = last_node->previous;
      }
      hashmap_remove(&lru->cache, &last_node->value->id);
      section_destroy(last_node->value);
      free(last_node);
    }
  }
  hashmap_put(&lru->cache, &section->id, node);
  sections_lru_cache_move(lru, node);
}

uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  return node != NULL;
}

void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node) {
   if (lru->first == NULL) {
    lru->first = node;
    lru->last = node;
   } else {
     if (lru->first == node) { //id is already most recently used
       return;
     }
     if (lru->first == lru->last) { //cache has one node
       node->next = lru->first;
       sections_lru_node_t* first_node = lru->first;
       first_node->previous = node;
       lru->last = first_node;
       lru->first = node;
     } else if (lru->last == node) { // section is the lru
       lru->last = node->previous;
       sections_lru_node_t* last_node = lru->last;
       last_node->next = NULL;
       node->next = lru->first;
       node->previous = NULL;
       sections_lru_node_t* first_node  = lru->first;
       first_node->previous = node;
       lru->first = node;
     } else { // section is somewhere in the middle;
       if ((node->next == NULL) && (node->previous == NULL)) {
         sections_lru_node_t* first_node = lru->first;
         first_node->previous = node;
         node->next = first_node;
         lru->first = node;
       } else {
         sections_lru_node_t* next_node = node->next;
         if (node->previous != NULL) {
           sections_lru_node_t* previous_node = node->previous;
           previous_node->next = next_node;
         }
         if (node->next != NULL) {
           next_node->previous = node->previous;
         }
         sections_lru_node_t* first_node = lru->first;
         first_node->previous = node;
         node->next = first_node;
         lru->first = node;
       }
     }
   }
}
