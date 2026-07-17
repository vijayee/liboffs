//
// Created by victor on 5/14/25.
//

#ifndef OFFS_NETWORK_H
#define OFFS_NETWORK_H

#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Timer/timer_actor.h"
#include "authority.h"
#include "gossip.h"
#include "ring_set.h"
#include "latency_cache.h"
#include "eabf.h"
#include "hebbian.h"
#include "hebbian_config.h"
#include "connection_manager.h"
#include "rate_limit.h"
#include "node_id.h"
#include "wanted_list.h"
#include "conn_state.h"
#include "message_log.h"
#include "../Configuration/config.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

typedef struct block_cache_t block_cache_t;
typedef struct relay_client_t relay_client_t;
typedef struct nat_detect_t nat_detect_t;
typedef struct quic_listener_t quic_listener_t;
typedef struct mdns_t mdns_t;

#define CLOSEST_NODES_PENDING_MAX 32

typedef struct closest_nodes_pending_t {
  uint64_t message_id;
  actor_t* reply_to;
  uint64_t deadline_ms;   /* 0 = no deadline (back-compat; never swept) */
} closest_nodes_pending_t;

// Pending StoreBlock aggregation. The origin records one entry per
// NETWORK_LOCAL_STORE_BLOCK with a non-NULL reply_to. Each StoreBlockResponse
// (accepted or declined) decrements replicas_remaining; when it hits 0 (or
// the deadline passes — swept by NETWORK_REQUEST_TIMEOUT_TICK), the origin
// sends a single aggregated NETWORK_STORE_BLOCK_RESULT to reply_to with the
// actual replica count. See audit #21.
typedef struct store_pending_t {
  uint64_t message_id;
  actor_t* reply_to;
  buffer_t* hash;          /* referenced; used for the result payload */
  uint32_t fib;
  uint8_t replicas_needed;
  uint8_t replicas_remaining;
  uint8_t accepted_count;
  uint64_t deadline_ms;    /* 0 = no deadline (back-compat; never swept) */
} store_pending_t;

// Pending QUIC connections awaiting salutation identity handshake
typedef struct pending_quic_t {
  void* quic_connection;           // HQUIC handle
  void* quic_stream;                // HQUIC persistent bidirectional stream handle
  struct sockaddr_storage peer_addr;
  uint8_t* peer_cert_der;          // peer's leaf cert (DER), extracted at CONNECTED; NULL if none
  size_t   peer_cert_der_len;
  struct pending_quic_t* next;
} pending_quic_t;

// Relay signed-nonce challenge (audit #8 relay; tier-5b). When a relayed
// message arrives from a peer admitted with relay_verified=false, the
// receiver records a fresh challenge (nonce + deadline) and sends
// WIRE_RELAY_CHALLENGE back via the relay. The responder signs the nonce
// and returns WIRE_RELAY_CHALLENGE_RESPONSE; the challenger verifies
// BLAKE3(public_key)==responder_id and the signature, then sets
// relay_verified=true. Unanswered challenges are swept by the
// NETWORK_REQUEST_TIMEOUT_TICK (reusing the tier-3 1s sweep).
typedef struct relay_challenge_t {
  node_id_t sender_id;         // the unverified relayed sender to challenge
  uint8_t  nonce[32];          // fresh CSPRNG nonce
  uint64_t deadline_ms;       // now_ms + network->request_timeout_ms
  uint32_t relay_endpoint_id; // route the challenge back via the relay
} relay_challenge_t;

typedef struct network_t {
  actor_t actor;
  authority_t* authority;
  block_cache_t* block_cache;
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  gossip_handle_t gossip;
  ring_set_t* rings;
  latency_cache_t* latency_cache;
  eabf_table_t eabf_table;
  eabf_ttl_table_t eabf_ttl;
  wanted_list_t* wanted_list;
  hebbian_table_t hebbian;
  rate_limit_table_t rate_limits;
  message_log_t* log;             /* Test-only message event log (NULL in release builds) */
  topology_metrics_t* topology_metrics;
  connection_manager_t conn_mgr;
  struct respiration_actor_t* respiration;
  ATOMIC(uint64_t) gossip_timer_id;
  ATOMIC(uint64_t) eabf_maintenance_timer_id;
  uint64_t hebbian_decay_timer_id;
  ATOMIC(uint64_t) metrics_push_timer_id;
  ATOMIC(uint64_t) ping_capacity_timer_id;
  ATOMIC(uint64_t) request_timer_id;  /* 1s sweep tick for wanted_list +
                                         closest_pending expiry (#5/#6/#9) */
  uint32_t request_timeout_ms;        /* per-pending-request deadline; default 30s */
  ATOMIC(uint8_t) running;
  uint32_t gossip_init_interval_s;
  size_t gossip_init_count;
  uint32_t gossip_steady_interval_s;
  uint32_t gossip_timeout_ms;
  float hebbian_decay_factor;
  uint32_t eabf_base_ttl_ms;
  uint32_t eabf_maintenance_ms;
  uint32_t respiration_tau_min_ms;
  uint32_t respiration_tau_max_ms;
  size_t relay_max_retries;
  uint32_t relay_retry_delay_ms;

  relay_client_t* relay;          /* Connected relay client (or NULL) */
  nat_detect_t* nat_detect;       /* NAT detection module */
  nat_type_e local_nat_type;      /* Detected NAT type */
  mdns_t* mdns;                    /* mDNS responder (NULL if not started) */

  pending_quic_t* pending_connections;  /* QUIC connections awaiting salutation */

  // Friend peer reconnect state
  ATOMIC(uint64_t) friend_reconnect_timer_id;

  ATOMIC(uint64_t) next_message_id;  /* monotonic per-node counter; avoids the
                                         time(NULL)*1000 second-granularity
                                         collisions that cross-delivered
                                         closest_pending entries. See audit #6. */

  closest_nodes_pending_t closest_pending[CLOSEST_NODES_PENDING_MAX];
  size_t closest_pending_count;

  // Pending StoreBlock aggregation (audit #21). Grown on demand; swept by
  // network_handle_request_timeout_tick. The array is a flat array of
  // store_pending_t (one per still-unresolved origin StoreBlock).
  store_pending_t* store_pending;
  size_t           store_pending_count;
  size_t           store_pending_capacity;

  // Pending relay signed-nonce challenges (audit #8 relay; tier-5b). Grown
  // on demand; swept by network_handle_request_timeout_tick. The array is
  // a flat array of relay_challenge_t (no per-entry allocations), so add and
  // remove are memmove/swap-with-last; the sweep just compacts in place.
  relay_challenge_t* relay_challenges;
  size_t            relay_challenge_count;
  size_t            relay_challenge_capacity;

  quic_listener_t* quic_listener;      /* QUIC listener for direct P2P connections */

#ifdef HAS_MSQUIC
  const struct QUIC_API_TABLE* msquic;
#endif
} network_t;

