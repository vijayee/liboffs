//
// Created by victor on 8/5/25.
//
#include "sections.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"

void sections_lru_cache_move(sections_lru_cache_t* lru, size_t section_id);

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
    if (node->previous != NULL) {
      free(node->previous);
    }
    if (node->next) {
     free(node->next);
    }
    free(node);
  }
  hashmap_cleanup(&lru->cache);
  if(lru->first != NULL) {
    free(lru->first);
  }
  if(lru->last != NULL) {
    free(lru->last);
  }
  free(lru);
}

section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  if (node == NULL) {
    return NULL;
  } else {
    sections_lru_cache_move(lru, section_id);
    return node->value;
  }
}

void sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  if (node != NULL) {
    if (node->previous == NULL) { // Node is the _start of the list
      if (node->next == NULL) {// List has only one node
        hashmap_remove(&lru->cache, &section_id);
        if (lru->first != NULL) {
          free(lru->first);
          lru->first = NULL;
        }
        if (lru->last != NULL) {
          free(lru->last);
          lru->last = NULL;
        }
      } else {
        sections_lru_node_t* next_node = hashmap_get(&lru->cache, node->next);
        if (next_node->previous != NULL) {
          free(next_node->previous);
          next_node->previous = NULL;
        }
        *lru->first = *node->next;
        hashmap_remove(&lru->cache, &section_id);
        free(node->next);
      }
    } else {//Node is not at the start
      sections_lru_node_t* previous_node = hashmap_get(&lru->cache, node->previous);
      if (node->next == NULL) { // Node is the end of the list
        free(previous_node->next);
        previous_node->next = NULL;
        hashmap_remove(&lru->cache, &section_id);
      } else { // Node is in the middle of the list
        sections_lru_node_t* next_node = hashmap_get(&lru->cache, node->next);
        if (next_node->previous == NULL) {
          node->previous = get_clear_memory(sizeof(size_t));
        }
        *next_node->previous = *node->previous;
        *previous_node->next = *node->next;
        hashmap_remove(&lru->cache, &section_id);
      }
      free(node->previous);
    }
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
      sections_lru_node_t* last_node = hashmap_get(&lru->cache, lru->last);
      if(last_node->previous == NULL) { // We are at the end and its equal to the start
        hashmap_remove(&lru->cache, lru->last);
        free(lru->last);
        lru->last = NULL;
        if (lru->first != NULL) {
          free(lru->first);
          lru->first = NULL;
        }
      } else {
        sections_lru_node_t* new_last_node = hashmap_get(&lru->cache, last_node->previous);
        if (new_last_node->next != NULL) {
          free(new_last_node->next);
          new_last_node->next = NULL;
        }
        hashmap_remove(&lru->cache, lru->last);
        *lru->last = *last_node->previous;
      }

      section_destroy(last_node->value);
      if (last_node->previous != NULL) {
        free(last_node->previous);
      }
      if (last_node->next != NULL) {
        free(last_node->next);
      }
      free(last_node);
    }
  }
  hashmap_put(&lru->cache, &section->id, node);
  sections_lru_cache_move(lru, section->id);
}

uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  return node != NULL;
}

void sections_lru_cache_move(sections_lru_cache_t* lru, size_t section_id) {
   if (lru->first == NULL) {
    lru->first = get_clear_memory(sizeof(size_t));
    *lru->first = section_id;
    if (lru->last == NULL) {
      lru->last= get_clear_memory(sizeof(size_t));
    }
    *lru->last = section_id;
   } else {
     if (*lru->first == section_id) { //id is already most recently used
       return;
     }
     if (*lru->first == *lru->last) { //cache has one node
       sections_lru_node_t* node  = hashmap_get(&lru->cache, &section_id);
       if (node->next == NULL) {
         node->next = get_clear_memory(sizeof(size_t));
       }
       *node->next = *lru->first;
       sections_lru_node_t* first_node  = hashmap_get(&lru->cache, lru->first);
       if (first_node->previous == NULL) {
         first_node->previous = get_clear_memory(sizeof(size_t));
       }
       *first_node->previous = section_id;
       *lru->last = *lru->first;
       *lru->first = section_id;
     } else if (*lru->last == section_id) { // section is the lru
       sections_lru_node_t* node  = hashmap_get(&lru->cache, &section_id);
       *lru->last = *node->previous;
       sections_lru_node_t* last_node  = hashmap_get(&lru->cache, lru->last);
       if (last_node->next != NULL) {
         free(last_node->next);
       }
       last_node->next = NULL;
       if (node->next == NULL) {
         node->next = get_clear_memory(sizeof(size_t));
       }
       *node->next = *lru->first;
       if (node->previous != NULL) {
         free(node->previous);
       }
       node->previous = NULL;
       sections_lru_node_t* first_node  = hashmap_get(&lru->cache, lru->first);
       if (first_node->previous == NULL) {
         first_node->previous = get_clear_memory(sizeof(size_t));
       }
       *first_node->previous = section_id;
       *lru->first = section_id;
     } else { // section is somewhere in the middle;
       sections_lru_node_t* node  = hashmap_get(&lru->cache, &section_id);
       sections_lru_node_t* next_node =  hashmap_get(&lru->cache, node->next);
       if (node->previous != NULL) {
         sections_lru_node_t* previous_node = hashmap_get(&lru->cache, node->previous);
         *previous_node->next = *node->next;
       }
       if (node->next == NULL) { //node is new in the list
         node->next = get_clear_memory(sizeof(size_t));
         *node->next = *lru->first;
         sections_lru_node_t* first_node =  hashmap_get(&lru->cache, lru->first);
         if (first_node->previous == NULL) {
           first_node->previous = get_clear_memory(sizeof(size_t));
         }
         *first_node->previous = section_id;
         *lru->first = section_id;
       } else {
         next_node->previous = node->previous;

         *node->next = *lru->first;
         if (next_node->previous != NULL) {
           free(next_node->previous);
         }
         next_node->previous = NULL;
         *lru->first = section_id;
       }
     }
   }
}
