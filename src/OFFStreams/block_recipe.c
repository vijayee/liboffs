//
// Created by victor on 5/7/26.
//

#include "block_recipe.h"
#include "../Util/allocator.h"
#include "../Util/error.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Scheduler/scheduler.h"
#include "../Network/network.h"
#include "../Util/log.h"
#include <string.h>

// --- Generic recipe pull ---

void block_recipe_pull(block_recipe_t* recipe) {
  message_t msg;
  msg.type = READABLE_PULL;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&recipe->stream.actor, &msg);
}

// --- NewBlocksRecipe ---

void new_blocks_recipe_dispatch(void* state, message_t* msg) {
  new_blocks_recipe_t* recipe = (new_blocks_recipe_t*)state;
  switch (msg->type) {
    case READABLE_PULL: {
      if (recipe->recipe.stream.is_deactivated) {
        break;
      }
      block_t* block = block_create_random_block_by_type(recipe->recipe.block_type);
      if (block == NULL) {
        async_error_t* err = OFFS_ERROR("Block creation failed");
        stream_deactivate((stream_t*)recipe, err);
        recipe->recipe.stream.is_deactivated = 1;
        break;
      }
      if (recipe->recipe.put_is_ephemeral) {
        /* Ephemeral put: the recipe holds the one claim on its own output —
           the consuming stream re-puts the block through the plain path so
           the claim is never doubled. */
        block_cache_put_ephemeral(recipe->recipe.bc, block, NULL);
      } else {
        block_cache_put(recipe->recipe.bc, block, 0, NULL);
      }
      stream_notify((stream_t*)recipe, data_event,
                    CONSUME(block, block_t), (void (*)(void*))block_destroy);
      break;
    }
    case CLOSE_STREAM: {
      stream_notify((stream_t*)recipe, close_event, NULL, NULL);
      recipe->recipe.stream.is_deactivated = 1;
      break;
    }
    default:
      stream_dispatch(state, msg);
      break;
  }
}

new_blocks_recipe_t* new_blocks_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type) {
  new_blocks_recipe_t* recipe = get_clear_memory(sizeof(new_blocks_recipe_t));
  recipe->recipe.bc = bc;
  recipe->recipe.block_type = block_type;

  stream_init((stream_t*)recipe, pull, readable_stream, 0, pool,
              (void (*)(stream_t*))new_blocks_recipe_destroy);
  recipe->recipe.stream.actor.state = recipe;
  recipe->recipe.stream.actor.dispatch = new_blocks_recipe_dispatch;

  return recipe;
}

void new_blocks_recipe_destroy(new_blocks_recipe_t* recipe) {
  if (refcounter_dereference_is_zero((refcounter_t*)recipe)) {
    stream_deinit((stream_t*)recipe);
    free(recipe);
  }
}

void new_blocks_recipe_pull(new_blocks_recipe_t* recipe) {
  block_recipe_pull((block_recipe_t*)recipe);
}

// --- RecyclerRecipe ---

