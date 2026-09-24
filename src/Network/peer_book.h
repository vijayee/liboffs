//
// Created by victor on 9/24/26.
//

#ifndef OFFS_PEER_BOOK_H
#define OFFS_PEER_BOOK_H

#include "../Actor/actor.h"
#include "../Timer/timer_actor.h"
#include "../Scheduler/scheduler.h"
#include "../Platform/platform_thread.h"
#include "../Util/atomic_compat.h"
#include "authority.h"
#include "peer_info.h"
#include <stdint.h>
#include <stddef.h>

/* Peer-book actor — the single serialization point for the peer lists.

   INVARIANT: after peer_book_start(), every access to
   authority->friend_peers / bootstrap_peers / managed_bootstrap_peers from a
   thread other than the peer-book actor must go through the messages this
   module sends on the caller's behalf (peer_book_friend_*,
   peer_book_bootstrap_*, peer_book_snapshot_*). Direct list access is legal
   only:
     (a) before peer_book_start() — startup config seeding
         (authority_set_bootstrap_peers in offsd _startup and
         offs_node_restart Phase 3) and peer-store loading
         (authority_load_peers);
     (b) after peer_book_stop() — the operator save in offsd's _shutdown (the
         reconnect tick is no longer armed, so the actor is quiet);
     (c) after the scheduler pool has been stopped (offs_node_stop Phase 8
         final authority_save_peers, offs_node_restart Phase 3 re-seed) — no
         worker can run the actor, so the lists are quiescent.
   The storage physically stays in authority_t so the authority_save_peers /
   peer_store plumbing is unchanged; its location is an implementation detail
   — the peer-book actor is the synchronization point. The authority_* list
   helpers (authority_bootstrap_add, ...) are now startup-phase-only. */

typedef struct network_t network_t;

/* Mutation operations carried by PEER_BOOK_MUTATION. Result codes follow the
   established authority contract: 0 ok, -1 invalid endpoint / OOM /
   not-found, -2 duplicate (add ops) or config-source conflict (bootstrap
   remove). */
typedef enum peer_book_op_e {
  peer_book_op_friend_add = 0,
  peer_book_op_friend_remove,
  peer_book_op_bootstrap_add,
  peer_book_op_bootstrap_remove
} peer_book_op_e;

/* Round-trip timeout for the bounded synchronous helpers below. The
   peer-book actor does microseconds of work per message, so this budget is
   only consumed under scheduler starvation; on timeout the request is
   orphaned to the actor (see the ownership note on peer_book_mutation_t). */
#define PEER_BOOK_TIMEOUT_MS 2000u

typedef enum peer_book_snapshot_kind_e {
  peer_book_snapshot_kind_friends = 0,   /* deep-copied peer_info_t list */
  peer_book_snapshot_kind_bootstrap      /* endpoint strings, config + managed */
} peer_book_snapshot_kind_e;

/* Round-trip reply plumbing, embedded as the FIRST member of both request
   payloads so the dispatcher can address either type through it. The mutex
   and condvar are request-owned: they are created by the caller-side helper
   and destroyed by whoever frees the request shell (the caller on success,
   the actor on orphan). */
typedef struct peer_book_reply_t {
  platform_mutex_t* mutex;
  platform_condvar_t* cv;
  int done;
  int orphaned;
} peer_book_reply_t;

/* Payload for PEER_BOOK_MUTATION. The caller allocates the struct (heap),
   fills the op inputs as owned heap copies, sends it via the internal
   round-trip helper, and waits on reply.done. Ownership of the request shell
   is decided under the reply mutex:
     - success (done observed under the lock): the caller frees the request
       itself — the actor stopped touching it after the unlock that followed
       the signal;
     - timeout: the caller sets orphaned = 1 under the lock and frees
       NOTHING — ownership of the whole request (shell + input copies)
       transfers to the actor, which frees them after applying the op.
   Input copies: the caller deep-copies its decode-side structures into the
   request (peer_info copy, strdup'd endpoint) so a timeout can never leave
   the actor reading the caller's stack. The actor destroys the input copies
   after apply (into the list on success, or outright on duplicate/invalid). */
typedef struct peer_book_mutation_t {
  peer_book_reply_t reply;    /* must stay first (see peer_book_reply_t) */
  peer_book_op_e op;
  peer_info_t* friend_info;   /* owned deep copy (friend_add); NULL otherwise */
  node_id_t target_id;        /* value copy (friend_remove) */
  char* endpoint;             /* owned heap copy (bootstrap add/remove) */
  int result;                 /* actor fills: 0 ok / -1 invalid-or-oom /
                                 -2 duplicate-or-not-found, per op */
} peer_book_mutation_t;

/* Payload for PEER_BOOK_SNAPSHOT. Reply fields are actor-built copies; on
   success the caller steals them (peer_info copies: peer_info_destroy +
   free; endpoint strings: free) and frees the request shell. On timeout the
   caller sets reply.orphaned = 1 and the actor frees everything. */
