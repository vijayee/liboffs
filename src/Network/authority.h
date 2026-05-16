//
// Created by victor on 5/14/25.
//

#ifndef OFFS_AUTHORITY_H
#define OFFS_AUTHORITY_H

#include <stdint.h>
#include <stddef.h>
#include "../Configuration/config.h"
#include "../Util/atomic_compat.h"
#include "node_id.h"

typedef struct topology_metrics_t topology_metrics_t;

typedef enum node_phase_e {
  NODE_PHASE_INHALE = 0,
  NODE_PHASE_NEUTRAL = 1,
  NODE_PHASE_EXHALE = 2
} node_phase_e;

typedef struct authority_t {
  config_t* config;

  char** bootstrap_peers;
  size_t bootstrap_peer_count;

  char* peer_store_path;
  char** persisted_peers;
  size_t persisted_peer_count;

  char* ca_cert_path;
  char* node_cert_path;
  char* node_key_path;

  char* relay_url;
  size_t max_peers;
  size_t max_inflight;

  node_id_t local_id;

  ATOMIC(float) capacity;
  ATOMIC(node_phase_e) phase;

  topology_metrics_t* metrics_server;  // NULL if not configured
} authority_t;

authority_t* authority_create(config_t* config);
void authority_destroy(authority_t* authority);

// Generate local_id from certificate public key (or random if no cert)
int authority_init_local_id(authority_t* authority);

// Save/load config-only state (bootstrap peers, paths, local_id)
int authority_save(const authority_t* authority);
int authority_load(authority_t* authority);

// Save/load runtime peer state (Hebbian weights, ring nodes, latency data)
// network_t is forward-declared to avoid circular includes
typedef struct network_t network_t;
int authority_save_peers(const authority_t* authority, const network_t* network);
int authority_load_peers(authority_t* authority, network_t* network);

void authority_update_capacity(authority_t* authority, float capacity);

#endif // OFFS_AUTHORITY_H