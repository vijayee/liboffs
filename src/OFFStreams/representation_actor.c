//
// Created by victor on 9/18/26.
//

#include "representation_actor.h"
#include "../Network/network.h"
#include "../Util/allocator.h"
#include <string.h>

#define REPRESENTATION_DESCRIPTOR_PAD 32

static size_t _rep_block_size_for_type(block_size_e type) {
  switch (type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

static int _rep_is_zero_hash(buffer_t* hash) {
  for (size_t idx = 0; idx < hash->size; idx++) {
    if (hash->data[idx] != 0) {
      return 0;
    }
  }
  return 1;
}

/* Send the summary reply to reply_to exactly once (the field is nulled after
   the send so later messages cannot double-reply). */
static void _rep_reply(representation_actor_t* rep) {
  if (rep->reply_to == NULL) {
    return;
  }
  representation_op_result_payload_t* payload =
      get_clear_memory(sizeof(representation_op_result_payload_t));
  payload->result = rep->result;
  payload->blocks_touched = rep->blocks_touched;
  payload->reply_to = NULL;
  message_t reply;
  reply.type = REPRESENTATION_OP_RESULT;
  reply.payload = payload;
  reply.payload_destroy = free;
  actor_send(rep->reply_to, &reply);
  rep->reply_to = NULL;
}

static void _rep_maybe_finish(representation_actor_t* rep) {
  if (!rep->walk_done || rep->outstanding_ops > 0) {
    return;
  }
  _rep_reply(rep);
}

/* Dedup within one walk (a descriptor may list the same hash twice — a
   double-op would double-claim/double-release), then issue the block-level
   op and count it until its result comes back. */
static void _rep_apply_to_hash(representation_actor_t* rep, buffer_t* hash) {
  for (int idx = 0; idx < rep->seen_hashes.length; idx++) {
    if (buffer_compare(rep->seen_hashes.data[idx], hash) == 0) {
      return;
    }
  }
  vec_push(&rep->seen_hashes, REFERENCE(hash, buffer_t));
  rep->blocks_touched++;
  rep->outstanding_ops++;
  switch (rep->op) {
    case REPRESENTATION_OP_MARK_PERMANENT:
      block_cache_ephemeral(rep->bc, hash, CACHE_EPHEMERAL_CLEAR, &rep->actor);
      break;
    case REPRESENTATION_OP_DELETE_EPHEMERAL:
      block_cache_ephemeral(rep->bc, hash, CACHE_EPHEMERAL_RELEASE, &rep->actor);
      break;
    case REPRESENTATION_OP_PIN:
      block_cache_pin(rep->bc, hash, &rep->actor);
      break;
    case REPRESENTATION_OP_UNPIN:
      block_cache_unpin(rep->bc, hash, &rep->actor);
      break;
  }
}

/* Mirror block_recipe.c's _process_descriptor_block: data hashes are
   pad-byte slices up to the cut point (zero slices are padding), and the
   last pad bytes of the block are the next-descriptor pointer (zero-padded
   when the chain ends). The descriptor block's own hash is part of the
   representation and gets the op too. */
static void _rep_process_descriptor_block(representation_actor_t* rep, buffer_t* block_data,
                                          buffer_t* descriptor_block_hash) {
  size_t descriptor_pad = REPRESENTATION_DESCRIPTOR_PAD;
  size_t block_size = _rep_block_size_for_type(rep->bc->type);
  size_t cut_point = (block_size / descriptor_pad) * descriptor_pad;

  if (block_data->size < descriptor_pad) {
    /* Malformed block — it cannot even hold a next-descriptor pointer. */
    rep->result = -2;
    return;
  }

  /* Data area only: the last descriptor_pad bytes are the next-block pointer. */
  size_t data_end = block_data->size - descriptor_pad;
  if (cut_point < block_data->size) {
    data_end = cut_point - descriptor_pad;
  }

  size_t offset = 0;
  while (offset + descriptor_pad <= data_end) {
    buffer_t* hash = buffer_slice(block_data, offset, offset + descriptor_pad);
    if (hash != NULL) {
      if (!_rep_is_zero_hash(hash)) {
        _rep_apply_to_hash(rep, hash);
      }
      DESTROY(hash, buffer);
    }
    offset += descriptor_pad;
  }

  _rep_apply_to_hash(rep, descriptor_block_hash);

  size_t next_hash_start = block_data->size - descriptor_pad;
  if (next_hash_start >= data_end) {
    buffer_t* next_hash = buffer_slice(block_data, next_hash_start, block_data->size);
    if (next_hash != NULL) {
      if (!_rep_is_zero_hash(next_hash)) {
        rep->next_descriptor_hash = next_hash;
      } else {
        DESTROY(next_hash, buffer);
      }
    }
  }
}

void representation_actor_dispatch(void* state, message_t* msg) {
  representation_actor_t* rep = (representation_actor_t*)state;
  switch (msg->type) {
    case READABLE_PULL: {
      /* Kick from create: fetch the entry descriptor block. */
      block_cache_get(rep->bc, rep->descriptor_hash, &rep->actor);
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      if (result->block == NULL) {
        /* Descriptor block missing — the walk cannot proceed. Outstanding ops
           from earlier chained blocks still finish before the reply goes out. */
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
        }
        rep->result = -1;
        rep->walk_done = 1;
        _rep_maybe_finish(rep);
        break;
      }
      _rep_process_descriptor_block(rep, result->block->data, result->hash);
      DESTROY(result->block, block);
      DESTROY(result->hash, buffer);
      if (rep->next_descriptor_hash != NULL) {
        buffer_t* next_hash = rep->next_descriptor_hash;
        rep->next_descriptor_hash = NULL;
        block_cache_get(rep->bc, next_hash, &rep->actor);
        DESTROY(next_hash, buffer);
      } else {
        rep->walk_done = 1;
        /* The representation is no longer ephemeral (committed or gone) —
           drop its descriptor hash from the advisory registry. PIN/UNPIN
           leave the representation's ephemeral status alone. */
        if ((rep->op == REPRESENTATION_OP_MARK_PERMANENT ||
             rep->op == REPRESENTATION_OP_DELETE_EPHEMERAL) &&
            rep->bc->registry != NULL) {
          ephemeral_registry_remove(rep->bc->registry, rep->descriptor_hash);
        }
        _rep_maybe_finish(rep);
      }
      break;
    }
    case CACHE_EPHEMERAL_RESULT: {
      cache_ephemeral_result_payload_t* result =
          (cache_ephemeral_result_payload_t*)msg->payload;
      buffer_t* announce_hash = NULL;
      /* A newly-committed block (it carried claims before the CLEAR) enters
         the world of permanent blocks — announce it like a put would. */
      if (rep->op == REPRESENTATION_OP_MARK_PERMANENT && result->previous_count > 0 &&
          rep->network != NULL && result->hash != NULL) {
        announce_hash = REFERENCE(result->hash, buffer_t);
      }
      if (result->hash != NULL) {
        /* Nulls result->hash so actor_run's payload_destroy skips it. */
        DESTROY(result->hash, buffer);
      }
      rep->outstanding_ops--;
      if (announce_hash != NULL) {
        network_local_store_block_payload_t* store_payload =
            get_clear_memory(sizeof(network_local_store_block_payload_t));
        store_payload->hash = announce_hash;
        store_payload->fib = 1;
        store_payload->reply_to = NULL;
        message_t store_msg;
        store_msg.type = NETWORK_LOCAL_STORE_BLOCK;
        store_msg.payload = store_payload;
        store_msg.payload_destroy = network_local_store_block_payload_destroy;
        actor_send(&rep->network->actor, &store_msg);
      }
      _rep_maybe_finish(rep);
      break;
    }
    case CACHE_PIN_RESULT:
    case CACHE_UNPIN_RESULT: {
      rep->outstanding_ops--;
      _rep_maybe_finish(rep);
      break;
    }
    default:
      break;
  }
}

representation_actor_t* representation_actor_create(block_cache_t* bc, network_t* network,
                                                    buffer_t* descriptor_hash,
                                                    representation_op_e op, actor_t* reply_to) {
  representation_actor_t* rep = get_clear_memory(sizeof(representation_actor_t));
  rep->bc = bc;
  rep->network = network;
  rep->descriptor_hash = REFERENCE(descriptor_hash, buffer_t);
  rep->op = op;
  rep->reply_to = reply_to;
  vec_init(&rep->seen_hashes);
  actor_init(&rep->actor, rep, representation_actor_dispatch, bc->pool);
  /* Kick the walk: the pull makes the actor fetch the entry descriptor. */
  message_t msg;
  msg.type = READABLE_PULL;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&rep->actor, &msg);
  return rep;
}

void representation_actor_destroy(representation_actor_t* rep) {
  if (rep == NULL) {
    return;
  }
  /* External-thread teardown only (see header): park the pool so the actor's
     last run has fully finished before its memory goes away. */
  scheduler_pool_wait_for_idle(rep->bc->pool);
  actor_destroy(&rep->actor);
  for (int idx = 0; idx < rep->seen_hashes.length; idx++) {
    DESTROY(rep->seen_hashes.data[idx], buffer);
  }
  vec_deinit(&rep->seen_hashes);
  if (rep->next_descriptor_hash != NULL) {
    DESTROY(rep->next_descriptor_hash, buffer);
  }
  if (rep->descriptor_hash != NULL) {
    DESTROY(rep->descriptor_hash, buffer);
  }
  free(rep);
}