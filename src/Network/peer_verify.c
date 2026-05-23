//
// Created by victor on 5/22/26.
//

#include "peer_verify.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/bio.h>

struct peer_verify_ctx_t {
  X509_STORE* store;
};

peer_verify_ctx_t* peer_verify_ctx_create(const uint8_t* ca_cert_data, size_t ca_cert_len) {
  if (ca_cert_data == NULL || ca_cert_len == 0) {
    return NULL;
  }

  const unsigned char* cursor = ca_cert_data;
  X509* ca_cert = d2i_X509(NULL, &cursor, (long)ca_cert_len);
  if (ca_cert == NULL) {
    log_error("peer_verify: failed to parse DER CA certificate");
    return NULL;
  }

  X509_STORE* store = X509_STORE_new();
  if (store == NULL) {
    X509_free(ca_cert);
    return NULL;
  }

  if (X509_STORE_add_cert(store, ca_cert) != 1) {
    log_error("peer_verify: failed to add CA cert to store");
    X509_free(ca_cert);
    X509_STORE_free(store);
    return NULL;
  }
  X509_free(ca_cert);

  peer_verify_ctx_t* ctx = get_clear_memory(sizeof(peer_verify_ctx_t));
  if (ctx == NULL) {
    X509_STORE_free(store);
    return NULL;
  }
  ctx->store = store;
  return ctx;
}

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx) {
  if (ctx == NULL) return;
  if (ctx->store != NULL) {
    X509_STORE_free(ctx->store);
  }
  free(ctx);
}

int peer_verify_validate(peer_verify_ctx_t* ctx, void* certificate) {
  if (ctx == NULL || certificate == NULL) {
    return -1;
  }

  QUIC_CERTIFICATE* cert = (QUIC_CERTIFICATE*)certificate;
  const unsigned char* cursor = cert->Certificate;
  X509* peer_cert = d2i_X509(NULL, &cursor, (long)cert->CertificateLength);
  if (peer_cert == NULL) {
    log_error("peer_verify: failed to parse peer certificate DER");
    return -1;
  }

  X509_STORE_CTX* verify_ctx = X509_STORE_CTX_new();
  if (verify_ctx == NULL) {
    X509_free(peer_cert);
    return -1;
  }

  X509_STORE_CTX_init(verify_ctx, ctx->store, peer_cert, NULL);
  int result = X509_verify_cert(verify_ctx);

  if (result != 1) {
    int err = X509_STORE_CTX_get_error(verify_ctx);
    log_error("peer_verify: certificate verification failed: %s",
              X509_verify_cert_error_string(err));
  }

  X509_STORE_CTX_free(verify_ctx);
  X509_free(peer_cert);
  return (result == 1) ? 0 : -1;
}
