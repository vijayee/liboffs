//
// Created by victor on 5/14/25.
//

#include "authority.h"
#include "endpoint.h"
#include "network.h"
#include "hebbian.h"
#include "ring_set.h"
#include "net_node.h"
#include "pem_key.h"
#include "respiration.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/base58.h"
#include "../Platform/platform_atomic.h"
#include "peer_info.h"

peer_info_t* authority_copy_peer_info(const peer_info_t* source) {
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
      size_t host_len = strlen(address->host);
      char* host = get_clear_memory(host_len + 1);
      if (host == NULL) {
        peer_info_destroy(copy);
        free(copy);
        return NULL;
      }
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

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <cbor.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

// Peer store format version
#define PEER_STORE_VERSION 3

// --- Lifecycle ---

authority_t* authority_create(config_t* config) {
  if (config == NULL) return NULL;
  authority_t* authority = get_clear_memory(sizeof(authority_t));
  if (authority == NULL) return NULL;
  authority->config = config;
  authority->max_peers = 64;
  authority->max_inflight = 256;
  /* Default false: no CA validation. Set allow_secure=true to require a CA. */
  authority->allow_secure = config->allow_secure;
  return authority;
}

void authority_destroy(authority_t* authority) {
  if (authority == NULL) return;
  if (authority->peer_store_path != NULL) {
    free(authority->peer_store_path);
  }
  if (authority->bootstrap_peers != NULL) {
    for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
      peer_info_destroy(authority->bootstrap_peers[index].info);
      free(authority->bootstrap_peers[index].info);
    }
    free(authority->bootstrap_peers);
  }
  if (authority->managed_bootstrap_peers != NULL) {
    for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
      free(authority->managed_bootstrap_peers[index]);
    }
    free(authority->managed_bootstrap_peers);
  }
  if (authority->persisted_peers != NULL) {
    for (size_t index = 0; index < authority->persisted_peer_count; index++) {
      free(authority->persisted_peers[index]);
    }
    free(authority->persisted_peers);
  }
  if (authority->ca_cert_data != NULL) free(authority->ca_cert_data);
  if (authority->node_cert_path != NULL) free(authority->node_cert_path);
  if (authority->node_key_path != NULL) free(authority->node_key_path);
  if (authority->relay_url != NULL) free(authority->relay_url);
  if (authority->metrics_server_url != NULL) free(authority->metrics_server_url);
  if (authority->public_key != NULL) free(authority->public_key);
  if (authority->node_private_key != NULL) EVP_PKEY_free(authority->node_private_key);
  if (authority->friend_peers != NULL) {
    for (size_t index = 0; index < authority->friend_peer_count; index++) {
      peer_info_destroy(authority->friend_peers[index]);
      free(authority->friend_peers[index]);
    }
    free(authority->friend_peers);
  }
  free(authority);
}

// --- Local ID initialization ---

int authority_init_local_id(authority_t* authority) {
  if (authority == NULL) return -1;

  // If local_id is already set (e.g., from persistence), just regenerate .str
  if (!node_id_is_null(&authority->local_id)) {
    base58_encode(authority->local_id.hash, NODE_ID_HASH_SIZE,
                  authority->local_id.str, NODE_ID_STRING_SIZE);
    return 0;
  }

  // Derive node_id from certificate public key and cache the key
  if (authority->node_cert_path != NULL) {
    size_t key_len = 0;
    uint8_t* public_key = pem_extract_public_key(authority->node_cert_path, &key_len);
    if (public_key != NULL && key_len > 0) {
      int rc = node_id_from_public_key(public_key, key_len, &authority->local_id);
      if (rc == 0) {
        authority->public_key = public_key;
        authority->public_key_len = key_len;
        /* Also cache the private key (if a node_key_path is set) so the
         * relay responder can sign nonce challenges without re-reading
         * the PEM file on every signature. See audit #8. */
        if (authority->node_key_path != NULL && authority->node_private_key == NULL) {
          BIO* bio = BIO_new_file(authority->node_key_path, "r");
          if (bio != NULL) {
            EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
            BIO_free(bio);
            if (pkey != NULL) {
              authority->node_private_key = pkey;
            } else {
              log_error("authority_init_local_id: failed to load private key from: %s",
                        authority->node_key_path);
            }
          } else {
            log_error("authority_init_local_id: failed to open private key file: %s",
                      authority->node_key_path);
          }
        }
        return 0;
      }
      free(public_key);
    }
    log_error("authority_init_local_id: failed to derive node_id from cert, generating random");
  }

  // No cert or extraction failed — generate a random node_id
  authority->public_key = NULL;
  authority->public_key_len = 0;
  node_id_generate(&authority->local_id);
  return 0;
}

// --- Bootstrap peers (config-seeded + operator-managed) ---

/* True when the parsed endpoint is already present in either the config-seeded
 * or the operator-managed list. Entries are compared by parsed host+port, so
 * non-normalized (e.g. hand-seeded) entries compare stably. */
static int authority_bootstrap_contains(authority_t* authority, const char* endpoint) {
  char host[256];
  uint16_t port = 0;
  if (endpoint_parse(endpoint, host, sizeof(host), &port) != 0) return 1;

  for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
    const bootstrap_entry_t* entry = &authority->bootstrap_peers[index];
    if (entry->info == NULL || entry->info->address_count == 0 ||
        entry->info->addresses[0].host == NULL) continue;
    if (strcmp(entry->info->addresses[0].host, host) == 0 &&
        entry->info->addresses[0].port == port) {
      return 1;
    }
  }
  for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
    char managed_host[256];
    uint16_t managed_port = 0;
    if (endpoint_parse(authority->managed_bootstrap_peers[index], managed_host,
                       sizeof(managed_host), &managed_port) == 0 &&
        strcmp(managed_host, host) == 0 && managed_port == port) {
      return 1;
    }
  }
  return 0;
}

/* Re-encode a parsed endpoint so stored strings are normalized
 * ("host:port", or "[ipv6]:port" when the host contains ':'). The buffer
 * holds the worst case "[host]:65535" + NUL: brackets, colon, 5 digits. */
