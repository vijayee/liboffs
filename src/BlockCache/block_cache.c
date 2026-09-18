//
// Created by victor on 9/10/25.
//

#include "block_cache.h"
#include "sections.h"
#include "../Network/authority.h"
#include "../Network/respiration.h"
#include "../Network/respiration_actor.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Util/path_join.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Scheduler/scheduler.h"
#include "../Util/log.h"
#include "../Timer/timer_actor.h"
#include "../Platform/platform.h"
#include <stdatomic.h>
#include <time.h>

bool block_cache_verify_read_hash(const buffer_t* data, const buffer_t* stored_hash) {
  return block_verify_hash(data, stored_hash);
}

/* Victim-candidate check for respiration exhale: pinned permanent blocks and
   ephemeral blocks are never shed. LRU/capacity behavior ignores both fields. */
bool block_cache_entry_is_sheddable(const index_entry_t* entry) {
  return entry != NULL && entry->pin_count == 0 && entry->ephemeral_count == 0;
}

void respiration_exhale_payload_destroy(void* ptr) {
  respiration_exhale_payload_t* payload = (respiration_exhale_payload_t*)ptr;
  if (payload == NULL) return;
  if (payload->hashes != NULL) {
    for (size_t idx = 0; idx < payload->count; idx++) {
      if (payload->hashes[idx] != NULL) {
        DESTROY(payload->hashes[idx], buffer);
      }
    }
    free(payload->hashes);
  }
  if (payload->ejection_dates != NULL) {
    free(payload->ejection_dates);
  }
  free(payload);
}

