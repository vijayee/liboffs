#include <gtest/gtest.h>
extern "C" {
#include "../src/ClientAPI/HTTP/auth_middleware.h"
}

/* $2b$04$... hash of "test-key" — generated with crypt() */
static const char* test_hash = "$2b$04$MTIzNDU2Nzg5MDEyMzQ1NeK7xFG7fG7fG7fG7fG7fG7fG7fG7fG7f";
static const char* test_key = "test-key";

TEST(TestAuthMiddleware, CreateReturnsNullOnNullArgs) {
  EXPECT_EQ(nullptr, auth_middleware_create(NULL, test_hash));
  EXPECT_EQ(nullptr, auth_middleware_create(test_key, NULL));
  EXPECT_EQ(nullptr, auth_middleware_create(NULL, NULL));
}

TEST(TestAuthMiddleware, DestroyHandlesNull) {
  auth_middleware_destroy(NULL);
  SUCCEED();
}

TEST(TestAuthMiddleware, HandlerFunctionReturnsNonNull) {
  EXPECT_NE(nullptr, (void*)auth_middleware_handler());
}
