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

typedef struct scheduler_pool_t scheduler_pool_t;

typedef struct actor_t {
  message_queue_t queue;
  ATOMIC(uint8_t) flags;
  scheduler_pool_t* pool;
  void* state;
  void (*dispatch)(void* state, message_t* msg);
} actor_t;

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg), scheduler_pool_t* pool);
void actor_destroy(actor_t* actor);
bool actor_send(actor_t* actor, message_t* msg);
bool actor_run(actor_t* actor, size_t batch_size);

#endif // OFFS_ACTOR_H