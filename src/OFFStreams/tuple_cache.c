//
// Created by victor on 5/7/26.
//

#include "tuple_cache.h"
#include "../Util/allocator.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Scheduler/scheduler.h"
#include <string.h>

/* Hash and compare functions for tuple_t keys in the hashmap */
static size_t tuple_hash_func(const tuple_t* key) {
  return (size_t)tuple_hash((tuple_t*)key);
}

static int tuple_compare_func(const tuple_t* left, const tuple_t* right) {
  return !tuple_equals((tuple_t*)left, (tuple_t*)right);
}

static tuple_t* tuple_dup_func(const tuple_t* key) {
  return (tuple_t*)refcounter_reference((refcounter_t*)key);
}

static void tuple_free_func(tuple_t* key) {
  tuple_destroy(key);
}

/* Move a node to the front of the LRU list */
static void tuple_cache_lru_move(tuple_cache_lru_t* lru, tuple_cache_lru_node_t* node) {
  if (lru->first == NULL) {
    lru->first = node;
    lru->last = node;
  } else {
    if (lru->first == node) {
      return;
    }
    if (lru->first == lru->last) {
      node->next = lru->first;
      lru->first->previous = node;
      lru->last = lru->first;
      lru->first = node;
    } else if (lru->last == node) {
      lru->last = node->previous;
      lru->last->next = NULL;
      node->next = lru->first;
      node->previous = NULL;
      lru->first->previous = node;
      lru->first = node;
    } else {
      if (node->next == NULL && node->previous == NULL) {
        lru->first->previous = node;
        node->next = lru->first;
        lru->first = node;
      } else {
        if (node->previous != NULL) {
          node->previous->next = node->next;
        }
        if (node->next != NULL) {
          node->next->previous = node->previous;
        }
        node->next = lru->first;
        node->previous = NULL;
        lru->first->previous = node;
        lru->first = node;
      }
    }
  }
}

tuple_cache_lru_t* tuple_cache_lru_create(size_t capacity) {
  tuple_cache_lru_t* lru = get_clear_memory(sizeof(tuple_cache_lru_t));
  lru->capacity = capacity;
  lru->first = NULL;
  lru->last = NULL;
  hashmap_init(&lru->cache, tuple_hash_func, tuple_compare_func);
  hashmap_set_key_alloc_funcs(&lru->cache, tuple_dup_func, tuple_free_func);
  return lru;
}

void tuple_cache_lru_destroy(tuple_cache_lru_t* lru) {
  tuple_cache_lru_node_t* node;
  hashmap_foreach_data(node, &lru->cache) {
    if (node->value != NULL) {
      DESTROY(node->value, buffer);
    }
    DESTROY(node->key, tuple);
    free(node);
  }
  hashmap_cleanup(&lru->cache);
  free(lru);
}

buffer_t* tuple_cache_lru_get(tuple_cache_lru_t* lru, tuple_t* key) {
  tuple_cache_lru_node_t* node = hashmap_get(&lru->cache, key);
  if (node == NULL) {
    return NULL;
  }
  tuple_cache_lru_move(lru, node);
  return (buffer_t*)refcounter_reference((refcounter_t*)node->value);
}

void tuple_cache_lru_put(tuple_cache_lru_t* lru, tuple_t* key, buffer_t* value) {
  if (lru->capacity == 0) {
    return;
  }
  tuple_cache_lru_node_t* node = hashmap_get(&lru->cache, key);
  if (node == NULL) {
    node = get_clear_memory(sizeof(tuple_cache_lru_node_t));
    node->key = (tuple_t*)refcounter_reference((refcounter_t*)key);
    node->value = (buffer_t*)refcounter_reference((refcounter_t*)value);
    if (hashmap_size(&lru->cache) >= lru->capacity) {
      if (lru->last != NULL) {
        tuple_cache_lru_node_t* last_node = lru->last;
        if (last_node->previous == NULL) {
          lru->last = NULL;
          lru->first = NULL;
        } else {
          lru->last = last_node->previous;
          lru->last->next = NULL;
        }
        hashmap_remove(&lru->cache, last_node->key);
        if (last_node->value != NULL) {
          DESTROY(last_node->value, buffer);
        }
        DESTROY(last_node->key, tuple);
        free(last_node);
      }
    }
    hashmap_put(&lru->cache, key, node);
  } else {
    if (node->value != NULL) {
      DESTROY(node->value, buffer);
    }
    node->value = (buffer_t*)refcounter_reference((refcounter_t*)value);
  }
  tuple_cache_lru_move(lru, node);
}

