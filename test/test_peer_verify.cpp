#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <cstdio>
#include <string>
#include <filesystem>

extern "C" {
#include "../src/Network/peer_verify.h"
#include "../src/Network/pem_key.h"
#include "../src/Network/authority.h"
#include "../src/Configuration/config.h"
#include "../src/Util/allocator.h"
#include "../src/Platform/platform_posix_compat.h"
#include "../../tools/offs-ca/ca_ops.h"
#include <openssl/pem.h>
#include <openssl/x509.h>
}

static std::vector<uint8_t> pem_to_der(const char* pem_path) {
  BIO* bio = BIO_new_file(pem_path, "r");
  if (bio == NULL) return {};
  X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (cert == NULL) return {};

  int der_len = i2d_X509(cert, NULL);
  std::vector<uint8_t> der(der_len);
  uint8_t* cursor = der.data();
  i2d_X509(cert, &cursor);
  X509_free(cert);
  return der;
}

static bool file_exists(const char* path) {
  std::ifstream file(path);
  return file.good();
}

class PeerVerifyTest : public ::testing::Test {
protected:
  std::vector<uint8_t> ca_der;
  std::string tmp_dir;
  std::string ca_cert_path;

  void SetUp() override {
    // The suite needs a CA certificate in DER form. certs/ca_cert.pem is a
    // gitignored developer artifact that does not exist on a fresh clone, so
    // generate one into a throwaway temp dir via the same ca_generate routine
    // the offs-ca tool exposes. This keeps the test self-contained on every
    // platform instead of depending on a stale file left on disk.
    char tmpl[] = "/tmp/offs-peer-verify-test.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    ASSERT_NE(dir, nullptr);
    tmp_dir = dir;
    ca_cert_path = tmp_dir + "/ca_cert.pem";
    std::string ca_key_path = tmp_dir + "/ca_key.pem";
    ASSERT_EQ(ca_generate("/CN=TestCA", 3650, NULL,
                          ca_cert_path.c_str(), ca_key_path.c_str()), 0);
    ca_der = pem_to_der(ca_cert_path.c_str());
    ASSERT_FALSE(ca_der.empty());
  }

  void TearDown() override {
    // std::filesystem::remove_all works on every platform; the previous
    // system("rm -rf ...") was a silent no-op on Windows (rm is not a
    // command there) and leaked the generated cert temp dir.
    if (!tmp_dir.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(tmp_dir, ec);
    }
  }
};

TEST_F(PeerVerifyTest, NullDataReturnsNull) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(NULL, 0);
  EXPECT_EQ(ctx, nullptr);
}

TEST_F(PeerVerifyTest, ZeroLengthReturnsNull) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), 0);
  EXPECT_EQ(ctx, nullptr);
}

TEST_F(PeerVerifyTest, ValidCACreatesContext) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  ASSERT_NE(ctx, nullptr);
  const char* path = peer_verify_ctx_path(ctx);
  ASSERT_NE(path, nullptr);
  EXPECT_TRUE(file_exists(path));
  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, DestroyRemovesTempFile) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  ASSERT_NE(ctx, nullptr);
  std::string path(peer_verify_ctx_path(ctx));
  ASSERT_TRUE(file_exists(path.c_str()));
  peer_verify_ctx_destroy(ctx);
  EXPECT_FALSE(file_exists(path.c_str()));
}

TEST_F(PeerVerifyTest, CorruptDERReturnsNull) {
  uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03};
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(garbage, sizeof(garbage));
  EXPECT_EQ(ctx, nullptr);
}

TEST_F(PeerVerifyTest, DestroyNullIsSafe) {
  peer_verify_ctx_destroy(NULL);
  SUCCEED();
}

TEST_F(PeerVerifyTest, PathReturnsNullForNull) {
  EXPECT_EQ(peer_verify_ctx_path(NULL), nullptr);
}

