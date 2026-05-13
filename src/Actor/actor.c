//
// Created by victor on 5/6/25.
//

#include "actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/allocator.h"
#include <stdbool.h>
#include <stdlib.h>

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg), scheduler_pool_t* pool) {
  message_queue_init(&actor->queue);
  atomic_store(&actor->flags, 0);
  actor->pool = pool;
  actor->state = state;
  actor->dispatch = dispatch;
}

void actor_destroy(actor_t* actor) {
  message_queue_destroy(&actor->queue);
}

bool actor_send(actor_t* actor, message_t* msg) {
  message_node_t* node = get_clear_memory(sizeof(message_node_t));
  node->msg = *msg;
  bool was_empty = message_queue_push(&actor->queue, node, node);
  if (was_empty) {
    atomic_fetch_or(&actor->flags, ACTOR_FLAG_SCHEDULED);
    if (actor->pool != NULL) {
      scheduler_inject(actor->pool, actor);
    }
  }
  return was_empty;
}

bool actor_run(actor_t* actor, size_t batch_size) {
  for (size_t count = 0; count < batch_size; count++) {
    message_node_t* node = message_queue_pop(&actor->queue);
    if (node == NULL) {
      break;
    }
    actor->dispatch(actor->state, &node->msg);
    if (node->msg.payload_destroy != NULL && node->msg.payload != NULL) {
      node->msg.payload_destroy(node->msg.payload);
    }
    /* If dispatch requested self-destruction, stop processing immediately.
       The caller (typically the scheduler) must check ACTOR_FLAG_DESTROY
       and skip further operations on this actor. */
    if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
      return false;
    }
  }
  if (message_queue_markempty(&actor->queue)) {
    atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_SCHEDULED);
    return false;
  }
  return true;
}