void tuple_cache_lru_remove(tuple_cache_lru_t* lru, tuple_t* key) {
  tuple_cache_lru_node_t* node = hashmap_get(&lru->cache, key);
  if (node == NULL) {
    return;
  }
  if (node->previous == NULL) {
    if (node->next == NULL) {
      lru->first = NULL;
      lru->last = NULL;
    } else {
      node->next->previous = NULL;
      lru->first = node->next;
    }
  } else {
    if (node->next == NULL) {
      node->previous->next = NULL;
      lru->last = node->previous;
    } else {
      node->previous->next = node->next;
      node->next->previous = node->previous;
    }
  }
  hashmap_remove(&lru->cache, key);
  if (node->value != NULL) {
    DESTROY(node->value, buffer);
  }
  DESTROY(node->key, tuple);
  free(node);
}

uint8_t tuple_cache_lru_contains(tuple_cache_lru_t* lru, tuple_t* key) {
  return hashmap_get(&lru->cache, key) != NULL;
}

size_t tuple_cache_lru_size(tuple_cache_lru_t* lru) {
  return hashmap_size(&lru->cache);
}

/* ---- Payload types ---- */

typedef struct {
  tuple_t* key;
  actor_t* reply_to;
  buffer_t* result;
} tuple_cache_get_payload_t;

typedef struct {
  tuple_t* key;
  buffer_t* value;
  actor_t* reply_to;
} tuple_cache_put_payload_t;

typedef struct {
  tuple_t* key;
  actor_t* reply_to;
} tuple_cache_remove_payload_t;

typedef struct {
  tuple_t* key;
  actor_t* reply_to;
  uint8_t result;
} tuple_cache_contains_payload_t;

typedef struct {
  actor_t* reply_to;
  size_t result;
} tuple_cache_size_payload_t;

/* ---- Actor dispatch ---- */

void tuple_cache_dispatch(void* state, message_t* msg) {
  tuple_cache_t* tc = (tuple_cache_t*)state;
  switch (msg->type) {
    case TUPLE_CACHE_GET: {
      tuple_cache_get_payload_t* payload = (tuple_cache_get_payload_t*)msg->payload;
      payload->result = tuple_cache_lru_get(tc->lru, payload->key);
      if (payload->reply_to != NULL) {
        tuple_cache_get_result_payload_t* result = get_clear_memory(sizeof(tuple_cache_get_result_payload_t));
        result->key = payload->key;
        result->value = payload->result;
        result->reply_to = NULL;
        message_t reply;
        reply.type = TUPLE_CACHE_GET_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(payload->reply_to, &reply);
      }
      break;
    }
    case TUPLE_CACHE_PUT: {
      tuple_cache_put_payload_t* payload = (tuple_cache_put_payload_t*)msg->payload;
      tuple_cache_lru_put(tc->lru, payload->key, payload->value);
      break;
    }
    case TUPLE_CACHE_REMOVE: {
      tuple_cache_remove_payload_t* payload = (tuple_cache_remove_payload_t*)msg->payload;
      tuple_cache_lru_remove(tc->lru, payload->key);
      break;
    }
    case TUPLE_CACHE_CONTAINS: {
      tuple_cache_contains_payload_t* payload = (tuple_cache_contains_payload_t*)msg->payload;
      payload->result = tuple_cache_lru_contains(tc->lru, payload->key);
      break;
    }
    case TUPLE_CACHE_SIZE: {
      tuple_cache_size_payload_t* payload = (tuple_cache_size_payload_t*)msg->payload;
      payload->result = tuple_cache_lru_size(tc->lru);
      break;
    }
    default:
      break;
  }
}