TEST_F(PeerVerifyTest, PemFileIsReadable) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  ASSERT_NE(ctx, nullptr);
  const char* path = peer_verify_ctx_path(ctx);

  // Verify the PEM file can be parsed back by OpenSSL
  BIO* bio = BIO_new_file(path, "r");
  ASSERT_NE(bio, nullptr);
  X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  EXPECT_NE(cert, nullptr);
  X509_free(cert);

  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, FromPemFileLoadsCA) {
  // The PEM-file helper is what relay_server_main / wt_transport /
  // offs_client use to convert a configured --ca / ca_path into a
  // peer_verify_ctx. It must produce a context whose temp PEM file
  // parses back to the same CA cert.
  peer_verify_ctx_t* ctx = peer_verify_ctx_create_from_pem_file(ca_cert_path.c_str());
  ASSERT_NE(ctx, nullptr);
  const char* path = peer_verify_ctx_path(ctx);
  ASSERT_NE(path, nullptr);
  EXPECT_TRUE(file_exists(path));

  BIO* bio = BIO_new_file(path, "r");
  ASSERT_NE(bio, nullptr);
  X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  EXPECT_NE(cert, nullptr);
  X509_free(cert);

  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, FromPemFileNullPathReturnsNull) {
  EXPECT_EQ(peer_verify_ctx_create_from_pem_file(NULL), nullptr);
}

TEST_F(PeerVerifyTest, FromPemFileMissingReturnsNull) {
  EXPECT_EQ(peer_verify_ctx_create_from_pem_file("/nonexistent/ca.pem"), nullptr);
}

// Audit #8: peer_verify_extract_pubkey must return the public key in the
// exact byte format the salutation carries (determined by pem_extract_public_key,
// which the salutation encoder uses via authority->public_key). For Ed25519
// that is the raw 32-byte key. Generate a CA + node cert, extract the salutation
// key (pem_extract_public_key) and the cert key (peer_verify_extract_pubkey
// from DER) and assert they are byte-identical — otherwise the pin would
// always mismatch and reject every legitimate peer.
class PeerVerifyExtractPubkeyTest : public PeerVerifyTest {
protected:
  std::string node_cert_path;
  std::string node_key_path;
  std::string csr_path;

  void SetUp() override {
    PeerVerifyTest::SetUp();
    node_cert_path = tmp_dir + "/node_cert.pem";
    node_key_path = tmp_dir + "/node_key.pem";
    csr_path = tmp_dir + "/node.csr";
    std::string ca_key_path = tmp_dir + "/ca_key.pem";
    ASSERT_EQ(ca_generate_csr("TestNode", "ed25519",
                              node_key_path.c_str(), csr_path.c_str()), 0);
    ASSERT_EQ(ca_sign_csr(csr_path.c_str(), ca_cert_path.c_str(),
                          ca_key_path.c_str(), 3650,
                          node_cert_path.c_str()), 0);
  }
};

TEST_F(PeerVerifyExtractPubkeyTest, MatchesSalutationKey_Ed25519) {
  // The salutation encoder path: pem_extract_public_key on the node cert.
  size_t salut_key_len = 0;
  uint8_t* salut_key = pem_extract_public_key(node_cert_path.c_str(), &salut_key_len);
  ASSERT_NE(salut_key, nullptr);
  ASSERT_GT(salut_key_len, 0u);

  // The pin path: peer_verify_extract_pubkey on the DER form of the same cert.
  std::vector<uint8_t> der = pem_to_der(node_cert_path.c_str());
  ASSERT_FALSE(der.empty());
  uint8_t* cert_key = nullptr;
  size_t cert_key_len = 0;
  ASSERT_EQ(peer_verify_extract_pubkey(der.data(), der.size(), &cert_key, &cert_key_len), 0);
  ASSERT_NE(cert_key, nullptr);
  ASSERT_GT(cert_key_len, 0u);

  EXPECT_EQ(cert_key_len, salut_key_len);
  EXPECT_EQ(memcmp(cert_key, salut_key, cert_key_len), 0)
      << "extracted cert key must byte-match the salutation public_key";

  free(salut_key);
  free(cert_key);
}

TEST_F(PeerVerifyExtractPubkeyTest, NullInputsReturnFailure) {
  uint8_t* out_key = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(peer_verify_extract_pubkey(NULL, 10, &out_key, &out_len), -1);
  EXPECT_EQ(peer_verify_extract_pubkey((const uint8_t*)"", 0, &out_key, &out_len), -1);
  std::vector<uint8_t> der = pem_to_der(node_cert_path.c_str());
  ASSERT_FALSE(der.empty());
  EXPECT_EQ(peer_verify_extract_pubkey(der.data(), der.size(), NULL, &out_len), -1);
  EXPECT_EQ(peer_verify_extract_pubkey(der.data(), der.size(), &out_key, NULL), -1);
}

