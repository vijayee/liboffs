//
// Created by victor on 5/22/26.
//

#include "peer_verify.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Platform/platform_file.h"
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <stdio.h>
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

peer_verify_ctx_t* peer_verify_ctx_create_from_pem_file(const char* pem_path) {
  if (pem_path == NULL) {
    return NULL;
  }
  FILE* pem_file = fopen(pem_path, "rb");
  if (pem_file == NULL) {
    log_error("peer_verify: failed to open CA PEM file: %s", pem_path);
    return NULL;
  }
  X509* ca_cert = PEM_read_X509(pem_file, NULL, NULL, NULL);
  fclose(pem_file);
  if (ca_cert == NULL) {
    log_error("peer_verify: failed to parse PEM CA certificate: %s", pem_path);
    return NULL;
  }
  unsigned char* der_data = NULL;
  int der_len = i2d_X509(ca_cert, &der_data);
  X509_free(ca_cert);
  if (der_len <= 0 || der_data == NULL) {
    log_error("peer_verify: failed to convert PEM CA to DER: %s", pem_path);
    if (der_data != NULL) OPENSSL_free(der_data);
    return NULL;
  }
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(der_data, (size_t)der_len);
  OPENSSL_free(der_data);
  return ctx;
}

const char* peer_verify_ctx_path(const peer_verify_ctx_t* ctx) {
  if (ctx == NULL) return NULL;
  return ctx->temp_path;
}

int peer_verify_extract_pubkey(const uint8_t* cert_der, size_t cert_len,
                               uint8_t** out_key, size_t* out_len) {
  if (cert_der == NULL || cert_len == 0 || out_key == NULL || out_len == NULL) {
    return -1;
  }

  *out_key = NULL;
  *out_len = 0;

  const unsigned char* cursor = cert_der;
  X509* cert = d2i_X509(NULL, &cursor, (long)cert_len);
  if (cert == NULL) {
    log_error("peer_verify: failed to parse DER peer certificate");
    return -1;
  }

  EVP_PKEY* pkey = X509_get_pubkey(cert);
  X509_free(cert);
  if (pkey == NULL) {
    log_error("peer_verify: failed to get public key from peer certificate");
    return -1;
  }

  // Mirror pem_extract_public_key: raw bytes first (Ed25519/X25519), then
  // DER SubjectPublicKeyInfo (RSA/EC). The salutation public_key is built
  // by pem_extract_public_key, so the extracted cert key must use the
  // exact same encoding or the byte-compare in network_handle_salutation
  // would always mismatch. Fall through to DER on raw second-call failure
  // (matches pem_extract_public_key, which also falls through). See audit #8.
  size_t raw_len = 0;
  if (EVP_PKEY_get_raw_public_key(pkey, NULL, &raw_len) == 1 && raw_len > 0) {
    uint8_t* raw_key = get_clear_memory(raw_len);
    if (raw_key != NULL && EVP_PKEY_get_raw_public_key(pkey, raw_key, &raw_len) == 1) {
      EVP_PKEY_free(pkey);
      *out_key = raw_key;
      *out_len = raw_len;
      return 0;
    }
    free(raw_key);
  }

  int der_len = i2d_PUBKEY(pkey, NULL);
  if (der_len > 0) {
    uint8_t* der_buf = get_clear_memory((size_t)der_len);
    if (der_buf != NULL) {
      uint8_t* der_ptr = der_buf;
      int written = i2d_PUBKEY(pkey, &der_ptr);
      EVP_PKEY_free(pkey);
      if (written > 0) {
        *out_key = der_buf;
        *out_len = (size_t)written;
        return 0;
      }
      free(der_buf);
    } else {
      EVP_PKEY_free(pkey);
    }
  } else {
    EVP_PKEY_free(pkey);
  }

  log_error("peer_verify: failed to extract public key bytes");
  return -1;
}
