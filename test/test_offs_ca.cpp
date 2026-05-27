#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include "../../tools/offs-ca/ca_ops.h"

#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
}

class TestOffsCa : public ::testing::Test {
protected:
  std::string tmp_dir;

  void SetUp() override {
    char tmpl[] = "/tmp/offs-ca-test.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    ASSERT_NE(dir, nullptr);
    tmp_dir = dir;
  }

  void TearDown() override {
    std::string cmd = "rm -rf " + tmp_dir;
    (void)system(cmd.c_str());
  }

  std::string path(const char* name) const {
    return tmp_dir + "/" + name;
  }

  bool file_exists(const std::string& path) const {
    std::ifstream f(path);
    return f.good();
  }

  static bool verify_cert_chain(const std::string& cert_path,
                                 const std::string& ca_path) {
    BIO* cert_bio = BIO_new_file(cert_path.c_str(), "r");
    if (!cert_bio) return false;
    X509* cert = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    BIO_free(cert_bio);
    if (!cert) return false;

    BIO* ca_bio = BIO_new_file(ca_path.c_str(), "r");
    if (!ca_bio) { X509_free(cert); return false; }
    X509* ca = PEM_read_bio_X509(ca_bio, NULL, NULL, NULL);
    BIO_free(ca_bio);
    if (!ca) { X509_free(cert); return false; }

    EVP_PKEY* ca_key = X509_get_pubkey(ca);
    if (!ca_key) { X509_free(cert); X509_free(ca); return false; }

    int result = X509_verify(cert, ca_key);

    EVP_PKEY_free(ca_key);
    X509_free(cert);
    X509_free(ca);
    return result == 1;
  }
};

/* ---- Ed25519 (default) ---- */

TEST_F(TestOffsCa, GenerateCaCert) {
  std::string cert_path = path("ca_cert.pem");
  std::string key_path = path("ca_key.pem");

  int ret = ca_generate("/CN=TestCA", 3650, NULL, cert_path.c_str(), key_path.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(cert_path));
  EXPECT_TRUE(file_exists(key_path));
}

TEST_F(TestOffsCa, GenerateCaCertRejectsNullPath) {
  int ret = ca_generate("/CN=TestCA", 3650, NULL, "valid.pem", NULL);
  EXPECT_EQ(ret, -1);

  ret = ca_generate("/CN=TestCA", 3650, NULL, NULL, "valid.pem");
  EXPECT_EQ(ret, -1);
}

TEST_F(TestOffsCa, GenerateNodeCsr) {
  std::string key_path = path("node_key.pem");
  std::string csr_path = path("node.csr");

  int ret = ca_generate_csr("test-node", NULL, key_path.c_str(), csr_path.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(key_path));
  EXPECT_TRUE(file_exists(csr_path));
}

TEST_F(TestOffsCa, GenerateNodeCsrRejectsNull) {
  int ret = ca_generate_csr(NULL, NULL, "key.pem", "csr.pem");
  EXPECT_EQ(ret, -1);

  ret = ca_generate_csr("test", NULL, NULL, "csr.pem");
  EXPECT_EQ(ret, -1);

  ret = ca_generate_csr("test", NULL, "key.pem", NULL);
  EXPECT_EQ(ret, -1);
}

TEST_F(TestOffsCa, SignCsrAndVerify) {
  std::string ca_cert = path("ca_cert.pem");
  std::string ca_key = path("ca_key.pem");
  ASSERT_EQ(ca_generate("/CN=TestCA", 3650, NULL, ca_cert.c_str(), ca_key.c_str()), 0);

  std::string node_key = path("node_key.pem");
  std::string node_csr = path("node.csr");
  ASSERT_EQ(ca_generate_csr("test-node", NULL, node_key.c_str(), node_csr.c_str()), 0);

  std::string node_cert = path("node_cert.pem");
  int ret = ca_sign_csr(node_csr.c_str(), ca_cert.c_str(), ca_key.c_str(),
                         365, node_cert.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(node_cert));
  EXPECT_TRUE(verify_cert_chain(node_cert, ca_cert));
}

