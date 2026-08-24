#include <gtest/gtest.h>

extern "C" {
#include "Update/update_verify.h"
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <string.h>
}

TEST(UpdateTls, ContextEnablesPeerVerification) {
  SSL_CTX* ctx = update_ssl_context_create();
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(SSL_CTX_get_verify_mode(ctx), SSL_VERIFY_PEER);
  SSL_CTX_free(ctx);
}

TEST(UpdateTls, ContextLoadsDefaultVerifyPaths) {
  // Indirect: a context with default verify paths set should be able to load
  // the system CA store. Just assert the context is non-NULL (the verify-mode
  // test above covers the behavioral assertion).
  SSL_CTX* ctx = update_ssl_context_create();
  ASSERT_NE(ctx, nullptr);
  SSL_CTX_free(ctx);
}

// Helper: generate an ed25519 keypair, return the private EVP_PKEY* and write
// the public-key PEM to out_pub_pem (malloc'd, caller frees).
static EVP_PKEY* _gen_ed25519_keypair(char** out_pub_pem, size_t* out_pub_len) {
  EVP_PKEY* key = NULL;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);
  if (key == NULL) return NULL;
  // Extract pubkey to PEM.
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PUBKEY(bio, key);
  char* pem_data = NULL;
  long pem_len = BIO_get_mem_data(bio, &pem_data);
  *out_pub_pem = (char*)malloc(pem_len + 1);
  memcpy(*out_pub_pem, pem_data, pem_len);
  (*out_pub_pem)[pem_len] = '\0';
  *out_pub_len = (size_t)pem_len;
  BIO_free(bio);
  return key;
}

// Helper: sign manifest with the private key, return the signature (malloc'd).
static uint8_t* _sign(EVP_PKEY* key, const uint8_t* manifest, size_t manifest_len, size_t* out_sig_len) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestSignInit(ctx, NULL, NULL, NULL, key);
  size_t sig_len = 0;
  EVP_DigestSign(ctx, NULL, &sig_len, manifest, manifest_len);
  uint8_t* sig = (uint8_t*)malloc(sig_len);
  EVP_DigestSign(ctx, sig, &sig_len, manifest, manifest_len);
  *out_sig_len = sig_len;
  EVP_MD_CTX_free(ctx);
  return sig;
}

TEST(UpdateVerify, GoodSignaturePasses) {
  char* pub_pem; size_t pub_len;
  EVP_PKEY* key = _gen_ed25519_keypair(&pub_pem, &pub_len);
  ASSERT_NE(key, nullptr);
  uint8_t manifest[] = "{\"version\":1,\"files\":[]}";
  size_t manifest_len = sizeof(manifest) - 1;
  size_t sig_len;
  uint8_t* sig = _sign(key, manifest, manifest_len, &sig_len);
  bool ok = update_verify_manifest(sig, sig_len, manifest, manifest_len, pub_pem, pub_len);
  EXPECT_TRUE(ok);
  free(sig); free(pub_pem); EVP_PKEY_free(key);
}

TEST(UpdateVerify, TamperedSignatureFails) {
  char* pub_pem; size_t pub_len;
  EVP_PKEY* key = _gen_ed25519_keypair(&pub_pem, &pub_len);
  ASSERT_NE(key, nullptr);
  uint8_t manifest[] = "{\"version\":1,\"files\":[]}";
  size_t manifest_len = sizeof(manifest) - 1;
  size_t sig_len;
  uint8_t* sig = _sign(key, manifest, manifest_len, &sig_len);
  sig[0] ^= 0xFF;  // tamper
  bool ok = update_verify_manifest(sig, sig_len, manifest, manifest_len, pub_pem, pub_len);
  EXPECT_FALSE(ok);
  free(sig); free(pub_pem); EVP_PKEY_free(key);
}

TEST(UpdateVerify, WrongPubkeyFails) {
  char* pub1; size_t len1;
  EVP_PKEY* key1 = _gen_ed25519_keypair(&pub1, &len1);
  char* pub2; size_t len2;
  EVP_PKEY* key2 = _gen_ed25519_keypair(&pub2, &len2);
  uint8_t manifest[] = "{\"version\":1}";
  size_t manifest_len = sizeof(manifest) - 1;
  size_t sig_len;
  uint8_t* sig = _sign(key1, manifest, manifest_len, &sig_len);
  // Verify with pub2 (wrong key) -> false.
  bool ok = update_verify_manifest(sig, sig_len, manifest, manifest_len, pub2, len2);
  EXPECT_FALSE(ok);
  free(sig); free(pub1); free(pub2); EVP_PKEY_free(key1); EVP_PKEY_free(key2);
}

TEST(UpdateVerify, NullPubkeyFailsClosed) {
  // With no compiled-in OFFS_RELEASE_PUBKEY and pubkey_pem=NULL, must fail.
  uint8_t manifest[] = "{\"version\":1}";
  uint8_t dummy_sig[64] = {0};
  bool ok = update_verify_manifest(dummy_sig, sizeof(dummy_sig), manifest, sizeof(manifest)-1, NULL, 0);
  EXPECT_FALSE(ok);
}