//
// Created by victor on 5/14/25.
//

#ifndef OFFS_AUTHORITY_H
#define OFFS_AUTHORITY_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/evp.h>
#include "../Configuration/config.h"
#include "../Util/atomic_compat.h"
#include "node_id.h"

#include "peer_info.h"

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

  uint8_t* ca_cert_data;   // DER-encoded CA certificate (NULL if none)
  size_t   ca_cert_len;    // Length of ca_cert_data
  char* node_cert_path;
  char* node_key_path;

  /* If true, allow NO_CERTIFICATE_VALIDATION when no CA is configured
   * (trusted-LAN/research; logs a warning). Default false — fail closed
   * without a CA. See audit #11. */
  bool allow_insecure;

  char* relay_url;
  size_t max_peers;
  size_t max_inflight;

  node_id_t local_id;

  uint8_t* public_key;      // Raw public key for salutation (NULL if random node_id)
  size_t   public_key_len;  // Length of public_key

  EVP_PKEY* node_private_key;  // cached private key (loaded once from node_key_path);
                                // NULL if no key path or load failed. See audit #8.

  peer_info_t** friend_peers;
  size_t friend_peer_count;

  ATOMIC(float) capacity;
  ATOMIC(node_phase_e) phase;

  char* metrics_server_url;            // http://host:port/report, NULL if disabled
} authority_t;

authority_t* authority_create(config_t* config);
void authority_destroy(authority_t* authority);

// Generate local_id from certificate public key (or random if no cert)
int authority_init_local_id(authority_t* authority);

// Load a PEM-encoded CA certificate, convert to DER, store in authority.
// Returns 0 on success, -1 on failure.
int authority_load_ca_cert(authority_t* authority, const char* pem_path);

// Sign a 32-byte nonce with the authority's cached node_private_key. Returns
// 0 on success and writes a freshly-allocated signature to *out_sig (caller
// frees with free()). Sets *out_sig_len. Returns -1 on failure. Used by the
// relay responder (audit #8 / tier5b) to sign challenges. See audit #8.
int authority_sign_nonce(authority_t* authority, const uint8_t nonce[32],
                         uint8_t** out_sig, size_t* out_sig_len);

// Save/load config-only state (bootstrap peers, paths, local_id)
int authority_save(const authority_t* authority);
int authority_load(authority_t* authority);

// Save/load runtime peer state (Hebbian weights, ring nodes, latency data)
// network_t is forward-declared to avoid circular includes
typedef struct network_t network_t;
int authority_save_peers(const authority_t* authority, const network_t* network);
int authority_load_peers(authority_t* authority, network_t* network);

void authority_update_capacity(authority_t* authority, float capacity);
void authority_update_phase(authority_t* authority, float capacity);

#endif // OFFS_AUTHORITY_H