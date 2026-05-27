#include "../src/Network/peer_info.h"
#include "../src/Util/base58.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cbor.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void test_encode_decode_roundtrip(void) {
  TEST("encode_decode_roundtrip");

  peer_info_t original;
  memset(&original, 0, sizeof(original));

  // Fill in a realistic node_id
  memset(original.node_id.hash, 0xAB, NODE_ID_HASH_SIZE);
  base58_encode(original.node_id.hash, NODE_ID_HASH_SIZE,
                original.node_id.str, NODE_ID_STRING_SIZE);

  // Public key
  uint8_t key_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  original.public_key = key_data;
  original.public_key_len = sizeof(key_data);

  // Addresses
  original.address_count = 2;
  original.addresses = calloc(2, sizeof(peer_address_t));
  original.addresses[0].type = PEER_ADDR_DIRECT;
  original.addresses[0].host = strdup("192.168.1.1");
  original.addresses[0].port = 12345;
  original.addresses[0].relay_id = 0;
  original.addresses[1].type = PEER_ADDR_RELAY;
  original.addresses[1].host = strdup("relay.example.com");
  original.addresses[1].port = 443;
  original.addresses[1].relay_id = 42;

  // Encode
  cbor_item_t* encoded = peer_info_encode(&original);
  if (encoded == NULL) FAIL("encode returned NULL");

  // Decode
  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = peer_info_decode(encoded, &decoded);
  if (rc != 0) FAIL("decode returned error");

  // Verify
  if (!node_id_equals(&original.node_id, &decoded.node_id)) FAIL("node_id mismatch");
  if (decoded.public_key_len != original.public_key_len) FAIL("public_key_len mismatch");
  if (memcmp(decoded.public_key, original.public_key, original.public_key_len) != 0)
    FAIL("public_key data mismatch");
  if (decoded.address_count != 2) FAIL("address_count mismatch");
  if (decoded.addresses[0].type != PEER_ADDR_DIRECT) FAIL("address[0].type mismatch");
  if (strcmp(decoded.addresses[0].host, "192.168.1.1") != 0) FAIL("address[0].host mismatch");

  cbor_decref(&encoded);
  peer_info_destroy(&decoded);
  free(original.addresses[0].host);
  free(original.addresses[1].host);
  free(original.addresses);

  PASS();
}

static void test_base58_roundtrip(void) {
  TEST("base58_roundtrip");

  peer_info_t original;
  memset(&original, 0, sizeof(original));

  memset(original.node_id.hash, 0xCD, NODE_ID_HASH_SIZE);
  base58_encode(original.node_id.hash, NODE_ID_HASH_SIZE,
                original.node_id.str, NODE_ID_STRING_SIZE);

  uint8_t key_data[] = {0xAA, 0xBB, 0xCC};
  original.public_key = key_data;
  original.public_key_len = sizeof(key_data);

  original.address_count = 1;
  original.addresses = calloc(1, sizeof(peer_address_t));
  original.addresses[0].type = PEER_ADDR_DIRECT;
  original.addresses[0].host = strdup("10.0.0.1");
  original.addresses[0].port = 9999;
  original.addresses[0].relay_id = 0;

  char* b58 = peer_info_to_base58(&original);
  if (b58 == NULL) FAIL("base58 encode returned NULL");

  peer_info_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = peer_info_from_base58(b58, &decoded);
  if (rc != 0) FAIL("base58 decode returned error");
  if (!node_id_equals(&original.node_id, &decoded.node_id)) FAIL("node_id mismatch");
  if (decoded.public_key_len != original.public_key_len) FAIL("public_key_len mismatch");
  if (decoded.address_count != 1) FAIL("address_count mismatch");
  if (decoded.addresses[0].type != PEER_ADDR_DIRECT) FAIL("address[0].type mismatch");
  if (decoded.addresses[0].port != 9999) FAIL("address[0].port mismatch");
  if (decoded.addresses[0].relay_id != 0) FAIL("address[0].relay_id mismatch");
  if (strcmp(decoded.addresses[0].host, "10.0.0.1") != 0) FAIL("address[0].host mismatch");

  free(b58);
  peer_info_destroy(&decoded);
  free(original.addresses[0].host);
  free(original.addresses);

  PASS();
}

static void test_peer_info_equals(void) {
  TEST("peer_info_equals");

  peer_info_t left;
  peer_info_t right;
  memset(&left, 0, sizeof(left));
  memset(&right, 0, sizeof(right));

  // Same hash → equal
  memset(left.node_id.hash, 0x42, NODE_ID_HASH_SIZE);
  memset(right.node_id.hash, 0x42, NODE_ID_HASH_SIZE);
  base58_encode(left.node_id.hash, NODE_ID_HASH_SIZE, left.node_id.str, NODE_ID_STRING_SIZE);
  base58_encode(right.node_id.hash, NODE_ID_HASH_SIZE, right.node_id.str, NODE_ID_STRING_SIZE);

  if (!peer_info_equals(&left, &right)) FAIL("same hash should be equal");

  // Different hash → not equal
  right.node_id.hash[0] = 0xFF;
  if (peer_info_equals(&left, &right)) FAIL("different hash should not be equal");

  // NULL handling
  if (peer_info_equals(NULL, &right)) FAIL("NULL left should return false");
  if (peer_info_equals(&left, NULL)) FAIL("NULL right should return false");
  if (peer_info_equals(NULL, NULL)) FAIL("both NULL should return false");

  PASS();
}

static void test_destroy_null_safety(void) {
  TEST("destroy_null_safety");

  // These should not crash
  peer_info_destroy(NULL);
  peer_address_destroy(NULL);

  // Empty peer_info destroy
  peer_info_t info;
  memset(&info, 0, sizeof(info));
  peer_info_destroy(&info);

  PASS();
}

static void test_invalid_base58(void) {
  TEST("invalid_base58");

  peer_info_t info;
  memset(&info, 0, sizeof(info));
  int rc = peer_info_from_base58("invalid_base58_string!!!", &info);
  if (rc != -1) FAIL("should reject invalid base58 string");
  rc = peer_info_from_base58("", &info);
  if (rc != -1) FAIL("should reject empty string");

  PASS();
}

int main(void) {
  printf("Peer Info Tests:\n");

  test_encode_decode_roundtrip();
  test_base58_roundtrip();
  test_peer_info_equals();
  test_destroy_null_safety();
  test_invalid_base58();

  printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
