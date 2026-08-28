#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

extern "C" {
#include "../src/QR/qr.h"
}

namespace qr_test {

/* Build a binary P6 PPM of the given dimensions filled with the given gray
   value (RGB triplets r=g=b=gray). Returns malloc'd buffer; caller frees. */
static uint8_t* _make_ppm(int width, int height, uint8_t gray, size_t* out_len) {
  char header[64];
  int header_len = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", width, height);
  size_t pixel_len = (size_t)width * height * 3;
  uint8_t* ppm = (uint8_t*)malloc(header_len + pixel_len);
  if (ppm == NULL) return NULL;
  memcpy(ppm, header, header_len);
  for (size_t i = 0; i < pixel_len; i++) ppm[header_len + i] = gray;
  *out_len = header_len + pixel_len;
  return ppm;
}

TEST(QrEncode, RejectsNullAndEmpty) {
  size_t len = 0;
  EXPECT_TRUE(qr_encode_to_ppm(NULL, 10, &len) == NULL);
  uint8_t one = 0x01;
  EXPECT_TRUE(qr_encode_to_ppm(&one, 0, &len) == NULL);
}

TEST(QrRoundTrip, PayloadSurvivesEncodeDecode) {
  /* 64 pseudo-random bytes (deterministic LCG so the test never flakes) */
  uint8_t payload[64];
  uint32_t state = 12345;
  for (size_t i = 0; i < sizeof(payload); i++) {
    state = state * 1103515245 + 12345;
    payload[i] = (uint8_t)(state >> 16);
  }

  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(payload, sizeof(payload), &ppm_len);
  ASSERT_NE(ppm, nullptr);
  ASSERT_GT(ppm_len, 0u);
  /* Generated images are binary P6 */
  EXPECT_EQ(0, memcmp(ppm, "P6\n", 3));

  size_t decoded_len = 0;
  uint8_t* decoded = qr_decode_from_ppm(ppm, ppm_len, &decoded_len);
  free(ppm);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded_len, sizeof(payload));
  EXPECT_EQ(0, memcmp(decoded, payload, sizeof(payload)));
  free(decoded);
}

TEST(QrDecode, RejectsBadMagic) {
  const char* not_ppm = "P5\n1 1\n255\n";
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm((const uint8_t*)not_ppm, strlen(not_ppm), &len) == NULL);
}

TEST(QrDecode, RejectsWrongMaxval) {
  const char* ppm = "P6\n1 1\n65535\n";
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm((const uint8_t*)ppm, strlen(ppm), &len) == NULL);
}

TEST(QrDecode, RejectsTruncatedPixels) {
  size_t full_len = 0;
  uint8_t* ppm = _make_ppm(10, 10, 255, &full_len);
  ASSERT_NE(ppm, nullptr);
  size_t header_len = full_len - 10u * 10u * 3u;
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm(ppm, header_len + 10, &len) == NULL);
  free(ppm);
}

TEST(QrDecode, NoQrCodeInBlankImage) {
  size_t ppm_len = 0;
  uint8_t* ppm = _make_ppm(200, 200, 255, &ppm_len);
  ASSERT_NE(ppm, nullptr);
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm(ppm, ppm_len, &len) == NULL);
  free(ppm);
}

TEST(QrDecode, RejectsNullAndEmpty) {
  size_t len = 0;
  EXPECT_TRUE(qr_decode_from_ppm(NULL, 10, &len) == NULL);
  EXPECT_TRUE(qr_decode_from_ppm((const uint8_t*)"P6\n1 1\n255\n", 0, &len) == NULL);
}

TEST(QrDecode, OutLenUntouchedOnFailure) {
  size_t len = 42;
  const char* garbage = "not a ppm";
  EXPECT_TRUE(qr_decode_from_ppm((const uint8_t*)garbage, strlen(garbage), &len) == NULL);
  EXPECT_EQ(42u, len);
}

TEST(QrRoundTrip, SingleBytePayload) {
  const uint8_t payload = 0xAB;
  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(&payload, 1, &ppm_len);
  ASSERT_NE(ppm, nullptr);
  size_t decoded_len = 0;
  uint8_t* decoded = qr_decode_from_ppm(ppm, ppm_len, &decoded_len);
  free(ppm);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(1u, decoded_len);
  EXPECT_EQ(payload, decoded[0]);
  free(decoded);
}

TEST(QrRoundTrip, MaxCapacityPayload) {
  /* Byte-mode EC-M capacity for the highest QR version libqrencode emits
     (version 40): 2331 bytes. This is the exact regime where quirc's
     region budget used to be exhausted, so it guards the QUIRC_MAX_REGIONS
     wiring in CMakeLists.txt. */
  static const size_t kPayloadLen = 2331;
  uint8_t payload[kPayloadLen];
  uint32_t state = 987654321;
  for (size_t i = 0; i < kPayloadLen; i++) {
    state = state * 1103515245 + 12345;
    payload[i] = (uint8_t)(state >> 16);
  }

  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(payload, kPayloadLen, &ppm_len);
  ASSERT_NE(ppm, nullptr);

  size_t decoded_len = 0;
  uint8_t* decoded = qr_decode_from_ppm(ppm, ppm_len, &decoded_len);
  free(ppm);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(kPayloadLen, decoded_len);
  EXPECT_EQ(0, memcmp(decoded, payload, kPayloadLen));
  free(decoded);
}

}  // namespace qr_test