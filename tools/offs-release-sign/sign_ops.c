// offs-release-sign: ed25519 release manifest signing operations.
//
// Mirrors the OpenSSL 3.x ed25519 idioms used by tools/offs-ca (keygen via
// EVP_PKEY_CTX_new_id + EVP_PKEY_keygen_init + EVP_PKEY_keygen, PEM I/O via
// PEM_write_bio_PrivateKey / PEM_write_bio_PUBKEY / PEM_read_bio_PrivateKey)
// and test/test_update_verify.cpp's _sign helper (one-shot EVP_DigestSignInit
// with a NULL digest, which is the correct pure-ed25519 signing API).

#include "sign_ops.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- keygen ---- */

int release_sign_keygen(const char* priv_path, const char* pub_path) {
  if (!priv_path || !pub_path) return -1;

  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create ed25519 key context\n");
    return -1;
  }

  if (EVP_PKEY_keygen_init(ctx) <= 0) {
    fprintf(stderr, "ed25519 keygen init failed\n");
    ERR_print_errors_fp(stderr);
    EVP_PKEY_CTX_free(ctx);
    return -1;
  }

  EVP_PKEY* key = NULL;
  if (EVP_PKEY_keygen(ctx, &key) <= 0) {
    fprintf(stderr, "ed25519 key generation failed\n");
    ERR_print_errors_fp(stderr);
    EVP_PKEY_CTX_free(ctx);
    return -1;
  }
  EVP_PKEY_CTX_free(ctx);

  int ret = -1;
  BIO* priv_bio = BIO_new_file(priv_path, "w");
  if (!priv_bio) {
    fprintf(stderr, "Failed to open %s for writing\n", priv_path);
    goto keygen_done;
  }
  if (!PEM_write_bio_PrivateKey(priv_bio, key, NULL, NULL, 0, NULL, NULL)) {
    fprintf(stderr, "Failed to write private key PEM to %s\n", priv_path);
    ERR_print_errors_fp(stderr);
    BIO_free(priv_bio);
    goto keygen_done;
  }
  BIO_free(priv_bio);

  BIO* pub_bio = BIO_new_file(pub_path, "w");
  if (!pub_bio) {
    fprintf(stderr, "Failed to open %s for writing\n", pub_path);
    goto keygen_done;
  }
  if (!PEM_write_bio_PUBKEY(pub_bio, key)) {
    fprintf(stderr, "Failed to write public key PEM to %s\n", pub_path);
    ERR_print_errors_fp(stderr);
    BIO_free(pub_bio);
    goto keygen_done;
  }
  BIO_free(pub_bio);

  ret = 0;

keygen_done:
  EVP_PKEY_free(key);
  return ret;
}

/* ---- sign ---- */

static uint8_t* _read_file(const char* path, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "Failed to open %s for reading\n", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fprintf(stderr, "fseek failed on %s\n", path);
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size < 0) {
    fprintf(stderr, "ftell failed on %s\n", path);
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fprintf(stderr, "fseek-rewind failed on %s\n", path);
    fclose(f);
    return NULL;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)size);
  if (!buf) {
    fprintf(stderr, "Out of memory reading %s (%ld bytes)\n", path, size);
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  if (got != (size_t)size) {
    fprintf(stderr, "Short read on %s (%zu/%ld)\n", path, got, size);
    free(buf);
    return NULL;
  }
  *out_len = (size_t)size;
  return buf;
}

int release_sign_sign(const char* key_path, const char* manifest_path) {
  if (!key_path || !manifest_path) return -1;

  size_t manifest_len = 0;
  uint8_t* manifest = _read_file(manifest_path, &manifest_len);
  if (!manifest) return -1;

  BIO* key_bio = BIO_new_file(key_path, "r");
  if (!key_bio) {
    fprintf(stderr, "Failed to open %s for reading\n", key_path);
    free(manifest);
    return -1;
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
  BIO_free(key_bio);
  if (!key) {
    fprintf(stderr, "Failed to read private key from %s\n", key_path);
    ERR_print_errors_fp(stderr);
    free(manifest);
    return -1;
  }

  int ret = -1;
  uint8_t* sig = NULL;
  EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
  if (!md_ctx) {
    fprintf(stderr, "EVP_MD_CTX_new failed\n");
    goto sign_done;
  }

  /* ed25519 one-shot: NULL digest, NULL engine — pure Ed25519 per RFC 8032. */
  if (EVP_DigestSignInit(md_ctx, NULL, NULL, NULL, key) <= 0) {
    fprintf(stderr, "EVP_DigestSignInit failed\n");
    ERR_print_errors_fp(stderr);
    goto sign_done;
  }

  size_t sig_len = 0;
  if (EVP_DigestSign(md_ctx, NULL, &sig_len, manifest, manifest_len) <= 0) {
    fprintf(stderr, "EVP_DigestSign (size query) failed\n");
    ERR_print_errors_fp(stderr);
    goto sign_done;
  }
  if (sig_len != 64) {
    fprintf(stderr, "Unexpected ed25519 signature length %zu (expected 64)\n", sig_len);
    goto sign_done;
  }

  sig = (uint8_t*)malloc(sig_len);
  if (!sig) {
    fprintf(stderr, "Out of memory allocating %zu-byte signature\n", sig_len);
    goto sign_done;
  }
  if (EVP_DigestSign(md_ctx, sig, &sig_len, manifest, manifest_len) <= 0) {
    fprintf(stderr, "EVP_DigestSign failed\n");
    ERR_print_errors_fp(stderr);
    goto sign_done;
  }

  {
    size_t sig_path_len = strlen(manifest_path) + 5; /* ".sig" + NUL */
    char* sig_path = (char*)malloc(sig_path_len);
    if (!sig_path) {
      fprintf(stderr, "Out of memory building signature path\n");
      goto sign_done;
    }
    snprintf(sig_path, sig_path_len, "%s.sig", manifest_path);
    FILE* sf = fopen(sig_path, "wb");
    if (!sf) {
      fprintf(stderr, "Failed to open %s for writing\n", sig_path);
      free(sig_path);
      goto sign_done;
    }
    size_t wrote = fwrite(sig, 1, sig_len, sf);
    fclose(sf);
    free(sig_path);
    if (wrote != sig_len) {
      fprintf(stderr, "Short write on signature (%zu/%zu)\n", wrote, sig_len);
      goto sign_done;
    }
  }

  ret = 0;

sign_done:
  if (sig) free(sig);
  if (md_ctx) EVP_MD_CTX_free(md_ctx);
  EVP_PKEY_free(key);
  free(manifest);
  return ret;
}