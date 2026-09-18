//
// Created by victor on 9/18/26.
//

#ifndef OFFS_REPRESENTATION_ACTOR_H
#define OFFS_REPRESENTATION_ACTOR_H

#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../BlockCache/block_cache.h"
#include "../Buffer/buffer.h"
#include "../Scheduler/scheduler.h"
#include "../Util/vec.h"
#include <stdint.h>

typedef struct network_t network_t;

/* Operation a representation actor applies to EVERY block in a
   representation's descriptor chain (data hashes + the descriptor blocks
   themselves). */
typedef enum {
  REPRESENTATION_OP_MARK_PERMANENT = 0,  /* CLEAR every claim — commit */
  REPRESENTATION_OP_DELETE_EPHEMERAL = 1, /* RELEASE every claim — delete at 0 */
  REPRESENTATION_OP_PIN = 2,
  REPRESENTATION_OP_UNPIN = 3
} representation_op_e;

/* Payload for REPRESENTATION_OP_RESULT */
typedef struct {
  int result;             /* 0 = ok, -1 = descriptor missing, -2 = walk error */
  size_t blocks_touched;
  actor_t* reply_to;
} representation_op_result_payload_t;

/* Walks a representation's descriptor chain (descriptor blocks chained by the
   trailing 32-byte next-descriptor hash), issues one block-level op per block
   hash it sees — deduplicated within the walk — then replies to reply_to with
   a summary. MARK_PERMANENT additionally announces every newly-committed
   block (previous_count > 0) to the network and removes the representation's
   descriptor hash from the ephemeral registry; DELETE_EPHEMERAL also removes
   the descriptor hash from the registry. Blocks released to 0 are deleted
   regardless of pins; blocks shared with another representation (count > 1)
   survive a delete and merely drop one claim.

   Destroy is for EXTERNAL callers only (routes/tests teardown): it parks the
   scheduler pool and then tears the actor down. Never destroy it from inside
   its own dispatch. */
typedef struct representation_actor_t {
  actor_t actor;
  block_cache_t* bc;
  network_t* network;          /* NULL = local-only; announce skipped */
  buffer_t* descriptor_hash;   /* entry descriptor block being walked */
  buffer_t* next_descriptor_hash;
  representation_op_e op;
  actor_t* reply_to;
  int result;                  /* summary result — 0 unless the walk failed */
  size_t blocks_touched;
  size_t outstanding_ops;      /* block-level ops with no result yet */
  vec_t(buffer_t*) seen_hashes; /* dedup within the walk — the vec_t macro
                                   expands to an anonymous struct, so a named
                                   vec_buffer_t typedef here would collide with
                                   block_recipe.h's in any TU including both */
  uint8_t walk_done;
} representation_actor_t;

representation_actor_t* representation_actor_create(block_cache_t* bc, network_t* network,
                                                    buffer_t* descriptor_hash,
                                                    representation_op_e op, actor_t* reply_to);
void representation_actor_destroy(representation_actor_t* actor);
void representation_actor_dispatch(void* state, message_t* msg);

#endif // OFFS_REPRESENTATION_ACTOR_H