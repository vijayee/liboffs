//
// Created by victor on 5/22/26.
//

#include "peer_verify.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Platform/platform_file.h"
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <string.h>

struct peer_verify_ctx_t {
  char* temp_path;  // path to temporary PEM file, NULL if creation failed
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

  BIO* mem_bio = BIO_new(BIO_s_mem());
  if (mem_bio == NULL) {
    X509_free(ca_cert);
    return NULL;
  }

  if (PEM_write_bio_X509(mem_bio, ca_cert) != 1) {
    log_error("peer_verify: failed to convert DER to PEM");
    BIO_free(mem_bio);
    X509_free(ca_cert);
    return NULL;
  }
  X509_free(ca_cert);

  char* pem_data = NULL;
  long pem_len = BIO_get_mem_data(mem_bio, &pem_data);
  if (pem_data == NULL || pem_len <= 0) {
    BIO_free(mem_bio);
    return NULL;
  }

  char* temp_path = platform_temp_file_write((const uint8_t*)pem_data, (size_t)pem_len);
  BIO_free(mem_bio);
  if (temp_path == NULL) {
    log_error("peer_verify: failed to write CA cert to temp file");
    return NULL;
  }

  peer_verify_ctx_t* ctx = get_clear_memory(sizeof(peer_verify_ctx_t));
  if (ctx == NULL) {
    platform_file_unlink(temp_path);
    free(temp_path);
    return NULL;
  }
  ctx->temp_path = temp_path;
  return ctx;
}

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx) {
  if (ctx == NULL) return;
  if (ctx->temp_path != NULL) {
    platform_file_unlink(ctx->temp_path);
    free(ctx->temp_path);
  }
  free(ctx);
}

const char* peer_verify_ctx_path(const peer_verify_ctx_t* ctx) {
  if (ctx == NULL) return NULL;
  return ctx->temp_path;
}
