#ifndef OFFS_UPDATE_VERIFY_H
#define OFFS_UPDATE_VERIFY_H

#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Create an SSL_CTX configured for TLS peer verification: loads default CA
 * paths, enables SSL_VERIFY_PEER, sets verify depth 4. Returns NULL on
 * failure. Caller frees with SSL_CTX_free. */
SSL_CTX* update_ssl_context_create(void);

/* The compiled-in release public key (PEM), or NULL if OFFS_RELEASE_PUBKEY
 * was not set at build time (fail-closed: verification always fails). */
const char* update_verify_load_pubkey(void);

/* Verify an ed25519 signature over a manifest. If pubkey_pem is non-NULL, use
 * it; otherwise use the compiled-in release key (update_verify_load_pubkey).
 * Returns true on a valid signature, false on any failure (bad sig, wrong key,
 * NULL pubkey, OOM). pubkey_pem_len is the PEM length (or 0 if pubkey_pem is
 * NULL and the compiled-in key is used). */
bool update_verify_manifest(const uint8_t* sig, size_t sig_len,
                            const uint8_t* manifest, size_t manifest_len,
                            const char* pubkey_pem, size_t pubkey_pem_len);

#endif /* OFFS_UPDATE_VERIFY_H */