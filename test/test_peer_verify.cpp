#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <cstdio>
#include <string>
#include <filesystem>

extern "C" {
#include "../src/Network/peer_verify.h"
#include "../src/Network/pem_key.h"
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
