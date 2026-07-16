#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <cstdio>
#include <string>
#include <filesystem>

extern "C" {
#include "../src/Network/peer_verify.h"
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