TEST_F(PeerVerifyExtractPubkeyTest, CorruptDERReturnsFailure) {
  uint8_t garbage[] = {0x30, 0x82, 0x00, 0x01, 0xff, 0xfe, 0xfd, 0xfc};
  uint8_t* out_key = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(peer_verify_extract_pubkey(garbage, sizeof(garbage), &out_key, &out_len), -1);
  EXPECT_EQ(out_key, nullptr);
  EXPECT_EQ(out_len, 0u);
}

TEST_F(PeerVerifyExtractPubkeyTest, MismatchedCertKeyIsNotEqual) {
  // A different node cert must yield a different key — otherwise the pin
  // could never detect an impersonation. Generate a second node cert.
  std::string node2_key_path = tmp_dir + "/node2_key.pem";
  std::string node2_csr_path = tmp_dir + "/node2.csr";
  std::string node2_cert_path = tmp_dir + "/node2_cert.pem";
  std::string ca_key_path = tmp_dir + "/ca_key.pem";
  ASSERT_EQ(ca_generate_csr("TestNode2", "ed25519",
                            node2_key_path.c_str(), node2_csr_path.c_str()), 0);
  ASSERT_EQ(ca_sign_csr(node2_csr_path.c_str(), ca_cert_path.c_str(),
                        ca_key_path.c_str(), 3650,
                        node2_cert_path.c_str()), 0);

  size_t key1_len = 0;
  uint8_t* key1 = pem_extract_public_key(node_cert_path.c_str(), &key1_len);
  ASSERT_NE(key1, nullptr);
  std::vector<uint8_t> der2 = pem_to_der(node2_cert_path.c_str());
  ASSERT_FALSE(der2.empty());
  uint8_t* key2 = nullptr;
  size_t key2_len = 0;
  ASSERT_EQ(peer_verify_extract_pubkey(der2.data(), der2.size(), &key2, &key2_len), 0);
  ASSERT_NE(key2, nullptr);

  // Two distinct Ed25519 keys are both 32 bytes — the length matches but the
  // bytes must differ, otherwise the pin could never detect an impersonation.
  EXPECT_EQ(key2_len, key1_len);
  EXPECT_NE(memcmp(key1, key2, key1_len), 0);

  free(key1);
  free(key2);
}

// Audit #8 / tier5b: pem_key_sign_nonce + pem_key_verify_nonce round-trip on
// Ed25519 keys (the key type the relay challenge responder uses). The test
// fixture generates an Ed25519 node cert + key; the public key extracted
// from the cert (raw 32 bytes via pem_extract_public_key) is what the
// challenger would carry, and the private key PEM is what the responder
// signs with. The verify must reconstruct the EVP_PKEY from the raw 32
// bytes — exercising the raw Ed25519 path in pem_key_pubkey_from_bytes.
class PemKeySignVerifyTest : public PeerVerifyExtractPubkeyTest {};

TEST_F(PemKeySignVerifyTest, SignAndVerifyRoundTrip_Ed25519) {
  uint8_t nonce[32];
  for (int i = 0; i < 32; i++) nonce[i] = (uint8_t)(i * 7 + 1);

  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  ASSERT_EQ(pem_key_sign_nonce(node_key_path.c_str(), nonce, &sig, &sig_len), 0);
  ASSERT_NE(sig, nullptr);
  ASSERT_GT(sig_len, 0u);

  size_t pub_len = 0;
  uint8_t* pub = pem_extract_public_key(node_cert_path.c_str(), &pub_len);
  ASSERT_NE(pub, nullptr);
  ASSERT_EQ(pub_len, 32u) << "Ed25519 raw public key must be 32 bytes";

  EXPECT_EQ(pem_key_verify_nonce(pub, pub_len, nonce, sig, sig_len), 0)
      << "signature must verify under the matching public key";

  free(pub);
  free(sig);
}