TEST_F(TestOffsCa, SignCsrRejectsNullParams) {
  int ret = ca_sign_csr(NULL, "ca.pem", "key.pem", 365, "cert.pem");
  EXPECT_EQ(ret, -1);

  ret = ca_sign_csr("csr.pem", NULL, "key.pem", 365, "cert.pem");
  EXPECT_EQ(ret, -1);

  ret = ca_sign_csr("csr.pem", "ca.pem", NULL, 365, "cert.pem");
  EXPECT_EQ(ret, -1);

  ret = ca_sign_csr("csr.pem", "ca.pem", "key.pem", 365, NULL);
  EXPECT_EQ(ret, -1);
}

TEST_F(TestOffsCa, SignCsrRejectsInvalidCsr) {
  std::string ca_cert = path("ca_cert.pem");
  std::string ca_key = path("ca_key.pem");
  ASSERT_EQ(ca_generate("/CN=TestCA", 3650, NULL, ca_cert.c_str(), ca_key.c_str()), 0);

  std::string bad_csr = path("bad.csr");
  FILE* f = fopen(bad_csr.c_str(), "w");
  ASSERT_NE(f, nullptr);
  fputs("not a valid CSR", f);
  fclose(f);

  std::string node_cert = path("node_cert.pem");
  int ret = ca_sign_csr(bad_csr.c_str(), ca_cert.c_str(), ca_key.c_str(),
                         365, node_cert.c_str());
  EXPECT_EQ(ret, -1);
}

TEST_F(TestOffsCa, SignCsrWithMismatchedCaKeyProducesUnverifiableCert) {
  std::string ca1_cert = path("ca1_cert.pem");
  std::string ca1_key = path("ca1_key.pem");
  ASSERT_EQ(ca_generate("/CN=CA1", 3650, NULL, ca1_cert.c_str(), ca1_key.c_str()), 0);

  std::string ca2_cert = path("ca2_cert.pem");
  std::string ca2_key = path("ca2_key.pem");
  ASSERT_EQ(ca_generate("/CN=CA2", 3650, NULL, ca2_cert.c_str(), ca2_key.c_str()), 0);

  std::string node_key = path("node_key.pem");
  std::string node_csr = path("node.csr");
  ASSERT_EQ(ca_generate_csr("test-node", NULL, node_key.c_str(), node_csr.c_str()), 0);

  std::string node_cert = path("node_cert.pem");
  int ret = ca_sign_csr(node_csr.c_str(), ca1_cert.c_str(), ca2_key.c_str(),
                         365, node_cert.c_str());
  ASSERT_EQ(ret, 0);
  EXPECT_FALSE(verify_cert_chain(node_cert, ca1_cert));
}

TEST_F(TestOffsCa, MultipleNodes) {
  std::string ca_cert = path("ca_cert.pem");
  std::string ca_key = path("ca_key.pem");
  ASSERT_EQ(ca_generate("/CN=TestCA", 3650, NULL, ca_cert.c_str(), ca_key.c_str()), 0);

  for (int i = 0; i < 3; i++) {
    char name[32];
    snprintf(name, sizeof(name), "node-%d", i);

    std::string node_key = path((std::string(name) + "_key.pem").c_str());
    std::string node_csr = path((std::string(name) + ".csr").c_str());
    std::string node_cert = path((std::string(name) + "_cert.pem").c_str());

    ASSERT_EQ(ca_generate_csr(name, NULL, node_key.c_str(), node_csr.c_str()), 0);
    ASSERT_EQ(ca_sign_csr(node_csr.c_str(), ca_cert.c_str(), ca_key.c_str(),
                           365, node_cert.c_str()), 0);

    EXPECT_TRUE(verify_cert_chain(node_cert, ca_cert));
  }
}

/* ---- RSA key types ---- */

