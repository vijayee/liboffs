//
// Created by victor on 8/5/25.
//
#include "sections.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Util/path_join.h"
#include "../Util/log.h"

void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node);
void round_robin_save(void* ctx);

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


round_robin_t* round_robin_create(char* robin_path, hierarchical_timing_wheel_t* wheel) {
  round_robin_t* robin = get_clear_memory(sizeof(round_robin_t));
  platform_lock_init(&robin->lock);
  robin->debouncer = debouncer_create(wheel, (void*) robin, round_robin_save, round_robin_save,5, 5000);
  robin->path = path_join(robin_path, ".robin");
  return robin;
}

void round_robin_destroy(round_robin_t* robin) {
  debouncer_destroy(robin->debouncer);
  free(robin->path);
  platform_lock_destroy(&robin->lock);
  round_robin_node_t* current = robin->first;
  while (current != NULL) {
    round_robin_node_t* next = current;
    free(current);
    current = next;
  }
  free(robin);
}

void round_robin_add(round_robin_t* robin, size_t id) {
  round_robin_node_t* node = get_clear_memory(sizeof(round_robin_node_t));
  node->id = id;
  node->previous = NULL;
  node->next = NULL;
  platform_lock(&robin->lock);
  if ((robin->last == NULL) && (robin->first == NULL)) {
    robin->first = node;
    robin->last = node;
  } else {
    node->previous = robin->last;
    robin->last->next= node;
    robin->last = node;
  }
  debouncer_debounce(robin->debouncer);
  robin->size++;
  platform_unlock(&robin->lock);
}

size_t round_robin_next(round_robin_t* robin){
  platform_lock(&robin->lock);
  if ((robin->last == NULL) && (robin->first == NULL)) { // No nodes
    platform_unlock(&robin->lock);
    return 0;
  } else {
    round_robin_node_t* node = robin->first;
    if (robin->last == node) { // One node
      size_t id = node->id;
      platform_unlock(&robin->lock);
      return id;
    } else {
      robin->first = node->next;
      if (node->next != NULL) {
        node->next->previous = NULL;
        node->next = NULL;
      }
      size_t id = node->id;
      node->previous = robin->last;
      robin->last->next= node;
      robin->last = node;
      platform_unlock(&robin->lock);
      return id;
    }
  }
}

void round_robin_remove(round_robin_t* robin, size_t id){
  platform_lock(&robin->lock);
  if ((robin->last == NULL) && (robin->first == NULL)) {
    platform_unlock(&robin->lock);
    return;
  }
  round_robin_node_t* current = robin->first;
  while (current != NULL) {
    if (current->id == id) {
      if (robin->last == current) {
        robin->last = current->previous;
      }
      if (robin->first == current) {
        robin->first = current->next;
      }
      if (current->previous != NULL) {
        current->previous->next = current->next;
      }
      if (current->next != NULL) {
        current->next->previous = current->previous;
      }
      free(current);
      debouncer_debounce(robin->debouncer);
      robin->size--;
      platform_unlock(&robin->lock);
      return;
    } else {
      current = current->next;
    }
  }
  platform_unlock(&robin->lock);
}

cbor_item_t* round_robin_to_cbor(round_robin_t* robin) {
  cbor_item_t* array= cbor_new_definite_array(robin->size);
  round_robin_node_t* current = robin->first;
  bool success = true;
  while (current != NULL) {
    success &= cbor_array_push(array, cbor_move(cbor_build_uint64(current->id)));
    current = current->next;
  }
  if (!success){
   cbor_decref(&array);
   return NULL;
  } else {
    return array;
  }
}

round_robin_t* cbor_to_round_robin(cbor_item_t* cbor, char* robin_path, hierarchical_timing_wheel_t* wheel) {
  if(!cbor_isa_array(cbor)) {
    return NULL;
  }
  round_robin_t* robin = round_robin_create(robin_path, wheel);
  size_t size = cbor_array_size(cbor);
  for(size_t i = 0; i < size; i++) {
    cbor_item_t* cbor_id = cbor_array_get(cbor, i);
    round_robin_add(robin,cbor_get_uint64(cbor_id));
  }
  return robin;
}

void round_robin_save(void* ctx) {
  round_robin_t* robin = (round_robin_t*) ctx;
  platform_lock(&robin->lock);
  cbor_item_t* cbor = round_robin_to_cbor(robin);
  platform_unlock(&robin->lock);
  if (cbor == NULL) {
    log_error("Failed to save robin file");
    return;
  }
  uint8_t* cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  FILE* robin_file = fopen(robin->path,"wb");
  if (robin_file == NULL) {
    log_error("Failed to save robin file");
    return;
  }
  fwrite(cbor_data,cbor_size,1,robin_file);
  fflush(robin_file);
  fclose(robin_file);
  free(cbor_data);
  cbor_decref(&cbor);
}