typedef struct peer_book_snapshot_t {
  peer_book_reply_t reply;    /* must stay first (see peer_book_reply_t) */
  peer_book_snapshot_kind_e kind;
  /* reply: peer_book_snapshot_friends */
  peer_info_t** friends;
  size_t friend_count;
  /* reply: peer_book_snapshot_bootstrap */
  char** config_endpoints;
  size_t config_count;
  char** managed_endpoints;
  size_t managed_count;
} peer_book_snapshot_t;

/* Payload for PEER_BOOK_RECONNECT (peer-book actor -> network actor,
   fire-and-forget). Carries the peer-book-owned copies of the three lists at
   tick time so the network actor can run the partition-heal + friend
   reconnect logic on its own thread without touching authority state.
   Freed by peer_book_reconnect_payload_destroy. */
typedef struct peer_book_reconnect_payload_t {
  peer_info_t** friends;
  size_t friend_count;
  char** config_endpoints;
  size_t config_count;
  char** managed_endpoints;
  size_t managed_count;
} peer_book_reconnect_payload_t;

/* Payload for PEER_BOOK_SAVE_SNAPSHOT (peer-book actor -> network actor,
   fire-and-forget). friend strings are the Base58 encodings
   authority_save_peers_snapshot writes to peer-store index 5; managed
   entries are the raw endpoint strings for index 6. Freed by
   peer_book_save_snapshot_destroy. */
typedef struct peer_book_save_snapshot_t {
  char** b58_friends;
  size_t b58_friend_count;
  char** managed_endpoints;
  size_t managed_count;
} peer_book_save_snapshot_t;

typedef struct peer_book_t {
  actor_t actor;
  authority_t* authority;
  network_t* network;             /* borrowed; NULL in list-only fixtures */
  timer_actor_t* timer;           /* borrowed; NULL disables the reconnect tick */
  scheduler_pool_t* pool;
  ATOMIC(uint64_t) reconnect_timer_id;  /* armed by peer_book_start */
  ATOMIC(uint8_t) started;
} peer_book_t;

/* Create the actor. Does not start it: the node calls peer_book_start AFTER
   the startup-phase direct seeding/loading of the lists is complete. */
peer_book_t* peer_book_create(authority_t* authority, network_t* network,
                              timer_actor_t* timer, scheduler_pool_t* pool);

/* Arm the reconnect tick (when a timer was supplied) and admit external
   requests. Must be called after all startup-phase direct list access is
   complete. */
int peer_book_start(peer_book_t* peer_book);

/* Quiesce: external requests start failing and the reconnect tick stops
   firing. Non-blocking. */
void peer_book_stop(peer_book_t* peer_book);

/* Cancel the tick timer and free the actor. NULL-safe. */
void peer_book_destroy(peer_book_t* peer_book);

/* Dispatch entry (wired via actor_init). */
void peer_book_dispatch(void* state, message_t* msg);

/* Payload destructors (used by actor_send on the producing side). */
void peer_book_reconnect_payload_destroy(void* ptr);
void peer_book_save_snapshot_destroy(void* ptr);

/* Snapshot-array frees for the round-trip results the caller stole: destroy
   an owned peer_info_t* array (peer_book_snapshot_friends reply) or an owned
   heap-string array (peer_book_snapshot_bootstrap reply). NULL-safe. */
void peer_book_free_peer_info_array(peer_info_t** entries, size_t count);
void peer_book_free_string_array(char** entries, size_t count);

/* --- Bounded synchronous round-trips for callers on other threads ---
   All return the authority-contract result codes (0 ok / -1 invalid-or-oom /
   -2 duplicate-or-not-found per op) or -1 on timeout / stopped actor. On
   timeout a mutation may still have been applied by the actor later; the
   request was orphaned to it (see peer_book_mutation_t). */

/* friend add: info is deep-copied onto the request (the caller retains and
   still owns its own copy — safe to connect with after success). */
int peer_book_friend_add(peer_book_t* peer_book, const peer_info_t* info,
                         uint32_t timeout_ms);
/* friend remove by node id: 0 removed, -1 not found (or timeout / stopped). */
int peer_book_friend_remove(peer_book_t* peer_book, const node_id_t* target_id,
                            uint32_t timeout_ms);
/* bootstrap add/remove of a "host:port" / "[ipv6]:port" endpoint (copied). */
int peer_book_bootstrap_add(peer_book_t* peer_book, const char* endpoint,
                            uint32_t timeout_ms);
int peer_book_bootstrap_remove(peer_book_t* peer_book, const char* endpoint,
                               uint32_t timeout_ms);
/* Snapshot the bootstrap lists as heap endpoint string arrays. On success
   the caller frees *config_endpoints / *managed_endpoints (array + strings).
   On timeout nothing is returned (arrays stay with the actor). */
int peer_book_snapshot_bootstrap(peer_book_t* peer_book,
                                 char*** config_endpoints, size_t* config_count,
                                 char*** managed_endpoints, size_t* managed_count,
                                 uint32_t timeout_ms);
/* Snapshot the friend list as deep-copied peer_info_t entries. On success
   the caller destroys each entry (peer_info_destroy + free) and frees the
   array. */
int peer_book_snapshot_friends(peer_book_t* peer_book,
                               peer_info_t*** friends, size_t* friend_count,
                               uint32_t timeout_ms);

#endif // OFFS_PEER_BOOK_H