TEST_F(PemKeySignVerifyTest, VerifyRejectsWrongPublicKey) {
  uint8_t nonce[32];
  for (int i = 0; i < 32; i++) nonce[i] = (uint8_t)(0xFF ^ i);

  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  ASSERT_EQ(pem_key_sign_nonce(node_key_path.c_str(), nonce, &sig, &sig_len), 0);
  ASSERT_NE(sig, nullptr);

  // Generate a second distinct key pair and verify with that public key —
  // the signature was made under node_key, so node2's public key must reject.
  std::string node2_key_path = tmp_dir + "/node2_key.pem";
  std::string node2_csr_path = tmp_dir + "/node2.csr";
  std::string node2_cert_path = tmp_dir + "/node2_cert.pem";
  std::string ca_key_path = tmp_dir + "/ca_key.pem";
  ASSERT_EQ(ca_generate_csr("TestNode2", "ed25519",
                            node2_key_path.c_str(), node2_csr_path.c_str()), 0);
  ASSERT_EQ(ca_sign_csr(node2_csr_path.c_str(), ca_cert_path.c_str(),
                        ca_key_path.c_str(), 3650,
                        node2_cert_path.c_str()), 0);
  size_t pub2_len = 0;
  uint8_t* pub2 = pem_extract_public_key(node2_cert_path.c_str(), &pub2_len);
  ASSERT_NE(pub2, nullptr);
  ASSERT_EQ(pub2_len, 32u);

  EXPECT_EQ(pem_key_verify_nonce(pub2, pub2_len, nonce, sig, sig_len), -1)
      << "signature under a wrong public key must be rejected";

  free(pub2);
  free(sig);
}

TEST_F(PemKeySignVerifyTest, VerifyRejectsTamperedSignature) {
  uint8_t nonce[32];
  for (int i = 0; i < 32; i++) nonce[i] = (uint8_t)(i + 1);

  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  ASSERT_EQ(pem_key_sign_nonce(node_key_path.c_str(), nonce, &sig, &sig_len), 0);
  ASSERT_NE(sig, nullptr);
  ASSERT_GE(sig_len, 2u);

  // Flip a byte in the middle of the signature — Ed25519 is all-or-nothing,
  // any change invalidates the whole signature.
  sig[sig_len / 2] ^= 0xAA;

  size_t pub_len = 0;
  uint8_t* pub = pem_extract_public_key(node_cert_path.c_str(), &pub_len);
  ASSERT_NE(pub, nullptr);
  ASSERT_EQ(pub_len, 32u);

  EXPECT_EQ(pem_key_verify_nonce(pub, pub_len, nonce, sig, sig_len), -1)
      << "tampered signature must be rejected";

  free(pub);
  free(sig);
}

TEST_F(PemKeySignVerifyTest, VerifyRejectsTamperedNonce) {
  uint8_t nonce[32];
  for (int i = 0; i < 32; i++) nonce[i] = (uint8_t)(i + 1);

  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  ASSERT_EQ(pem_key_sign_nonce(node_key_path.c_str(), nonce, &sig, &sig_len), 0);
  ASSERT_NE(sig, nullptr);

  // Verify against a different nonce — the signature is bound to the
  // original nonce, so a different nonce must fail.
  uint8_t wrong_nonce[32];
  for (int i = 0; i < 32; i++) wrong_nonce[i] = (uint8_t)(i + 2);

  size_t pub_len = 0;
  uint8_t* pub = pem_extract_public_key(node_cert_path.c_str(), &pub_len);
  ASSERT_NE(pub, nullptr);
  ASSERT_EQ(pub_len, 32u);

  EXPECT_EQ(pem_key_verify_nonce(pub, pub_len, wrong_nonce, sig, sig_len), -1)
      << "signature over a different nonce must be rejected";
  // Sanity: the original nonce still verifies.
  EXPECT_EQ(pem_key_verify_nonce(pub, pub_len, nonce, sig, sig_len), 0);

  free(pub);
  free(sig);
}

TEST_F(PemKeySignVerifyTest, SignRejectsNullInputs) {
  uint8_t nonce[32] = {0};
  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  EXPECT_EQ(pem_key_sign_nonce(NULL, nonce, &sig, &sig_len), -1);
  EXPECT_EQ(pem_key_sign_nonce(node_key_path.c_str(), NULL, &sig, &sig_len), -1);
  EXPECT_EQ(pem_key_sign_nonce(node_key_path.c_str(), nonce, NULL, &sig_len), -1);
  EXPECT_EQ(pem_key_sign_nonce(node_key_path.c_str(), nonce, &sig, NULL), -1);
}

TEST_F(PemKeySignVerifyTest, SignRejectsMissingKeyFile) {
  uint8_t nonce[32] = {0};
  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  EXPECT_EQ(pem_key_sign_nonce("/nonexistent/key.pem", nonce, &sig, &sig_len), -1);
  EXPECT_EQ(sig, nullptr);
  EXPECT_EQ(sig_len, 0u);
}

