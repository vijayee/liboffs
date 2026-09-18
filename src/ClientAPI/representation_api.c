//
// Representation-level ephemeral/pin wire ops shared by every transport.
//
#include "representation_api.h"
#include "client_api_wire.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../BlockCache/block_cache.h"
#include "../OFFStreams/off_url.h"
#include "../OFFStreams/representation_actor.h"
#include "../RefCounter/refcounter.h"
#include "../Scheduler/scheduler.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include <stdlib.h>
#include <string.h>

/* Deferred-destruction wrapper for a representation actor. The completion
   dispatch runs on a pool worker, so it can NEVER call
   representation_actor_destroy inline: that destroy parks the pool via
   scheduler_pool_wait_for_idle, and a worker waiting for its own pool's
   idleness (itself included) would never return. Instead the completion
   queues this wrapper through scheduler_pool_defer_cleanup and the drain —
   which only ever runs when every worker is already idle — performs the
   destroy.

   The wrapper's leading refcounter_t satisfies defer_cleanup's
   hold-a-reference contract: defer_cleanup refcounter_references the object
   it is handed, so handing it a bare representation_actor_t (whose first
   member is a live actor mailbox, not a refcounter) would corrupt the
   mailbox's head pointer. The reference is never released — the drain's
   destructor frees the wrapper unconditionally, matching the
   off_routes.c rep_route_defer_t pattern exactly. */
typedef struct {
  refcounter_t refcounter;
  representation_actor_t* rep;
} rep_api_defer_t;

static void _rep_api_deferred_destroy(rep_api_defer_t* defer) {
  representation_actor_t* rep = defer->rep;
  refcounter_destroy_lock(&defer->refcounter);
  free(defer);
  representation_actor_destroy(rep);
}

/* Completion context for the five wire ops. actor stays the FIRST member:
   the destroy path runs actor_destroy (tearing the mailbox down) before
   deferring the struct to the pool's cleanup drain, and defer_cleanup's
   hold-a-reference then lands on the mailbox's dead head field — memory
   nothing reads again before free. This mirrors the off_routes.c
   rep_route_context_t / list_route_context_t lifecycle exactly. */
typedef struct {
  actor_t actor;
  rep_api_connection_t* conn;
  rep_api_send_frame_fn send_frame;
  rep_api_send_error_fn send_error;
  scheduler_pool_t* pool;
  int response_code;  /* CLIENT_API_REP_*_RESPONSE for the pending op */
} rep_api_context_t;

static void _rep_api_context_destroy(rep_api_context_t* ctx) {
  atomic_fetch_or(&ctx->actor.flags, ACTOR_FLAG_DESTROY);
  actor_destroy(&ctx->actor);
  scheduler_pool_defer_cleanup(ctx->pool, ctx, free);
}

/* Completion for the four representation ops: encode the summary response
   frame, queue the rep actor's deferred destruction, then tear the
   completion context down through the same deferred path. */
static void _rep_api_op_dispatch(void* state, message_t* msg) {
  rep_api_context_t* ctx = (rep_api_context_t*)state;
  if (msg->type != REPRESENTATION_OP_RESULT) {
    return;
  }
  representation_op_result_payload_t* result =
      (representation_op_result_payload_t*)msg->payload;

  client_api_rep_response_t response;
  response.status = result->result == 0 ? 0 : 1;
  response.blocks = result->blocks_touched;
  cbor_item_t* frame = client_api_rep_response_encode(ctx->response_code, &response);
  ctx->send_frame(ctx->conn, frame);

  /* Queue the representation actor's deferred destruction. The pointer
     comes from the result payload itself (payload->source), so this works
     even when the whole walk completed before representation_actor_create
     returned. */
  if (result->source != NULL) {
    rep_api_defer_t* defer = get_clear_memory(sizeof(rep_api_defer_t));
    refcounter_init(&defer->refcounter);
    defer->rep = result->source;
    scheduler_pool_defer_cleanup(ctx->pool, defer,
                                 (void (*)(void*))_rep_api_deferred_destroy);
  }

  _rep_api_context_destroy(ctx);
}

/* Completion for the ephemeral list: copy each entry into the response
   encoding, then honor the mirror's steal contract (destroy every
   referenced hash, free the three arrays, empty the shell) so actor_run's
   payload_destroy frees the emptied shell exactly once. */