/* Payload for NETWORK_SHUTDOWN_CONNECTIONS. The network actor does the
   ConnectionShutdown loop and sets done_flag when done; the main thread polls
   done_flag (10ms sleep, 5s cap) — consistent with the F6/F7 quiesce pattern.
   The main thread allocates and frees the payload; the network actor only
   sets the flag — it must NOT free it. See concurrency-pass.md F3. */
typedef struct {
  ATOMIC(bool) done_flag;
} network_shutdown_payload_t;

network_t* network_create(authority_t* authority, block_cache_t* block_cache,
                          timer_actor_t* timer, scheduler_pool_t* pool,
                          const config_t* config);
void network_destroy(network_t* network);
void network_dispatch(void* state, message_t* msg);
int network_connect_relay(network_t* network, const char* host, uint16_t port);
int network_connect_peer(network_t* network, const char* host, uint16_t port);

/* Start NAT type detection by connecting two relay clients (relay_a and
   relay_b) and comparing their reflexive addresses. The detected NAT type
   is delivered to the network actor via NETWORK_NAT_TYPE_DETECTED, which
   updates network->local_nat_type and re-evaluates all existing peers'
   conn_state. If this is never called, local_nat_type stays at the
   conservative default NAT_TYPE_PORT_RESTRICTED_CONE (try direct, fall back
   to relay). See audit #18. Returns 0 on success, -1 on error. */
int network_start_nat_detect(network_t* network,
                              const char* relay_a_host, uint16_t relay_a_port,
                              const char* relay_b_host, uint16_t relay_b_port);

/* Start the mDNS responder for same-LAN auto-discovery. Idempotent —
   calling start on an already-running responder is a no-op. POSIX-only;
   stubbed on Windows. See audit #18. */
int network_start_mdns(network_t* network);

/* Stop the mDNS responder. Idempotent. */
void network_stop_mdns(network_t* network);

/* Try a peer_info's candidate addresses in priority order (HOST -> SRFLX ->
   DIRECT -> RELAY) and connect via the first that works. RELAY candidates
   are admitted via the connection manager with relay_endpoint_id set (no
   QUIC connection). is_friend controls friend pinning on admission.
   Returns 0 if any candidate connected, -1 otherwise. See audit #18. */
struct peer_address_t;  /* forward decl — peer_info.h */
int network_connect_peer_candidates(network_t* network, const node_id_t* remote_id,
                                    const struct peer_address_t* addresses,
                                    size_t address_count, bool is_friend);

void network_shutdown_connections(network_t* network);

// Start connections to bootstrap and friend peers. Called after node start.
void network_start_connections(network_t* network);

#ifndef NDEBUG
/* Test-only accessor for the per-node monotonic message ID counter. Used to
   verify the counter yields unique IDs across rapid calls (audit #6). */
uint64_t network_next_message_id_for_test(network_t* network);

/* Test-only accessors for the relay challenge table (audit #8 relay;
   tier-5b). These wrap the static table helpers so the unit tests can
   exercise add/find/remove/sweep without a full network + relay fixture.
   The nonce parameter is 32 bytes. */
int network_relay_challenge_find_for_test(network_t* network,
                                          const node_id_t* sender_id);
int network_relay_challenge_append_for_test(network_t* network,
                                            const node_id_t* sender_id,
                                            const uint8_t nonce[32],
                                            uint64_t deadline_ms,
                                            uint32_t relay_endpoint_id);
void network_relay_challenge_remove_for_test(network_t* network,
                                             const node_id_t* sender_id);
size_t network_relay_challenge_count_for_test(network_t* network);
void network_relay_challenge_sweep_for_test(network_t* network,
                                            uint64_t now_ms);
/* Copy the challenge at the given index into *out. Returns 0 on success,
   -1 if the index is out of range. */
int network_relay_challenge_get_for_test(network_t* network, size_t index,
                                          relay_challenge_t* out);
#endif

#endif // OFFS_NETWORK_H
