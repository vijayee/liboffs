//
// Created by victor on 5/16/26.
//

#ifndef OFFS_RELAY_CLIENT_H
#define OFFS_RELAY_CLIENT_H

#include "../Actor/actor.h"
#include "../Network/network.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "../Platform/platform.h"
#include "../Network/stream_framer.h"
#include "../Network/wire.h"
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

typedef struct relay_client_destroy_node_t {
  pd_watcher_t* watcher;
  struct relay_client_destroy_node_t* next;
} relay_client_destroy_node_t;

typedef struct relay_client_t {
  actor_t actor;
  network_t* network;
  scheduler_pool_t* pool;

#ifdef HAS_MSQUIC
  const struct QUIC_API_TABLE* msquic;
  HQUIC registration;
  HQUIC configuration;
  HQUIC connection;
  HQUIC stream;
  uint8_t owns_registration;  // 1 if we created the registration, 0 if shared
#else
  void* msquic;
  void* registration;
  void* configuration;
  void* connection;
  void* stream;
  uint8_t owns_registration;
#endif

  stream_framer_t* framer;
  uint32_t local_endpoint_id;
  uint32_t reflexive_addr;   /* Server-reflexive IP (host byte order, from ADDR_RESPONSE). 0 if unknown. */
  uint16_t reflexive_port;   /* Server-reflexive port (host byte order, from ADDR_RESPONSE). 0 if unknown. */
  wire_addr_t reflexive6;    /* Server-reflexive v6 address (from ADDR_RESPONSE). NONE only before any ADDR_RESPONSE arrives; after decode the wire decoder always fills it (V4 fallback for old-format senders), so it is V4 or V6. */
  ATOMIC(uint8_t) connected;

  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;

  platform_mutex_t* destroy_lock;
  relay_client_destroy_node_t* destroy_head;

  struct sockaddr_storage relay_addr;
  char* cert_path;
  char* key_path;

  // Retry state: when the QUIC connection fails with UNREACHABLE,
  // we automatically retry up to max_retries times at the base delay, then
  // keep retrying long-haul with exponential backoff (RELAY_BACKOFF_*)
  // until the relay answers; success resets both counters.
  char* relay_host;
  uint16_t relay_port;
  uint8_t retry_count;
  uint8_t shutdown_pending;  // 1 if we're intentionally shutting down
  uint8_t max_retries;
  uint32_t retry_delay_ms;
  uint8_t backoff_attempt;   // consecutive failed connects (capped, for the backoff shift)
#ifdef HAS_MSQUIC
  HQUIC shared_registration;  // shared registration from quic_listener (may be NULL)
  void* peer_verify;  // peer_verify_ctx_t* — NULL if no CA cert loaded
  void* win_cert_store;    // HCERTSTORE from the Windows PKCS12 import, else NULL
  void* win_cert_context;  // PCCERT_CONTEXT handed to msquic, else NULL
#else
  void* shared_registration;
  void* peer_verify;
#endif
} relay_client_t;

relay_client_t* relay_client_create(network_t* network, scheduler_pool_t* pool,
                                    uint8_t max_retries, uint32_t retry_delay_ms);
void relay_client_destroy(relay_client_t* client);
int relay_client_connect(relay_client_t* client, const char* host, uint16_t port,
#ifdef HAS_MSQUIC
                          HQUIC shared_registration
#else
                          void* shared_registration
#endif
                          );
void relay_client_disconnect(relay_client_t* client);
void relay_client_dispatch(void* state, message_t* msg);

// Message types for relay_client actor
#define RELAY_CLIENT_ADDR_REQUEST  1
#define RELAY_CLIENT_SEND          2
#define RELAY_CLIENT_RETRY         3

// Long-haul backoff for a relay that stays down past the initial retry
// budget: the delay keeps doubling up to RELAY_BACKOFF_MAX_MS instead of
// hammering a dead relay at the base interval (which flooded the daemon log
// with one retry cycle every ~18s). Cap the shift so the delay can never
// overflow the unsigned long arithmetic.
#define RELAY_BACKOFF_MAX_ATTEMPT 12
#define RELAY_BACKOFF_MAX_MS 60000ul

typedef struct relay_retry_payload_t {
  unsigned long delay_ms;
} relay_retry_payload_t;

#endif // OFFS_RELAY_CLIENT_H