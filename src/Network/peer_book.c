//
// Created by victor on 9/24/26.
//

#include "peer_book.h"
#include "network.h"
#include "endpoint.h"
#include "../Actor/message.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <stdlib.h>
#include <string.h>

/* Peer-book actor — owns and serializes all peer-list access. See the
   invariant comment in peer_book.h. */

/* Cadence of the friend reconnect / partition-heal tick. Previously
   FRIEND_RECONNECT_INTERVAL_MS (5000) on the network actor in network.c; the
   timer now targets this actor, which replies fire-and-forget with a
   PEER_BOOK_RECONNECT snapshot so the network actor never blocks and never
   reads the authority lists off its own thread. */
#define PEER_BOOK_RECONNECT_INTERVAL_MS 5000u

/* --- Deep copy helper (caller thread or actor thread, whichever builds) --- */

/* Deep-copy a peer_info_t: node_id by value, public_key and address host
   strings duplicated. Returns NULL on allocation failure. The copy is fully
   owned by the receiver and released with peer_info_destroy + free. */
static peer_info_t* _peer_book_copy_peer_info(const peer_info_t* source) {
  if (source == NULL) return NULL;

  peer_info_t* copy = get_clear_memory(sizeof(peer_info_t));
  if (copy == NULL) return NULL;
  memcpy(&copy->node_id, &source->node_id, sizeof(node_id_t));

  if (source->public_key != NULL && source->public_key_len > 0) {
    copy->public_key = get_clear_memory(source->public_key_len);
    if (copy->public_key == NULL) {
      free(copy);
      return NULL;
    }
    memcpy(copy->public_key, source->public_key, source->public_key_len);
    copy->public_key_len = source->public_key_len;
  }

  if (source->addresses != NULL && source->address_count > 0) {
    copy->addresses = get_clear_memory(source->address_count * sizeof(peer_address_t));
    if (copy->addresses == NULL) {
      peer_info_destroy(copy);
      free(copy);
      return NULL;
    }
    for (size_t index = 0; index < source->address_count; index++) {
      const peer_address_t* address = &source->addresses[index];
      if (address->host == NULL) continue;
      size_t host_len = strlen(address->host);
      char* host = get_clear_memory(host_len + 1);
      if (host == NULL) continue;
      memcpy(host, address->host, host_len);
      copy->addresses[copy->address_count].type = address->type;
      copy->addresses[copy->address_count].port = address->port;
      copy->addresses[copy->address_count].relay_id = address->relay_id;
      copy->addresses[copy->address_count].host = host;
      copy->address_count++;
    }
  }
  return copy;
}

static void _peer_book_destroy_peer_info_array(peer_info_t** entries, size_t count) {
  if (entries == NULL) return;
  for (size_t index = 0; index < count; index++) {
    if (entries[index] != NULL) {
      peer_info_destroy(entries[index]);
      free(entries[index]);
    }
  }
  free(entries);
}

static void _peer_book_destroy_string_array(char** entries, size_t count) {
  if (entries == NULL) return;
  for (size_t index = 0; index < count; index++) {
    free(entries[index]);
  }
  free(entries);
}

void peer_book_free_peer_info_array(peer_info_t** entries, size_t count) {
  _peer_book_destroy_peer_info_array(entries, count);
}

void peer_book_free_string_array(char** entries, size_t count) {
  _peer_book_destroy_string_array(entries, count);
}

/* --- Mutation apply (actor thread only) --- */

/* FRIEND_ADD: duplicate (by node_id) -> -2 (copy destroyed); OOM -> -1
   (copy destroyed); success -> 0 and the copy moves into the list. */