static size_t _block_size_for_type(block_size_e type) {
  switch (type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

static int _is_zero_buffer(buffer_t* buf) {
  for (size_t i = 0; i < buf->size; i++) {
    if (buf->data[i] != 0) {
      return 0;
    }
  }
  return 1;
}

/* Start loading the descriptor for the current ori via async block_cache_get. */
static void _start_descriptor_load(recycler_recipe_t* recipe) {
  if (recipe->ori_index >= recipe->oris.length) {
    stream_notify((stream_t*)recipe, complete_event, NULL, NULL);
    stream_notify((stream_t*)recipe, close_event, NULL, NULL);
    recipe->recipe.stream.is_deactivated = 1;
    return;
  }

  ori_t* current_ori = recipe->oris.data[recipe->ori_index];
  if (current_ori->descriptor_hash == NULL) {
    recipe->ori_index++;
    _start_descriptor_load(recipe);
    return;
  }

  recipe->loading_descriptor = 1;
  recipe->descriptor_offset = 0;
  recipe->block_size = _block_size_for_type(current_ori->block_type);
  recipe->descriptor_pad = current_ori->file_hash != NULL ? current_ori->file_hash->size : 32;
  recipe->cut_point = (recipe->block_size / recipe->descriptor_pad) * recipe->descriptor_pad;
  vec_init(&recipe->front_hashes);
  vec_init(&recipe->back_hashes);

  /* Advisory registry probe: a positive flags the source as possibly
     ephemeral, but the exact enforcement happens at data-block fetch time
     (bloom false positives resolve there; false negatives self-heal). */
  if (recipe->recipe.bc != NULL && recipe->recipe.bc->registry != NULL) {
    ephemeral_registry_check(recipe->recipe.bc->registry, current_ori->descriptor_hash,
                            &recipe->recipe.stream.actor);
  }

  block_cache_get(recipe->recipe.bc, current_ori->descriptor_hash,
                        &recipe->recipe.stream.actor);
}

/* Process a descriptor block, extracting front/back hashes and the next hash.
   Returns 1 if we need another descriptor block (stored in recipe->next_descriptor_hash),
   or 0 if processing is complete for this ori. */
static int _process_descriptor_block(recycler_recipe_t* recipe, buffer_t* block_data) {
  ori_t* current_ori = recipe->oris.data[recipe->ori_index];
  size_t offset = 0;

  /* Skip hashes based on descriptor_offset (first time only) */
  if (current_ori->descriptor_offset > 0 && recipe->descriptor_offset == 0) {
    size_t skip_hashes = current_ori->descriptor_offset / recipe->descriptor_pad;
    offset = skip_hashes * recipe->descriptor_pad;
    recipe->descriptor_offset = offset;
  }

  /* Extract hashes up to the cut_point boundary (data area only).
     The last descriptor_pad bytes of the block are the next-block pointer. */
  size_t data_end = block_data->size - recipe->descriptor_pad;
  if (recipe->cut_point < block_data->size) {
    data_end = recipe->cut_point - recipe->descriptor_pad;
  }

  while (offset + recipe->descriptor_pad <= data_end) {
    buffer_t* hash = buffer_slice(block_data, offset, offset + recipe->descriptor_pad);
    if (hash == NULL) {
      offset += recipe->descriptor_pad;
      continue;
    }

    /* Skip zero-filled hashes (padding) */
    if (_is_zero_buffer(hash)) {
      DESTROY(hash, buffer);
      offset += recipe->descriptor_pad;
      continue;
    }

    size_t tuple_position = (offset / recipe->descriptor_pad) % current_ori->tuple_size;
    if (tuple_position < current_ori->tuple_size - 1) {
      vec_push(&recipe->front_hashes, hash);
    } else {
      vec_push(&recipe->back_hashes, hash);
    }

    offset += recipe->descriptor_pad;
  }

  /* Extract the next descriptor hash from the last descriptor_pad bytes */
  size_t next_hash_start = block_data->size - recipe->descriptor_pad;
  if (next_hash_start >= data_end) {
    buffer_t* next_hash = buffer_slice(block_data, next_hash_start, block_data->size);
    if (next_hash != NULL && !_is_zero_buffer(next_hash)) {
      recipe->next_descriptor_hash = next_hash;
    } else {
      if (next_hash != NULL) {
        DESTROY(next_hash, buffer);
      }
    }
  }

  return recipe->next_descriptor_hash != NULL ? 1 : 0;
}

/* Combine front and back hashes into the descriptor vector, set descriptor_loaded. */
static void _finish_descriptor_load(recycler_recipe_t* recipe) {
  /* The endcap is the last back hash */
  if (recipe->back_hashes.length > 0) {
    recipe->endcap = recipe->back_hashes.data[recipe->back_hashes.length - 1];
    recipe->back_hashes.length--;
  }

  /* Append remaining back hashes to front hashes */
  for (int i = 0; i < recipe->back_hashes.length; i++) {
    vec_push(&recipe->front_hashes, recipe->back_hashes.data[i]);
  }
  vec_deinit(&recipe->back_hashes);

  /* Transfer front_hashes to descriptor and zero out front_hashes
     to prevent double-free (they share the same underlying array) */
  recipe->descriptor = recipe->front_hashes;
  recipe->front_hashes.data = NULL;
  recipe->front_hashes.length = 0;
  recipe->front_hashes.capacity = 0;
  recipe->descriptor_index = 0;
  recipe->descriptor_loaded = 1;
  recipe->loading_descriptor = 0;
}

/* Fetch-time ephemeral enforcement for a recycled data block. The exact
   check walks the block's index entry: the registry CHECK is advisory only
   (bloom false positives resolve here, false negatives self-heal via ADD).
   Returns 1 when the block may be delivered downstream (permanent source,
   or a claim acquired/committed per the mode), 0 when the recipe has been
   deactivated and the caller must destroy the block and stop. */
static uint8_t _recycler_enforce_ephemeral(recycler_recipe_t* recipe, block_t* block) {
  if (recipe->recipe.bc == NULL) {
    return 1;
  }
  index_entry_t* entry = index_peek(recipe->recipe.bc->index, block->hash);
  if (entry == NULL || entry->ephemeral_count == 0) {
    return 1;
  }

  /* Discovered an ephemeral block — self-heal the registry for this source. */
  if (recipe->recipe.bc->registry != NULL) {
    ori_t* source_ori = recipe->oris.data[recipe->ori_index];
    if (source_ori->descriptor_hash != NULL) {
      ephemeral_registry_add(recipe->recipe.bc->registry, source_ori->descriptor_hash);
    }
  }

  if (recipe->recipe.put_is_ephemeral || recipe->override_mode == RECYCLE_EPHEMERAL_PROPAGATE) {
    /* Propagate: acquire the consuming representation's claim. After a
       successful put these claims are the consuming representation's ONLY
       reference protection on the shared source blocks — they are released
       solely by recycler_recipe_release_acquired (failure rollback). */
    block_cache_ephemeral(recipe->recipe.bc, block->hash, CACHE_EPHEMERAL_ACQUIRE, NULL);
    vec_push(&recipe->acquired_hashes,
             (buffer_t*)refcounter_reference((refcounter_t*)block->hash));
    return 1;
  }

  if (recipe->override_mode == RECYCLE_EPHEMERAL_COMMIT) {
    /* Commit: clear the source block to permanent and announce it like a
       put would. */
    block_cache_ephemeral(recipe->recipe.bc, block->hash, CACHE_EPHEMERAL_CLEAR, NULL);
    if (recipe->network != NULL) {
      network_local_store_block_payload_t* store_payload =
          get_clear_memory(sizeof(network_local_store_block_payload_t));
      store_payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
      store_payload->fib = entry->counter.fib > 0 ? entry->counter.fib : 1;
      store_payload->reply_to = NULL;
      message_t store_msg;
      store_msg.type = NETWORK_LOCAL_STORE_BLOCK;
      store_msg.payload = store_payload;
      store_msg.payload_destroy = network_local_store_block_payload_destroy;
      actor_send(&recipe->network->actor, &store_msg);
    }
    return 1;
  }

  return 0;
}

/* Fetch the next data block from the descriptor. */
static void _try_fetch_next(recycler_recipe_t* recipe) {
  if (recipe->recipe.stream.is_deactivated) {
    return;
  }

  /* Skip zero hashes in the descriptor */
  while (recipe->descriptor_index < recipe->descriptor.length) {
    buffer_t* hash = recipe->descriptor.data[recipe->descriptor_index];
    if (!_is_zero_buffer(hash)) {
      break;
    }
    recipe->descriptor_index++;
  }

  if (recipe->descriptor_index >= recipe->descriptor.length) {
    /* Current ori exhausted, try next ori */
    for (int i = 0; i < recipe->descriptor.length; i++) {
      DESTROY(recipe->descriptor.data[i], buffer);
    }
    vec_deinit(&recipe->descriptor);
    vec_init(&recipe->descriptor);
    if (recipe->endcap != NULL) {
      DESTROY(recipe->endcap, buffer);
      recipe->endcap = NULL;
    }
    recipe->descriptor_index = 0;
    recipe->descriptor_loaded = 0;
    recipe->ori_index++;

    _start_descriptor_load(recipe);
    return;
  }

  buffer_t* hash = recipe->descriptor.data[recipe->descriptor_index];
  recipe->descriptor_index++;
  block_cache_get(recipe->recipe.bc, hash, &recipe->recipe.stream.actor);
}

void recycler_recipe_dispatch(void* state, message_t* msg) {
  recycler_recipe_t* recipe = (recycler_recipe_t*)state;
  switch (msg->type) {
    case READABLE_PULL: {
      if (recipe->recipe.stream.is_deactivated) {
        break;
      }
      if (!recipe->descriptor_loaded && !recipe->loading_descriptor) {
        _start_descriptor_load(recipe);
      }
      recipe->pending_pull++;
      if (recipe->descriptor_loaded) {
        _try_fetch_next(recipe);
      }
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;

      if (recipe->recipe.stream.is_deactivated) {
        if (result->block != NULL) {
          DESTROY(result->block, block);
        }
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
        }
        break;
      }

      if (recipe->loading_descriptor) {
        /* Processing a descriptor block */
        if (result->block == NULL) {
          /* Descriptor block not found */
          if (recipe->network != NULL) {
            /* Network-aware: send NETWORK_LOCAL_FIND_BLOCK */
            recipe->state = RECIPE_AWAITING_NETWORK;
            recipe->pending_fetch_hash = REFERENCE(result->hash, buffer_t);
            network_local_find_block_payload_t* payload = get_clear_memory(sizeof(network_local_find_block_payload_t));
            payload->hash = recipe->pending_fetch_hash;
            payload->reply_to = &recipe->recipe.stream.actor;
            message_t msg;
            msg.type = NETWORK_LOCAL_FIND_BLOCK;
            msg.payload = payload;
            msg.payload_destroy = network_local_find_block_payload_destroy;
            actor_send(&recipe->network->actor, &msg);
            if (result->hash != NULL) {
              DESTROY(result->hash, buffer);
            }
          } else {
            /* Local-only: skip this ori */
            if (result->hash != NULL) {
              DESTROY(result->hash, buffer);
            }
            /* Clean up any partial front/back hashes */
            for (int i = 0; i < recipe->front_hashes.length; i++) {
              DESTROY(recipe->front_hashes.data[i], buffer);
            }
            vec_deinit(&recipe->front_hashes);
            for (int i = 0; i < recipe->back_hashes.length; i++) {
              DESTROY(recipe->back_hashes.data[i], buffer);
            }
            vec_deinit(&recipe->back_hashes);
            if (recipe->next_descriptor_hash != NULL) {
              DESTROY(recipe->next_descriptor_hash, buffer);
              recipe->next_descriptor_hash = NULL;
            }
            recipe->ori_index++;
            recipe->loading_descriptor = 0;
            _start_descriptor_load(recipe);
          }
          break;
        }

        buffer_t* block_data = result->block->data;
        int need_more = _process_descriptor_block(recipe, block_data);
        DESTROY(result->block, block);
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
        }

        if (need_more && recipe->next_descriptor_hash != NULL) {
          /* Fetch next descriptor block */
          buffer_t* hash = recipe->next_descriptor_hash;
          recipe->next_descriptor_hash = NULL;
          block_cache_get(recipe->recipe.bc, hash, &recipe->recipe.stream.actor);
        } else {
          /* All descriptor blocks loaded — finalize */
          _finish_descriptor_load(recipe);
          if (recipe->pending_pull > 0 && !recipe->recipe.stream.is_deactivated) {
            _try_fetch_next(recipe);
          }
        }
        break;
      }

      /* Data block result — enforce the ephemeral exclusion rule, then
         transfer ownership of result->block to stream_notify */
      if (result->block != NULL) {
        if (!_recycler_enforce_ephemeral(recipe, result->block)) {
          /* Default mode: hard error — unverified data must not enter a
             permanent representation. */
          DESTROY(result->block, block);
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
          }
          stream_deactivate((stream_t*)recipe, OFFS_ERROR("recycle source is ephemeral/unverified"));
          recipe->recipe.stream.is_deactivated = 1;
          break;
        }
        stream_notify((stream_t*)recipe, data_event,
                      CONSUME(result->block, block_t), (void (*)(void*))block_destroy);
        recipe->pending_pull--;
      } else {
        /* Data block not found */
        if (recipe->network != NULL) {
          /* Network-aware: send NETWORK_LOCAL_FIND_BLOCK */
          recipe->state = RECIPE_AWAITING_NETWORK;
          recipe->pending_fetch_hash = REFERENCE(result->hash, buffer_t);
          network_local_find_block_payload_t* payload = get_clear_memory(sizeof(network_local_find_block_payload_t));
          payload->hash = recipe->pending_fetch_hash;
          payload->reply_to = &recipe->recipe.stream.actor;
          message_t msg;
          msg.type = NETWORK_LOCAL_FIND_BLOCK;
          msg.payload = payload;
          msg.payload_destroy = network_local_find_block_payload_destroy;
          actor_send(&recipe->network->actor, &msg);
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
          }
        } else {
          /* Local-only: deactivate */
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
          }
          stream_deactivate((stream_t*)recipe, OFFS_ERROR("Block not found"));
          recipe->recipe.stream.is_deactivated = 1;
        }
        break;
      }
      if (result->hash != NULL) {
        DESTROY(result->hash, buffer);
      }

      /* Try to serve next pending pull */
      if (recipe->pending_pull > 0 && !recipe->recipe.stream.is_deactivated) {
        _try_fetch_next(recipe);
      }
      break;
    }
    case NETWORK_FIND_BLOCK_RESULT: {
      network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
      if (result->found) {
        if (result->block != NULL) {
          /* Direct-return: network provided the block. Process it the same
           * way as CACHE_GET_RESULT success (two branches: descriptor vs data). */
          if (recipe->loading_descriptor) {
            /* Descriptor block: process it (mirror CACHE_GET_RESULT lines 328-345) */
            buffer_t* block_data = result->block->data;
            int need_more = _process_descriptor_block(recipe, block_data);
            DESTROY(result->block, block);
            result->block = NULL;
            if (need_more && recipe->next_descriptor_hash != NULL) {
              buffer_t* hash = recipe->next_descriptor_hash;
              recipe->next_descriptor_hash = NULL;
              block_cache_get(recipe->recipe.bc, hash, &recipe->recipe.stream.actor);
            } else {
              _finish_descriptor_load(recipe);
              if (recipe->pending_pull > 0 && !recipe->recipe.stream.is_deactivated) {
                _try_fetch_next(recipe);
              }
            }
          } else {
            /* Data block: enforce the ephemeral exclusion rule (mirror
               CACHE_GET_RESULT's data branch), then transfer ownership to
               stream_notify. */
            if (!_recycler_enforce_ephemeral(recipe, result->block)) {
              DESTROY(result->block, block);
              stream_deactivate((stream_t*)recipe, OFFS_ERROR("recycle source is ephemeral/unverified"));
              recipe->recipe.stream.is_deactivated = 1;
            } else {
              stream_notify((stream_t*)recipe, data_event,
                            CONSUME(result->block, block_t), (void (*)(void*))block_destroy);
              recipe->pending_pull--;
              result->block = NULL;  /* ownership transferred via CONSUME; null to prevent destroy double-free */
            }
          }
        } else {
          /* Local path: block is in the cache. Re-fetch as before. */
          block_cache_get(recipe->recipe.bc, recipe->pending_fetch_hash, &recipe->recipe.stream.actor);
          recipe->state = RECIPE_FETCHING_BLOCK;
        }
      } else {
        /* Block not found on network */
        if (recipe->loading_descriptor) {
          /* Descriptor block not found on network — skip this ori */
          for (int i = 0; i < recipe->front_hashes.length; i++) {
            DESTROY(recipe->front_hashes.data[i], buffer);
          }
          vec_deinit(&recipe->front_hashes);
          for (int i = 0; i < recipe->back_hashes.length; i++) {
            DESTROY(recipe->back_hashes.data[i], buffer);
          }
          vec_deinit(&recipe->back_hashes);
          if (recipe->next_descriptor_hash != NULL) {
            DESTROY(recipe->next_descriptor_hash, buffer);
            recipe->next_descriptor_hash = NULL;
          }
          recipe->ori_index++;
          recipe->loading_descriptor = 0;
          _start_descriptor_load(recipe);
        } else {
          /* Data block not found on network — deactivate */
          stream_deactivate((stream_t*)recipe, OFFS_ERROR("Block not found on network"));
          recipe->recipe.stream.is_deactivated = 1;
        }
      }
      if (recipe->pending_fetch_hash != NULL) {
        DESTROY(recipe->pending_fetch_hash, buffer);
        recipe->pending_fetch_hash = NULL;
      }
      break;
    }
    case EPHEMERAL_REGISTRY_CHECK_RESULT: {
      ephemeral_registry_check_result_payload_t* result =
          (ephemeral_registry_check_result_payload_t*)msg->payload;
      if (result->present) {
        recipe->source_flagged = 1;
        log_warn("recycler: source flagged ephemeral by registry — enforcing at fetch time");
      }
      break;
    }
    case CLOSE_STREAM: {
      if (recipe->pending_fetch_hash != NULL) {
        DESTROY(recipe->pending_fetch_hash, buffer);
        recipe->pending_fetch_hash = NULL;
      }
      stream_notify((stream_t*)recipe, close_event, NULL, NULL);
      recipe->recipe.stream.is_deactivated = 1;
      break;
    }
    default:
      stream_dispatch(state, msg);
      break;
  }
}

