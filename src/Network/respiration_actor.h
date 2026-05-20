//
// Created by victor on 5/19/25.
//

#ifndef OFFS_RESPIRATION_ACTOR_H
#define OFFS_RESPIRATION_ACTOR_H

#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Buffer/buffer.h"
#include "node_id.h"
#include <stddef.h>
#include <stdint.h>

/* Exhale states */
#define RESPIRATION_IDLE       0
#define RESPIRATION_VERIFYING  1  /* Finding blocks on network */
#define RESPIRATION_STORING    2  /* Storing blocks to peers before deleting */

typedef struct respiration_pending_t {
  buffer_t* hash;
  uint64_t ejection_date;
} respiration_pending_t;

typedef struct respiration_actor_t {
  actor_t actor;
  struct network_t* network;
  scheduler_pool_t* pool;
  ATOMIC(uint8_t) state;          /* RESPIRATION_IDLE, VERIFYING, or STORING */
  /* Blocks confirmed redundant (found elsewhere, can delete) */
  buffer_t** redundant_hashes;
  size_t redundant_count;
  size_t redundant_capacity;
  /* Blocks not found elsewhere (must store to peers before deleting) */
  buffer_t** preserved_hashes;
  size_t preserved_count;
  size_t preserved_capacity;
  /* Pending find-block or store-block queries in flight */
  respiration_pending_t* pending;
  size_t pending_count;
  size_t pending_capacity;
} respiration_actor_t;

respiration_actor_t* respiration_actor_create(struct network_t* network, scheduler_pool_t* pool);
void respiration_actor_destroy(respiration_actor_t* actor);
void respiration_actor_dispatch(void* state, message_t* msg);

#endif // OFFS_RESPIRATION_ACTOR_H