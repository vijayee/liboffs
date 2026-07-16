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

// Load a PEM-encoded private key from key_path. Returns NULL on failure.
// The caller owns the returned EVP_PKEY and must EVP_PKEY_free it.
static EVP_PKEY* pem_key_load_private(const char* key_path) {
  if (key_path == NULL) return NULL;

  BIO* bio = BIO_new_file(key_path, "r");
  if (bio == NULL) {
    log_error("pem_key_load_private: failed to open key file: %s", key_path);
    return NULL;
  }

  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (pkey == NULL) {
    log_error("pem_key_load_private: failed to parse PEM private key: %s", key_path);
    return NULL;
  }
  return pkey;
}

int pem_key_sign_nonce(const char* key_path, const uint8_t nonce[32],
                       uint8_t** out_sig, size_t* out_sig_len) {
  if (key_path == NULL || nonce == NULL || out_sig == NULL || out_sig_len == NULL) {
    return -1;
  }

  *out_sig = NULL;
  *out_sig_len = 0;

  EVP_PKEY* pkey = pem_key_load_private(key_path);
  if (pkey == NULL) return -1;

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == NULL) {
    EVP_PKEY_free(pkey);
    log_error("pem_key_sign_nonce: EVP_MD_CTX_new failed");
    return -1;
  }

  /* NULL digest = the key's default. Ed25519 uses its own internal digest
   * (ignores this argument); RSA/EC use SHA-512 by default. This is the
   * standard one-shot EVP_DigestSign form that works for all key types. */
  if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) != 1) {
    log_error("pem_key_sign_nonce: EVP_DigestSignInit failed");
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return -1;
  }

  size_t sig_len = 0;
  if (EVP_DigestSign(ctx, NULL, &sig_len, nonce, 32) != 1 || sig_len == 0) {
    log_error("pem_key_sign_nonce: EVP_DigestSign length probe failed");
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return -1;
  }

  uint8_t* sig = get_clear_memory(sig_len);
  if (sig == NULL) {
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return -1;
  }

  if (EVP_DigestSign(ctx, sig, &sig_len, nonce, 32) != 1) {
    log_error("pem_key_sign_nonce: EVP_DigestSign failed");
    free(sig);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return -1;
  }

  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  *out_sig = sig;
  *out_sig_len = sig_len;
  return 0;
}

// Reconstruct an EVP_PKEY from the same byte format pem_extract_public_key
// produces: raw Ed25519/X25519 bytes (EVP_PKEY_new_raw_public_key) first,
// then DER SubjectPublicKeyInfo (d2i_PUBKEY). Returns NULL on failure. The
// caller owns the returned EVP_PKEY and must EVP_PKEY_free it. See audit #8.
static EVP_PKEY* pem_key_pubkey_from_bytes(const uint8_t* public_key,
                                           size_t public_key_len) {
  if (public_key == NULL || public_key_len == 0) return NULL;

  /* Raw Ed25519 is 32 bytes; raw Ed448 is 57 bytes. Both are signing keys
   * usable with EVP_DigestVerify. X25519/X448 are key-agreement only and
   * would never be used to sign a nonce. EVP_PKEY_new_raw_public_key rejects
   * DER input (ASN.1 parse mismatch), so the raw attempt is a safe first
   * probe. RSA/EC keys fall through to d2i_PUBKEY. */
  if (public_key_len == 32) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                 public_key, public_key_len);
    if (pkey != NULL) return pkey;
  } else if (public_key_len == 57) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED448, NULL,
                                                 public_key, public_key_len);
    if (pkey != NULL) return pkey;
  }

  const uint8_t* cursor = public_key;
  EVP_PKEY* pkey = d2i_PUBKEY(NULL, &cursor, (long)public_key_len);
  if (pkey != NULL) return pkey;

  return NULL;
}

int pem_key_verify_nonce(const uint8_t* public_key, size_t public_key_len,
                         const uint8_t nonce[32],
                         const uint8_t* signature, size_t signature_len) {
  if (public_key == NULL || nonce == NULL || signature == NULL || signature_len == 0) {
    return -1;
  }

  EVP_PKEY* pkey = pem_key_pubkey_from_bytes(public_key, public_key_len);
  if (pkey == NULL) {
    log_error("pem_key_verify_nonce: failed to reconstruct EVP_PKEY from public key bytes");
    return -1;
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == NULL) {
    EVP_PKEY_free(pkey);
    log_error("pem_key_verify_nonce: EVP_MD_CTX_new failed");
    return -1;
  }

  if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) != 1) {
    log_error("pem_key_verify_nonce: EVP_DigestVerifyInit failed");
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return -1;
  }

  /* Returns 1 on success, 0 on invalid signature, <0 on error. Treat anything
   * other than 1 as failure (-1). */
  int result = EVP_DigestVerify(ctx, signature, signature_len, nonce, 32);
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);

  return (result == 1) ? 0 : -1;
}