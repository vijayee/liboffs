//
// Created by victor on 9/10/25.
//

#include "block_cache.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Util/path_join.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include <stdatomic.h>
#include <time.h>
#include <sched.h>

void block_lru_cache_move(block_lru_cache_t* lru, block_lru_node_t* node);
void block_cache_dispatch(void* state, message_t* msg);

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
    index_entry_destroy(node->entry);
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
    return (block_t*) refcounter_reference((refcounter_t*) node->value);
  }
}

void block_lru_cache_delete(block_lru_cache_t* lru, buffer_t* hash) {
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
  if (node != NULL) {
    if (node->previous == NULL) {
      if (node->next == NULL) {
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
    } else {
      block_lru_node_t* previous_node = node->previous;
      if (node->next == NULL) {
        previous_node->next = NULL;
      } else {
        block_lru_node_t* next_node = node->next;
        next_node->previous = node->previous;
        previous_node->next = node->next;
      }
    }
    hashmap_remove(&lru->cache, hash);
    index_entry_destroy(node->entry);
    block_destroy(node->value);
    free(node);
  }
}

index_entry_t* block_lru_cache_put(block_lru_cache_t* lru, block_t* block, index_entry_t* entry) {
  if (lru->size == 0) {
    return NULL;
  }
  block_lru_node_t* node = hashmap_get(&lru->cache, block->hash);
  if (node == NULL) {
    node = get_clear_memory(sizeof(block_lru_node_t));
    node->previous = NULL;
    node->next = NULL;
    node->value = (block_t*) refcounter_reference((refcounter_t*) block);
    node->entry = (index_entry_t*) refcounter_reference((refcounter_t*) entry);
  }
  index_entry_t* ejected = NULL;
  if (hashmap_size(&lru->cache) == lru->size) {
    if (lru->last != NULL) {
      block_lru_node_t* last_node = lru->last;
      if(last_node->previous == NULL) {
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
      refcounter_yield((refcounter_t*) last_node->entry);
      ejected = last_node->entry;
      free(last_node);
    }
  }
  hashmap_put(&lru->cache, block->hash, node);
  block_lru_cache_move(lru, node);
  return ejected;
}

uint8_t block_lru_cache_contains(block_lru_cache_t* lru, buffer_t* hash) {
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
  return node != NULL;
}

index_entry_t* block_lru_cache_peek_entry(block_lru_cache_t* lru, buffer_t* hash) {
  block_lru_node_t* node = hashmap_get(&lru->cache, hash);
  if (node == NULL) return NULL;
  return node->entry;
}

void block_lru_cache_move(block_lru_cache_t* lru, block_lru_node_t* node) {
  if (lru->first == NULL) {
    lru->first = node;
    lru->last = node;
  } else {
    if (lru->first == node) {
      return;
    }
    if (lru->first == lru->last) {
      node->next = lru->first;
      block_lru_node_t* first_node = lru->first;
      first_node->previous = node;
      lru->last = first_node;
      lru->first = node;
    } else if (lru->last == node) {
      lru->last = node->previous;
      block_lru_node_t* last_node = lru->last;
      last_node->next = NULL;
      node->next = lru->first;
      node->previous = NULL;
      block_lru_node_t* first_node = lru->first;
      first_node->previous = node;
      lru->first = node;
    } else {
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

/* ---- block_cache dispatch ---- */

void block_cache_dispatch(void* state, message_t* msg) {
  block_cache_t* block_cache = (block_cache_t*)state;
  switch (msg->type) {
    case CACHE_PUT: {
      cache_put_payload_t* p = (cache_put_payload_t*)msg->payload;
      p->result = -1;
      index_entry_t* entry = index_get(block_cache->index, p->block->hash);
      if (entry == NULL) {
        entry = index_entry_create(p->block->hash);
        int result = sections_write(block_cache->sections, p->block->data,
                                     &entry->section_id, &entry->section_index);
        if (result) {
          index_entry_destroy(entry);
          p->result = result;
        } else {
          index_entry_t* ejection = (index_entry_t*)refcounter_reference(
              (refcounter_t*)block_lru_cache_put(block_cache->lru, p->block, entry));
          if (ejection) {
            index_set_entry_ejection(block_cache->index, ejection, time(NULL));
            index_entry_destroy(ejection);
          }
          refcounter_yield((refcounter_t*) entry);
          index_add(block_cache->index, entry);
          p->result = 0;
        }
      } else {
        /* Block already exists in index — nothing to do */
        p->result = 0;
      }
      break;
    }
    case CACHE_GET: {
      cache_get_payload_t* p = (cache_get_payload_t*)msg->payload;
      p->result = NULL;
      index_entry_t* entry = (index_entry_t*)refcounter_reference(
          (refcounter_t*)index_get(block_cache->index, p->hash));
      if (entry == NULL) {
        /* Block not in index */
      } else {
        block_t* block = block_lru_cache_get(block_cache->lru, p->hash);
        if (block == NULL) {
          buffer_t* data = sections_read(block_cache->sections,
                                         entry->section_id, entry->section_index);
          if (data != NULL) {
            block = block_create_existing_data_hash(data, entry->hash);
            if (block != NULL) {
              index_entry_t* ejection = (index_entry_t*)refcounter_reference(
                  (refcounter_t*)block_lru_cache_put(block_cache->lru, block, entry));
              if (ejection) {
                index_set_entry_ejection(block_cache->index, ejection, time(NULL));
                index_entry_destroy(ejection);
              }
              p->result = block;
            }
          }
        } else {
          p->result = block;
        }
        index_entry_destroy(entry);
      }
      break;
    }
    case CACHE_REMOVE: {
      cache_remove_payload_t* p = (cache_remove_payload_t*)msg->payload;
      p->result = -1;
      index_entry_t* entry = block_lru_cache_peek_entry(block_cache->lru, p->hash);
      if (entry == NULL) {
        entry = index_peek(block_cache->index, p->hash);
      }
      if (entry == NULL) {
        p->result = 0;
      } else {
        size_t section_id = entry->section_id;
        size_t section_index = entry->section_index;
        index_remove(block_cache->index, p->hash);
        block_lru_cache_delete(block_cache->lru, p->hash);
        sections_deallocate(block_cache->sections, section_id, section_index);
        p->result = 0;
      }
      break;
    }
    default:
      break;
  }
}

/* Sync helper: run the block_cache actor until our message is processed. */
static void _block_cache_run_until_done(block_cache_t* block_cache) {
  while (true) {
    uint8_t flags = atomic_load(&block_cache->actor.flags);
    if (flags & ACTOR_FLAG_RUNNING) {
      sched_yield();
      continue;
    }
    if (!atomic_compare_exchange_strong(&block_cache->actor.flags, &flags,
                                        flags | ACTOR_FLAG_RUNNING)) {
      sched_yield();
      continue;
    }
    bool has_more = true;
    while (has_more) {
      has_more = actor_run(&block_cache->actor, ACTOR_BATCH_SIZE);
    }
    atomic_fetch_and(&block_cache->actor.flags, ~ACTOR_FLAG_RUNNING);
    break;
  }
}

/* ---- block_cache implementation ---- */

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, timer_actor_t* timer_actor) {
  block_cache_t* block_cache = get_clear_memory(sizeof(block_cache_t));
  refcounter_init_actor((refcounter_t*) block_cache);
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
  block_cache->sections = sections_create(folder, config.section_size, config.cache_size, config.max_tuple_size, type, timer_actor, config.section_wait, config.section_max_wait);
  int error_code;
  block_cache->index = index_create(config.index_bucket_size, folder, timer_actor, config.index_wait, config.index_max_wait, &error_code);
  actor_init(&block_cache->actor, block_cache, block_cache_dispatch);
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
    actor_destroy(&block_cache->actor);
    free(block_cache);
  }
}

/* Sync wrapper: sends CACHE_PUT message to the block_cache actor and
   processes it inline. The caller must YIELD the block before calling.
   Returns 0 on success, non-zero on failure. */
int block_cache_put(block_cache_t* block_cache, block_t* block) {
  cache_put_payload_t payload;
  payload.block = block;
  payload.reply_to = NULL;
  payload.result = -1;

  message_t msg;
  msg.type = CACHE_PUT;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  actor_send(&block_cache->actor, &msg);
  _block_cache_run_until_done(block_cache);

  return payload.result;
}

/* Sync wrapper: sends CACHE_GET message to the block_cache actor and
   processes it inline. Returns a new reference to the block, or NULL
   if not found. The caller must destroy the returned block. */
block_t* block_cache_get(block_cache_t* block_cache, buffer_t* hash) {
  cache_get_payload_t payload;
  payload.hash = hash;
  payload.reply_to = NULL;
  payload.result = NULL;

  message_t msg;
  msg.type = CACHE_GET;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  actor_send(&block_cache->actor, &msg);
  _block_cache_run_until_done(block_cache);

  return payload.result;
}

/* Sync wrapper: sends CACHE_REMOVE message to the block_cache actor and
   processes it inline. Returns 0 on success, non-zero on failure. */
int block_cache_remove(block_cache_t* block_cache, buffer_t* hash) {
  cache_remove_payload_t payload;
  payload.hash = hash;
  payload.reply_to = NULL;
  payload.result = -1;

  message_t msg;
  msg.type = CACHE_REMOVE;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  actor_send(&block_cache->actor, &msg);
  _block_cache_run_until_done(block_cache);

  return payload.result;
}

size_t block_cache_count(block_cache_t* block_cache) {
  return index_count(block_cache->index);
}