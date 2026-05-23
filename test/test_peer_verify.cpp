#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <cstdio>

extern "C" {
#include "../src/Network/peer_verify.h"
#include "../src/Util/allocator.h"
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

class PeerVerifyTest : public ::testing::Test {
protected:
  std::vector<uint8_t> ca_der;
  std::vector<uint8_t> leaf_der;
  std::vector<uint8_t> other_ca_der;

  void SetUp() override {
    ca_der = pem_to_der("test/certs/ca_cert.pem");
    leaf_der = pem_to_der("test/certs/leaf_cert.pem");
    other_ca_der = pem_to_der("test/certs/other_ca_cert.pem");
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
  EXPECT_NE(ctx, nullptr);
  peer_verify_ctx_destroy(ctx);
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

#ifdef HAS_MSQUIC
#include <msquic.h>

TEST_F(PeerVerifyTest, ValidLeafPasses) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  ASSERT_NE(ctx, nullptr);

  QUIC_CERTIFICATE cert;
  memset(&cert, 0, sizeof(cert));
  cert.Certificate = leaf_der.data();
  cert.CertificateLength = (uint32_t)leaf_der.size();

  EXPECT_EQ(peer_verify_validate(ctx, &cert), 0);
  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, WrongCAFails) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(other_ca_der.data(), other_ca_der.size());
  ASSERT_NE(ctx, nullptr);

  QUIC_CERTIFICATE cert;
  memset(&cert, 0, sizeof(cert));
  cert.Certificate = leaf_der.data();
  cert.CertificateLength = (uint32_t)leaf_der.size();

  EXPECT_NE(peer_verify_validate(ctx, &cert), 0);
  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, NullCertificateFails) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  ASSERT_NE(ctx, nullptr);
  EXPECT_NE(peer_verify_validate(ctx, NULL), 0);
  peer_verify_ctx_destroy(ctx);
}
#endif // HAS_MSQUIC
