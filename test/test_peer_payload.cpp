#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "../src/ClientAPI/peer_handlers.h"
#include "../src/QR/qr.h"
#include "../src/Network/peer_info.h"
#include <cbor.h>
}

namespace peer_payload_test {

/* A minimal valid peer_info: one DIRECT address, 4-byte public key. The
   public_key is heap-copied because peer_info_destroy() frees it. */
static peer_info_t _make_info() {
  peer_info_t info;
  memset(&info, 0, sizeof(info));
  for (size_t i = 0; i < NODE_ID_HASH_SIZE; i++) info.node_id.hash[i] = (uint8_t)(i + 1);
  snprintf(info.node_id.str, NODE_ID_STRING_SIZE, "testnode");
  static const uint8_t key[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  info.public_key = (uint8_t*)malloc(sizeof(key));
  memcpy(info.public_key, key, sizeof(key));
  info.public_key_len = sizeof(key);
  info.address_count = 1;
  info.addresses = (peer_address_t*)calloc(1, sizeof(peer_address_t));
  info.addresses[0].type = PEER_ADDR_DIRECT;
  info.addresses[0].host = strdup("10.0.0.1");
  info.addresses[0].port = 23401;
  return info;
}

static void _free_info(peer_info_t* info) {
  peer_info_destroy(info);
}

TEST(PeerInfoFromPayload, FormatTwoQrImageRoundTrips) {
  peer_info_t original = _make_info();

  cbor_item_t* encoded = peer_info_encode(&original);
  ASSERT_NE(encoded, nullptr);
  size_t serialized_len = cbor_serialized_size(encoded);
  uint8_t* serialized = (uint8_t*)malloc(serialized_len);
  ASSERT_GT(cbor_serialize(encoded, serialized, serialized_len), 0u);
  cbor_decref(&encoded);

  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(serialized, serialized_len, &ppm_len);
  free(serialized);
  ASSERT_NE(ppm, nullptr);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, peer_info_from_payload(2, ppm, ppm_len, &decoded));
  free(ppm);

  EXPECT_EQ(0, memcmp(decoded.node_id.hash, original.node_id.hash, NODE_ID_HASH_SIZE));
  ASSERT_EQ(1u, decoded.address_count);
  EXPECT_STREQ(decoded.addresses[0].host, "10.0.0.1");
  EXPECT_EQ(23401, decoded.addresses[0].port);
  peer_info_destroy(&decoded);
  _free_info(&original);
}

TEST(PeerInfoFromPayload, FormatZeroCborRoundTrips) {
  peer_info_t original = _make_info();

  cbor_item_t* encoded = peer_info_encode(&original);
  ASSERT_NE(encoded, nullptr);
  size_t serialized_len = cbor_serialized_size(encoded);
  uint8_t* serialized = (uint8_t*)malloc(serialized_len);
  ASSERT_GT(cbor_serialize(encoded, serialized, serialized_len), 0u);
  cbor_decref(&encoded);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, peer_info_from_payload(0, serialized, serialized_len, &decoded));
  free(serialized);

  EXPECT_EQ(0, memcmp(decoded.node_id.hash, original.node_id.hash, NODE_ID_HASH_SIZE));
  peer_info_destroy(&decoded);
  _free_info(&original);
}

TEST(PeerInfoFromPayload, FormatOneBase58RoundTrips) {
  peer_info_t original = _make_info();

  char* b58 = peer_info_to_base58(&original);
  ASSERT_NE(b58, nullptr);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, peer_info_from_payload(1, (const uint8_t*)b58, strlen(b58), &decoded));
  free(b58);

  EXPECT_EQ(0, memcmp(decoded.node_id.hash, original.node_id.hash, NODE_ID_HASH_SIZE));
  peer_info_destroy(&decoded);
  _free_info(&original);
}

TEST(PeerInfoFromPayload, FormatTwoGarbageImageRejected) {
  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  const char* garbage = "not an image at all";
  EXPECT_NE(0, peer_info_from_payload(2, (const uint8_t*)garbage, strlen(garbage), &decoded));
}