static int _peer_book_apply_friend_add(authority_t* authority,
                                       peer_info_t** friend_info_inout) {
  peer_info_t* friend_info = *friend_info_inout;
  if (friend_info == NULL) return -1;

  for (size_t index = 0; index < authority->friend_peer_count; index++) {
    if (peer_info_equals(authority->friend_peers[index], friend_info)) {
      peer_info_destroy(friend_info);
      free(friend_info);
      *friend_info_inout = NULL;
      return -2;  /* already a friend */
    }
  }

  size_t new_count = authority->friend_peer_count + 1;
  peer_info_t** expanded = realloc(authority->friend_peers,
                                   new_count * sizeof(peer_info_t*));
  if (expanded == NULL) {
    peer_info_destroy(friend_info);
    free(friend_info);
    *friend_info_inout = NULL;
    return -1;  /* OOM */
  }
  authority->friend_peers = expanded;
  authority->friend_peers[authority->friend_peer_count] = friend_info;
  authority->friend_peer_count = new_count;
  *friend_info_inout = NULL;  /* list owns the copy now */
  return 0;
}

/* FRIEND_REMOVE: 0 removed, -1 not found. Does not shrink the array (the
   next add reallocs); authority_destroy frees the array. */
static int _peer_book_apply_friend_remove(authority_t* authority,
                                          const node_id_t* target_id) {
  size_t found_index = 0;
  int found = 0;
  for (size_t index = 0; index < authority->friend_peer_count; index++) {
    if (node_id_equals(&authority->friend_peers[index]->node_id, target_id)) {
      found_index = index;
      found = 1;
      break;
    }
  }
  if (!found) return -1;

  peer_info_destroy(authority->friend_peers[found_index]);
  free(authority->friend_peers[found_index]);
  for (size_t index = found_index; index + 1 < authority->friend_peer_count; index++) {
    authority->friend_peers[index] = authority->friend_peers[index + 1];
  }
  authority->friend_peer_count--;
  return 0;
}

static void _peer_book_apply_mutation(peer_book_t* peer_book,
                                      peer_book_mutation_t* request) {
  authority_t* authority = peer_book->authority;
  switch (request->op) {
    case peer_book_op_friend_add:
      request->result = _peer_book_apply_friend_add(authority,
                                                    &request->friend_info);
      break;
    case peer_book_op_friend_remove:
      request->result = _peer_book_apply_friend_remove(authority,
                                                       &request->target_id);
      break;
    case peer_book_op_bootstrap_add:
      /* The existing authority contract helpers now run only on the actor
         thread — the same code the handlers used to call inline. */
      request->result = authority_bootstrap_add(authority, request->endpoint);
      break;
    case peer_book_op_bootstrap_remove:
      request->result = authority_bootstrap_remove(authority, request->endpoint);
      break;
    default:
      request->result = -1;
      break;
  }
}

/* Frees the whole mutation request (shell + any unconsumed input copies +
   reply plumbing). The apply step nulls consumed input copies, so on the
   normal free path there is nothing left to release beyond the shell. */
static void _peer_book_mutation_free(peer_book_mutation_t* request) {
  if (request == NULL) return;
  if (request->friend_info != NULL) {
    peer_info_destroy(request->friend_info);
    free(request->friend_info);
  }
  if (request->endpoint != NULL) {
    free(request->endpoint);
  }
  if (request->reply.mutex != NULL) platform_mutex_destroy(request->reply.mutex);
  if (request->reply.cv != NULL) platform_condvar_destroy(request->reply.cv);
  free(request);
}

static void _peer_book_handle_mutation(peer_book_t* peer_book,
                                       peer_book_mutation_t* request) {
  if (request == NULL) return;
  _peer_book_apply_mutation(peer_book, request);

  platform_mutex_lock(request->reply.mutex);
  if (request->reply.orphaned) {
    /* The caller timed out and handed us ownership — free the request shell.
       The apply above still happened: a timed-out mutation may have been
       applied (documented in peer_book.h). */
    platform_mutex_unlock(request->reply.mutex);
    _peer_book_mutation_free(request);
    return;
  }
  request->reply.done = 1;
  platform_condvar_signal(request->reply.cv);
  platform_mutex_unlock(request->reply.mutex);
}

/* --- Snapshot handling --- */