char* authority_bootstrap_encode(const char* host, uint16_t port) {
  size_t length = strlen(host) + 12;
  char* stored = get_memory(length);
  if (stored == NULL) return NULL;
  if (strchr(host, ':') != NULL) {
    snprintf(stored, length, "[%s]:%u", host, (unsigned)port);
  } else {
    snprintf(stored, length, "%s:%u", host, (unsigned)port);
  }
  return stored;
}

int authority_bootstrap_add(authority_t* authority, const char* endpoint) {
  if (authority == NULL || endpoint == NULL) return -1;
  char host[256];
  uint16_t port = 0;
  if (endpoint_parse(endpoint, host, sizeof(host), &port) != 0) return -1;
  if (authority_bootstrap_contains(authority, endpoint)) return -2;

  char* stored = authority_bootstrap_encode(host, port);
  if (stored == NULL) return -1;

  size_t new_count = authority->managed_bootstrap_peer_count + 1;
  char** expanded =
      realloc(authority->managed_bootstrap_peers, new_count * sizeof(char*));
  if (expanded == NULL) {
    free(stored);
    return -1;
  }
  authority->managed_bootstrap_peers = expanded;
  authority->managed_bootstrap_peers[authority->managed_bootstrap_peer_count] = stored;
  authority->managed_bootstrap_peer_count = new_count;
  return 0;
}

int authority_bootstrap_remove(authority_t* authority, const char* endpoint) {
  if (authority == NULL || endpoint == NULL) return -1;

  char host[256];
  uint16_t port = 0;
  if (endpoint_parse(endpoint, host, sizeof(host), &port) != 0) return -1;

  for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
    char config_host[256];
    uint16_t config_port = 0;
    const bootstrap_entry_t* entry = &authority->bootstrap_peers[index];
    if (entry->info != NULL && entry->info->address_count > 0 &&
        entry->info->addresses[0].host != NULL &&
        strcmp(entry->info->addresses[0].host, host) == 0 &&
        entry->info->addresses[0].port == port) {
      return -2;  // config-seeded entries are immutable at runtime
    }
  }

  for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
    char managed_host[256];
    uint16_t managed_port = 0;
    if (endpoint_parse(authority->managed_bootstrap_peers[index], managed_host,
                       sizeof(managed_host), &managed_port) == 0 &&
        strcmp(managed_host, host) == 0 && managed_port == port) {
      free(authority->managed_bootstrap_peers[index]);
      for (size_t shift = index; shift + 1 < authority->managed_bootstrap_peer_count; shift++) {
        authority->managed_bootstrap_peers[shift] =
            authority->managed_bootstrap_peers[shift + 1];
      }
      authority->managed_bootstrap_peer_count--;
      return 0;
    }
  }
  return -1;
}

int authority_set_bootstrap_peers(authority_t* authority, const char* csv) {
  if (authority == NULL) return -1;
  if (csv == NULL || csv[0] == '\0') {
    /* Empty seed clears the list unconditionally. */
    return authority_set_bootstrap_entries(authority, NULL, NULL, 0);
  }

  /* Pass 1: parse every token into a temporary peer_info entry — endpoint
     only (single HOST-candidate address at host:port), unpinned
     (trust-on-first-use); nothing is mutated until the whole CSV validates. */
  char* copy = strdup(csv);
  if (copy == NULL) return -1;
  peer_info_t** parsed = NULL;
  uint8_t* parsed_pinned = NULL;
  size_t parsed_count = 0;
  char* saveptr = NULL;
  for (char* token = strtok_r(copy, ",", &saveptr); token != NULL;
       token = strtok_r(NULL, ",", &saveptr)) {
    char host[256];
    uint16_t port = 0;
    if (endpoint_parse(token, host, sizeof(host), &port) != 0) {
      /* Not an endpoint — accept a base58 peer_info (the /peer/info
         interchange form) so a config can pin the bootstrap's full identity
         and candidate list. Pinned: the salutation must confirm node_id. */
      peer_info_t decoded;
      memset(&decoded, 0, sizeof(decoded));
      if (peer_info_from_base58(token, &decoded) != 0 ||
          decoded.node_id.hash == NULL) {
        peer_info_destroy(&decoded);
        free(copy);
        goto fail;
      }
      peer_info_t** expanded = realloc(parsed, (parsed_count + 1) * sizeof(peer_info_t*));
      if (expanded == NULL) { peer_info_destroy(&decoded); free(copy); goto fail; }
      parsed = expanded;
      uint8_t* expanded_pinned =
          realloc(parsed_pinned, (parsed_count + 1) * sizeof(uint8_t));
      if (expanded_pinned == NULL) { peer_info_destroy(&decoded); free(copy); goto fail; }
      parsed_pinned = expanded_pinned;
      peer_info_t* stored = get_clear_memory(sizeof(peer_info_t));
      if (stored == NULL) { peer_info_destroy(&decoded); free(copy); goto fail; }
      memcpy(stored, &decoded, sizeof(*stored));
      parsed[parsed_count] = stored;
      parsed_pinned[parsed_count] = 1;
      parsed_count++;
      continue;
    }
    bool duplicate = false;
    for (size_t prior = 0; prior < parsed_count && !duplicate; prior++) {
      char prior_host[256];
      uint16_t prior_port = 0;
      if (parsed[prior]->addresses != NULL && parsed[prior]->address_count > 0 &&
          parsed[prior]->addresses[0].host != NULL &&
          strcmp(parsed[prior]->addresses[0].host, host) == 0 &&
          parsed[prior]->addresses[0].port == port) {
        duplicate = true;
      }
    }
    if (duplicate) continue;

    peer_info_t* info = get_clear_memory(sizeof(peer_info_t));
    if (info == NULL) { free(copy); goto fail; }
    info->addresses = get_clear_memory(sizeof(peer_address_t));
    if (info->addresses == NULL) { peer_info_destroy(info); free(copy); goto fail; }
    info->addresses[0].type = PEER_ADDR_HOST;
    info->addresses[0].host = get_clear_memory(strlen(host) + 1);
    if (info->addresses[0].host == NULL) { peer_info_destroy(info); free(copy); goto fail; }
    memcpy(info->addresses[0].host, host, strlen(host));
    info->addresses[0].port = port;
    info->address_count = 1;

    peer_info_t** expanded =
        realloc(parsed, (parsed_count + 1) * sizeof(peer_info_t*));
    if (expanded == NULL) { peer_info_destroy(info); free(copy); goto fail; }
    parsed = expanded;
    uint8_t* expanded_pinned =
        realloc(parsed_pinned, (parsed_count + 1) * sizeof(uint8_t));
    if (expanded_pinned == NULL) { peer_info_destroy(info); free(copy); goto fail; }
    parsed_pinned = expanded_pinned;
    parsed[parsed_count] = info;
    parsed_pinned[parsed_count] = 0;
    parsed_count++;
  }
  free(copy);

  /* Pass 2: swap — free the old list only after full validation. */
  int rc = authority_set_bootstrap_entries(authority, parsed, parsed_pinned,
                                           parsed_count);
  for (size_t index = 0; index < parsed_count; index++) {
    peer_info_destroy(parsed[index]);
    free(parsed[index]);
  }
  free(parsed_pinned);
  free(parsed);
  if (rc != 0) {
    log_error("authority_set_bootstrap_peers: invalid bootstrap_peers CSV: %s", csv);
  }
  return rc;

fail:
  for (size_t index = 0; index < parsed_count; index++) {
    peer_info_destroy(parsed[index]);
    free(parsed[index]);
  }
  free(parsed_pinned);
  free(parsed);
  log_error("authority_set_bootstrap_peers: invalid bootstrap_peers CSV: %s", csv);
  return -1;
}