TEST(PeerInfoFromPayload, FormatTwoNonPeerInfoQrRejected) {
  /* A valid QR whose payload is not peer_info CBOR */
  const char* payload = "hello, not a peer info map";
  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm((const uint8_t*)payload, strlen(payload), &ppm_len);
  ASSERT_NE(ppm, nullptr);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_NE(0, peer_info_from_payload(2, ppm, ppm_len, &decoded));
  free(ppm);
}

TEST(PeerInfoFromPayload, UnknownFormatRejected) {
  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_NE(0, peer_info_from_payload(7, (const uint8_t*)"x", 1, &decoded));
}

/* A realistic peer_info: 8 addresses (DIRECT/SRFLX/RELAY mix) so the encoded
   CBOR lands near 300 bytes — the size a real daemon emits. Builds on
   _make_info() and owns everything via peer_info_destroy(). */
static peer_info_t _make_realistic_info() {
  peer_info_t info = _make_info();
  const size_t rich_count = 8;
  info.addresses = (peer_address_t*)realloc(info.addresses, rich_count * sizeof(peer_address_t));
  memset(info.addresses + 1, 0, (rich_count - 1) * sizeof(peer_address_t));
  for (size_t i = 1; i < rich_count; i++) {
    char host[16];
    snprintf(host, sizeof(host), "203.0.113.%zu", 9 + i);
    info.addresses[i].host = strdup(host);
    info.addresses[i].port = (uint16_t)(23401 + i);
    switch (i % 3) {
      case 0:
        info.addresses[i].type = PEER_ADDR_RELAY;
        info.addresses[i].relay_id = (uint32_t)(1000 + i);
        break;
      case 1:
        info.addresses[i].type = PEER_ADDR_SRFLX;
        break;
      default:
        info.addresses[i].type = PEER_ADDR_DIRECT;
        break;
    }
  }
  info.address_count = rich_count;
  return info;
}

TEST(PeerInfoFromPayload, RealisticQrResponseFitsWireCap) {
  /* A realistic peer_info (~300-byte CBOR) encodes to a QR PPM well under
     the 2 MB peer-info wire cap, and decodes back. Guards the cap bump in
     client_api_peer_info_response_decode. */
  peer_info_t original = _make_realistic_info();

  cbor_item_t* encoded = peer_info_encode(&original);
  ASSERT_NE(encoded, nullptr);
  size_t serialized_len = cbor_serialized_size(encoded);
  uint8_t* serialized = (uint8_t*)malloc(serialized_len);
  ASSERT_GT(cbor_serialize(encoded, serialized, serialized_len), 0u);
  cbor_decref(&encoded);

  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(serialized, serialized_len, &ppm_len);
  free(serialized);
  ASSERT_NE(ppm, nullptr);
  EXPECT_LT(ppm_len, 2u * 1024u * 1024u);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, peer_info_from_payload(2, ppm, ppm_len, &decoded));
  free(ppm);
  EXPECT_EQ(original.address_count, decoded.address_count);
  peer_info_destroy(&decoded);
  _free_info(&original);
}

TEST(PeerInfoFromPayload, FormatTwoTruncatedCborRejected) {
  /* Valid QR image, but the embedded payload is truncated CBOR —
     exercises the cbor_load error leg of the format-2 path. */
  peer_info_t original = _make_info();
  cbor_item_t* encoded = peer_info_encode(&original);
  ASSERT_NE(encoded, nullptr);
  size_t serialized_len = cbor_serialized_size(encoded);
  uint8_t* serialized = (uint8_t*)malloc(serialized_len);
  ASSERT_GT(cbor_serialize(encoded, serialized, serialized_len), 0u);
  cbor_decref(&encoded);

  size_t ppm_len = 0;
  uint8_t* ppm = qr_encode_to_ppm(serialized, serialized_len / 2, &ppm_len);
  free(serialized);
  ASSERT_NE(ppm, nullptr);

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_NE(0, peer_info_from_payload(2, ppm, ppm_len, &decoded));
  free(ppm);
  _free_info(&original);
}

}  // namespace peer_payload_test