static void _peer_book_snapshot_friends_locked(peer_book_t* peer_book,
                                               peer_book_snapshot_t* request) {
  authority_t* authority = peer_book->authority;
  if (authority->friend_peer_count > 0) {
    request->friends = get_clear_memory(authority->friend_peer_count *
                                        sizeof(peer_info_t*));
    if (request->friends == NULL) return;  /* reply stays empty */
    for (size_t index = 0; index < authority->friend_peer_count; index++) {
      peer_info_t* copy = _peer_book_copy_peer_info(authority->friend_peers[index]);
      if (copy == NULL) continue;
      request->friends[request->friend_count++] = copy;
    }
  }
}

static void _peer_book_snapshot_bootstrap_locked(peer_book_t* peer_book,
                                                 peer_book_snapshot_t* request) {
  authority_t* authority = peer_book->authority;
  if (authority->bootstrap_peer_count > 0) {
    request->config_endpoints = get_clear_memory(authority->bootstrap_peer_count *
                                                 sizeof(char*));
    if (request->config_endpoints != NULL) {
      for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
        char* copy = strdup(authority->bootstrap_peers[index]);
        if (copy == NULL) continue;
        request->config_endpoints[request->config_count++] = copy;
      }
    }
  }
  if (authority->managed_bootstrap_peer_count > 0) {
    request->managed_endpoints =
        get_clear_memory(authority->managed_bootstrap_peer_count * sizeof(char*));
    if (request->managed_endpoints != NULL) {
      for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
        char* copy = strdup(authority->managed_bootstrap_peers[index]);
        if (copy == NULL) continue;
        request->managed_endpoints[request->managed_count++] = copy;
      }
    }
  }
}

static void _peer_book_snapshot_free(peer_book_snapshot_t* request) {
  if (request == NULL) return;
  _peer_book_destroy_peer_info_array(request->friends, request->friend_count);
  _peer_book_destroy_string_array(request->config_endpoints, request->config_count);
  _peer_book_destroy_string_array(request->managed_endpoints, request->managed_count);
  if (request->reply.mutex != NULL) platform_mutex_destroy(request->reply.mutex);
  if (request->reply.cv != NULL) platform_condvar_destroy(request->reply.cv);
  free(request);
}

static void _peer_book_handle_snapshot(peer_book_t* peer_book,
                                       peer_book_snapshot_t* request) {
  if (request == NULL) return;

  platform_mutex_lock(request->reply.mutex);
  if (request->reply.orphaned) {
    platform_mutex_unlock(request->reply.mutex);
    _peer_book_snapshot_free(request);
    return;
  }
  if (request->kind == peer_book_snapshot_kind_friends) {
    _peer_book_snapshot_friends_locked(peer_book, request);
  } else {
    _peer_book_snapshot_bootstrap_locked(peer_book, request);
  }
  request->reply.done = 1;
  platform_condvar_signal(request->reply.cv);
  platform_mutex_unlock(request->reply.mutex);
}

/* --- Fire-and-forget payloads --- */

void peer_book_reconnect_payload_destroy(void* ptr) {
  peer_book_reconnect_payload_t* payload = (peer_book_reconnect_payload_t*)ptr;
  if (payload == NULL) return;
  _peer_book_destroy_peer_info_array(payload->friends, payload->friend_count);
  _peer_book_destroy_string_array(payload->config_endpoints, payload->config_count);
  _peer_book_destroy_string_array(payload->managed_endpoints, payload->managed_count);
  free(payload);
}

void peer_book_save_snapshot_destroy(void* ptr) {
  peer_book_save_snapshot_t* snapshot = (peer_book_save_snapshot_t*)ptr;
  if (snapshot == NULL) return;
  _peer_book_destroy_string_array(snapshot->b58_friends, snapshot->b58_friend_count);
  _peer_book_destroy_string_array(snapshot->managed_endpoints, snapshot->managed_count);
  free(snapshot);
}

/* Reconnect tick: the actor owns the lists, so building the snapshot is
   copy-only work. The network actor consumes the payload on its own thread
   and runs the heal + friend reconnect + direct-upgrade logic there. */
