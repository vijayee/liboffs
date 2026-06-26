//
// Created by victor on 5/6/25.
//

#ifndef OFFS_ACTOR_H
#define OFFS_ACTOR_H

#include "message_queue.h"
#include "../Util/atomic_compat.h"
#include <stddef.h>
#include <stdint.h>

#define ACTOR_BATCH_SIZE 32
#define ACTOR_FLAG_SCHEDULED 0x01
#define ACTOR_FLAG_OVERLOADED 0x02
#define ACTOR_FLAG_RUNNING 0x04
#define ACTOR_FLAG_DESTROY 0x08
#define ACTOR_FLAG_PRESSURED 0x10
#define ACTOR_FLAG_MUTED 0x20

typedef struct scheduler_pool_t scheduler_pool_t;

typedef struct muted_sender_node_t {
  struct actor_t* sender;
  struct muted_sender_node_t* next;
} muted_sender_node_t;

typedef struct actor_t {
  message_queue_t queue;
  ATOMIC(uint8_t) flags;
  ATOMIC(muted_sender_node_t*) pressured_senders;
  scheduler_pool_t* pool;
  void* state;
  void (*dispatch)(void* state, message_t* msg);
  /* Intrusive doubly-linked list nodes for the owning pool's actor registry.
     The registry lets scheduler_pool_wait_for_idle enumerate every actor and
     re-inject any whose mailbox is non-empty but which is not scheduled — the
     recovery path for the elusive work-stealing strand (see
     scheduler_pool_wait_for_idle). Only touched under the pool's
     registry_lock; NULL for inline actors (pool == NULL). */
  struct actor_t* registry_prev;
  struct actor_t* registry_next;
} actor_t;

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg), scheduler_pool_t* pool);
void actor_destroy(actor_t* actor);
/* Unregister the actor from its pool's registry without draining the queue or
   running backpressure_release. The registry-only subset of actor_destroy, for
   callers that tear an actor down themselves (set ACTOR_FLAG_DESTROY, drain the
   queue, free the struct) and intentionally bypass actor_destroy to avoid
   backpressure_release waking worker threads during shutdown. Without this, a
   freed actor stays in the registry and the next registry traversal would
   dereference freed memory. Idempotent: nulls actor->pool, so a later call (or
   actor_destroy / the pool-destroy detach loop having nulled pool) is a no-op. */
void actor_detach_pool(actor_t* actor);
bool actor_send(actor_t* actor, message_t* msg);
bool actor_run(actor_t* actor, size_t batch_size);
void backpressure_apply(actor_t* actor);
void backpressure_release(actor_t* actor);

#endif // OFFS_ACTOR_H