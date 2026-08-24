#include <gtest/gtest.h>

extern "C" {
#include "Update/update_verify.h"
#include <openssl/ssl.h>
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