int authority_set_bootstrap_entries(authority_t* authority,
                                    peer_info_t* const* infos,
                                    const uint8_t* pinned,
                                    size_t count) {
  if (authority == NULL) return -1;
  if (count > 0 && (infos == NULL || pinned == NULL)) return -1;

  /* Pass 1: deep-copy every entry into a temporary array; nothing is
     mutated until the whole set validates. */
  peer_info_t** parsed = NULL;
  uint8_t* parsed_pinned = NULL;
  if (count > 0) {
    parsed = get_clear_memory(count * sizeof(peer_info_t*));
    parsed_pinned = get_clear_memory(count * sizeof(uint8_t));
    if (parsed == NULL || parsed_pinned == NULL) {
      free(parsed); free(parsed_pinned);
      return -1;
    }
  }
  for (size_t index = 0; index < count; index++) {
    if (infos[index] == NULL) {
      for (size_t prior = 0; prior < index; prior++) {
        peer_info_destroy(parsed[prior]);
        free(parsed[prior]);
      }
      free(parsed_pinned); free(parsed);
      log_error("authority_set_bootstrap_entries: NULL peer_info at %zu", index);
      return -1;
    }
    peer_info_t* copy = authority_copy_peer_info(infos[index]);
    if (copy == NULL) {
      for (size_t prior = 0; prior < index; prior++) {
        peer_info_destroy(parsed[prior]);
        free(parsed[prior]);
      }
      free(parsed_pinned); free(parsed);
      log_error("authority_set_bootstrap_entries: copy failed at %zu", index);
      return -1;
    }
    parsed[index] = copy;
    parsed_pinned[index] = pinned[index] ? 1 : 0;
  }

  /* Pass 2: swap — free the old list only after full validation. */
  for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
    peer_info_destroy(authority->bootstrap_peers[index].info);
    free(authority->bootstrap_peers[index].info);
  }
  free(authority->bootstrap_peers);
  authority->bootstrap_peers = NULL;
  authority->bootstrap_peer_count = 0;
  if (count > 0) {
    authority->bootstrap_peers = get_clear_memory(count * sizeof(bootstrap_entry_t));
    if (authority->bootstrap_peers == NULL) {
      for (size_t index = 0; index < count; index++) {
        peer_info_destroy(parsed[index]);
        free(parsed[index]);
      }
      free(parsed_pinned); free(parsed);
      return -1;
    }
    for (size_t index = 0; index < count; index++) {
      authority->bootstrap_peers[index].info = parsed[index];
      authority->bootstrap_peers[index].pinned = parsed_pinned[index];
      parsed[index] = NULL;
    }
    authority->bootstrap_peer_count = count;
  }
  free(parsed_pinned); free(parsed);
  return 0;
}