recycler_recipe_t* recycler_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type,
    vec_ori_t oris, network_t* network, uint8_t put_is_ephemeral,
    recycle_ephemeral_e override_mode) {
  recycler_recipe_t* recipe = get_clear_memory(sizeof(recycler_recipe_t));
  recipe->recipe.bc = bc;
  recipe->recipe.block_type = block_type;
  recipe->recipe.is_recycler = 1;
  recipe->recipe.put_is_ephemeral = put_is_ephemeral;
  recipe->override_mode = override_mode;
  recipe->source_flagged = 0;
  recipe->network = network;
  recipe->pending_fetch_hash = NULL;
  recipe->state = RECIPE_FETCHING_BLOCK;

  recipe->oris = oris;
  for (int i = 0; i < recipe->oris.length; i++) {
    REFERENCE(recipe->oris.data[i], ori_t);
  }
  recipe->ori_index = 0;

  vec_init(&recipe->acquired_hashes);
  vec_init(&recipe->descriptor);
  recipe->descriptor_index = 0;
  recipe->endcap = NULL;
  recipe->descriptor_loaded = 0;
  recipe->loading_descriptor = 0;
  recipe->pending_pull = 0;

  stream_init((stream_t*)recipe, pull, readable_stream, 0, pool,
              (void (*)(stream_t*))recycler_recipe_destroy);
  recipe->recipe.stream.actor.state = recipe;
  recipe->recipe.stream.actor.dispatch = recycler_recipe_dispatch;

  return recipe;
}

