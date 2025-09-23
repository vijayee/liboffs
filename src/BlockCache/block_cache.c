//
// Created by victor on 9/10/25.
//

#include "block_cache.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Util/path_join.h"
#include "../Workers/error.h"
#include "../Workers/work.h"


typedef struct {
  block_cache_t* block_cache;
  buffer_t* hash;
  promise_t* promise;
} block_cache_get_ctx;

typedef struct {
  block_cache_t* block_cache;
  block_t* block;
  promise_t* promise;
} block_cache_put_ctx;

typedef struct {
  block_cache_t* block_cache;
  buffer_t* hash;
  promise_t* promise;
} block_cache_remove_ctx;

void block_lru_cache_move(block_lru_cache_t* lru, block_lru_node_t* node);
void _block_cache_get(block_cache_get_ctx* ctx);
void _block_cache_get_abort(block_cache_get_ctx* ctx);
void _block_cache_put(block_cache_put_ctx* ctx);
void _block_cache_put_abort(block_cache_put_ctx* ctx);
void block_cache_remove(block_cache_t* block_cache, priority_t priority, buffer_t* hash, promise_t* promise);
void _block_cache_remove_abort(block_cache_remove_ctx* ctx);

block_lru_cache_t* block_lru_cache_create(size_t size) {
  block_lru_cache_t* lru = get_clear_memory(sizeof(block_lru_cache_t));
  lru->size = size;
  lru->first = NULL;
  platform_lock_init(&lru->lock);
  lru->last = NULL;
  hashmap_init(&lru->cache, (void*) hash_buffer, (void*) compare_buffer);
  hashmap_set_key_alloc_funcs(&lru->cache, (void*) refcounter_reference, (void*) buffer_destroy);
  return lru;
}

void block_lru_cache_destroy(block_lru_cache_t* lru) {
  block_lru_node_t* node;
  platform_lock_destroy(&lru->lock);
  hashmap_foreach_data(node, &lru->cache) {
    block_destroy(node->value);
    free(node);
  }
  hashmap_cleanup(&lru->cache);
  free(lru);
}

block_t* block_lru_cache_get(block_lru_cache_t* lru, buffer_t* hash) {
  platform_lock(&lru->lock);
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
  if (node == NULL) {
    platform_unlock(&lru->lock);
    return NULL;
  } else {
    block_lru_cache_move(lru, node);
    block_t* value = (block_t*) refcounter_reference( (refcounter_t*) node->value);
    platform_unlock(&lru->lock);
    refcounter_yield((refcounter_t*) value);
    return value;
  }
}

void block_lru_cache_delete(block_lru_cache_t* lru, buffer_t* hash) {
  platform_lock(&lru->lock);
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
  platform_unlock(&lru->lock);
}

