#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "../src/Util/base58.h"
#include "../src/Util/allocator.h"
}

static void _round_trip(size_t input_len) {
  uint8_t* input = (uint8_t*)get_clear_memory(input_len);
  ASSERT_NE(input, (uint8_t*)NULL);
  for (size_t i = 0; i < input_len; i++) {
    input[i] = (uint8_t)((i * 37 + 11) & 0xFF);
  }
  size_t b58_len = base58_encoded_length(input_len);
  char* encoded = (char*)get_clear_memory(b58_len + 1);
  ASSERT_NE(encoded, (char*)NULL);

  int encoded_len = base58_encode(input, input_len, encoded, b58_len + 1);
  ASSERT_GT(encoded_len, 0);

  uint8_t* decoded = (uint8_t*)get_clear_memory(input_len);
  ASSERT_NE(decoded, (uint8_t*)NULL);
  size_t bytes_written = 0;
  ASSERT_EQ(base58_decode(encoded, decoded, input_len, &bytes_written), 0);
  EXPECT_EQ(bytes_written, input_len);
  EXPECT_EQ(memcmp(decoded, input, input_len), 0);

  free(input);
  free(encoded);
  free(decoded);
}

/* IPv6-era peer_info CBOR (v6 candidates + SRFLX + RELAY) crosses the old
   512-byte BIGINT_SIZE/2 encode limit — base58_encode returned -1 and
   /peer/info 500'd on rich-candidate nodes. */
TEST(TestBase58, EncodesIPv6EraPeerInfoSizes) {
  _round_trip(514);   /* the exact size that broke the Azure bootstrap */
  _round_trip(600);
  _round_trip(1024);
}

TEST(TestBase58, OldLimitSizesStillRoundTrip) {
  _round_trip(1);
  _round_trip(33);            /* 32-byte hash region */
  _round_trip(337);           /* pre-IPv6 peer_info CBOR size */
}

TEST(TestBase58, OversizedInputStillRejected) {
  uint8_t* input = (uint8_t*)get_clear_memory(3000);
  ASSERT_NE(input, (uint8_t*)NULL);
  memset(input, 0xAB, 3000);
  char output[64];
  /* 3000 > BIGINT_SIZE/2 = 2048 — must be rejected, not truncated. */
  EXPECT_EQ(base58_encode(input, 3000, output, sizeof(output)), -1);
  free(input);
}