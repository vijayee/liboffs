//
// Created by victor on 5/20/26.
//
#ifndef OFFS_WT_TRANSPORT_H
#define OFFS_WT_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "../../Platform/platform.h"

#ifdef HAS_MSQUIC
#include <msquic.h>
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../BlockCache/block_cache.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../Network/msquic_singleton.h"
#include <poll-dancer/poll-dancer.h>
#include "wt_connection.h"
#include "../health_handler.h"
#include <stdbool.h>

typedef vec_t(wt_connection_t*) vec_wt_connection_t;

typedef struct wt_transport_destroy_node_t {
  void* item;
  struct wt_transport_destroy_node_t* next;
} wt_transport_destroy_node_t;

typedef struct wt_transport_t {
  actor_t actor;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  ATOMIC(uint8_t) running;
  ATOMIC(uint8_t) listening;
  platform_thread_t* thread;
  pd_loop_t* loop;
  platform_mutex_t* destroy_lock;
  wt_transport_destroy_node_t* destroy_head;

  const struct QUIC_API_TABLE* msquic;
  HQUIC registration;
  HQUIC configuration;
  HQUIC listener;
  char* host;
  uint16_t port;
  char* cert_path;
  char* key_path;
  /* CA certificate for validating client certs. NULL when no CA is
   * configured. If allow_secure is false, peer cert validation is disabled
   * with a logged info message. If allow_secure is true, wt_transport_start
   * fails closed. Holds a peer_verify_ctx_t* (opaque void* here so the header
   * does not pull in peer_verify.h). See audit #11. */
  void* peer_verify;
  /* When true, a CA must be configured and client certificates are
   * validated. When false (default), no CA is required and TLS proceeds
   * with NO_CERTIFICATE_VALIDATION and a logged info message
   * (trusted-LAN/research opt-in). See audit #11. */
  bool allow_secure;
  /* Windows/Schannel only: the msquic Schannel backend cannot load a server
   * cert from PEM files, so wt_transport_create imports the PEM pair into a
   * transient cert store and hands the resulting PCCERT_CONTEXT to msquic.
   * PFXImportCertStore persists the private key into a CNG key container
   * (Schannel's TLS 1.3 requirement); the store, the context, and the key
   * container are kept alive for the transport's lifetime (Schannel holds the
   * context for as long as the credential handle is open) and released /
   * deleted in wt_transport_destroy after ConfigurationClose. Stored as
   * opaque void* so the header does not pull in wincrypt.h. NULL on POSIX
   * and on the no-cert (insecure) path. */
  void* win_cert_store;
  void* win_cert_context;
  size_t max_connections;
  ATOMIC(size_t) active_connections;

  platform_mutex_t* conn_lock;
  vec_wt_connection_t connections;
  char* api_key_hash;
  health_context_t* health_ctx;
} wt_transport_t;

wt_transport_t* wt_transport_create(scheduler_pool_t* pool,
                                      block_cache_t* bc,
                                      ofd_cache_t* ofd_cache,
                                      tuple_cache_t* tc,
                                      const char* host,
                                      uint16_t port,
                                      const char* cert_path,
                                      const char* key_path,
                                      const char* ca_path,
                                      bool allow_secure,
                                      size_t max_connections,
                                      const char* api_key_hash,
                                      health_context_t* health_ctx);
void wt_transport_destroy(wt_transport_t* transport);
void wt_transport_start(wt_transport_t* transport);
void wt_transport_stop(wt_transport_t* transport);

#endif /* HAS_MSQUIC */
#endif /* OFFS_WT_TRANSPORT_H */