int authority_sign_nonce(authority_t* authority, const uint8_t nonce[32],
                         uint8_t** out_sig, size_t* out_sig_len) {
  if (authority == NULL || nonce == NULL || out_sig == NULL || out_sig_len == NULL) {
    return -1;
  }

  *out_sig = NULL;
  *out_sig_len = 0;

  /* Use the cached private key when available (the hot path — the responder
   * signs each challenge and a disk read per signature would be wasteful). */
  EVP_PKEY* pkey = authority->node_private_key;
  EVP_PKEY* loaded_pkey = NULL;
  if (pkey == NULL && authority->node_key_path != NULL) {
    /* Fallback: load on demand if authority_init_local_id did not run or the
     * cert path was not set. Cache the result so subsequent calls reuse it. */
    BIO* bio = BIO_new_file(authority->node_key_path, "r");
    if (bio == NULL) {
      log_error("authority_sign_nonce: failed to open key file: %s", authority->node_key_path);
      return -1;
    }
    loaded_pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (loaded_pkey == NULL) {
      log_error("authority_sign_nonce: failed to parse private key: %s", authority->node_key_path);
      return -1;
    }
    pkey = loaded_pkey;
    authority->node_private_key = loaded_pkey;  /* cache for next call */
  }
  if (pkey == NULL) {
    log_error("authority_sign_nonce: no private key available (no node_key_path and no cached key)");
    return -1;
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == NULL) {
    log_error("authority_sign_nonce: EVP_MD_CTX_new failed");
    return -1;
  }

  if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) != 1) {
    log_error("authority_sign_nonce: EVP_DigestSignInit failed");
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  size_t sig_len = 0;
  if (EVP_DigestSign(ctx, NULL, &sig_len, nonce, 32) != 1 || sig_len == 0) {
    log_error("authority_sign_nonce: EVP_DigestSign length probe failed");
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  uint8_t* sig = get_clear_memory(sig_len);
  if (sig == NULL) {
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  if (EVP_DigestSign(ctx, sig, &sig_len, nonce, 32) != 1) {
    log_error("authority_sign_nonce: EVP_DigestSign failed");
    free(sig);
    EVP_MD_CTX_free(ctx);
    return -1;
  }

  EVP_MD_CTX_free(ctx);
  *out_sig = sig;
  *out_sig_len = sig_len;
  return 0;
}

int authority_load_ca_cert(authority_t* authority, const char* pem_path) {
  if (authority == NULL || pem_path == NULL) return -1;

  BIO* bio = BIO_new_file(pem_path, "r");
  if (bio == NULL) {
    log_error("authority_load_ca_cert: failed to open PEM file: %s", pem_path);
    return -1;
  }

  X509* ca_cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (ca_cert == NULL) {
    log_error("authority_load_ca_cert: failed to parse PEM certificate");
    return -1;
  }

  int der_len = i2d_X509(ca_cert, NULL);
  if (der_len <= 0) {
    X509_free(ca_cert);
    log_error("authority_load_ca_cert: failed to get DER length");
    return -1;
  }

  uint8_t* der_data = get_memory((size_t)der_len);
  if (der_data == NULL) {
    X509_free(ca_cert);
    return -1;
  }

  uint8_t* cursor = der_data;
  int actual_len = i2d_X509(ca_cert, &cursor);
  X509_free(ca_cert);

  if (actual_len != der_len) {
    free(der_data);
    log_error("authority_load_ca_cert: DER encoding size mismatch");
    return -1;
  }

  if (authority->ca_cert_data != NULL) {
    free(authority->ca_cert_data);
  }
  authority->ca_cert_data = der_data;
  authority->ca_cert_len = (size_t)actual_len;
  return 0;
}

// --- Config-only save/load ---

int authority_save(const authority_t* authority) {
  if (authority == NULL || authority->peer_store_path == NULL) {
    return -1;
  }
  // Config-only save is a no-op for now — config comes from offs_config_t
  // The actual persistence is in authority_save_peers which includes runtime state
  return 0;
}

int authority_load(authority_t* authority) {
  if (authority == NULL || authority->peer_store_path == NULL) {
    return -1;
  }
  // Config-only load is a no-op — runtime state is loaded by authority_load_peers
  return 0;
}

// --- Runtime peer state persistence ---

// CBOR structure (v3 positional array; v2 = 8-field peer records, v3 = 12-field):
// [
//   uint8 (version = 3),               // index 0
//   bytes[32] (local_id),               // index 1
//   bytes (DER-encoded CA cert),        // index 2 — empty bytestring if none
//   [ [bytes[32], float], ... ],        // hebbian (index 3)
//   [ [bytes[32], uint32, uint16, float, float, float, uint8, float,
//       uint8, uint8, uint64, uint64], ... ]  // peers (index 4) — v3 12-field record
//   [ string, ... ]                     // friend peers as Base58 strings (index 5)
//   [ string, ... ]                     // managed bootstrap entries as "host:port" strings (index 6, v3 extension — optional)
// ]
// Peer record fields (v3): id, addr, port, latency_ms, weight, capacity, phase,
// availability, relay_verified, nat_type, last_seen_ms, bad_blocks_received.
// v2 files (8-field records) are still accepted by authority_load_peers.

int authority_save_peers(const authority_t* authority, const network_t* network) {
  if (authority == NULL || network == NULL) return -1;
  if (authority->peer_store_path == NULL) return -1;

  /* Startup/shutdown-phase save: read the friend and managed lists directly
     (legal per the peer_book.h invariant here) and delegate to the snapshot
     variant. */
  char** b58_friends = NULL;
  size_t b58_friend_count = 0;
  char** managed = NULL;
  size_t managed_count = 0;
  if (authority->friend_peer_count > 0) {
    b58_friends = get_clear_memory(authority->friend_peer_count * sizeof(char*));
    if (b58_friends != NULL) {
      for (size_t index = 0; index < authority->friend_peer_count; index++) {
        char* b58 = peer_info_to_base58(authority->friend_peers[index]);
        if (b58 == NULL) continue;
        b58_friends[b58_friend_count++] = b58;
      }
    }
  }
  managed = authority->managed_bootstrap_peers;
  managed_count = authority->managed_bootstrap_peer_count;

  int rc = authority_save_peers_snapshot(authority, network, b58_friends,
                                         b58_friend_count, managed,
                                         managed_count);

  if (b58_friends != NULL) {
    for (size_t index = 0; index < b58_friend_count; index++) {
      free(b58_friends[index]);
    }
    free(b58_friends);
  }
  return rc;
}

int authority_save_peers_snapshot(const authority_t* authority,
                                  const network_t* network,
                                  char** b58_friends,
                                  size_t b58_friend_count,
                                  char** managed_bootstrap,
                                  size_t managed_count) {
  if (authority == NULL || network == NULL) return -1;
  if (authority->peer_store_path == NULL) return -1;

  size_t hebbian_count = network->hebbian.count;
  size_t peer_count = (network->rings != NULL) ? ring_set_total_nodes(network->rings) : 0;

  cbor_item_t* root = cbor_new_definite_array(7);

  // Index 0: version
  {
    cbor_item_t* val = cbor_build_uint8(PEER_STORE_VERSION);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  }

  // Index 1: local_id
  {
    cbor_item_t* val = cbor_build_bytestring(authority->local_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  }

  // Index 2: ca_cert (DER bytes, or empty bytestring if none)
  if (authority->ca_cert_data != NULL && authority->ca_cert_len > 0) {
    cbor_item_t* val = cbor_build_bytestring(authority->ca_cert_data, authority->ca_cert_len);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  } else {
    cbor_item_t* val = cbor_build_bytestring(NULL, 0);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  }

  // Index 3: hebbian weights
  cbor_item_t* hebbian_array = cbor_new_definite_array(hebbian_count);
  for (size_t index = 0; index < hebbian_count; index++) {
    const hebbian_weight_t* entry = &network->hebbian.entries[index];
    cbor_item_t* pair = cbor_new_definite_array(2);
    cbor_item_t* id_item = cbor_build_bytestring(entry->peer_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_array_push(pair, id_item);
    cbor_decref(&id_item);
    cbor_item_t* weight_val = cbor_new_float8();
    cbor_set_float8(weight_val, (double)entry->weight);
    (void)cbor_array_push(pair, weight_val);
    cbor_decref(&weight_val);
    (void)cbor_array_push(hebbian_array, pair);
    cbor_decref(&pair);
  }
  (void)cbor_array_push(root, hebbian_array);
  cbor_decref(&hebbian_array);

  // Index 4: peers (v3 = 12-field records; rings may be NULL in a minimal
  // network — guard the loop so save is safe regardless).
  cbor_item_t* peers_array = cbor_new_definite_array(peer_count);
  if (network->rings != NULL) {
    for (size_t ring_idx = 0; ring_idx < network->rings->ring_count; ring_idx++) {
      ring_t* ring = &network->rings->rings[ring_idx];
      for (int node_idx = 0; node_idx < ring->primary.length; node_idx++) {
        net_node_t* node = ring->primary.data[node_idx];
        if (node == NULL) continue;
        cbor_item_t* peer = cbor_new_definite_array(12);
        cbor_item_t* id_bytes = cbor_build_bytestring(node->id.hash, NODE_ID_HASH_SIZE);
        (void)cbor_array_push(peer, id_bytes);
        cbor_decref(&id_bytes);
        cbor_item_t* addr_val = cbor_build_uint32(node->addr);
        (void)cbor_array_push(peer, addr_val);
        cbor_decref(&addr_val);
        cbor_item_t* port_val = cbor_build_uint16(node->port);
        (void)cbor_array_push(peer, port_val);
        cbor_decref(&port_val);
        cbor_item_t* lat = cbor_new_float4();
        cbor_set_float4(lat, node->latency_ms);
        (void)cbor_array_push(peer, lat);
        cbor_decref(&lat);
        cbor_item_t* wt = cbor_new_float4();
        cbor_set_float4(wt, node->weight);
        (void)cbor_array_push(peer, wt);
        cbor_decref(&wt);
        cbor_item_t* cap = cbor_new_float4();
        cbor_set_float4(cap, node->capacity);
        (void)cbor_array_push(peer, cap);
        cbor_decref(&cap);
        cbor_item_t* phase_val = cbor_build_uint8((uint8_t)node->phase);
        (void)cbor_array_push(peer, phase_val);
        cbor_decref(&phase_val);
        cbor_item_t* avail = cbor_new_float4();
        cbor_set_float4(avail, node->availability);
        (void)cbor_array_push(peer, avail);
        cbor_decref(&avail);
        // v3 fields 8-11: relay_verified, nat_type, last_seen_ms, bad_blocks_received
        cbor_item_t* relay_verified_val = cbor_build_uint8(node->relay_verified ? 1 : 0);
        (void)cbor_array_push(peer, relay_verified_val);
        cbor_decref(&relay_verified_val);
        cbor_item_t* nat_type_val = cbor_build_uint8((uint8_t)node->nat_type);
        (void)cbor_array_push(peer, nat_type_val);
        cbor_decref(&nat_type_val);
        cbor_item_t* last_seen_val = cbor_build_uint64(node->last_seen_ms);
        (void)cbor_array_push(peer, last_seen_val);
        cbor_decref(&last_seen_val);
        cbor_item_t* bad_blocks_val = cbor_build_uint64(node->bad_blocks_received);
        (void)cbor_array_push(peer, bad_blocks_val);
        cbor_decref(&bad_blocks_val);
        (void)cbor_array_push(peers_array, peer);
        cbor_decref(&peer);
      }
    }
  }
  (void)cbor_array_push(root, peers_array);
  cbor_decref(&peers_array);

  // Index 5: friend peers as Base58-encoded peer_info strings (snapshot or
  // direct startup/shutdown read, per the peer_book.h invariant)
  cbor_item_t* friends_arr = cbor_new_definite_array(b58_friend_count);
  for (size_t index = 0; index < b58_friend_count; index++) {
    cbor_item_t* b58_item = cbor_build_string(b58_friends[index]);
    (void)cbor_array_push(friends_arr, b58_item);
    cbor_decref(&b58_item);
  }
  (void)cbor_array_push(root, friends_arr);
  cbor_decref(&friends_arr);

  // Index 6: operator-managed bootstrap entries as normalized strings
  cbor_item_t* managed_arr = cbor_new_definite_array(managed_count);
  for (size_t index = 0; index < managed_count; index++) {
    cbor_item_t* str_item = cbor_build_string(managed_bootstrap[index]);
    (void)cbor_array_push(managed_arr, str_item);
    cbor_decref(&str_item);
  }
  (void)cbor_array_push(root, managed_arr);
  cbor_decref(&managed_arr);

  unsigned char* buffer = NULL;
  size_t buffer_size = 0;
  size_t length = cbor_serialize_alloc(root, &buffer, &buffer_size);
  cbor_decref(&root);
  if (length == 0 || buffer == NULL) return -1;

  int rc = platform_file_atomic_write(authority->peer_store_path, buffer, length);
  free(buffer);
  return rc;
}

int authority_load_peers(authority_t* authority, network_t* network) {
  if (authority == NULL || network == NULL) return -1;
  if (authority->peer_store_path == NULL) return -1;

  FILE* file = fopen(authority->peer_store_path, "rb");
  if (file == NULL) return -1;

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size <= 0) {
    fclose(file);
    return -1;
  }

  unsigned char* buffer = get_clear_memory((size_t)file_size);
  if (buffer == NULL) {
    fclose(file);
    return -1;
  }
  size_t read_bytes = fread(buffer, 1, (size_t)file_size, file);
  fclose(file);
  if (read_bytes == 0) {
    free(buffer);
    return -1;
  }

  struct cbor_load_result result;
  cbor_item_t* root = cbor_load(buffer, read_bytes, &result);
  free(buffer);
  if (result.error.code != CBOR_ERR_NONE || root == NULL) return -1;

  if (cbor_isa_array(root)) {
    // v2/v3 positional array format — v3 adds 4 fields per peer record.
    size_t arr_size = cbor_array_size(root);
    if (arr_size < 2) {
      cbor_decref(&root);
      return -1;
    }

    // Index 0: version — accept v2 (8-field peers) and v3 (12-field peers).
    cbor_item_t* version_item = cbor_array_get(root, 0);
    uint8_t version = cbor_isa_uint(version_item) ? (uint8_t)cbor_get_int(version_item) : 0;
    cbor_decref(&version_item);
    if (version != 2 && version != 3) {
      cbor_decref(&root);
      return -1;
    }

    // Index 1: local_id
    cbor_item_t* local_id_item = cbor_array_get(root, 1);
    if (cbor_isa_bytestring(local_id_item) && cbor_bytestring_length(local_id_item) == NODE_ID_HASH_SIZE) {
      memcpy(authority->local_id.hash, cbor_bytestring_handle(local_id_item), NODE_ID_HASH_SIZE);
      base58_encode(authority->local_id.hash, NODE_ID_HASH_SIZE,
                    authority->local_id.str, NODE_ID_STRING_SIZE);
    }
    cbor_decref(&local_id_item);

    // Index 2: ca_cert (may be empty bytestring)
    if (arr_size >= 3) {
      cbor_item_t* ca_cert_item = cbor_array_get(root, 2);
      if (cbor_isa_bytestring(ca_cert_item)) {
        size_t cert_len = cbor_bytestring_length(ca_cert_item);
        if (cert_len > 0) {
          // Free previously loaded CA cert if any
          if (authority->ca_cert_data != NULL) {
            free(authority->ca_cert_data);
          }
          authority->ca_cert_data = get_memory(cert_len);
          if (authority->ca_cert_data != NULL) {
            memcpy(authority->ca_cert_data, cbor_bytestring_handle(ca_cert_item), cert_len);
            authority->ca_cert_len = cert_len;
          }
        }
      }
      cbor_decref(&ca_cert_item);
    }

    // Index 3: hebbian
    if (arr_size >= 4) {
      cbor_item_t* hebbian_item = cbor_array_get(root, 3);
      if (cbor_isa_array(hebbian_item)) {
        size_t hebbian_len = cbor_array_size(hebbian_item);
        for (size_t he_idx = 0; he_idx < hebbian_len; he_idx++) {
          cbor_item_t* entry = cbor_array_get(hebbian_item, he_idx);
          if (cbor_isa_array(entry) && cbor_array_size(entry) == 2) {
            cbor_item_t* peer_id_item = cbor_array_get(entry, 0);
            cbor_item_t* weight_item = cbor_array_get(entry, 1);
            if (cbor_isa_bytestring(peer_id_item) && cbor_bytestring_length(peer_id_item) == NODE_ID_HASH_SIZE &&
                cbor_is_float(weight_item)) {
              node_id_t peer_id;
              memset(&peer_id, 0, sizeof(peer_id));
              memcpy(peer_id.hash, cbor_bytestring_handle(peer_id_item), NODE_ID_HASH_SIZE);
              float weight = (float)cbor_float_get_float8(weight_item);
              hebbian_table_set(&network->hebbian, &peer_id, weight);
            }
            cbor_decref(&peer_id_item);
            cbor_decref(&weight_item);
          }
          cbor_decref(&entry);
        }
      }
      cbor_decref(&hebbian_item);
    }

    // Index 4: peers
    if (arr_size >= 5) {
      cbor_item_t* peers_item = cbor_array_get(root, 4);
      if (cbor_isa_array(peers_item)) {
        size_t peers_len = cbor_array_size(peers_item);
        for (size_t peer_idx = 0; peer_idx < peers_len; peer_idx++) {
          cbor_item_t* peer = cbor_array_get(peers_item, peer_idx);
          // v2 records have 8 fields; v3 records have 12 (4 extra per-peer).
          size_t field_count = cbor_array_size(peer);
          if (cbor_isa_array(peer) && (field_count == 8 || field_count == 12)) {
            cbor_item_t* id_item = cbor_array_get(peer, 0);
            cbor_item_t* addr_item = cbor_array_get(peer, 1);
            cbor_item_t* port_item = cbor_array_get(peer, 2);
            cbor_item_t* lat_item = cbor_array_get(peer, 3);
            cbor_item_t* wt_item = cbor_array_get(peer, 4);
            cbor_item_t* cap_item = cbor_array_get(peer, 5);
            cbor_item_t* phase_item = cbor_array_get(peer, 6);
            cbor_item_t* avail_item = cbor_array_get(peer, 7);
            cbor_item_t* rv_item = NULL;
            cbor_item_t* nt_item = NULL;
            cbor_item_t* ls_item = NULL;
            cbor_item_t* bb_item = NULL;
            if (field_count == 12) {
              rv_item = cbor_array_get(peer, 8);
              nt_item = cbor_array_get(peer, 9);
              ls_item = cbor_array_get(peer, 10);
              bb_item = cbor_array_get(peer, 11);
            }
            if (cbor_isa_bytestring(id_item) && cbor_bytestring_length(id_item) == NODE_ID_HASH_SIZE) {
              node_id_t peer_id;
              memset(&peer_id, 0, sizeof(peer_id));
              memcpy(peer_id.hash, cbor_bytestring_handle(id_item), NODE_ID_HASH_SIZE);
              uint32_t addr = cbor_isa_uint(addr_item) ? (uint32_t)cbor_get_int(addr_item) : 0;
              uint16_t port = cbor_isa_uint(port_item) ? (uint16_t)cbor_get_int(port_item) : 0;
              net_node_t* node = net_node_create(&peer_id, addr, port);
              if (node != NULL) {
                if (cbor_is_float(lat_item)) node->latency_ms = (float)cbor_float_get_float4(lat_item);
                if (cbor_is_float(wt_item)) node->weight = (float)cbor_float_get_float4(wt_item);
                if (cbor_is_float(cap_item)) node->capacity = (float)cbor_float_get_float4(cap_item);
                if (cbor_isa_uint(phase_item)) node->phase = (node_phase_e)cbor_get_int(phase_item);
                if (cbor_is_float(avail_item)) node->availability = (float)cbor_float_get_float4(avail_item);
                if (field_count == 12) {
                  if (cbor_isa_uint(rv_item)) node->relay_verified = (cbor_get_int(rv_item) != 0);
                  if (cbor_isa_uint(nt_item)) node->nat_type = (nat_type_e)cbor_get_int(nt_item);
                  if (cbor_isa_uint(ls_item)) node->last_seen_ms = (uint64_t)cbor_get_int(ls_item);
                  if (cbor_isa_uint(bb_item)) node->bad_blocks_received = (uint64_t)cbor_get_int(bb_item);
                }
                // rings may be NULL in a minimal network — drop the node
                // rather than crash if there's nowhere to insert it.
                // TTL filter: when peer_state_ttl_ms > 0, drop peers whose
                // last_seen_ms is older than the TTL. last_seen_ms == 0 means
                // "never seen" (fresh) — keep. On drop, also remove the peer
                // from the hebbian table (loaded earlier from index 3) and
                // fall through to the cbor_decrefs below — do NOT `continue`,
                // or the cbor items would leak.
                bool dropped_stale = false;
                if (network->peer_state_ttl_ms > 0 && node->last_seen_ms != 0) {
                  uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
                  if (node->last_seen_ms <= now_ms) {
                    uint64_t age = now_ms - node->last_seen_ms;
                    if (age > (uint64_t)network->peer_state_ttl_ms) {
                      dropped_stale = true;
                    }
                  }
                }
                if (dropped_stale) {
                  hebbian_table_remove(&network->hebbian, &peer_id);
                  net_node_destroy(node);
                  node = NULL;
                } else if (network->rings != NULL) {
                  uint32_t latency_us = (uint32_t)(node->latency_ms * 1000.0f);
                  ring_set_insert(network->rings, node, latency_us);
                } else {
                  net_node_destroy(node);
                  node = NULL;
                }
              }
            }
            cbor_decref(&id_item);
            cbor_decref(&addr_item);
            cbor_decref(&port_item);
            cbor_decref(&lat_item);
            cbor_decref(&wt_item);
            cbor_decref(&cap_item);
            cbor_decref(&phase_item);
            cbor_decref(&avail_item);
            if (rv_item != NULL) cbor_decref(&rv_item);
            if (nt_item != NULL) cbor_decref(&nt_item);
            if (ls_item != NULL) cbor_decref(&ls_item);
            if (bb_item != NULL) cbor_decref(&bb_item);
          }
          cbor_decref(&peer);
        }
      }
      cbor_decref(&peers_item);
    }

    // Index 5: friend peers
    if (arr_size >= 6) {
      cbor_item_t* friends_item = cbor_array_get(root, 5);
      if (cbor_isa_array(friends_item)) {
        size_t friend_count = cbor_array_size(friends_item);
        if (authority->friend_peers != NULL) {
          for (size_t idx = 0; idx < authority->friend_peer_count; idx++) {
            peer_info_destroy(authority->friend_peers[idx]);
            free(authority->friend_peers[idx]);
          }
          free(authority->friend_peers);
          authority->friend_peers = NULL;
          authority->friend_peer_count = 0;
        }
        if (friend_count > 0) {
          authority->friend_peers = get_clear_memory(friend_count * sizeof(peer_info_t*));
          for (size_t index = 0; index < friend_count; index++) {
            cbor_item_t* b58_item = cbor_array_get(friends_item, index);
            if (cbor_isa_string(b58_item)) {
              char* b58 = strndup((char*)cbor_string_handle(b58_item), cbor_string_length(b58_item));
              peer_info_t* friend_info = get_clear_memory(sizeof(peer_info_t));
              if (peer_info_from_base58(b58, friend_info) == 0) {
                authority->friend_peers[authority->friend_peer_count++] = friend_info;
              } else {
                free(friend_info);
              }
              free(b58);
            }
            cbor_decref(&b58_item);
          }
        }
      }
      cbor_decref(&friends_item);
    }

    // Index 6: managed bootstrap entries (absent in older stores = empty)
    if (arr_size >= 7) {
      cbor_item_t* managed_item = cbor_array_get(root, 6);
      if (cbor_isa_array(managed_item)) {
        if (authority->managed_bootstrap_peers != NULL) {
          for (size_t idx = 0; idx < authority->managed_bootstrap_peer_count; idx++) {
            free(authority->managed_bootstrap_peers[idx]);
          }
          free(authority->managed_bootstrap_peers);
          authority->managed_bootstrap_peers = NULL;
          authority->managed_bootstrap_peer_count = 0;
        }
        size_t managed_count = cbor_array_size(managed_item);
        if (managed_count > 0) {
          authority->managed_bootstrap_peers =
              get_clear_memory(managed_count * sizeof(char*));
          for (size_t index = 0; index < managed_count; index++) {
            cbor_item_t* str_item = cbor_array_get(managed_item, index);
            if (cbor_isa_string(str_item)) {
              char discarded_host[256];
              uint16_t discarded_port = 0;
              char* stored = strndup((char*)cbor_string_handle(str_item),
                                     cbor_string_length(str_item));
              if (stored != NULL &&
                  endpoint_parse(stored, discarded_host, sizeof(discarded_host),
                                 &discarded_port) == 0) {
                authority->managed_bootstrap_peers[authority->managed_bootstrap_peer_count++] =
                    stored;
              } else {
                free(stored);
              }
            }
            cbor_decref(&str_item);
          }
        }
      }
      cbor_decref(&managed_item);
    }

    cbor_decref(&root);
  } else if (cbor_isa_map(root)) {
    // v1 map format — backward compatible, no ca_cert
    size_t map_size = cbor_map_size(root);
    uint8_t version = 0;
    for (size_t index = 0; index < map_size; index++) {
      struct cbor_pair pair = cbor_map_handle(root)[index];
      if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 1 &&
          cbor_string_handle(pair.key)[0] == 'v') {
        if (cbor_isa_uint(pair.value)) version = (uint8_t)cbor_get_int(pair.value);
      }
    }
    if (version != 1) {
      cbor_decref(&root);
      return -1;
    }

    // Extract local_id
    for (size_t index = 0; index < map_size; index++) {
      struct cbor_pair pair = cbor_map_handle(root)[index];
      if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 9 &&
          memcmp(cbor_string_handle(pair.key), "local_id", 9) == 0) {
        if (cbor_isa_bytestring(pair.value) && cbor_bytestring_length(pair.value) == NODE_ID_HASH_SIZE) {
          memcpy(authority->local_id.hash, cbor_bytestring_handle(pair.value), NODE_ID_HASH_SIZE);
          base58_encode(authority->local_id.hash, NODE_ID_HASH_SIZE,
                        authority->local_id.str, NODE_ID_STRING_SIZE);
        }
      }
    }

    // Extract hebbian weights
    for (size_t index = 0; index < map_size; index++) {
      struct cbor_pair pair = cbor_map_handle(root)[index];
      if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 7 &&
          memcmp(cbor_string_handle(pair.key), "hebbian", 7) == 0) {
        if (cbor_isa_array(pair.value)) {
          size_t hebbian_len = cbor_array_size(pair.value);
          for (size_t he_idx = 0; he_idx < hebbian_len; he_idx++) {
            cbor_item_t* entry = cbor_array_get(pair.value, he_idx);
            if (cbor_isa_array(entry) && cbor_array_size(entry) == 2) {
              cbor_item_t* peer_id_item = cbor_array_get(entry, 0);
              cbor_item_t* weight_item = cbor_array_get(entry, 1);
              if (cbor_isa_bytestring(peer_id_item) && cbor_bytestring_length(peer_id_item) == NODE_ID_HASH_SIZE &&
                  cbor_is_float(weight_item)) {
                node_id_t peer_id;
                memset(&peer_id, 0, sizeof(peer_id));
                memcpy(peer_id.hash, cbor_bytestring_handle(peer_id_item), NODE_ID_HASH_SIZE);
                float weight = (float)cbor_float_get_float8(weight_item);
                hebbian_table_set(&network->hebbian, &peer_id, weight);
              }
              cbor_decref(&peer_id_item);
              cbor_decref(&weight_item);
            }
            cbor_decref(&entry);
          }
        }
      }
    }

    // Extract peer data
    for (size_t index = 0; index < map_size; index++) {
      struct cbor_pair pair = cbor_map_handle(root)[index];
      if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 5 &&
          memcmp(cbor_string_handle(pair.key), "peers", 5) == 0) {
        if (cbor_isa_array(pair.value)) {
          size_t peers_len = cbor_array_size(pair.value);
          for (size_t peer_idx = 0; peer_idx < peers_len; peer_idx++) {
            cbor_item_t* peer = cbor_array_get(pair.value, peer_idx);
            if (cbor_isa_array(peer) && cbor_array_size(peer) == 8) {
              cbor_item_t* id_item = cbor_array_get(peer, 0);
              cbor_item_t* addr_item = cbor_array_get(peer, 1);
              cbor_item_t* port_item = cbor_array_get(peer, 2);
              cbor_item_t* lat_item = cbor_array_get(peer, 3);
              cbor_item_t* wt_item = cbor_array_get(peer, 4);
              cbor_item_t* cap_item = cbor_array_get(peer, 5);
              cbor_item_t* phase_item = cbor_array_get(peer, 6);
              cbor_item_t* avail_item = cbor_array_get(peer, 7);
              if (cbor_isa_bytestring(id_item) && cbor_bytestring_length(id_item) == NODE_ID_HASH_SIZE) {
                node_id_t peer_id;
                memset(&peer_id, 0, sizeof(peer_id));
                memcpy(peer_id.hash, cbor_bytestring_handle(id_item), NODE_ID_HASH_SIZE);
                uint32_t addr = cbor_isa_uint(addr_item) ? (uint32_t)cbor_get_int(addr_item) : 0;
                uint16_t port = cbor_isa_uint(port_item) ? (uint16_t)cbor_get_int(port_item) : 0;
                net_node_t* node = net_node_create(&peer_id, addr, port);
                if (node != NULL) {
                  if (cbor_is_float(lat_item)) node->latency_ms = (float)cbor_float_get_float4(lat_item);
                  if (cbor_is_float(wt_item)) node->weight = (float)cbor_float_get_float4(wt_item);
                  if (cbor_is_float(cap_item)) node->capacity = (float)cbor_float_get_float4(cap_item);
                  if (cbor_isa_uint(phase_item)) node->phase = (node_phase_e)cbor_get_int(phase_item);
                  if (cbor_is_float(avail_item)) node->availability = (float)cbor_float_get_float4(avail_item);
                  uint32_t latency_us = (uint32_t)(node->latency_ms * 1000.0f);
                  ring_set_insert(network->rings, node, latency_us);
                }
              }
              cbor_decref(&id_item);
              cbor_decref(&addr_item);
              cbor_decref(&port_item);
              cbor_decref(&lat_item);
              cbor_decref(&wt_item);
              cbor_decref(&cap_item);
              cbor_decref(&phase_item);
              cbor_decref(&avail_item);
            }
            cbor_decref(&peer);
          }
        }
      }
    }

    cbor_decref(&root);
  } else {
    cbor_decref(&root);
    return -1;
  }

  // Sync hebbian weights into ring nodes
  for (size_t index = 0; index < network->hebbian.count; index++) {
    hebbian_weight_t* weight = &network->hebbian.entries[index];
    net_node_t* node = ring_set_find_by_id(network->rings, &weight->peer_id);
    if (node != NULL) node->weight = weight->weight;
  }
  return 0;
}

void authority_update_capacity(authority_t* authority, float capacity) {
  if (authority == NULL) return;
  ATOMIC_STORE(&authority->capacity, capacity);
}

void authority_update_phase(authority_t* authority, float capacity) {
  if (authority == NULL) return;
  node_phase_e phase;
  if (capacity >= RESPIRATION_EXHALE_THRESHOLD) {
    phase = NODE_PHASE_EXHALE;
  } else if (capacity < RESPIRATION_INHALE_THRESHOLD) {
    phase = NODE_PHASE_INHALE;
  } else {
    phase = NODE_PHASE_NEUTRAL;
  }
  ATOMIC_STORE(&authority->phase, phase);
}