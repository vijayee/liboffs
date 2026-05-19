//
// Created by victor on 9/10/25.
//

#include "block_cache.h"
#include "sections.h"
#include "../Network/authority.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Util/path_join.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Scheduler/scheduler.h"
#include "../Util/log.h"
#include <stdatomic.h>
#include <time.h>

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
      reply.payload_destroy = free;
      actor_send(found->reply_to, &reply);

      index_entry_destroy(found->entry);
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
            index_entry_destroy(ejection);
          }
          if (is_async) { block_destroy(p->block); p->block = NULL; }
          refcounter_yield((refcounter_t*) entry);
          index_add(block_cache->index, entry);
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
          reply.payload_destroy = free;
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
              block = block_create_existing_data_hash(data, entry->hash);
              buffer_destroy(data);
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
            reply.payload_destroy = free;
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
      } else {
        size_t section_id = entry->section_id;
        size_t section_index = entry->section_index;
        index_remove(block_cache->index, p->hash);
        block_cache->current_bytes -= (size_t)block_cache->type;
        block_cache_update_capacity(block_cache);
        block_lru_cache_delete(block_cache->lru, p->hash);
        section_deallocate_payload_t dealloc_payload;
        dealloc_payload.index = section_index;
        dealloc_payload.reply_to = NULL;
        dealloc_payload.result = -1;
        message_t dealloc_msg;
        dealloc_msg.type = SECTION_DEALLOCATE;
        dealloc_msg.payload = &dealloc_payload;
        dealloc_msg.payload_destroy = NULL;
        sections_dispatch(block_cache->sections, &dealloc_msg);
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
          block = block_create_existing_data_hash(data, first_pending->entry->hash);
          if (block != NULL) {
            index_entry_t* ejection = (index_entry_t*)refcounter_reference(
                (refcounter_t*)block_lru_cache_put(block_cache->lru, block, first_pending->entry));
            if (ejection) {
              index_set_entry_ejection(block_cache->index, ejection, time(NULL));
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
    default:
      break;
  }
}

/* ---- block_cache implementation ---- */

block_cache_t* block_cache_create(config_t config, char* location, block_size_e type, timer_actor_t* timer_actor, scheduler_pool_t* pool, authority_t* authority, size_t max_capacity_bytes) {
  block_cache_t* block_cache = get_clear_memory(sizeof(block_cache_t));
  refcounter_init_actor((refcounter_t*) block_cache);
  block_cache->type = type;
  block_cache->pool = pool;
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
  block_cache->lru = block_lru_cache_create(config.lru_size);
  block_cache->sections = sections_create(folder, config.section_size, config.cache_size, config.max_tuple_size, type, timer_actor, pool, config.section_wait, config.section_max_wait);
  int error_code;
  block_cache->index = index_create(config.index_bucket_size, folder, timer_actor, config.index_wait, config.index_max_wait, config.max_snapshots, config.max_wals, &error_code);
  if (block_cache->index == NULL) {
    log_error("block_cache_create: index_create returned NULL (error_code=%d)", error_code);
  }
  if (max_capacity_bytes > 0) {
    block_cache->current_bytes = index_count(block_cache->index) * (size_t)type;
    block_cache_update_capacity(block_cache);
  }
  actor_init(&block_cache->actor, block_cache, block_cache_dispatch, pool);
  free(folder);
  return block_cache;
}

void block_cache_destroy(block_cache_t* block_cache) {
  if (block_cache == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*) block_cache)) {
    refcounter_destroy_lock((refcounter_t*) block_cache);
    index_destroy(block_cache->index);
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

  message_t msg;
  msg.type = CACHE_PUT;
  msg.payload = payload;
  msg.payload_destroy = cache_put_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}

void block_cache_remove(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to) {
  cache_remove_payload_t* payload = get_clear_memory(sizeof(cache_remove_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->result = -1;

  message_t msg;
  msg.type = CACHE_REMOVE;
  msg.payload = payload;
  msg.payload_destroy = cache_remove_payload_destroy;

  actor_send(&block_cache->actor, &msg);
}