TEST_F(TestOffsCa, GenerateCaCertRsa) {
  std::string cert_path = path("ca_cert.pem");
  std::string key_path = path("ca_key.pem");

  int ret = ca_generate("/CN=TestCA", 3650, "rsa", cert_path.c_str(), key_path.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(cert_path));
  EXPECT_TRUE(file_exists(key_path));
}

TEST_F(TestOffsCa, GenerateCaCertRsa4096) {
  std::string cert_path = path("ca_cert.pem");
  std::string key_path = path("ca_key.pem");

  int ret = ca_generate("/CN=TestCA", 3650, "rsa4096", cert_path.c_str(), key_path.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(cert_path));
  EXPECT_TRUE(file_exists(key_path));
}

TEST_F(TestOffsCa, RsaSignCsrAndVerify) {
  std::string ca_cert = path("ca_cert.pem");
  std::string ca_key = path("ca_key.pem");
  ASSERT_EQ(ca_generate("/CN=TestCA", 3650, "rsa", ca_cert.c_str(), ca_key.c_str()), 0);

  std::string node_key = path("node_key.pem");
  std::string node_csr = path("node.csr");
  ASSERT_EQ(ca_generate_csr("test-node", "rsa", node_key.c_str(), node_csr.c_str()), 0);

  std::string node_cert = path("node_cert.pem");
  ASSERT_EQ(ca_sign_csr(node_csr.c_str(), ca_cert.c_str(), ca_key.c_str(),
                         365, node_cert.c_str()), 0);

  EXPECT_TRUE(verify_cert_chain(node_cert, ca_cert));
}

/* ---- EC key types ---- */

TEST_F(TestOffsCa, GenerateCaCertPrime256v1) {
  std::string cert_path = path("ca_cert.pem");
  std::string key_path = path("ca_key.pem");

  int ret = ca_generate("/CN=TestCA", 3650, "prime256v1",
                        cert_path.c_str(), key_path.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(cert_path));
  EXPECT_TRUE(file_exists(key_path));
}

TEST_F(TestOffsCa, GenerateCaCertSecp384r1) {
  std::string cert_path = path("ca_cert.pem");
  std::string key_path = path("ca_key.pem");

  int ret = ca_generate("/CN=TestCA", 3650, "secp384r1",
                        cert_path.c_str(), key_path.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(cert_path));
  EXPECT_TRUE(file_exists(key_path));
}

TEST_F(TestOffsCa, GenerateCaCertSecp521r1) {
  std::string cert_path = path("ca_cert.pem");
  std::string key_path = path("ca_key.pem");

  int ret = ca_generate("/CN=TestCA", 3650, "secp521r1",
                        cert_path.c_str(), key_path.c_str());
  ASSERT_EQ(ret, 0);

  EXPECT_TRUE(file_exists(cert_path));
  EXPECT_TRUE(file_exists(key_path));
}

TEST_F(TestOffsCa, EcSignCsrAndVerify) {
  std::string ca_cert = path("ca_cert.pem");
  std::string ca_key = path("ca_key.pem");
  ASSERT_EQ(ca_generate("/CN=TestCA", 3650, "prime256v1",
                        ca_cert.c_str(), ca_key.c_str()), 0);

  std::string node_key = path("node_key.pem");
  std::string node_csr = path("node.csr");
  ASSERT_EQ(ca_generate_csr("test-node", "prime256v1",
                            node_key.c_str(), node_csr.c_str()), 0);

  std::string node_cert = path("node_cert.pem");
  ASSERT_EQ(ca_sign_csr(node_csr.c_str(), ca_cert.c_str(), ca_key.c_str(),
                         365, node_cert.c_str()), 0);

  EXPECT_TRUE(verify_cert_chain(node_cert, ca_cert));
}

/* ---- Error handling ---- */

TEST_F(TestOffsCa, RejectUnknownKeyType) {
  std::string cert_path = path("ca_cert.pem");
  std::string key_path = path("ca_key.pem");

  int ret = ca_generate("/CN=TestCA", 3650, "dsa",
                        cert_path.c_str(), key_path.c_str());
  EXPECT_EQ(ret, -1);
}