/* ---- Public API (sync wrappers) ---- */

tuple_cache_t* tuple_cache_create(size_t capacity, scheduler_pool_t* pool) {
  tuple_cache_t* tc = get_clear_memory(sizeof(tuple_cache_t));
  refcounter_init((refcounter_t*)tc);
  tc->lru = tuple_cache_lru_create(capacity);
  tc->pool = pool;
  actor_init(&tc->actor, tc, tuple_cache_dispatch, tc->pool);
  return tc;
}

void tuple_cache_destroy(tuple_cache_t* tc) {
  if (refcounter_dereference_is_zero((refcounter_t*)tc)) {
    tuple_cache_lru_destroy(tc->lru);
    actor_destroy(&tc->actor);
    refcounter_destroy_lock((refcounter_t*)tc);
    free(tc);
  }
}


/* ---- Async API ---- */

void tuple_cache_get_async(tuple_cache_t* tc, tuple_t* key, actor_t* reply_to) {
  tuple_cache_get_payload_t* payload = get_clear_memory(sizeof(tuple_cache_get_payload_t));
  payload->key = key;
  payload->reply_to = reply_to;
  payload->result = NULL;

  message_t msg;
  msg.type = TUPLE_CACHE_GET;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&tc->actor, &msg);
}

void tuple_cache_put_async(tuple_cache_t* tc, tuple_t* key, buffer_t* value) {
  tuple_cache_put_payload_t* payload = get_clear_memory(sizeof(tuple_cache_put_payload_t));
  payload->key = key;
  payload->value = value;
  payload->reply_to = NULL;

  message_t msg;
  msg.type = TUPLE_CACHE_PUT;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&tc->actor, &msg);
}

/* Sync API — direct dispatch */
void tuple_cache_update(tuple_cache_t* tc, tuple_t* key, buffer_t* value) {
  tuple_cache_put_payload_t payload;
  payload.key = key;
  payload.value = value;
  payload.reply_to = NULL;

  message_t msg;
  msg.type = TUPLE_CACHE_PUT;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  tuple_cache_dispatch(tc, &msg);
}

buffer_t* tuple_cache_apply(tuple_cache_t* tc, tuple_t* key) {
  tuple_cache_get_payload_t payload;
  payload.key = key;
  payload.reply_to = NULL;
  payload.result = NULL;

  message_t msg;
  msg.type = TUPLE_CACHE_GET;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  tuple_cache_dispatch(tc, &msg);
  return payload.result;
}

uint8_t tuple_cache_contains(tuple_cache_t* tc, tuple_t* key) {
  tuple_cache_contains_payload_t payload;
  payload.key = key;
  payload.reply_to = NULL;
  payload.result = 0;

  message_t msg;
  msg.type = TUPLE_CACHE_CONTAINS;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  tuple_cache_dispatch(tc, &msg);
  return payload.result;
}

size_t tuple_cache_size(tuple_cache_t* tc) {
  tuple_cache_size_payload_t payload;
  payload.reply_to = NULL;
  payload.result = 0;

  message_t msg;
  msg.type = TUPLE_CACHE_SIZE;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  tuple_cache_dispatch(tc, &msg);
  return payload.result;
}

void tuple_cache_remove(tuple_cache_t* tc, tuple_t* key) {
  tuple_cache_remove_payload_t payload;
  payload.key = key;
  payload.reply_to = NULL;

  message_t msg;
  msg.type = TUPLE_CACHE_REMOVE;
  msg.payload = &payload;
  msg.payload_destroy = NULL;

  tuple_cache_dispatch(tc, &msg);
}