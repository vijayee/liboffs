//
// Created by victor on 5/19/25.
//

#include "respiration_actor.h"
#include "network.h"
#include "respiration.h"
#include "authority.h"
#include "../BlockCache/block_cache.h"
#include "../Buffer/buffer.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/message.h"
#include <string.h>

static void _respiration_cache_remove_payload_destroy(void* ptr) {
  cache_remove_payload_t* payload = (cache_remove_payload_t*)ptr;
  if (payload == NULL) return;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

static void _respiration_add_redundant(respiration_actor_t* actor, buffer_t* hash) {
  if (actor->redundant_count >= actor->redundant_capacity) {
    size_t new_capacity = actor->redundant_capacity == 0 ? 16 : actor->redundant_capacity * 2;
    buffer_t** new_hashes = realloc(actor->redundant_hashes, sizeof(buffer_t*) * new_capacity);
    if (new_hashes == NULL) {
      DESTROY(hash, buffer);
      return;
    }
    actor->redundant_hashes = new_hashes;
    actor->redundant_capacity = new_capacity;
  }
  actor->redundant_hashes[actor->redundant_count++] = hash;
}

// Arm the exhale watchdog. A lost FindBlock or StoreBlock result would pin
// the state non-IDLE forever — the node could never shed blocks again. The
// watchdog fires after RESPIRATION_WATCHDOG_TIMEOUT_MS and resets the state
// to IDLE, cleaning up any pending hashes. See audit #29.
static void _respiration_watchdog_arm(respiration_actor_t* actor) {
  if (actor == NULL || actor->network == NULL || actor->network->timer == NULL) return;
  // Cancel any prior watchdog first (idempotent — timer_actor_cancel is a
  // no-op if the id is not found).
  uint64_t prior_id = atomic_load(&actor->watchdog_timer_id);
  if (prior_id != 0) {
    timer_actor_cancel(actor->network->timer, prior_id);
  }
  // timer_actor_set stores the new id into the atomic directly.
  timer_actor_set(actor->network->timer,
                  RESPIRATION_WATCHDOG_TIMEOUT_MS,
                  0, /* one-shot */
                  &actor->actor,
                  RESPIRATION_WATCHDOG_TIMEOUT,
                  &actor->watchdog_timer_id);
}

static void _respiration_watchdog_disarm(respiration_actor_t* actor) {
  if (actor == NULL || actor->network == NULL || actor->network->timer == NULL) return;
  uint64_t id = atomic_load(&actor->watchdog_timer_id);
  if (id != 0) {
    timer_actor_cancel(actor->network->timer, id);
    atomic_store(&actor->watchdog_timer_id, 0);
  }
}

// Reset the actor to IDLE and free all transient state. Used by both the
// normal completion path and the watchdog timeout.
static void _respiration_reset_to_idle(respiration_actor_t* actor) {
  if (actor == NULL) return;
  if (actor->redundant_hashes != NULL) {
    for (size_t idx = 0; idx < actor->redundant_count; idx++) {
      DESTROY(actor->redundant_hashes[idx], buffer);
    }
    actor->redundant_count = 0;
  }
  if (actor->preserved != NULL) {
    for (size_t idx = 0; idx < actor->preserved_count; idx++) {
      DESTROY(actor->preserved[idx].hash, buffer);
    }
    actor->preserved_count = 0;
  }
  if (actor->pending != NULL) {
    for (size_t idx = 0; idx < actor->pending_count; idx++) {
      if (actor->pending[idx].hash != NULL) {
        DESTROY(actor->pending[idx].hash, buffer);
      }
    }
    actor->pending_count = 0;
  }
  _respiration_watchdog_disarm(actor);
  atomic_store(&actor->state, RESPIRATION_IDLE);
}

static void _respiration_add_preserved(respiration_actor_t* actor, buffer_t* hash, uint64_t ejection_date) {
  if (actor->preserved_count >= actor->preserved_capacity) {
    size_t new_capacity = actor->preserved_capacity == 0 ? 16 : actor->preserved_capacity * 2;
    respiration_preserved_t* new_entries = realloc(actor->preserved, sizeof(respiration_preserved_t) * new_capacity);
    if (new_entries == NULL) {
      DESTROY(hash, buffer);
      return;
    }
    actor->preserved = new_entries;
    actor->preserved_capacity = new_capacity;
  }
  actor->preserved[actor->preserved_count].hash = hash;
  actor->preserved[actor->preserved_count].ejection_date = ejection_date;
  actor->preserved_count++;
}

static void _respiration_delete_redundant(respiration_actor_t* actor);

static void _respiration_find_pending_by_hash(respiration_actor_t* actor, buffer_t* hash, size_t* out_index) {
  for (size_t idx = 0; idx < actor->pending_count; idx++) {
    if (buffer_compare(actor->pending[idx].hash, hash) == 0) {
      *out_index = idx;
      return;
    }
  }
  *out_index = (size_t)-1;
}

static void _respiration_remove_pending(respiration_actor_t* actor, size_t index) {
  if (index >= actor->pending_count) return;
  if (actor->pending[index].hash != NULL) {
    DESTROY(actor->pending[index].hash, buffer);
  }
  /* Shift remaining entries down */
  for (size_t idx = index; idx < actor->pending_count - 1; idx++) {
    actor->pending[idx] = actor->pending[idx + 1];
  }
  actor->pending_count--;
}

void respiration_actor_dispatch(void* state, message_t* msg) {
  respiration_actor_t* actor = (respiration_actor_t*)state;
  if (actor == NULL) return;

  switch (msg->type) {
    case RESPIRATION_EXHALE_TRIGGER: {
      respiration_exhale_payload_t* payload = (respiration_exhale_payload_t*)msg->payload;
      if (payload == NULL) break;

      /* Ignore if already processing — payload_destroy will free after dispatch */
      if (atomic_load(&actor->state) != RESPIRATION_IDLE) {
        break;
      }

      /* Clean up any stale state from previous cycles */
      if (actor->redundant_hashes != NULL) {
        for (size_t idx = 0; idx < actor->redundant_count; idx++) {
          DESTROY(actor->redundant_hashes[idx], buffer);
        }
        actor->redundant_count = 0;
      }
      if (actor->preserved != NULL) {
        for (size_t idx = 0; idx < actor->preserved_count; idx++) {
          DESTROY(actor->preserved[idx].hash, buffer);
        }
        actor->preserved_count = 0;
      }
      if (actor->pending != NULL) {
        for (size_t idx = 0; idx < actor->pending_count; idx++) {
          DESTROY(actor->pending[idx].hash, buffer);
        }
        actor->pending_count = 0;
      }

      atomic_store(&actor->state, RESPIRATION_VERIFYING);

      /* Copy hashes and ejection_dates into pending array */
      for (size_t idx = 0; idx < payload->count; idx++) {
        if (actor->pending_count >= actor->pending_capacity) {
          size_t new_capacity = actor->pending_capacity == 0 ? 16 : actor->pending_capacity * 2;
          respiration_pending_t* new_pending = realloc(actor->pending, sizeof(respiration_pending_t) * new_capacity);
          if (new_pending == NULL) break;
          actor->pending = new_pending;
          actor->pending_capacity = new_capacity;
        }
        actor->pending[actor->pending_count].hash = REFERENCE(payload->hashes[idx], buffer_t);
        actor->pending[actor->pending_count].ejection_date = payload->ejection_dates[idx];
        actor->pending_count++;
      }

      /* Payload is freed by payload_destroy after dispatch returns */

      /* If no pending hashes, go back to idle */
      if (actor->pending_count == 0) {
        atomic_store(&actor->state, RESPIRATION_IDLE);
        break;
      }

      /* Arm the watchdog — a lost FindBlock result would otherwise pin the
         state VERIFYING forever. See audit #29. */
      _respiration_watchdog_arm(actor);

      /* Send NETWORK_LOCAL_FIND_BLOCK for each pending hash */
      size_t pending_to_resolve = actor->pending_count;
      for (size_t idx = 0; idx < pending_to_resolve; idx++) {
        network_local_find_block_payload_t* find_payload =
            get_clear_memory(sizeof(network_local_find_block_payload_t));
        if (find_payload == NULL) {
          /* Allocation failed — treat as not found, move to preserved */
          _respiration_add_preserved(actor, actor->pending[idx].hash,
                                      actor->pending[idx].ejection_date);
          actor->pending[idx].hash = NULL;
          continue;
        }
        find_payload->hash = REFERENCE(actor->pending[idx].hash, buffer_t);
        find_payload->reply_to = &actor->actor;

        message_t find_msg;
        find_msg.type = NETWORK_LOCAL_FIND_BLOCK;
        find_msg.payload = find_payload;
        find_msg.payload_destroy = network_local_find_block_payload_destroy;

        actor_send(&actor->network->actor, &find_msg);
      }
      /* Resolve any allocation-failed entries from pending */
      size_t write_idx = 0;
      for (size_t idx = 0; idx < actor->pending_count; idx++) {
        if (actor->pending[idx].hash != NULL) {
          actor->pending[write_idx++] = actor->pending[idx];
        }
      }
      actor->pending_count = write_idx;
      /* If all finds were resolved due to allocation failures, proceed to delete */
      if (actor->pending_count == 0) {
        _respiration_delete_redundant(actor);
      }
      break;
    }

    case NETWORK_FIND_BLOCK_RESULT: {
      /* Only process in VERIFYING state */
      if (atomic_load(&actor->state) != RESPIRATION_VERIFYING) break;

      network_find_block_result_payload_t* result =
          (network_find_block_result_payload_t*)msg->payload;
      if (result == NULL || result->hash == NULL) break;

      /* Find this hash in pending and remove it */
      size_t found_index = (size_t)-1;
      _respiration_find_pending_by_hash(actor, result->hash, &found_index);
      if (found_index == (size_t)-1) {
        /* Not in our pending list — ignore */
        break;
      }

      if (result->found) {
        /* Block found elsewhere — it's redundant */
        _respiration_add_redundant(actor, actor->pending[found_index].hash);
        actor->pending[found_index].hash = NULL; /* transferred ownership */
      } else {
        /* Block not found — preserve it */
        _respiration_add_preserved(actor, actor->pending[found_index].hash,
                                    actor->pending[found_index].ejection_date);
        actor->pending[found_index].hash = NULL; /* transferred ownership */
      }

      _respiration_remove_pending(actor, found_index);

      /* If all finds resolved, proceed to delete redundant blocks */
      if (actor->pending_count == 0) {
        _respiration_delete_redundant(actor);
      }
      break;
    }

    case NETWORK_STORE_BLOCK_RESULT: {
      /* Only process in STORING state */
      if (atomic_load(&actor->state) != RESPIRATION_STORING) break;

      network_store_block_result_payload_t* result =
          (network_store_block_result_payload_t*)msg->payload;
      if (result == NULL || result->hash == NULL) break;

      /* Find this hash in pending */
      size_t found_index = (size_t)-1;
      _respiration_find_pending_by_hash(actor, result->hash, &found_index);
      if (found_index == (size_t)-1) {
        break;
      }

      if (result->accepted) {
        /* Block stored to peers — delete local copy */
        cache_remove_payload_t* remove_payload = get_clear_memory(sizeof(cache_remove_payload_t));
        if (remove_payload != NULL) {
          remove_payload->hash = REFERENCE(actor->pending[found_index].hash, buffer_t);
          remove_payload->reply_to = NULL;

          message_t remove_msg;
          remove_msg.type = CACHE_REMOVE;
          remove_msg.payload = remove_payload;
          remove_msg.payload_destroy = _respiration_cache_remove_payload_destroy;

          actor_send(&actor->network->block_cache->actor, &remove_msg);
        }
      }
      /* If not accepted, keep the block locally (best effort) */

      _respiration_remove_pending(actor, found_index);

      /* If all store results resolved, check capacity and go idle */
      if (actor->pending_count == 0) {
        float capacity = atomic_load(&actor->network->authority->capacity);
        if (capacity >= RESPIRATION_EXHALE_THRESHOLD) {
          log_warn("respiration: capacity %.2f still above threshold after store-then-delete", capacity);
        }
        _respiration_watchdog_disarm(actor);
        atomic_store(&actor->state, RESPIRATION_IDLE);
      }
      break;
    }

    case RESPIRATION_WATCHDOG_TIMEOUT: {
      /* A FindBlock or StoreBlock result was lost (peer disconnected, message
         dropped) and the state has been non-IDLE for > RESPIRATION_WATCHDOG_TIMEOUT_MS.
         Reset to IDLE so the next exhale can proceed. See audit #29. */
      if (atomic_load(&actor->state) == RESPIRATION_IDLE) break;
      log_warn("respiration: watchdog timeout — resetting exhale state to IDLE "
               "(pending=%zu, redundant=%zu, preserved=%zu)",
               actor->pending_count, actor->redundant_count,
               actor->preserved_count);
      _respiration_reset_to_idle(actor);
      break;
    }

    default:
      break;
  }
}

static void _respiration_delete_redundant(respiration_actor_t* actor) {
  /* Delete redundant blocks via CACHE_REMOVE */
  for (size_t idx = 0; idx < actor->redundant_count; idx++) {
    cache_remove_payload_t* remove_payload = get_clear_memory(sizeof(cache_remove_payload_t));
    if (remove_payload == NULL) continue;
    remove_payload->hash = REFERENCE(actor->redundant_hashes[idx], buffer_t);
    remove_payload->reply_to = NULL; /* fire-and-forget */

    message_t remove_msg;
    remove_msg.type = CACHE_REMOVE;
    remove_msg.payload = remove_payload;
    remove_msg.payload_destroy = _respiration_cache_remove_payload_destroy;

    actor_send(&actor->network->block_cache->actor, &remove_msg);
  }

  /* Free redundant hash references */
  for (size_t idx = 0; idx < actor->redundant_count; idx++) {
    DESTROY(actor->redundant_hashes[idx], buffer);
  }
  actor->redundant_count = 0;

  /* Check if we're below the exhale threshold */
  float capacity = atomic_load(&actor->network->authority->capacity);
  if (capacity < RESPIRATION_EXHALE_THRESHOLD) {
    /* Capacity is below threshold — done, go idle */
    for (size_t idx = 0; idx < actor->preserved_count; idx++) {
      DESTROY(actor->preserved[idx].hash, buffer);
    }
    actor->preserved_count = 0;
    _respiration_watchdog_disarm(actor);
    atomic_store(&actor->state, RESPIRATION_IDLE);
    return;
  }

  /* Still above threshold — try store-then-delete for preserved blocks */
  if (actor->preserved_count > 0) {
    atomic_store(&actor->state, RESPIRATION_STORING);
    /* Re-arm the watchdog for the STORING phase — a lost StoreBlock result
       would pin the state STORING forever. See audit #29. */
    _respiration_watchdog_arm(actor);

    /* Move preserved hashes into pending array for tracking */
    size_t moved_count = 0;
    for (size_t idx = 0; idx < actor->preserved_count; idx++) {
      if (actor->pending_count >= actor->pending_capacity) {
        size_t new_capacity = actor->pending_capacity == 0 ? 16 : actor->pending_capacity * 2;
        respiration_pending_t* new_pending = realloc(actor->pending, sizeof(respiration_pending_t) * new_capacity);
        if (new_pending == NULL) break;
        actor->pending = new_pending;
        actor->pending_capacity = new_capacity;
      }
      actor->pending[actor->pending_count].hash = actor->preserved[idx].hash;
      actor->pending[actor->pending_count].ejection_date = actor->preserved[idx].ejection_date;
      actor->pending_count++;
      moved_count++;
    }
    /* Free any preserved hashes that couldn't be moved (best effort) */
    for (size_t idx = moved_count; idx < actor->preserved_count; idx++) {
      DESTROY(actor->preserved[idx].hash, buffer);
    }
    actor->preserved_count = 0;

    /* Send NETWORK_LOCAL_STORE_BLOCK for each pending hash */
    size_t store_count = actor->pending_count;
    for (size_t idx = 0; idx < store_count; idx++) {
      network_local_store_block_payload_t* store_payload =
          get_clear_memory(sizeof(network_local_store_block_payload_t));
      if (store_payload == NULL) {
        /* Allocation failed — keep block locally, remove from pending */
        DESTROY(actor->pending[idx].hash, buffer);
        actor->pending[idx].hash = NULL;
        continue;
      }
      store_payload->hash = REFERENCE(actor->pending[idx].hash, buffer_t);
      store_payload->fib = 1; /* minimum FIB for store propagation */
      store_payload->reply_to = &actor->actor;

      message_t store_msg;
      store_msg.type = NETWORK_LOCAL_STORE_BLOCK;
      store_msg.payload = store_payload;
      store_msg.payload_destroy = network_local_store_block_payload_destroy;

      actor_send(&actor->network->actor, &store_msg);
    }
    /* Compact pending: remove NULL entries from allocation failures */
    size_t write_idx = 0;
    for (size_t idx = 0; idx < actor->pending_count; idx++) {
      if (actor->pending[idx].hash != NULL) {
        actor->pending[write_idx++] = actor->pending[idx];
      }
    }
    actor->pending_count = write_idx;
    /* If all stores failed to allocate, go idle */
    if (actor->pending_count == 0) {
      _respiration_watchdog_disarm(actor);
      atomic_store(&actor->state, RESPIRATION_IDLE);
    }
  } else {
    /* No preserved blocks and capacity still above threshold — best effort, go idle */
    _respiration_watchdog_disarm(actor);
    atomic_store(&actor->state, RESPIRATION_IDLE);
  }
}

respiration_actor_t* respiration_actor_create(network_t* network, scheduler_pool_t* pool) {
  if (network == NULL || pool == NULL) return NULL;
  respiration_actor_t* actor = get_clear_memory(sizeof(respiration_actor_t));
  if (actor == NULL) return NULL;
  actor->network = network;
  actor->pool = pool;
  actor->state = ATOMIC_VAR_INIT(RESPIRATION_IDLE);
  actor->watchdog_timer_id = ATOMIC_VAR_INIT(0);
  actor->redundant_hashes = NULL;
  actor->redundant_count = 0;
  actor->redundant_capacity = 0;
  actor->preserved = NULL;
  actor->preserved_count = 0;
  actor->preserved_capacity = 0;
  actor->pending = NULL;
  actor->pending_count = 0;
  actor->pending_capacity = 0;
  actor_init(&actor->actor, actor, respiration_actor_dispatch, pool);
  return actor;
}

void respiration_actor_destroy(respiration_actor_t* actor) {
  if (actor == NULL) return;
  ATOMIC_STORE(&actor->state, RESPIRATION_IDLE);

  /* Cancel the watchdog timer before tearing down — otherwise a pending
     watchdog could fire after free. See audit #29. */
  _respiration_watchdog_disarm(actor);

  /* Free pending hashes */
  if (actor->pending != NULL) {
    for (size_t index = 0; index < actor->pending_count; index++) {
      if (actor->pending[index].hash != NULL) {
        DESTROY(actor->pending[index].hash, buffer);
      }
    }
    free(actor->pending);
    actor->pending = NULL;
    actor->pending_count = 0;
    actor->pending_capacity = 0;
  }

  /* Free redundant hashes */
  if (actor->redundant_hashes != NULL) {
    for (size_t index = 0; index < actor->redundant_count; index++) {
      if (actor->redundant_hashes[index] != NULL) {
        DESTROY(actor->redundant_hashes[index], buffer);
      }
    }
    free(actor->redundant_hashes);
    actor->redundant_hashes = NULL;
    actor->redundant_count = 0;
    actor->redundant_capacity = 0;
  }

  /* Free preserved entries */
  if (actor->preserved != NULL) {
    for (size_t index = 0; index < actor->preserved_count; index++) {
      if (actor->preserved[index].hash != NULL) {
        DESTROY(actor->preserved[index].hash, buffer);
      }
    }
    free(actor->preserved);
    actor->preserved = NULL;
    actor->preserved_count = 0;
    actor->preserved_capacity = 0;
  }

  actor_destroy(&actor->actor);
  memset(actor, 0xDD, sizeof(respiration_actor_t));
  free(actor);
}