TEST_F(PemKeySignVerifyTest, VerifyRejectsNullInputs) {
  uint8_t nonce[32] = {0};
  uint8_t pub[32] = {0};
  uint8_t sig[64] = {0};
  EXPECT_EQ(pem_key_verify_nonce(NULL, 32, nonce, sig, sizeof(sig)), -1);
  EXPECT_EQ(pem_key_verify_nonce(pub, 0, nonce, sig, sizeof(sig)), -1);
  EXPECT_EQ(pem_key_verify_nonce(pub, 32, NULL, sig, sizeof(sig)), -1);
  EXPECT_EQ(pem_key_verify_nonce(pub, 32, nonce, NULL, sizeof(sig)), -1);
  EXPECT_EQ(pem_key_verify_nonce(pub, 32, nonce, sig, 0), -1);
}

TEST_F(PemKeySignVerifyTest, VerifyRejectsGarbagePublicKey) {
  uint8_t nonce[32] = {0};
  uint8_t garbage[16] = {0xFF};  // Not 32 (Ed25519) or 57 (Ed448), not valid DER
  uint8_t sig[64] = {0};
  EXPECT_EQ(pem_key_verify_nonce(garbage, sizeof(garbage), nonce, sig, sizeof(sig)), -1);
}

// authority_sign_nonce uses the cached node_private_key (loaded once during
// authority_init_local_id from node_key_path) to sign a 32-byte nonce. The
// signature must verify under the matching public key extracted from the
// cert. This is the API the relay responder (Task 3) calls.
TEST_F(PemKeySignVerifyTest, AuthoritySignNonceVerifiesWithCachedKey) {
  config_t config = config_default();
  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);
  authority->node_cert_path = strdup(node_cert_path.c_str());
  authority->node_key_path = strdup(node_key_path.c_str());

  // authority_init_local_id loads the cert's public key AND caches the
  // private key (EVP_PKEY) into authority->node_private_key.
  ASSERT_EQ(authority_init_local_id(authority), 0);
  ASSERT_NE(authority->node_private_key, nullptr)
      << "private key must be cached on authority after init";
  ASSERT_NE(authority->public_key, nullptr);
  ASSERT_EQ(authority->public_key_len, 32u);

  uint8_t nonce[32];
  for (int i = 0; i < 32; i++) nonce[i] = (uint8_t)(0x42 + i);

  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  ASSERT_EQ(authority_sign_nonce(authority, nonce, &sig, &sig_len), 0);
  ASSERT_NE(sig, nullptr);
  ASSERT_GT(sig_len, 0u);

  // Verify under the authority's own cached public key (raw Ed25519 bytes).
  EXPECT_EQ(pem_key_verify_nonce(authority->public_key, authority->public_key_len,
                                  nonce, sig, sig_len), 0)
      << "authority_sign_nonce output must verify under the cached public key";

  free(sig);
  authority_destroy(authority);
}

TEST_F(PemKeySignVerifyTest, AuthoritySignNonceRejectsNullAuthority) {
  uint8_t nonce[32] = {0};
  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  EXPECT_EQ(authority_sign_nonce(NULL, nonce, &sig, &sig_len), -1);
}

TEST_F(PemKeySignVerifyTest, AuthoritySignNonceFailsWithoutKey) {
  config_t config = config_default();
  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);
  // No node_key_path set — sign must fail.
  uint8_t nonce[32] = {0};
  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  EXPECT_EQ(authority_sign_nonce(authority, nonce, &sig, &sig_len), -1);
  EXPECT_EQ(sig, nullptr);
  authority_destroy(authority);
}

TEST_F(PemKeySignVerifyTest, AuthorityDestroyFreesCachedPrivateKey) {
  // Sanity: authority_destroy must not leak the cached EVP_PKEY. Run under
  // valgrind to catch the leak; this test just exercises the path.
  config_t config = config_default();
  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);
  authority->node_cert_path = strdup(node_cert_path.c_str());
  authority->node_key_path = strdup(node_key_path.c_str());
  ASSERT_EQ(authority_init_local_id(authority), 0);
  ASSERT_NE(authority->node_private_key, nullptr);
  authority_destroy(authority);
}