void recycler_recipe_release_acquired(recycler_recipe_t* recipe) {
  /* Failure rollback only: the consuming put aborted, so no representation
     will ever release these claims itself. After a successful put these
     claims are the consuming representation's only reference protection on
     the shared source blocks — never call this then. */
  if (recipe->recipe.bc != NULL) {
    for (int i = 0; i < recipe->acquired_hashes.length; i++) {
      block_cache_ephemeral(recipe->recipe.bc, recipe->acquired_hashes.data[i],
                            CACHE_EPHEMERAL_RELEASE, NULL);
    }
  }
  for (int i = 0; i < recipe->acquired_hashes.length; i++) {
    DESTROY(recipe->acquired_hashes.data[i], buffer);
  }
  recipe->acquired_hashes.length = 0;
}

void recycler_recipe_destroy(recycler_recipe_t* recipe) {
  if (refcounter_dereference_is_zero((refcounter_t*)recipe)) {
    /* NO claim release here: after a successful put the acquired claims are
       the consuming representation's only reference protection on the
       recycled source blocks. Failure rollback is the caller's job, via
       recycler_recipe_release_acquired. */
    for (int i = 0; i < recipe->acquired_hashes.length; i++) {
      DESTROY(recipe->acquired_hashes.data[i], buffer);
    }
    vec_deinit(&recipe->acquired_hashes);

    for (int i = 0; i < recipe->oris.length; i++) {
      DESTROY(recipe->oris.data[i], ori);
    }
    vec_deinit(&recipe->oris);

    for (int i = 0; i < recipe->descriptor.length; i++) {
      DESTROY(recipe->descriptor.data[i], buffer);
    }
    vec_deinit(&recipe->descriptor);

    for (int i = 0; i < recipe->front_hashes.length; i++) {
      DESTROY(recipe->front_hashes.data[i], buffer);
    }
    vec_deinit(&recipe->front_hashes);

    for (int i = 0; i < recipe->back_hashes.length; i++) {
      DESTROY(recipe->back_hashes.data[i], buffer);
    }
    vec_deinit(&recipe->back_hashes);

    if (recipe->endcap != NULL) {
      DESTROY(recipe->endcap, buffer);
    }
    if (recipe->next_descriptor_hash != NULL) {
      DESTROY(recipe->next_descriptor_hash, buffer);
    }
    if (recipe->pending_fetch_hash != NULL) {
      DESTROY(recipe->pending_fetch_hash, buffer);
    }

    stream_deinit((stream_t*)recipe);
    free(recipe);
  }
}

void recycler_recipe_pull(recycler_recipe_t* recipe) {
  block_recipe_pull((block_recipe_t*)recipe);
}