static void _peer_book_handle_reconnect_tick(peer_book_t* peer_book) {
  if (!atomic_load(&peer_book->started)) return;
  if (peer_book->network == NULL || peer_book->authority == NULL) return;

  peer_book_reconnect_payload_t* payload =
      get_clear_memory(sizeof(peer_book_reconnect_payload_t));
  if (payload == NULL) return;

  authority_t* authority = peer_book->authority;
  if (authority->friend_peer_count > 0) {
    payload->friends = get_clear_memory(authority->friend_peer_count *
                                        sizeof(peer_info_t*));
    if (payload->friends != NULL) {
      for (size_t index = 0; index < authority->friend_peer_count; index++) {
        peer_info_t* copy = _peer_book_copy_peer_info(authority->friend_peers[index]);
        if (copy == NULL) continue;
        payload->friends[payload->friend_count++] = copy;
      }
    }
  }
  if (authority->bootstrap_peer_count > 0) {
    payload->config_endpoints = get_clear_memory(authority->bootstrap_peer_count *
                                                 sizeof(char*));
    if (payload->config_endpoints != NULL) {
      for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
        char* copy = strdup(authority->bootstrap_peers[index]);
        if (copy == NULL) continue;
        payload->config_endpoints[payload->config_count++] = copy;
      }
    }
  }
  if (authority->managed_bootstrap_peer_count > 0) {
    payload->managed_endpoints =
        get_clear_memory(authority->managed_bootstrap_peer_count * sizeof(char*));
    if (payload->managed_endpoints != NULL) {
      for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
        char* copy = strdup(authority->managed_bootstrap_peers[index]);
        if (copy == NULL) continue;
        payload->managed_endpoints[payload->managed_count++] = copy;
      }
    }
  }

  message_t message;
  memset(&message, 0, sizeof(message));
  message.type = PEER_BOOK_RECONNECT;
  message.payload = payload;
  message.payload_destroy = peer_book_reconnect_payload_destroy;
  /* Fire-and-forget: if the network actor is already destroyed the payload
     is freed by actor_send's drop path. */
  actor_send(&peer_book->network->actor, &message);
}

/* Debounced peer-state save (NETWORK_PEER_STATE_SAVE on the network actor
   asked us for the friend/managed lists): build the Base58 friend strings +
   managed endpoint strings the save needs and hand them to the network
   actor, which calls authority_save_peers_snapshot and clears the dirty
   flag. Neither actor ever blocks. */
static void _peer_book_handle_save(peer_book_t* peer_book) {
  if (peer_book->network == NULL || peer_book->authority == NULL) return;

  authority_t* authority = peer_book->authority;
  peer_book_save_snapshot_t* snapshot =
      get_clear_memory(sizeof(peer_book_save_snapshot_t));
  if (snapshot == NULL) return;

  if (authority->friend_peer_count > 0) {
    snapshot->b58_friends = get_clear_memory(authority->friend_peer_count *
                                             sizeof(char*));
    if (snapshot->b58_friends != NULL) {
      for (size_t index = 0; index < authority->friend_peer_count; index++) {
        char* b58 = peer_info_to_base58(authority->friend_peers[index]);
        if (b58 == NULL) continue;
        snapshot->b58_friends[snapshot->b58_friend_count++] = b58;
      }
    }
  }
  if (authority->managed_bootstrap_peer_count > 0) {
    snapshot->managed_endpoints =
        get_clear_memory(authority->managed_bootstrap_peer_count * sizeof(char*));
    if (snapshot->managed_endpoints != NULL) {
      for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
        char* copy = strdup(authority->managed_bootstrap_peers[index]);
        if (copy == NULL) continue;
        snapshot->managed_endpoints[snapshot->managed_count++] = copy;
      }
    }
  }

  message_t message;
  memset(&message, 0, sizeof(message));
  message.type = PEER_BOOK_SAVE_SNAPSHOT;
  message.payload = snapshot;
  message.payload_destroy = peer_book_save_snapshot_destroy;
  actor_send(&peer_book->network->actor, &message);
}