static void cache_get_result_payload_destroy(void* ptr) {
  cache_get_result_payload_t* payload = (cache_get_result_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  if (payload->block != NULL) {
    block_destroy(payload->block);
  }
  free(payload);
}

static void cache_put_payload_destroy(void* ptr) {
  cache_put_payload_t* payload = (cache_put_payload_t*)ptr;
  if (payload->block != NULL) {
    block_destroy(payload->block);
  }
  free(payload);
}

static void cache_put_result_payload_destroy(void* ptr) {
  cache_put_result_payload_t* payload = (cache_put_result_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

static void cache_remove_payload_destroy(void* ptr) {
  cache_remove_payload_t* payload = (cache_remove_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

static void cache_get_payload_destroy(void* ptr) {
  cache_get_payload_t* payload = (cache_get_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

static void cache_ephemeral_payload_destroy(void* ptr) {
  cache_ephemeral_payload_t* payload = (cache_ephemeral_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

static void cache_ephemeral_result_payload_destroy(void* ptr) {
  cache_ephemeral_result_payload_t* payload = (cache_ephemeral_result_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

static void cache_pin_payload_destroy(void* ptr) {
  cache_pin_payload_t* payload = (cache_pin_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

/* Tolerates the emptied shell (NULL arrays / count 0) that a consumer leaves
   behind after stealing the arrays. */
void cache_ephemeral_list_payload_destroy(cache_ephemeral_list_payload_t* payload) {
  if (payload == NULL) return;
  if (payload->hashes != NULL) {
    for (size_t idx = 0; idx < payload->count; idx++) {
      if (payload->hashes[idx] != NULL) {
        DESTROY(payload->hashes[idx], buffer);
      }
    }
    free(payload->hashes);
  }
  if (payload->ephemeral_counts != NULL) {
    free(payload->ephemeral_counts);
  }
  if (payload->pin_counts != NULL) {
    free(payload->pin_counts);
  }
  free(payload);
}

/* void* adapter so the typed destroy can serve as message_t.payload_destroy */
static void cache_ephemeral_list_payload_destroy_adapter(void* ptr) {
  cache_ephemeral_list_payload_destroy((cache_ephemeral_list_payload_t*)ptr);
}

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
  PLATFORM_DIAGNOSTIC_PUSH
  PLATFORM_DIAGNOSTIC_IGNORE(-Wmissing-field-initializers)
  hashmap_foreach_data(node, &lru->cache) {
    block_destroy(node->value);
    index_entry_destroy(node->entry);
    free(node);
  }
  PLATFORM_DIAGNOSTIC_POP
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
        lru->last = previous_node;
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
        node->previous = NULL;
        lru->first = node;
      }
    }
  }
}

/* ---- block_cache dispatch ---- */

/* Delete one entry from the index, LRU, and section store. `hash` must be a
   buffer the caller holds a reference to (it outlives the entry: index_remove
   drops the index's entry reference, which may free an entry that is not in
   the LRU). Captures the section index before index_remove for the same
   reason. */
static void _block_cache_delete_entry(block_cache_t* block_cache, buffer_t* hash,
                                      index_entry_t* entry) {
  size_t section_index = entry->section_index;
  index_remove(block_cache->index, hash);
  timer_actor_debounce(block_cache->timer_actor, block_cache->index_wait, 0, &block_cache->actor, INDEX_SAVE);
  block_cache->current_bytes -= (size_t)block_cache->type;
  block_cache_update_capacity(block_cache);
  block_lru_cache_delete(block_cache->lru, hash);
  section_deallocate_payload_t dealloc_payload;
  dealloc_payload.index = section_index;
  dealloc_payload.reply_to = NULL;
  dealloc_payload.result = -1;
  message_t dealloc_msg;
  dealloc_msg.type = SECTION_DEALLOCATE;
  dealloc_msg.payload = &dealloc_payload;
  dealloc_msg.payload_destroy = NULL;
  sections_dispatch(block_cache->sections, &dealloc_msg);
}

static void _block_cache_add_pending_get(block_cache_t* block_cache, buffer_t* hash,
                                          index_entry_t* entry, actor_t* reply_to) {
  pending_get_t* pending = get_clear_memory(sizeof(pending_get_t));
  pending->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  pending->entry = (index_entry_t*)refcounter_reference((refcounter_t*)entry);
  pending->reply_to = reply_to;
  pending->next = block_cache->pending_gets;
  block_cache->pending_gets = pending;
}

static pending_get_t* _block_cache_find_pending_get(block_cache_t* block_cache,
                                                      size_t section_id, size_t section_index) {
  pending_get_t** current = &block_cache->pending_gets;
  while (*current != NULL) {
    if ((*current)->entry->section_id == section_id &&
        (*current)->entry->section_index == section_index) {
      pending_get_t* found = *current;
      *current = found->next;
      return found;
    }
    current = &(*current)->next;
  }
  return NULL;
}

/* Find and resolve ALL pending gets for the same section coordinates.
   This handles concurrent requests for the same block from multiple actors. */
static void _block_cache_resolve_pending_gets(block_cache_t* block_cache,
                                               size_t section_id, size_t section_index,
                                               block_t* block) {
  pending_get_t** current = &block_cache->pending_gets;
  while (*current != NULL) {
    if ((*current)->entry->section_id == section_id &&
        (*current)->entry->section_index == section_index) {
      pending_get_t* found = *current;
      *current = found->next;

      cache_get_result_payload_t* result = get_clear_memory(sizeof(cache_get_result_payload_t));
      result->hash = found->hash;
      result->block = block ? (block_t*)refcounter_reference((refcounter_t*)block) : NULL;
      result->reply_to = NULL;
      message_t reply;
      reply.type = CACHE_GET_RESULT;
      reply.payload = result;
      reply.payload_destroy = cache_get_result_payload_destroy;
      actor_send(found->reply_to, &reply);

      index_entry_destroy(found->entry);
      found->hash = NULL;
      free(found);
      /* Don't advance current - the list head may have changed */
    } else {
      current = &(*current)->next;
    }
  }
}

void block_cache_dispatch(void* state, message_t* msg) {
  block_cache_t* block_cache = (block_cache_t*)state;
  if (block_cache == NULL) {
    log_error("block_cache_dispatch: block_cache is NULL");
    abort();
  }
  if (block_cache->index == NULL) {
    log_error("block_cache_dispatch: index is NULL for msg type %d", msg->type);
    abort();
  }
  switch (msg->type) {
    case CACHE_PUT: {
      cache_put_payload_t* p = (cache_put_payload_t*)msg->payload;
      int is_async = (msg->payload_destroy != NULL);
      p->result = CACHE_PUT_ERROR;
      uint32_t result_fib = 0;
      /* Save hash reference before block is destroyed in async path */
      buffer_t* result_hash = (buffer_t*)refcounter_reference((refcounter_t*)p->block->hash);
      index_entry_t* entry = index_peek(block_cache->index, p->block->hash);
      /* Reject new blocks if cache is at capacity */
      if (entry == NULL && block_cache->max_capacity_bytes > 0 &&
          block_cache->current_bytes + (size_t)block_cache->type > block_cache->max_capacity_bytes) {
        /* No room for a new block */
        if (is_async) { block_destroy(p->block); p->block = NULL; }
        DESTROY(result_hash, buffer);
        p->result = CACHE_PUT_FULL;
        if (p->reply_to != NULL) {
          cache_put_result_payload_t* result = get_clear_memory(sizeof(cache_put_result_payload_t));
          result->result = p->result;
          result->fib = 0;
          result->hash = NULL;
          result->reply_to = NULL;
          message_t reply;
          reply.type = CACHE_PUT_RESULT;
          reply.payload = result;
          reply.payload_destroy = free;
          actor_send(p->reply_to, &reply);
        }
        break;
      }
      if (entry == NULL) {
        entry = index_entry_create(p->block->hash);
        /* Set incoming FIB on new entry (0 for local puts, network-provided for remote) */
        entry->counter.fib = p->incoming_fib;
        sections_write_payload_t write_payload;
        write_payload.data = p->block->data;
        write_payload.reply_to = NULL;
        write_payload.result = -1;
        write_payload.section_id = 0;
        write_payload.section_index = 0;
        message_t sections_msg;
        sections_msg.type = SECTIONS_WRITE;
        sections_msg.payload = &write_payload;
        sections_msg.payload_destroy = NULL;
        sections_dispatch(block_cache->sections, &sections_msg);
        int write_result = write_payload.result;
        if (write_result) {
          log_error("CACHE_PUT: sections_write FAILED result=%d hash=%02x%02x%02x%02x...",
                    write_result, p->block->hash->data[0], p->block->hash->data[1],
                    p->block->hash->data[2], p->block->hash->data[3]);
          index_entry_destroy(entry);
          entry = NULL;
          if (is_async) { block_destroy(p->block); p->block = NULL; }
          DESTROY(result_hash, buffer);
          result_hash = NULL;
          p->result = CACHE_PUT_ERROR;
        } else {
          entry->section_id = write_payload.section_id;
          entry->section_index = write_payload.section_index;
          index_entry_t* ejection = (index_entry_t*)refcounter_reference(
              (refcounter_t*)block_lru_cache_put(block_cache->lru, p->block, entry));
          if (ejection) {
            index_set_entry_ejection(block_cache->index, ejection, time(NULL));
            timer_actor_debounce(block_cache->timer_actor, block_cache->index_wait, 0, &block_cache->actor, INDEX_SAVE);
            index_entry_destroy(ejection);
          }
          if (is_async) { block_destroy(p->block); p->block = NULL; }
          refcounter_yield((refcounter_t*) entry);
          index_add(block_cache->index, entry);
          timer_actor_debounce(block_cache->timer_actor, block_cache->index_wait, 0, &block_cache->actor, INDEX_SAVE);
          result_fib = entry->counter.fib;
          block_cache->current_bytes += (size_t)block_cache->type;
          block_cache_update_capacity(block_cache);
          p->result = CACHE_PUT_NEW;
        }
      } else {
        /* Block already exists — update FIB to max(local, incoming) */
        if (p->incoming_fib > entry->counter.fib) {
          entry->counter.fib = p->incoming_fib;
        }
        result_fib = entry->counter.fib;
        if (is_async) { block_destroy(p->block); p->block = NULL; }
        p->result = CACHE_PUT_EXISTS;
      }
      /* Apply the caller's ephemeral claim once the entry exists and the put
         succeeded. entry is valid here on both branches: the NEW branch added
         it to the index (the borrowed pointer stays live), the EXISTS branch
         peeked it; the error paths leave entry NULL or break earlier. */
      if (entry != NULL && p->acquire_ephemeral &&
          p->result != CACHE_PUT_ERROR && p->result != CACHE_PUT_FULL) {
        if (entry->ephemeral_count < UINT16_MAX) {
          entry->ephemeral_count += 1;
          index_write_entry_metadata(block_cache->index, entry);
        } else {
          log_error("CACHE_PUT: ephemeral claim overflow — put proceeds without claim");
        }
      }
      /* Async: send result back if reply_to is set */
      if (p->reply_to != NULL) {
        cache_put_result_payload_t* result = get_clear_memory(sizeof(cache_put_result_payload_t));
        result->result = p->result;
        result->fib = result_fib;
        result->hash = result_hash;
        result->reply_to = NULL;
        message_t reply;
        reply.type = CACHE_PUT_RESULT;
        reply.payload = result;
        reply.payload_destroy = cache_put_result_payload_destroy;
        actor_send(p->reply_to, &reply);
      } else {
        /* No reply_to — clean up hash reference */
        if (result_hash != NULL) {
          DESTROY(result_hash, buffer);
        }
      }
      break;
    }
    case CACHE_GET: {
      cache_get_payload_t* p = (cache_get_payload_t*)msg->payload;
      p->result = NULL;
      index_entry_t* entry = index_peek(block_cache->index, p->hash);
      if (entry == NULL) {
        /* Block not in index — send NULL result if async */
        if (p->reply_to != NULL) {
          cache_get_result_payload_t* result = get_clear_memory(sizeof(cache_get_result_payload_t));
          result->hash = (buffer_t*)refcounter_reference((refcounter_t*)p->hash);
          result->block = NULL;
          result->reply_to = NULL;
          message_t reply;
          reply.type = CACHE_GET_RESULT;
          reply.payload = result;
          reply.payload_destroy = cache_get_result_payload_destroy;
          actor_send(p->reply_to, &reply);
        }
      } else {
        block_t* block = block_lru_cache_get(block_cache->lru, p->hash);
        if (block == NULL) {
          if (p->reply_to != NULL) {
            /* Async: need to read from sections — track pending request */
            _block_cache_add_pending_get(block_cache, p->hash, entry, p->reply_to);
            sections_read(block_cache->sections, entry->section_id,
                                 entry->section_index, &block_cache->actor);
          } else {
            /* Read from sections via direct dispatch */
            sections_read_payload_t read_payload;
            read_payload.section_id = entry->section_id;
            read_payload.section_index = entry->section_index;
            read_payload.reply_to = NULL;
            read_payload.result = NULL;
            message_t sections_msg;
            sections_msg.type = SECTIONS_READ;
            sections_msg.payload = &read_payload;
            sections_msg.payload_destroy = NULL;
            sections_dispatch(block_cache->sections, &sections_msg);
            buffer_t* data = read_payload.result;
            if (data != NULL) {
              if (block_cache_verify_read_hash(data, entry->hash)) {
                block = block_create_existing_data_hash(data, entry->hash);
              } else {
                log_error("block_cache: read hash mismatch for section %u index %u — treating as miss",
                          (unsigned)entry->section_id, (unsigned)entry->section_index);
              }
              buffer_destroy(data);
              if (block != NULL) {
                index_entry_t* ejection = (index_entry_t*)refcounter_reference(
                    (refcounter_t*)block_lru_cache_put(block_cache->lru, block, entry));
                if (ejection) {
                  index_set_entry_ejection(block_cache->index, ejection, time(NULL));
                  timer_actor_debounce(block_cache->timer_actor, block_cache->index_wait, 0, &block_cache->actor, INDEX_SAVE);
                  index_entry_destroy(ejection);
                }
                p->result = block;
              }
            }
          }
        } else {
          /* Cache hit */
          if (p->reply_to != NULL) {
            /* Async: send result back directly */
            cache_get_result_payload_t* result = get_clear_memory(sizeof(cache_get_result_payload_t));
            result->hash = (buffer_t*)refcounter_reference((refcounter_t*)p->hash);
            result->block = block;
            result->reply_to = NULL;
            message_t reply;
            reply.type = CACHE_GET_RESULT;
            reply.payload = result;
            reply.payload_destroy = cache_get_result_payload_destroy;
            actor_send(p->reply_to, &reply);
          } else {
            p->result = block;
          }
        }
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
      } else if (entry->ephemeral_count > 0 && !p->force) {
        log_warn("CACHE_REMOVE: hash holds %u ephemeral claims — remove rejected (use force)",
                 (unsigned)entry->ephemeral_count);
        p->result = CACHE_REMOVE_EPHEMERAL_CLAIMED;
      } else if (entry->pin_count > 0 && !p->force) {
        log_warn("CACHE_REMOVE: hash holds %u pins — remove rejected (use force)",
                 (unsigned)entry->pin_count);
        p->result = CACHE_REMOVE_PINNED;
      } else {
        if (p->force && entry->pin_count > 0) {
          entry->pin_count = 0;
          index_write_entry_metadata(block_cache->index, entry);
        }
        _block_cache_delete_entry(block_cache, p->hash, entry);
        p->result = 0;
      }
      DESTROY(p->hash, buffer);
      p->hash = NULL;
      /* Async: send result back if reply_to is set */
      if (p->reply_to != NULL) {
        cache_remove_result_payload_t* result = get_clear_memory(sizeof(cache_remove_result_payload_t));
        result->result = p->result;
        result->reply_to = NULL;
        message_t reply;
        reply.type = CACHE_REMOVE_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case SECTIONS_READ_RESULT: {
      /* Async: sections read completed — resolve all pending gets for this section */
      sections_read_result_payload_t* p = (sections_read_result_payload_t*)msg->payload;
      buffer_t* data = p->data;
      block_t* block = NULL;
      if (data != NULL) {
        /* We need the entry to get the hash — find first pending to get it */
        pending_get_t* first_pending = _block_cache_find_pending_get(block_cache,
                                                                      p->section_id, p->section_index);
        if (first_pending != NULL) {
          if (block_cache_verify_read_hash(data, first_pending->entry->hash)) {
            block = block_create_existing_data_hash(data, first_pending->entry->hash);
          } else {
            log_error("block_cache: async read hash mismatch for section %u index %u — treating as miss",
                      (unsigned)p->section_id, (unsigned)p->section_index);
          }
          if (block != NULL) {
            index_entry_t* ejection = (index_entry_t*)refcounter_reference(
                (refcounter_t*)block_lru_cache_put(block_cache->lru, block, first_pending->entry));
            if (ejection) {
              index_set_entry_ejection(block_cache->index, ejection, time(NULL));
              timer_actor_debounce(block_cache->timer_actor, block_cache->index_wait, 0, &block_cache->actor, INDEX_SAVE);
              index_entry_destroy(ejection);
            }
          }
          /* Re-add the first pending so _resolve_all can find it along with others */
          first_pending->next = block_cache->pending_gets;
          block_cache->pending_gets = first_pending;
        }
      }

      /* Resolve ALL pending gets for this section, not just the first one */
      _block_cache_resolve_pending_gets(block_cache, p->section_id, p->section_index, block);

      if (block != NULL) {
        block_destroy(block);
      }
      if (data != NULL) {
        buffer_destroy(data);
        p->data = NULL;
      }
      break;
    }
    case CACHE_DEFRAGMENT: {
      cache_defragment_payload_t* p = (cache_defragment_payload_t*)msg->payload;
      p->result = -1;
      p->sections_defragmented = 0;
      p->blocks_relocated = 0;

      /* Dispatch SECTIONS_DEFRAGMENT synchronously */
      sections_defragment_payload_t sections_payload;
      memset(&sections_payload, 0, sizeof(sections_payload));
      sections_payload.occupancy_threshold = p->occupancy_threshold;
      sections_payload.reply_to = NULL;
      sections_payload.result = -1;
      sections_payload.sections_defragmented = 0;
      sections_payload.relocations = NULL;
      sections_payload.relocation_count = 0;
      message_t sections_msg;
      sections_msg.type = SECTIONS_DEFRAGMENT;
      sections_msg.payload = &sections_payload;
      sections_msg.payload_destroy = NULL;
      sections_dispatch(block_cache->sections, &sections_msg);

      if (sections_payload.result == 0 && sections_payload.relocation_count > 0) {
        /* Update the index: iterate all entries and apply relocations */
        index_entry_vec_t* entries = index_to_array(block_cache->index);
        for (size_t entry_idx = 0; entry_idx < entries->length; entry_idx++) {
          index_entry_t* entry = entries->data[entry_idx];
          for (size_t j = 0; j < sections_payload.relocation_count; j++) {
            if (entry->section_id == sections_payload.relocations[j].section_id &&
                entry->section_index == sections_payload.relocations[j].old_index) {
              entry->section_index = sections_payload.relocations[j].new_index;
              break;
            }
          }
          index_entry_destroy(entry);
        }
        vec_deinit(entries);
        free(entries);

        /* Force index snapshot to persist updated section_index values */
        index_debounce(block_cache->index);

        p->blocks_relocated = sections_payload.relocation_count;
      }

      if (sections_payload.relocations != NULL) {
        free(sections_payload.relocations);
      }

      p->result = sections_payload.result;
      p->sections_defragmented = sections_payload.sections_defragmented;

      /* Send completion message if async (reply_to is set) */
      if (p->reply_to != NULL) {
        cache_defragment_result_payload_t* result =
            get_clear_memory(sizeof(cache_defragment_result_payload_t));
        result->result = p->result;
        result->sections_defragmented = p->sections_defragmented;
        result->blocks_relocated = p->blocks_relocated;
        result->reply_to = NULL;
        message_t reply;
        reply.type = CACHE_DEFRAGMENT_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case INDEX_SAVE: {
      index_debounce(block_cache->index);
      break;
    }
    case CACHE_EPHEMERAL: {
      cache_ephemeral_payload_t* p = (cache_ephemeral_payload_t*)msg->payload;
      p->result = CACHE_EPHEMERAL_NOT_FOUND;
      p->previous_count = 0;
      p->new_count = 0;
      index_entry_t* entry = block_lru_cache_peek_entry(block_cache->lru, p->hash);
      if (entry == NULL) {
        entry = index_peek(block_cache->index, p->hash);
      }
      if (entry != NULL) {
        p->previous_count = entry->ephemeral_count;
        switch (p->op) {
          case CACHE_EPHEMERAL_ACQUIRE:
            if (entry->ephemeral_count == UINT16_MAX) {
              /* No silent saturation — the caller must be told the count is
                 exhausted (each claim maps to a live consumer). */
              log_warn("CACHE_EPHEMERAL: acquire rejected — ephemeral count saturated at %u",
                       (unsigned)entry->ephemeral_count);
              p->result = CACHE_EPHEMERAL_OVERFLOW;
              p->new_count = entry->ephemeral_count;
            } else {
              entry->ephemeral_count++;
              p->new_count = entry->ephemeral_count;
              index_write_entry_metadata(block_cache->index, entry);
              p->result = CACHE_EPHEMERAL_OK;
            }
            break;
          case CACHE_EPHEMERAL_RELEASE:
            if (entry->ephemeral_count > 0) {
              entry->ephemeral_count--;
              p->new_count = entry->ephemeral_count;
              index_write_entry_metadata(block_cache->index, entry);
              p->result = CACHE_EPHEMERAL_OK;
              if (entry->ephemeral_count == 0) {
                /* Last claim released — the block is deleted regardless of
                   pin status: ephemeral claims win over pins. */
                _block_cache_delete_entry(block_cache, p->hash, entry);
              }
            } else {
              /* Releasing a block nobody claimed is a benign no-op. */
              p->new_count = 0;
              p->result = CACHE_EPHEMERAL_OK;
            }
            break;
          case CACHE_EPHEMERAL_CLEAR:
            /* Commit the count to zero; announce/remove is the caller's job. */
            if (entry->ephemeral_count > 0) {
              entry->ephemeral_count = 0;
              index_write_entry_metadata(block_cache->index, entry);
            }
            p->new_count = 0;
            p->result = CACHE_EPHEMERAL_OK;
            break;
          default:
            log_warn("CACHE_EPHEMERAL: unknown op %d", (int)p->op);
            break;
        }
      }
      /* Async: send result back if reply_to is set */
      if (p->reply_to != NULL) {
        cache_ephemeral_result_payload_t* result = get_clear_memory(sizeof(cache_ephemeral_result_payload_t));
        result->result = p->result;
        result->previous_count = p->previous_count;
        result->new_count = p->new_count;
        result->hash = p->hash != NULL ? (buffer_t*)refcounter_reference((refcounter_t*)p->hash) : NULL;
        result->reply_to = NULL;
        message_t reply;
        reply.type = CACHE_EPHEMERAL_RESULT;
        reply.payload = result;
        reply.payload_destroy = cache_ephemeral_result_payload_destroy;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case CACHE_PIN:
    case CACHE_UNPIN: {
      cache_pin_payload_t* p = (cache_pin_payload_t*)msg->payload;
      p->result = CACHE_EPHEMERAL_NOT_FOUND;
      p->previous_count = 0;
      p->new_count = 0;
      index_entry_t* entry = block_lru_cache_peek_entry(block_cache->lru, p->hash);
      if (entry == NULL) {
        entry = index_peek(block_cache->index, p->hash);
      }
      if (entry != NULL) {
        p->previous_count = entry->pin_count;
        if (msg->type == CACHE_PIN) {
          if (entry->pin_count == UINT32_MAX) {
            /* Saturate with a warning rather than wrapping to 0. */
            log_warn("CACHE_PIN: pin count saturated at %u — holding",
                     (unsigned)entry->pin_count);
          } else {
            entry->pin_count++;
          }
        } else if (entry->pin_count > 0) {
          entry->pin_count--;
        }
        p->new_count = entry->pin_count;
        index_write_entry_metadata(block_cache->index, entry);
        p->result = CACHE_EPHEMERAL_OK;
      }
      /* Async: send result back if reply_to is set */
      if (p->reply_to != NULL) {
        cache_pin_result_payload_t* result = get_clear_memory(sizeof(cache_pin_result_payload_t));
        result->result = p->result;
        result->previous_count = p->previous_count;
        result->new_count = p->new_count;
        result->reply_to = NULL;
        message_t reply;
        reply.type = (msg->type == CACHE_PIN) ? CACHE_PIN_RESULT : CACHE_UNPIN_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      /* An unpin can make entries sheddable again — a cache at capacity whose
         entries were all pinned should be able to exhale now instead of
         waiting for the next put or deletion. Self-guards on
         max_capacity_bytes/authority, so no-network setups are unaffected. */
      if (msg->type == CACHE_UNPIN && p->result == CACHE_EPHEMERAL_OK) {
        block_cache_update_capacity(block_cache);
      }
      break;
    }
    case CACHE_EPHEMERAL_LIST: {
      cache_ephemeral_list_payload_t* p = (cache_ephemeral_list_payload_t*)msg->payload;
      p->hashes = NULL;
      p->ephemeral_counts = NULL;
      p->pin_counts = NULL;
      p->count = 0;
      index_entry_vec_t* entries = index_to_array(block_cache->index);
      size_t ephemeral_total = 0;
      for (size_t entry_idx = 0; entry_idx < entries->length; entry_idx++) {
        if (entries->data[entry_idx]->ephemeral_count > 0) {
          ephemeral_total++;
        }
      }
      if (ephemeral_total > 0) {
        p->hashes = get_clear_memory(sizeof(buffer_t*) * ephemeral_total);
        p->ephemeral_counts = get_clear_memory(sizeof(uint16_t) * ephemeral_total);
        p->pin_counts = get_clear_memory(sizeof(uint32_t) * ephemeral_total);
        size_t out_idx = 0;
        for (size_t entry_idx = 0; entry_idx < entries->length; entry_idx++) {
          index_entry_t* entry = entries->data[entry_idx];
          if (entry->ephemeral_count > 0) {
            p->hashes[out_idx] = (buffer_t*)refcounter_reference((refcounter_t*)entry->hash);
            p->ephemeral_counts[out_idx] = entry->ephemeral_count;
            p->pin_counts[out_idx] = entry->pin_count;
            out_idx++;
          }
        }
        p->count = ephemeral_total;
      }
      for (size_t entry_idx = 0; entry_idx < entries->length; entry_idx++) {
        index_entry_destroy(entries->data[entry_idx]);
      }
      vec_deinit(entries);
      free(entries);
      if (p->reply_to != NULL) {
        /* Mirror the same message (and payload) back to the reply actor. The
           consumer steals the arrays and nulls them in the payload, then its
           actor_run's payload_destroy frees the emptied shell. Null our copy
           so this actor_run does not destroy the payload a second time. */
        actor_send(p->reply_to, msg);
        msg->payload = NULL;
        msg->payload_destroy = NULL;
      }
      break;
    }
    default:
      break;
  }
}

/* ---- block_cache implementation ---- */

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, timer_actor_t* timer_actor, scheduler_pool_t* pool, authority_t* authority, size_t max_capacity_bytes) {
  block_cache_t* block_cache = get_clear_memory(sizeof(block_cache_t));
  refcounter_init_actor((refcounter_t*) block_cache);
  block_cache->type = type;
  block_cache->fsync_data = config.fsync_data;
  block_cache->pool = pool;
  block_cache->timer_actor = timer_actor;
  block_cache->index_wait = config.index_wait;
  block_cache->authority = authority;
  block_cache->max_capacity_bytes = max_capacity_bytes;
  block_cache->current_bytes = 0;
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
      folder = path_join(location, "mega");
      break;
  }
  actor_init(&block_cache->actor, block_cache, block_cache_dispatch, pool);
  block_cache->lru = block_lru_cache_create(config.lru_size);
  block_cache->sections = sections_create(folder, config.section_size, config.cache_size, config.max_tuple_size, type, timer_actor, pool, config.section_wait, config.section_max_wait, config.fsync_data);
  int error_code;
  block_cache->index = index_create(config.index_bucket_size, folder, config.index_wait, config.index_max_wait, config.max_snapshots, config.max_wals, &error_code);
  if (block_cache->index == NULL) {
    log_error("block_cache_create: index_create returned NULL (error_code=%d)", error_code);
  } else {
    /* Set after index_create so both return paths (normal load and the
       total-loss _index_new_empty fallback inside index_create) inherit
       the configured fsync_data. Tests that call index_create directly
       get a zeroed struct → fsync_data = false → fast path. */
    block_cache->index->fsync_data = config.fsync_data;
  }
  if (max_capacity_bytes > 0) {
    block_cache->current_bytes = index_count(block_cache->index) * (size_t)type;
    block_cache_update_capacity(block_cache);
  }
  /* Registry lives next to the index (same folder), so it must be created
     before the folder path is freed below. A failed create degrades to
     "no registry" (log, not abort) — the public wrappers are NULL-guarded,
     so the cache keeps working without the advisory filter instead of
     NULL-dereferencing later. */
  block_cache->registry = ephemeral_registry_create(folder, config, pool);
  if (block_cache->registry == NULL) {
    log_error("block_cache_create: ephemeral registry create failed — continuing without it");
  }
  free(folder);
  return block_cache;
}

void block_cache_destroy(block_cache_t* block_cache) {
  if (block_cache == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*) block_cache)) {
    refcounter_destroy_lock((refcounter_t*) block_cache);
    if (block_cache->timer_actor != NULL
        && block_cache->pool != NULL && !atomic_load(&block_cache->pool->terminate)) {
      /* Mark the actor destroyed BEFORE flushing debounces — any subsequent
         TIMER_COMPLETION forwarded from the timer_actor will be dropped
         at actor_send (ACTOR_FLAG_DESTROY check) rather than racing with
         index/sections teardown. The flush still cancels the pd_timer
         and frees the completion payload, but no new work reaches the
         actor. */
      atomic_fetch_or(&block_cache->actor.flags, ACTOR_FLAG_DESTROY);
      timer_actor_debounce_flush(block_cache->timer_actor,
                                  &block_cache->actor, INDEX_SAVE);
      platform_sleep_ms(10);
      scheduler_pool_wait_for_idle(block_cache->pool);
    }
    index_destroy(block_cache->index);
    /* The registry actor must not be processing when destroyed — the
       wait_for_idle above covers it (the registry's own destroy re-waits). */
    ephemeral_registry_destroy(block_cache->registry);
    sections_destroy(block_cache->sections);
    block_lru_cache_destroy(block_cache->lru);
    actor_destroy(&block_cache->actor);
    /* Fill freed memory with poison pattern to detect use-after-free */
    memset(block_cache, 0xDD, sizeof(block_cache_t));
    free(block_cache);
  }
}

size_t block_cache_count(block_cache_t* block_cache) {
  return index_count(block_cache->index);
}

void block_cache_update_capacity(block_cache_t* block_cache) {
  if (block_cache == NULL) return;
  if (block_cache->max_capacity_bytes == 0) return;
  if (block_cache->authority == NULL) return;
  float capacity = (float)block_cache->current_bytes / (float)block_cache->max_capacity_bytes;
  if (capacity > 1.0f) capacity = 1.0f;
  authority_update_capacity(block_cache->authority, capacity);
  authority_update_phase(block_cache->authority, capacity);
  if (respiration_should_exhale(capacity) && block_cache->respiration != NULL) {
    respiration_actor_t* respiration = block_cache->respiration;
    if (atomic_load(&respiration->state) == RESPIRATION_IDLE) {
      index_entry_vec_t* entries = index_entries_by_ejection_date(block_cache->index);
      /* Pinned permanent blocks and ephemeral blocks are never shed — count
         the sheddable candidates first and skip the trigger entirely when
         there is nothing to exhale. */
      size_t candidate_count = 0;
      if (entries != NULL) {
        for (size_t entry_idx = 0; entry_idx < entries->length; entry_idx++) {
          if (block_cache_entry_is_sheddable(entries->data[entry_idx])) {
            candidate_count++;
          }
        }
      }
      if (candidate_count > 0) {
        respiration_exhale_payload_t* payload = get_clear_memory(sizeof(respiration_exhale_payload_t));
        payload->count = candidate_count;
        payload->hashes = get_clear_memory(sizeof(buffer_t*) * candidate_count);
        payload->ejection_dates = get_clear_memory(sizeof(uint64_t) * candidate_count);
        payload->capacity = capacity;
        size_t out_idx = 0;
        for (size_t entry_idx = 0; entry_idx < entries->length; entry_idx++) {
          index_entry_t* entry = entries->data[entry_idx];
          if (block_cache_entry_is_sheddable(entry)) {
            payload->hashes[out_idx] = (buffer_t*)refcounter_reference((refcounter_t*)entry->hash);
            payload->ejection_dates[out_idx] = entry->ejection_date;
            out_idx++;
          }
        }
        message_t msg;
        msg.type = RESPIRATION_EXHALE_TRIGGER;
        msg.payload = payload;
        msg.payload_destroy = respiration_exhale_payload_destroy;
        actor_send(&respiration->actor, &msg);
      }
      if (entries != NULL) {
        for (size_t entry_idx = 0; entry_idx < entries->length; entry_idx++) {
          index_entry_destroy(entries->data[entry_idx]);
        }
        vec_deinit(entries);
        free(entries);
      }
    }
  }
}

void block_cache_set_max_capacity(block_cache_t* block_cache, size_t max_capacity_bytes) {
  if (block_cache == NULL) return;
  block_cache->max_capacity_bytes = max_capacity_bytes;
  if (max_capacity_bytes > 0) {
    block_cache->current_bytes = index_count(block_cache->index) * (size_t)block_cache->type;
    block_cache_update_capacity(block_cache);
  }
}

/* ---- Async API ---- */

void block_cache_get(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to) {
  cache_get_payload_t* payload = get_clear_memory(sizeof(cache_get_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->result = NULL;

  message_t msg;
  msg.type = CACHE_GET;
  msg.payload = payload;
  msg.payload_destroy = cache_get_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_put(block_cache_t* block_cache, block_t* block, uint32_t incoming_fib, actor_t* reply_to) {
  cache_put_payload_t* payload = get_clear_memory(sizeof(cache_put_payload_t));
  payload->block = (block_t*)refcounter_reference((refcounter_t*)block);
  payload->incoming_fib = incoming_fib;
  payload->reply_to = reply_to;
  payload->result = CACHE_PUT_ERROR;
  payload->acquire_ephemeral = 0;

  message_t msg;
  msg.type = CACHE_PUT;
  msg.payload = payload;
  msg.payload_destroy = cache_put_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

/* Put that acquires one claim (count += 1) on the stored block; the first
   claim on a fresh block makes it 1. Works for both a new put and a block
   that already exists. */
void block_cache_put_ephemeral(block_cache_t* block_cache, block_t* block, actor_t* reply_to) {
  cache_put_payload_t* payload = get_clear_memory(sizeof(cache_put_payload_t));
  payload->block = (block_t*)refcounter_reference((refcounter_t*)block);
  payload->incoming_fib = 0;
  payload->reply_to = reply_to;
  payload->acquire_ephemeral = 1;
  payload->result = CACHE_PUT_ERROR;

  message_t msg;
  msg.type = CACHE_PUT;
  msg.payload = payload;
  msg.payload_destroy = cache_put_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_remove(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to) {
  block_cache_remove_ex(block_cache, hash, 0, reply_to);
}

void block_cache_remove_ex(block_cache_t* block_cache, buffer_t* hash, uint8_t force, actor_t* reply_to) {
  cache_remove_payload_t* payload = get_clear_memory(sizeof(cache_remove_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->force = force;
  payload->result = -1;

  message_t msg;
  msg.type = CACHE_REMOVE;
  msg.payload = payload;
  msg.payload_destroy = cache_remove_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_ephemeral(block_cache_t* block_cache, buffer_t* hash, cache_ephemeral_op_e op, actor_t* reply_to) {
  cache_ephemeral_payload_t* payload = get_clear_memory(sizeof(cache_ephemeral_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->op = op;
  payload->result = CACHE_EPHEMERAL_NOT_FOUND;
  payload->previous_count = 0;
  payload->new_count = 0;

  message_t msg;
  msg.type = CACHE_EPHEMERAL;
  msg.payload = payload;
  msg.payload_destroy = cache_ephemeral_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_pin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to) {
  cache_pin_payload_t* payload = get_clear_memory(sizeof(cache_pin_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->result = CACHE_EPHEMERAL_NOT_FOUND;
  payload->previous_count = 0;
  payload->new_count = 0;

  message_t msg;
  msg.type = CACHE_PIN;
  msg.payload = payload;
  msg.payload_destroy = cache_pin_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_unpin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to) {
  cache_pin_payload_t* payload = get_clear_memory(sizeof(cache_pin_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->result = CACHE_EPHEMERAL_NOT_FOUND;
  payload->previous_count = 0;
  payload->new_count = 0;

  message_t msg;
  msg.type = CACHE_UNPIN;
  msg.payload = payload;
  msg.payload_destroy = cache_pin_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_list_ephemeral(block_cache_t* block_cache, actor_t* reply_to) {
  cache_ephemeral_list_payload_t* payload = get_clear_memory(sizeof(cache_ephemeral_list_payload_t));
  payload->reply_to = reply_to;
  payload->count = 0;
  payload->hashes = NULL;
  payload->ephemeral_counts = NULL;
  payload->pin_counts = NULL;

  message_t msg;
  msg.type = CACHE_EPHEMERAL_LIST;
  msg.payload = payload;
  msg.payload_destroy = cache_ephemeral_list_payload_destroy_adapter;

  actor_send(&block_cache->actor, &msg);
}

int block_cache_can_fit(block_cache_t* block_cache, size_t required_bytes) {
  if (block_cache == NULL) {
    return CACHE_FIT_OK;
  }
  if (block_cache->max_capacity_bytes == 0) {
    return CACHE_FIT_OK;
  }
  if (block_cache->current_bytes + required_bytes > block_cache->max_capacity_bytes) {
    return CACHE_FIT_FULL;
  }
  return CACHE_FIT_OK;
}

void block_cache_defragment(block_cache_t* block_cache, float occupancy_threshold, actor_t* reply_to) {
  cache_defragment_payload_t* payload = get_clear_memory(sizeof(cache_defragment_payload_t));
  payload->occupancy_threshold = occupancy_threshold;
  payload->reply_to = reply_to;
  payload->result = -1;
  payload->sections_defragmented = 0;
  payload->blocks_relocated = 0;

  message_t msg;
  msg.type = CACHE_DEFRAGMENT;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_sync(block_cache_t* block_cache) {
  if (block_cache == NULL) return;

  /* Flush the index debounce timer — sends INDEX_SAVE immediately
     through the block_cache actor's dispatch path. */
  timer_actor_debounce_flush(block_cache->timer_actor,
                             &block_cache->actor, INDEX_SAVE);

  /* Wait for the timer thread to process the flush and deliver INDEX_SAVE
     to the block_cache actor, then wait for the scheduler to process it.
     The timer thread polls at 100ms intervals, so 10ms is a safe minimum. */
  platform_sleep_ms(10);
  scheduler_pool_wait_for_idle(block_cache->pool);

  /* Sync the WAL to disk for crash durability. */
  index_sync(block_cache->index);
}