//
// Created by victor on 5/6/25.
//

#include "actor.h"
#include "../Util/allocator.h"
#include <stdbool.h>
#include <stdlib.h>

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg)) {
  message_queue_init(&actor->queue);
  atomic_store(&actor->flags, 0);
  actor->state = state;
  actor->dispatch = dispatch;
  platform_lock_init(&actor->run_lock);
  platform_condition_init(&actor->run_cond);
}

void actor_destroy(actor_t* actor) {
  message_queue_destroy(&actor->queue);
  platform_lock_destroy(&actor->run_lock);
  platform_condition_destroy(&actor->run_cond);
}

void actor_claim_running(actor_t* actor) {
  platform_lock(&actor->run_lock);
  while (true) {
    uint8_t flags = atomic_load(&actor->flags);
    if (flags & ACTOR_FLAG_RUNNING) {
      platform_condition_wait(&actor->run_lock, &actor->run_cond);
      continue;
    }
    if (atomic_compare_exchange_strong(&actor->flags, &flags, flags | ACTOR_FLAG_RUNNING)) {
      break;
    }
  }
  platform_unlock(&actor->run_lock);
}

void actor_release_running(actor_t* actor) {
  atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_RUNNING);
  platform_lock(&actor->run_lock);
  platform_broadcast_condition(&actor->run_cond);
  platform_unlock(&actor->run_lock);
}

bool actor_send(actor_t* actor, message_t* msg) {
  message_node_t* node = get_clear_memory(sizeof(message_node_t));
  node->msg = *msg;
  bool was_empty = message_queue_push(&actor->queue, node, node);
  if (was_empty) {
    atomic_fetch_or(&actor->flags, ACTOR_FLAG_SCHEDULED);
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
  }
  if (message_queue_markempty(&actor->queue)) {
    atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_SCHEDULED);
    return false;
  }
  return true;
}