void peer_book_dispatch(void* state, message_t* msg) {
  peer_book_t* peer_book = (peer_book_t*)state;
  if (peer_book == NULL || msg == NULL) return;
  switch (msg->type) {
    case PEER_BOOK_RECONNECT_TICK:
      _peer_book_handle_reconnect_tick(peer_book);
      break;
    case PEER_BOOK_MUTATION:
      _peer_book_handle_mutation(peer_book, (peer_book_mutation_t*)msg->payload);
      break;
    case PEER_BOOK_SNAPSHOT:
      _peer_book_handle_snapshot(peer_book, (peer_book_snapshot_t*)msg->payload);
      break;
    case PEER_BOOK_SAVE:
      _peer_book_handle_save(peer_book);
      break;
    default:
      break;
  }
}

/* --- Lifecycle --- */

peer_book_t* peer_book_create(authority_t* authority, network_t* network,
                              timer_actor_t* timer, scheduler_pool_t* pool) {
  if (authority == NULL || pool == NULL) return NULL;
  peer_book_t* peer_book = get_clear_memory(sizeof(peer_book_t));
  if (peer_book == NULL) return NULL;
  peer_book->authority = authority;
  peer_book->network = network;
  peer_book->timer = timer;
  peer_book->pool = pool;
  peer_book->reconnect_timer_id = ATOMIC_VAR_INIT(0);
  peer_book->started = ATOMIC_VAR_INIT(0);
  actor_init(&peer_book->actor, peer_book, peer_book_dispatch, pool);
  return peer_book;
}

int peer_book_start(peer_book_t* peer_book) {
  if (peer_book == NULL) return -1;
  if (atomic_load(&peer_book->started)) return 0;
  /* Arm the recurring reconnect tick. The first fire lands one interval
     after start, past the startup-phase list seeding/loading window. */
  if (peer_book->timer != NULL) {
    timer_actor_set(peer_book->timer,
                    PEER_BOOK_RECONNECT_INTERVAL_MS,
                    PEER_BOOK_RECONNECT_INTERVAL_MS,
                    &peer_book->actor,
                    PEER_BOOK_RECONNECT_TICK,
                    &peer_book->reconnect_timer_id);
  }
  ATOMIC_STORE(&peer_book->started, 1);
  return 0;
}

void peer_book_stop(peer_book_t* peer_book) {
  if (peer_book == NULL) return;
  ATOMIC_STORE(&peer_book->started, 0);
}

void peer_book_destroy(peer_book_t* peer_book) {
  if (peer_book == NULL) return;
  peer_book_stop(peer_book);
  /* Cancel the tick timer before tearing down the actor so no late tick can
     fire into a freed actor. timer_actor_cancel is a no-op for unknown ids. */
  uint64_t timer_id = atomic_load(&peer_book->reconnect_timer_id);
  if (timer_id != 0 && peer_book->timer != NULL) {
    timer_actor_cancel(peer_book->timer, timer_id);
  }
  actor_destroy(&peer_book->actor);
  memset(peer_book, 0xDD, sizeof(peer_book_t));
  free(peer_book);
}

/* --- Bounded synchronous round-trips --- */

/* Shared round-trip for the request payloads embedding peer_book_reply_t as
   their first member. Returns:
     0  — done; the caller reads reply fields / result and frees the request;
     -1 — timed out; the request was orphaned to the actor and the caller
          must NOT free anything;
     1  — send failure (actor stopped or destroyed, or queue torn down);
          nobody will process the request, so the caller frees it itself.
   payload_destroy stays NULL: the request shell is owned by the
   caller/orphan scheme, never by the queue. */
