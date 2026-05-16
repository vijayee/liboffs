//
// Created by victor on 5/16/26.
//

#include "pem_key.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <string.h>

uint8_t* pem_extract_public_key(const char* cert_path, size_t* out_len) {
  if (cert_path == NULL || out_len == NULL) return NULL;

  *out_len = 0;

  BIO* bio = BIO_new_file(cert_path, "r");
  if (bio == NULL) {
    log_error("pem_extract_public_key: failed to open cert file: %s", cert_path);
    return NULL;
  }

  X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (cert == NULL) {
    log_error("pem_extract_public_key: failed to parse PEM certificate: %s", cert_path);
    return NULL;
  }

  EVP_PKEY* pkey = X509_get_pubkey(cert);
  X509_free(cert);
  if (pkey == NULL) {
    log_error("pem_extract_public_key: failed to get public key from certificate");
    return NULL;
  }

  // Try raw public key extraction first (Ed25519, X25519, etc.)
  size_t raw_len = 0;
  if (EVP_PKEY_get_raw_public_key(pkey, NULL, &raw_len) == 1 && raw_len > 0) {
    uint8_t* raw_key = get_clear_memory(raw_len);
    if (raw_key != NULL && EVP_PKEY_get_raw_public_key(pkey, raw_key, &raw_len) == 1) {
      EVP_PKEY_free(pkey);
      *out_len = raw_len;
      return raw_key;
    }
    free(raw_key);
  }

  // Fallback: RSA and EC keys — encode as DER SubjectPublicKeyInfo
  int der_len = i2d_PUBKEY(pkey, NULL);
  if (der_len > 0) {
    uint8_t* der_buf = get_clear_memory((size_t)der_len);
    if (der_buf != NULL) {
      uint8_t* der_ptr = der_buf;
      int written = i2d_PUBKEY(pkey, &der_ptr);
      EVP_PKEY_free(pkey);
      if (written > 0) {
        *out_len = (size_t)written;
        return der_buf;
      }
      free(der_buf);
    }
  }

  EVP_PKEY_free(pkey);
  log_error("pem_extract_public_key: failed to extract public key bytes");
  return NULL;
}