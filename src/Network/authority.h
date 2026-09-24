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

  char** managed_bootstrap_peers;
  size_t managed_bootstrap_peer_count;

  char* peer_store_path;
  char** persisted_peers;
  size_t persisted_peer_count;

  uint8_t* ca_cert_data;   // DER-encoded CA certificate (NULL if none)
  size_t   ca_cert_len;    // Length of ca_cert_data
  char* node_cert_path;
  char* node_key_path;

  /* If true, require a configured CA and validate peer certificates.
   * Default false — run without CA validation; connections are still
   * encrypted. See audit #11 / 2026-07-21 allow_secure rename. */
  bool allow_secure;

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

/* Save the runtime peer state reading the friend (index 5) and managed
 * bootstrap (index 6) lists directly off authority_t. Legal only in the
 * startup/shutdown phases of the peer_book.h invariant (before
 * peer_book_start / after peer_book_stop or pool stop); at runtime the
 * network actor uses authority_save_peers_snapshot with the lists fetched
 * from the peer-book actor. */
int authority_save_peers(const authority_t* authority, const network_t* network);

/* Same save, but the friend and managed bootstrap lists are supplied by the
 * caller (the peer-book actor's PEER_BOOK_SAVE_SNAPSHOT payload): b58_friends
 * holds the Base58 peer_info strings for index 5, managed_bootstrap the
 * endpoint strings for index 6. Both arrays are borrowed (read-only here;
 * the char**-not-const-char** signature keeps the C qualifier rules happy
 * for both the actor's owned char** arrays and this file's local arrays). */
int authority_save_peers_snapshot(const authority_t* authority,
                                  const network_t* network,
                                  char** b58_friends,
                                  size_t b58_friend_count,
                                  char** managed_bootstrap,
                                  size_t managed_count);

int authority_load_peers(authority_t* authority, network_t* network);

/* Bootstrap peers. bootstrap_peers is config-seeded and immutable at runtime;
 * managed_bootstrap_peers is operator-added and persisted in peer-store
 * index 6. Endpoints are "host:port" or "[ipv6]:port" strings.
 *
 * STARTUP-PHASE-ONLY: once the peer-book actor is running (peer_book_start,
 * see src/Network/peer_book.h) these helpers and direct access to the
 * friend_peers / bootstrap_peers / managed_bootstrap_peers arrays are legal
 * only on the peer-book actor thread — runtime mutations and snapshots go
 * through peer_book_* messages. The helpers below remain the single
 * implementation of the add/remove/seed contract and are invoked by the
 * peer-book actor's dispatch.
 *
 * authority_bootstrap_add returns 0 = added, -1 = invalid endpoint,
 * -2 = duplicate (in either list).
 * authority_bootstrap_remove returns 0 = removed, -1 = not found,
 * -2 = config-source entry (conflict). */
int authority_bootstrap_add(authority_t* authority, const char* endpoint);
int authority_bootstrap_remove(authority_t* authority, const char* endpoint);
int authority_set_bootstrap_peers(authority_t* authority, const char* csv);

void authority_update_capacity(authority_t* authority, float capacity);
void authority_update_phase(authority_t* authority, float capacity);

#endif // OFFS_AUTHORITY_H