static int _peer_book_round_trip(peer_book_t* peer_book, uint32_t type,
                                 peer_book_reply_t* reply, uint32_t timeout_ms) {
  if (peer_book == NULL || reply == NULL) return 1;
  if (!atomic_load(&peer_book->started)) return 1;
  /* actor_send's return value is a scheduling hint (was the mailbox empty),
     NOT a delivery acknowledgement — a concurrent sender can make it false
     while the message is queued. The only real "nobody will process this"
     case is a destroyed actor/mailbox, checked here. payload_destroy stays
     NULL, so a message dropped in the DESTROY-check-then-push TOCTOU leaks
     the request shell during teardown instead of UAFing the waiting caller
     (requests are short-lived and teardown drains HTTP first). */
  if (atomic_load(&peer_book->actor.flags) & ACTOR_FLAG_DESTROY) return 1;

  message_t message;
  memset(&message, 0, sizeof(message));
  message.type = type;
  message.payload = reply;
  message.payload_destroy = NULL;
  actor_send(&peer_book->actor, &message);

  platform_mutex_lock(reply->mutex);
  for (;;) {
    if (reply->done) break;
    if (platform_condvar_timed_wait(reply->cv, reply->mutex, timeout_ms) != 0) {
      if (!reply->done) {
        /* Hand the request over to the actor: it may still be queued or
           mid-apply. The actor frees it after processing. */
        reply->orphaned = 1;
        platform_mutex_unlock(reply->mutex);
        return -1;
      }
      break;
    }
  }
  platform_mutex_unlock(reply->mutex);
  return 0;
}

/* Request-shell cleanup used by the caller-side helpers on the send-failure
   path (the request never reached the actor, so nothing else frees it). */
static void _peer_book_mutation_request_destroy(peer_book_mutation_t* request) {
  if (request == NULL) return;
  if (request->friend_info != NULL) {
    peer_info_destroy(request->friend_info);
    free(request->friend_info);
  }
  if (request->endpoint != NULL) free(request->endpoint);
  if (request->reply.mutex != NULL) platform_mutex_destroy(request->reply.mutex);
  if (request->reply.cv != NULL) platform_condvar_destroy(request->reply.cv);
  free(request);
}

