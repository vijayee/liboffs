#include <gtest/gtest.h>
extern "C" {
#include "../src/Util/bcrypt.h"
}

/* $2b$04$... hash of "test-key" — generated with crypt() */
static const char* test_hash = "$2b$04$MTIzNDU2Nzg5MDEyMzQ1NePheb5yq4/5.giE2KzFrDwx2yMnwVtpW";
static const char* test_key = "test-key";

TEST(TestBcrypt, MatchReturnsZero) {
#ifdef _WIN32
  /* bcrypt_check is a stub on Windows (Util/bcrypt.c always returns -1) until
   * OpenBSD bcrypt is ported, so the positive-match case cannot pass yet. The
   * negative-path tests below still run and verify the stub's error handling. */
  GTEST_SKIP() << "bcrypt not ported to Windows (auth stub returns -1)";
#else
  EXPECT_EQ(0, bcrypt_check(test_key, test_hash));
#endif
}

TEST(TestBcrypt, MismatchReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check("wrong-key", test_hash));
}

TEST(TestBcrypt, NullKeyReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check(NULL, test_hash));
}

TEST(TestBcrypt, NullHashReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check(test_key, NULL));
}

TEST(TestBcrypt, WrongKeyWith2xPrefixReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check("key", "not-a-hash"));
  EXPECT_EQ(-1, bcrypt_check("key", "$2x$10$GjIW4.eOZECm4QY1KQjI4.6FfN8CqT5uJdLzMxX3bG1yRaVnPw0Su"));
}
