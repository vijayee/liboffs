#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "../src/Util/bcrypt.h"
}

/* $2b$04$... hash of "test-key" — generated with crypt() */
static const char* test_hash = "$2b$04$MTIzNDU2Nzg5MDEyMzQ1NePheb5yq4/5.giE2KzFrDwx2yMnwVtpW";
static const char* test_key = "test-key";

TEST(TestBcrypt, MatchReturnsZero) {
  EXPECT_EQ(0, bcrypt_check(test_key, test_hash));
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

/* A truncated stored hash must not match a prefix of the recomputed hash.
   The old strcmp-based compare also rejected this (strcmp returns non-zero
   when lengths differ), but a naive constant-time loop that only compared
   strlen(out) bytes would accept a truncated stored hash. */
TEST(TestBcrypt, TruncatedHashRejected) {
  char truncated[40];
  strncpy(truncated, test_hash, sizeof(truncated) - 1);
  truncated[sizeof(truncated) - 1] = '\0';
  EXPECT_EQ(-1, bcrypt_check(test_key, truncated));
}

/* A hash that differs only in the last byte must still be rejected — guards
   against an off-by-one in the constant-time loop that compares the full
   recomputed length. */
TEST(TestBcrypt, LastByteDifferenceRejected) {
  char modified[64];
  strncpy(modified, test_hash, sizeof(modified));
  modified[sizeof(modified) - 1] = '\0';
  size_t len = strlen(modified);
  ASSERT_GT(len, (size_t)0);
  modified[len - 1] = (modified[len - 1] == 'a' ? 'b' : 'a');
  EXPECT_EQ(-1, bcrypt_check(test_key, modified));
}

TEST(TestBcryptGenerate, Produces2bPrefixedHashThatVerifies) {
  char out[64];
  ASSERT_EQ(0, bcrypt_generate(test_key, 4, out, sizeof(out)));
  /* Must be a $2b$ hash, zero-padded two-digit cost, NUL-terminated. */
  EXPECT_EQ(0, strncmp(out, "$2b$04$", 7));
  /* The generated hash must verify against the original key. */
  EXPECT_EQ(0, bcrypt_check(test_key, out));
  /* And must NOT verify against a different key. */
  EXPECT_EQ(-1, bcrypt_check("other-key", out));
}

TEST(TestBcryptGenerate, DifferentSaltsProduceDifferentHashes) {
  char out1[64];
  char out2[64];
  ASSERT_EQ(0, bcrypt_generate(test_key, 4, out1, sizeof(out1)));
  ASSERT_EQ(0, bcrypt_generate(test_key, 4, out2, sizeof(out2)));
  /* Same key, fresh random salt each time -> different hashes, both verify. */
  EXPECT_STRNE(out1, out2);
  EXPECT_EQ(0, bcrypt_check(test_key, out1));
  EXPECT_EQ(0, bcrypt_check(test_key, out2));
}

TEST(TestBcryptGenerate, RejectsInvalidCost) {
  char out[64];
  EXPECT_EQ(-1, bcrypt_generate(test_key, 3, out, sizeof(out)));   /* too low */
  EXPECT_EQ(-1, bcrypt_generate(test_key, 32, out, sizeof(out)));  /* too high */
}

TEST(TestBcryptGenerate, RejectsBadArgs) {
  char out[64];
  EXPECT_EQ(-1, bcrypt_generate(NULL, 4, out, sizeof(out)));
  EXPECT_EQ(-1, bcrypt_generate(test_key, 4, NULL, sizeof(out)));
  EXPECT_EQ(-1, bcrypt_generate(test_key, 4, out, 16)); /* buffer too small */
}