static int _peer_book_mutation_round_trip(peer_book_t* peer_book,
                                          peer_book_mutation_t* request,
                                          uint32_t timeout_ms) {
  int trip = _peer_book_round_trip(peer_book, PEER_BOOK_MUTATION,
                                   &request->reply, timeout_ms);
  if (trip == 1) {
    /* Never queued — the actor will never touch it. */
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  if (trip != 0) return -1;  /* orphaned to the actor; do not free */

  int result = request->result;
  /* _peer_book_mutation_free releases the shell plus any input copies the
     apply did not consume (e.g. the bootstrap endpoint string, which
     authority_bootstrap_add re-encodes into its own storage). */
  _peer_book_mutation_free(request);
  return result;
}

int peer_book_friend_add(peer_book_t* peer_book, const peer_info_t* info,
                         uint32_t timeout_ms) {
  if (peer_book == NULL || info == NULL) return -1;
  peer_book_mutation_t* request = get_clear_memory(sizeof(peer_book_mutation_t));
  if (request == NULL) return -1;
  request->reply.mutex = platform_mutex_create();
  request->reply.cv = platform_condvar_create();
  if (request->reply.mutex == NULL || request->reply.cv == NULL) {
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  request->op = peer_book_op_friend_add;
  request->friend_info = _peer_book_copy_peer_info(info);
  if (request->friend_info == NULL) {
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  return _peer_book_mutation_round_trip(peer_book, request, timeout_ms);
}

int peer_book_friend_remove(peer_book_t* peer_book, const node_id_t* target_id,
                            uint32_t timeout_ms) {
  if (peer_book == NULL || target_id == NULL) return -1;
  peer_book_mutation_t* request = get_clear_memory(sizeof(peer_book_mutation_t));
  if (request == NULL) return -1;
  request->reply.mutex = platform_mutex_create();
  request->reply.cv = platform_condvar_create();
  if (request->reply.mutex == NULL || request->reply.cv == NULL) {
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  request->op = peer_book_op_friend_remove;
  memcpy(&request->target_id, target_id, sizeof(node_id_t));
  return _peer_book_mutation_round_trip(peer_book, request, timeout_ms);
}

int peer_book_bootstrap_add(peer_book_t* peer_book, const char* endpoint,
                            uint32_t timeout_ms) {
  if (peer_book == NULL || endpoint == NULL) return -1;
  peer_book_mutation_t* request = get_clear_memory(sizeof(peer_book_mutation_t));
  if (request == NULL) return -1;
  request->reply.mutex = platform_mutex_create();
  request->reply.cv = platform_condvar_create();
  if (request->reply.mutex == NULL || request->reply.cv == NULL) {
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  request->op = peer_book_op_bootstrap_add;
  request->endpoint = strdup(endpoint);
  if (request->endpoint == NULL) {
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  return _peer_book_mutation_round_trip(peer_book, request, timeout_ms);
}

int peer_book_bootstrap_remove(peer_book_t* peer_book, const char* endpoint,
                               uint32_t timeout_ms) {
  if (peer_book == NULL || endpoint == NULL) return -1;
  peer_book_mutation_t* request = get_clear_memory(sizeof(peer_book_mutation_t));
  if (request == NULL) return -1;
  request->reply.mutex = platform_mutex_create();
  request->reply.cv = platform_condvar_create();
  if (request->reply.mutex == NULL || request->reply.cv == NULL) {
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  request->op = peer_book_op_bootstrap_remove;
  request->endpoint = strdup(endpoint);
  if (request->endpoint == NULL) {
    _peer_book_mutation_request_destroy(request);
    return -1;
  }
  return _peer_book_mutation_round_trip(peer_book, request, timeout_ms);
}

int peer_book_snapshot_bootstrap(peer_book_t* peer_book,
                                 char*** config_endpoints, size_t* config_count,
                                 char*** managed_endpoints, size_t* managed_count,
                                 uint32_t timeout_ms) {
  if (peer_book == NULL || config_endpoints == NULL || config_count == NULL ||
      managed_endpoints == NULL || managed_count == NULL) {
    return -1;
  }
  *config_endpoints = NULL;
  *config_count = 0;
  *managed_endpoints = NULL;
  *managed_count = 0;

  peer_book_snapshot_t* request = get_clear_memory(sizeof(peer_book_snapshot_t));
  if (request == NULL) return -1;
  request->reply.mutex = platform_mutex_create();
  request->reply.cv = platform_condvar_create();
  if (request->reply.mutex == NULL || request->reply.cv == NULL) {
    _peer_book_snapshot_free(request);
    return -1;
  }
  request->kind = peer_book_snapshot_bootstrap;

  int trip = _peer_book_round_trip(peer_book, PEER_BOOK_SNAPSHOT,
                                   &request->reply, timeout_ms);
  if (trip == 1) {
    _peer_book_snapshot_free(request);
    return -1;
  }
  if (trip != 0) return -1;  /* orphaned to the actor; nothing to free */

  *config_endpoints = request->config_endpoints;
  *config_count = request->config_count;
  *managed_endpoints = request->managed_endpoints;
  *managed_count = request->managed_count;
  platform_mutex_destroy(request->reply.mutex);
  platform_condvar_destroy(request->reply.cv);
  free(request);
  return 0;
}

int peer_book_snapshot_friends(peer_book_t* peer_book,
                               peer_info_t*** friends, size_t* friend_count,
                               uint32_t timeout_ms) {
  if (peer_book == NULL || friends == NULL || friend_count == NULL) return -1;
  *friends = NULL;
  *friend_count = 0;

  peer_book_snapshot_t* request = get_clear_memory(sizeof(peer_book_snapshot_t));
  if (request == NULL) return -1;
  request->reply.mutex = platform_mutex_create();
  request->reply.cv = platform_condvar_create();
  if (request->reply.mutex == NULL || request->reply.cv == NULL) {
    _peer_book_snapshot_free(request);
    return -1;
  }
  request->kind = peer_book_snapshot_kind_friends;

  int trip = _peer_book_round_trip(peer_book, PEER_BOOK_SNAPSHOT,
                                   &request->reply, timeout_ms);
  if (trip == 1) {
    _peer_book_snapshot_free(request);
    return -1;
  }
  if (trip != 0) return -1;  /* orphaned to the actor; do not free */

  *friends = request->friends;
  *friend_count = request->friend_count;
  platform_mutex_destroy(request->reply.mutex);
  platform_condvar_destroy(request->reply.cv);
  free(request);
  return 0;
}