static void _rep_api_list_dispatch(void* state, message_t* msg) {
  rep_api_context_t* ctx = (rep_api_context_t*)state;
  if (msg->type != CACHE_EPHEMERAL_LIST) {
    return;
  }
  cache_ephemeral_list_payload_t* payload =
      (cache_ephemeral_list_payload_t*)msg->payload;
  if (payload == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                    "Ephemeral list failed");
    _rep_api_context_destroy(ctx);
    return;
  }

  client_api_ephemeral_list_response_t response;
  memset(&response, 0, sizeof(response));
  response.status = CLIENT_API_STATUS_OK;
  response.count = payload->count;
  if (payload->count > 0) {
    response.hashes = get_clear_memory(sizeof(uint8_t*) * payload->count);
    response.claims = get_clear_memory(sizeof(uint16_t) * payload->count);
    response.pins = get_clear_memory(sizeof(uint32_t) * payload->count);
    for (size_t entry_index = 0; entry_index < payload->count; entry_index++) {
      /* Every response hash buffer is 32 bytes: copy what the (referenced)
         index hash carries and zero-fill any shortfall, so a short hash can
         never read past the buffer while the wire shape stays fixed. */
      response.hashes[entry_index] = get_clear_memory(32);
      buffer_t* hash = (payload->hashes != NULL) ? payload->hashes[entry_index] : NULL;
      if (hash != NULL && hash->data != NULL && hash->size > 0) {
        size_t bytes = hash->size < 32 ? hash->size : 32;
        memcpy(response.hashes[entry_index], hash->data, bytes);
      }
      response.claims[entry_index] = (payload->ephemeral_counts != NULL)
                                         ? payload->ephemeral_counts[entry_index]
                                         : 0;
      response.pins[entry_index] = (payload->pin_counts != NULL)
                                       ? payload->pin_counts[entry_index]
                                       : 0;
    }
  }

  cbor_item_t* frame = client_api_ephemeral_list_response_encode(&response);
  ctx->send_frame(ctx->conn, frame);
  client_api_ephemeral_list_response_destroy(&response);

  /* Consumer contract (block_cache.h): steal the arrays — destroy each
     referenced hash, free the three arrays — then empty the shell (NULL the
     pointers, zero the count) and leave msg->payload for actor_run's
     payload_destroy, which frees the emptied shell exactly once. */
  if (payload->hashes != NULL) {
    for (size_t entry_index = 0; entry_index < payload->count; entry_index++) {
      if (payload->hashes[entry_index] != NULL) {
        DESTROY(payload->hashes[entry_index], buffer);
      }
    }
    free(payload->hashes);
    payload->hashes = NULL;
  }
  if (payload->ephemeral_counts != NULL) {
    free(payload->ephemeral_counts);
    payload->ephemeral_counts = NULL;
  }
  if (payload->pin_counts != NULL) {
    free(payload->pin_counts);
    payload->pin_counts = NULL;
  }
  payload->count = 0;

  _rep_api_context_destroy(ctx);
}

static rep_api_context_t* _rep_api_context_create(
    rep_api_connection_t* conn, rep_api_send_frame_fn send_frame,
    rep_api_send_error_fn send_error, scheduler_pool_t* pool,
    void (*dispatch)(void* state, message_t* msg), int response_code) {
  rep_api_context_t* ctx = get_clear_memory(sizeof(rep_api_context_t));
  ctx->conn = conn;
  ctx->send_frame = send_frame;
  ctx->send_error = send_error;
  ctx->pool = pool;
  ctx->response_code = response_code;
  actor_init(&ctx->actor, ctx, dispatch, pool);
  return ctx;
}

void client_api_representation_handle(block_cache_t* bc, scheduler_pool_t* pool,
                                     network_t* network, cbor_item_t* frame, int op_code,
                                     rep_api_send_frame_fn send_frame,
                                     rep_api_connection_t* conn,
                                     rep_api_send_error_fn send_error) {
  if (bc == NULL || pool == NULL || frame == NULL) {
    if (send_error != NULL) {
      send_error(conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Transport not ready");
    }
    return;
  }

  if (op_code == CLIENT_API_EPHEMERAL_LIST_REQUEST) {
    rep_api_context_t* ctx = _rep_api_context_create(conn, send_frame, send_error,
                                                      pool, _rep_api_list_dispatch, 0);
    block_cache_list_ephemeral(bc, &ctx->actor);
    return;
  }

  client_api_rep_request_t request;
  memset(&request, 0, sizeof(request));
  if (client_api_rep_request_decode(frame, &request) != 0) {
    send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid representation op request");
    return;
  }

  off_url_t* url = off_url_parse(request.url);
  client_api_rep_request_destroy(&request);
  if (url == NULL || url->descriptor_hash == NULL) {
    if (url != NULL) {
      off_url_destroy(url);
    }
    send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid OFF URL");
    return;
  }

  representation_op_e op;
  switch (op_code) {
    case CLIENT_API_REP_MARK_PERMANENT_REQUEST:
      op = REPRESENTATION_OP_MARK_PERMANENT;
      break;
    case CLIENT_API_REP_DELETE_EPHEMERAL_REQUEST:
      op = REPRESENTATION_OP_DELETE_EPHEMERAL;
      break;
    case CLIENT_API_REP_PIN_REQUEST:
      op = REPRESENTATION_OP_PIN;
      break;
    case CLIENT_API_REP_UNPIN_REQUEST:
      op = REPRESENTATION_OP_UNPIN;
      break;
    default:
      off_url_destroy(url);
      send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Unknown representation op");
      return;
  }

  rep_api_context_t* ctx = _rep_api_context_create(conn, send_frame, send_error,
                                                   pool, _rep_api_op_dispatch,
                                                   /* each response code is its
                                                      request code + 1 (42→43,
                                                      44→45, 46→47, 48→49) */
                                                   op_code + 1);

  /* representation_actor_create references the descriptor hash itself; it
     kicks the walk before returning, and the completion can fire before
     create returns — the reply carries the actor pointer (payload->source),
     so nothing here needs the create's return value. */
  representation_actor_create(bc, network, url->descriptor_hash, op, &ctx->actor);
  off_url_destroy(url);
}