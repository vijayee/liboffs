#include <gtest/gtest.h>
extern "C" {
#include "../src/Util/bcrypt.h"
}

/* $2b$04$... hash of "test-key" — generated with crypt() */
static const char* test_hash = "$2b$04$MTIzNDU2Nzg5MDEyMzQ1NePheb5yq4/5.giE2KzFrDwx2yMnwVtpW";
static const char* test_key = "test-key";

TEST(Bcrypt, MatchReturnsZero) {
  EXPECT_EQ(0, bcrypt_check(test_key, test_hash));
}

TEST(Bcrypt, MismatchReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check("wrong-key", test_hash));
}

TEST(Bcrypt, NullKeyReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check(NULL, test_hash));
}

TEST(Bcrypt, NullHashReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check(test_key, NULL));
}

TEST(Bcrypt, InvalidPrefixReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check("key", "not-a-hash"));
  EXPECT_EQ(-1, bcrypt_check("key", "$2x$10$GjIW4.eOZECm4QY1KQjI4.6FfN8CqT5uJdLzMxX3bG1yRaVnPw0Su"));
}