void block_lru_cache_put(block_lru_cache_t* lru, block_t* block) {
  platform_lock(&lru->lock);
  if (lru->size == 0) {
    platform_unlock(&lru->lock);
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
  platform_unlock(&lru->lock);
}

uint8_t block_lru_cache_contains(block_lru_cache_t* lru, buffer_t* hash) {
  platform_lock(&lru->lock);
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
  uint8_t result = node != NULL;
  platform_unlock(&lru->lock);
  return result;
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

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, work_pool_t* pool, hierarchical_timing_wheel_t* wheel) {
  block_cache_t* block_cache = get_clear_memory(sizeof(block_cache_t));
  refcounter_init((refcounter_t*) block_cache);
  block_cache->pool = (work_pool_t*) refcounter_reference((void*) pool);
  block_cache->type = type;
  char* folder;
  switch (type) {
    case standard:
      folder = path_join(location, "blocks");
      break;
    case mini:
      folder = path_join(location, "mini");
      break;
    case nano:
      folder = path_join(location, "nano");
      break;
    case mega:
      folder = path_join(location, "nano");
      break;
  }
  block_cache->lru = block_lru_cache_create(config.lru_size);
  block_cache->sections = sections_create(folder, config.section_size, config.cache_size, config.max_tuple_size, type, wheel, config.section_wait, config.section_max_wait);
  block_cache->index = index_create(config.index_bucket_size,folder, wheel, config.index_wait, config.index_max_wait);
  free(folder);
  return block_cache;
}

void block_cache_destroy(block_cache_t* block_cache) {
  refcounter_dereference((refcounter_t*) block_cache);
  if (refcounter_count((refcounter_t*) block_cache) == 0) {
    refcounter_destroy_lock((refcounter_t*) block_cache);
    index_destroy(block_cache->index);
    sections_destroy(block_cache->sections);
    block_lru_cache_destroy(block_cache->lru);
    work_pool_destroy(block_cache->pool);
    free(block_cache);
  }
}


void block_cache_put(block_cache_t* block_cache, priority_t priority, block_t* block, promise_t* promise) {
  block_cache_put_ctx* ctx = get_memory(sizeof(block_cache_put_ctx));
  ctx->block_cache = block_cache;
  ctx->block = (block_t*) refcounter_reference((refcounter_t*)block);
  refcounter_yield((refcounter_t*) ctx-> block);
  work_t* work = work_create(priority, ctx,(void*)_block_cache_put,(void*)_block_cache_put_abort);
  work_pool_enqueue(block_cache->pool, work);
}

void _block_cache_put(block_cache_put_ctx* ctx) {
  block_cache_t* block_cache = ctx->block_cache;
  block_t* block = (block_t*) refcounter_reference((refcounter_t*)ctx->block);
  promise_t* promise = ctx->promise;
  free(ctx);
  index_entry_t* entry = index_get(block_cache->index, block->hash);
  if (entry == NULL) {
    entry = index_entry_create(block->hash);
    int result = sections_write(block_cache->sections, block->data, &entry->section_id, &entry->section_index);
    if (result) {
      index_entry_destroy(entry);
      promise_reject(promise, ERROR("Section Write Error"));
    } else {
      refcounter_yield((refcounter_t*) entry);
      index_add(block_cache->index, entry);
      promise_resolve(promise, NULL);
    }
    block_lru_cache_put(block_cache->lru, block);
  } else {
    promise_resolve(promise, NULL);
  }
  block_destroy(block);
}

void _block_cache_put_abort(block_cache_put_ctx* ctx) {
  promise_t* promise = ctx->promise;
  block_t* block = (block_t*) refcounter_reference((refcounter_t*)ctx->block);
  block_destroy(block);
  free(ctx);
  promise_reject(promise, ERROR("Cache Put Aborted"));
}


void block_cache_get(block_cache_t* block_cache, priority_t priority, buffer_t* hash, promise_t* promise)  {
  block_cache_get_ctx* ctx = get_memory(sizeof(block_cache_get_ctx));
  ctx->promise = promise;
  ctx->block_cache = block_cache;
  ctx->hash = (buffer_t*) refcounter_reference((refcounter_t*)hash);
  refcounter_yield((refcounter_t*) ctx->hash);
  work_t* work = work_create(priority, ctx,(void*)_block_cache_get,(void*)_block_cache_get_abort);
  work_pool_enqueue(block_cache->pool, work);
}

void _block_cache_get(block_cache_get_ctx* ctx) {
  block_cache_t* block_cache = ctx->block_cache;
  buffer_t* hash = refcounter_reference((refcounter_t*) ctx->hash);
  promise_t* promise = ctx->promise;
  free(ctx);
  index_entry_t* entry = index_get(block_cache->index, hash);
  if (entry == NULL) {
    promise_resolve(promise, NULL);
  } else {
    block_t* block = block_lru_cache_get(block_cache->lru, hash);
    if (block == NULL) {
      buffer_t* data = sections_read(block_cache->sections, entry->section_id, entry->section_index);
      if (data == NULL) {
        promise_reject(promise, ERROR("Section Read Error"));
      } else {
        block = block_create_existing_data_hash(data, entry->hash);
        if (block != NULL) {
          block_lru_cache_put(block_cache->lru, block);
          refcounter_yield((refcounter_t *) block);
          promise_resolve(promise, (void *) block);
        } else {
          promise_reject(promise, ERROR("Failed to form block"));
        }
      }
    } else {
      block = (block_t*) refcounter_reference((refcounter_t*) block);
      refcounter_yield((refcounter_t *) block);
      promise_resolve(promise, (void *) block);
    }
  }
  buffer_destroy(hash);
}

void _block_cache_get_abort(block_cache_get_ctx* ctx) {
  promise_t* promise = ctx->promise;
  buffer_t* hash = (buffer_t*) refcounter_reference((refcounter_t*)ctx->hash);
  buffer_destroy(hash);
  free(ctx);
  promise_reject(promise, ERROR("Cache Get Aborted"));
}


void block_cache_remove(block_cache_t* block_cache, priority_t priority, buffer_t* hash, promise_t* promise) {
  block_cache_remove_ctx* ctx = get_memory(sizeof(block_cache_remove_ctx));
  ctx->promise = promise;
  ctx->block_cache = block_cache;
  ctx->hash = (buffer_t*) refcounter_reference((refcounter_t*)hash);
  refcounter_yield((refcounter_t*) ctx->hash);
  work_t* work = work_create(priority, ctx,(void*)_block_cache_get,(void*)_block_cache_get_abort);
  work_pool_enqueue(block_cache->pool, work);
}

void _block_cache_remove(block_cache_remove_ctx* ctx) {
  block_cache_t* block_cache = ctx->block_cache;
  buffer_t* hash = (buffer_t*) refcounter_reference((refcounter_t*)ctx->hash);
  promise_t* promise = ctx->promise;
  free(ctx);
  index_entry_t* entry = index_get(block_cache->index, hash);
  if (entry == NULL) {
    promise_resolve(promise, NULL);
  } else {
    sections_deallocate(block_cache->sections, entry->section_id, entry->section_index);
    index_remove(block_cache->index, hash);
    block_lru_cache_delete(block_cache->lru, hash);
    promise_resolve(promise,NULL);
  }
}

void _block_cache_remove_abort(block_cache_remove_ctx* ctx) {
  promise_t* promise = ctx->promise;
  buffer_t* hash = (buffer_t*) refcounter_reference((refcounter_t*)ctx->hash);
  buffer_destroy(hash);
  free(ctx);
  promise_reject(promise, ERROR("Cache Get Aborted"));
}