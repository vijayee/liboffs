//
// Created by victor on 5/6/25.
//

#ifndef OFFS_ACTOR_H
#define OFFS_ACTOR_H

#include "message_queue.h"
#include "../Util/threadding.h"
#include "../Util/atomic_compat.h"
#include <stdint.h>

#define ACTOR_BATCH_SIZE 32
#define ACTOR_FLAG_SCHEDULED 0x01
#define ACTOR_FLAG_OVERLOADED 0x02
#define ACTOR_FLAG_RUNNING 0x04

typedef struct actor_t {
  message_queue_t queue;
  ATOMIC(uint8_t) flags;
  void* state;
  void (*dispatch)(void* state, message_t* msg);
  PLATFORMLOCKTYPE(run_lock);
  PLATFORMCONDITIONTYPE(run_cond);
} actor_t;

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg));
void actor_destroy(actor_t* actor);
bool actor_send(actor_t* actor, message_t* msg);
bool actor_run(actor_t* actor, size_t batch_size);

/* Claim ACTOR_FLAG_RUNNING, blocking efficiently until available.
   Uses a condition variable instead of spin-yield, mirroring
   Pony's approach of OS-level thread suspension for idle waits. */
void actor_claim_running(actor_t* actor);

/* Clear ACTOR_FLAG_RUNNING and wake any threads waiting in
   actor_claim_running. Must be called after actor_run completes. */
void actor_release_running(actor_t* actor);

#endif // OFFS_ACTOR_H
