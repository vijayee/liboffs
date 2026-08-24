#include "update_verify.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdbool.h>
#include <string.h>

#ifndef OFFS_RELEASE_PUBKEY
#define OFFS_RELEASE_PUBKEY ""
#endif

SSL_CTX* update_ssl_context_create(void) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == NULL) {
    return NULL;
  }
  SSL_CTX_set_default_verify_paths(ctx);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_verify_depth(ctx, 4);
  return ctx;
}

const char* update_verify_load_pubkey(void) {
  return (OFFS_RELEASE_PUBKEY[0] != '\0') ? OFFS_RELEASE_PUBKEY : NULL;
}

bool update_verify_manifest(const uint8_t* sig, size_t sig_len,
                            const uint8_t* manifest, size_t manifest_len,
                            const char* pubkey_pem, size_t pubkey_pem_len) {
  if (sig == NULL || manifest == NULL) return false;
  const char* pem = pubkey_pem;
  size_t pem_len = pubkey_pem_len;
  if (pem == NULL) {
    pem = update_verify_load_pubkey();
    if (pem == NULL) return false;  // fail-closed: no release key compiled in
    pem_len = strlen(pem);
  }
  BIO* bio = BIO_new_mem_buf(pem, (int)pem_len);
  if (bio == NULL) return false;
  EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (key == NULL) return false;
  /* ed25519 one-shot verify: EVP_DigestVerifyInit with a NULL digest selects
   * pure Ed25519 (no pre-hash), then EVP_DigestVerify performs the one-shot
   * verification over the manifest bytes. */
  EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
  if (md_ctx == NULL) {
    EVP_PKEY_free(key);
    return false;
  }
  bool ok = false;
  if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, key) == 1) {
    ok = EVP_DigestVerify(md_ctx, sig, sig_len, manifest, manifest_len) == 1;
  }
  EVP_MD_CTX_free(md_ctx);
  EVP_PKEY_free(key);
  return ok;
}