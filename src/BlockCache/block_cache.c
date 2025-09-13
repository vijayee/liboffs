//
// Created by victor on 9/10/25.
//

#include "block_cache.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"

void block_lru_cache_move(block_lru_cache_t* lru, block_lru_node_t* node);

block_lru_cache_t* block_lru_cache_create(size_t size) {
  block_lru_cache_t* lru = get_clear_memory(sizeof(block_lru_cache_t));
  lru->size = size;
  lru->first = NULL;
  lru->last = NULL;
  hashmap_init(&lru->cache, (void*) hash_buffer, (void*) compare_buffer);
  hashmap_set_key_alloc_funcs(&lru->cache, (void*) refcounter_reference, (void*) buffer_destroy);
  return lru;
}

void block_lru_cache_destroy(block_lru_cache_t* lru) {
  block_lru_node_t* node;
  hashmap_foreach_data(node, &lru->cache) {
    block_destroy(node->value);
    free(node);
  }
  hashmap_cleanup(&lru->cache);
  free(lru);
}

block_t* block_lru_cache_get(block_lru_cache_t* lru, buffer_t* hash) {
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
  if (node == NULL) {
    return NULL;
  } else {
    block_lru_cache_move(lru, node);
    return node->value;
  }
}

void block_lru_cache_delete(block_lru_cache_t* lru, buffer_t* hash) {
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
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
        block_lru_node_t* next_node = node->next;
        if (next_node->previous != NULL) {
          next_node->previous = NULL;
        }
        lru->first = node->next;
      }
    } else {//Node is not at the start
      block_lru_node_t* previous_node = node->previous;
      if (node->next == NULL) { // Node is the end of the list
        previous_node->next = NULL;
      } else { // Node is in the middle of the list
        block_lru_node_t* next_node = node->next;
        next_node->previous = node->previous;
        previous_node->next = node->next;
      }
    }
    hashmap_remove(&lru->cache, hash);
    block_destroy(node->value);
    free(node);
  }
}

void block_lru_cache_put(block_lru_cache_t* lru, block_t* block) {
  if (lru->size == 0) {
    return;
  }
  block_lru_node_t* node = hashmap_get(&lru->cache, block->hash);
  // Add a new node if none exists already
  if (node == NULL) {
    node = get_clear_memory(sizeof(block_lru_node_t));
    node->previous = NULL;
    node->next = NULL;
    node->value = refcounter_reference((refcounter_t*) block);
  }
  // Cache Ejection
  if (hashmap_size(&lru->cache) == lru->size) {
    if (lru->last != NULL) {
      block_lru_node_t* last_node = lru->last;
      if(last_node->previous == NULL) { // We are at the end and its equal to the start
        lru->last = NULL;
        if (lru->first != NULL) {
          lru->first = NULL;
        }
      } else {
        block_lru_node_t* new_last_node = last_node->previous;
        if (new_last_node->next != NULL) {
          new_last_node->next = NULL;
        }
        lru->last = last_node->previous;
      }
      hashmap_remove(&lru->cache, last_node->value->hash);
      block_destroy(last_node->value);
      free(last_node);
    }
  }
  hashmap_put(&lru->cache, block->hash, node);
  block_lru_cache_move(lru, node);
}

uint8_t block_lru_cache_contains(block_lru_cache_t* lru, buffer_t* hash) {
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
  return node != NULL;
}

void block_lru_cache_move(block_lru_cache_t* lru, block_lru_node_t* node) {
  if (lru->first == NULL) {
    lru->first = node;
    lru->last = node;
  } else {
    if (lru->first == node) { //hash is already most recently used
      return;
    }
    if (lru->first == lru->last) { //cache has one node
      node->next = lru->first;
      block_lru_node_t* first_node = lru->first;
      first_node->previous = node;
      lru->last = first_node;
      lru->first = node;
    } else if (lru->last == node) { // node is the lru
      lru->last = node->previous;
      block_lru_node_t* last_node = lru->last;
      last_node->next = NULL;
      node->next = lru->first;
      node->previous = NULL;
      block_lru_node_t* first_node  = lru->first;
      first_node->previous = node;
      lru->first = node;
    } else { // block is somewhere in the middle;
      if ((node->next == NULL) && (node->previous == NULL)) {
        block_lru_node_t* first_node = lru->first;
        first_node->previous = node;
        node->next = first_node;
        lru->first = node;
      } else {
        block_lru_node_t* next_node = node->next;
        if (node->previous != NULL) {
          block_lru_node_t* previous_node = node->previous;
          previous_node->next = next_node;
        }
        if (node->next != NULL) {
          next_node->previous = node->previous;
        }
        block_lru_node_t* first_node = lru->first;
        first_node->previous = node;
        node->next = first_node;
        lru->first = node;
      }
    }
  }
}
