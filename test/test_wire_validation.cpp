#include <gtest/gtest.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include "../src/Util/validation.h"
#include "../src/Util/allocator.h"
#include "../src/Network/wire.h"
#include <cbor.h>
}

// Helper: encode a PUT request as CBOR
static cbor_item_t* _build_put_request(const char* content_type,
                                        const char* file_name,
                                        uint64_t stream_length) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PUT_REQUEST);
  cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_string(content_type);
  cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_string(file_name);
  cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(stream_length);
  cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

TEST(TestWireValidation, RejectsPutWithNullContentType) {
  cbor_item_t* cbor = _build_put_request("application/json", "file.txt", 100);
  // Overwrite content_type slot with null
  cbor_item_t* null_item = cbor_new_null();
  cbor_array_replace(cbor, 1, null_item);
  // null_item is now owned by the array, destroyed with it
  cbor_decref(&null_item);

  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  if (rc == 0) client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
  EXPECT_NE(rc, 0);
}

TEST(TestWireValidation, RejectsPutWithEmptyContentType) {
  cbor_item_t* cbor = _build_put_request("", "file.txt", 100);
  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  if (rc == 0) client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
  EXPECT_NE(rc, 0);
}

TEST(TestWireValidation, RejectsPutWithEmptyFileName) {
  cbor_item_t* cbor = _build_put_request("text/plain", "", 100);
  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  if (rc == 0) client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
  EXPECT_NE(rc, 0);
}

TEST(TestWireValidation, RejectsPutWithZeroStreamLength) {
  cbor_item_t* cbor = _build_put_request("text/plain", "file.txt", 0);
  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  if (rc == 0) client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
  EXPECT_NE(rc, 0);
}

// stream_length is the total size of a streaming PUT (the sum of all
// subsequent PUT_DATA frames), not a single message size. It is intentionally
// left unbounded apart from the != 0 check: capping it at
// OFFS_MAX_CBOR_MESSAGE_SIZE rejected any upload larger than 64 MB at PUT_START
// before a single PUT_DATA frame was processed, and the server never allocates
// stream_length bytes up front. The real per-frame bound is the data_size cap
// in client_api_put_request_decode. So an oversized stream_length must be
// accepted here. (See client_api_wire.c:155-163 for the rationale.)
TEST(TestWireValidation, AcceptsPutWithOversizedStreamLength) {
  cbor_item_t* cbor = _build_put_request("text/plain", "file.txt",
      (uint64_t)OFFS_MAX_CBOR_MESSAGE_SIZE + 1);
  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  if (rc == 0) client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
  EXPECT_EQ(rc, 0);
}

TEST(TestWireValidation, AcceptsValidPutRequest) {
  cbor_item_t* cbor = _build_put_request("text/plain", "readme.txt", 1024);
  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  EXPECT_EQ(rc, 0);
  client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RejectsPutWithOversizedContentType) {
  char long_type[260];
  memset(long_type, 'a', 257);
  long_type[257] = '\0';

  cbor_item_t* cbor = _build_put_request(long_type, "file.txt", 100);
  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  if (rc == 0) client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
  EXPECT_NE(rc, 0);
}

// Build a FindBlockResponse where array[9] (data bytestring) is 16 bytes,
// but array[10] (data_len) claims 4096. The decoder must use the bytestring
// length (16), not the declared 4096, so block_data_len == 16 and the
// nested buffer is exactly 16 bytes.
static cbor_item_t* _build_node_id_array() {
  // node_id is encoded as [hash_bytestring(32), str_bytestring]
  cbor_item_t* node = cbor_new_definite_array(2);
  uint8_t hash_bytes[32] = {0};
  cbor_item_t* hash_item = cbor_build_bytestring(hash_bytes, 32);
  cbor_array_push(node, hash_item); cbor_decref(&hash_item);
  cbor_item_t* str_item = cbor_build_bytestring((const unsigned char*)"node", 4);
  cbor_array_push(node, str_item); cbor_decref(&str_item);
  return node;
}

static cbor_item_t* _build_find_block_response_mismatched_len() {
  cbor_item_t* array = cbor_new_definite_array(12);
  cbor_item_t* item;
  item = cbor_build_uint8(WIRE_FIND_BLOCK_RESPONSE); cbor_array_push(array, item); cbor_decref(&item);
  // message_id hi/lo
  item = cbor_build_uint64(0); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(1); cbor_array_push(array, item); cbor_decref(&item);
  // block_hash (32 bytes)
  uint8_t hash[32] = {0}; item = cbor_build_bytestring(hash, 32); cbor_array_push(array, item); cbor_decref(&item);
  // found
  item = cbor_build_uint8(1); cbor_array_push(array, item); cbor_decref(&item);
  // holder (node_id)
  item = _build_node_id_array(); cbor_array_push(array, item); cbor_decref(&item);
  // fib, path (empty array)
  item = cbor_build_uint32(0); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_new_definite_array(0); cbor_array_push(array, item); cbor_decref(&item);
  // latency
  item = cbor_build_uint64(0); cbor_array_push(array, item); cbor_decref(&item);
  // data bytestring — 16 bytes of 0xAA
  uint8_t data[16]; memset(data, 0xAA, 16);
  item = cbor_build_bytestring(data, 16); cbor_array_push(array, item); cbor_decref(&item);
  // data_len — the LIE: claims 4096
  item = cbor_build_uint64(4096); cbor_array_push(array, item); cbor_decref(&item);
  // bfib
  item = cbor_build_uint32(0); cbor_array_push(array, item); cbor_decref(&item);
  return array;
}

TEST(TestWireValidation, FindBlockResponseUsesBytestringLengthNotDeclared) {
  cbor_item_t* cbor = _build_find_block_response_mismatched_len();
  wire_find_block_response_t* msg = (wire_find_block_response_t*)get_clear_memory(sizeof(wire_find_block_response_t));
  ASSERT_NE(msg, nullptr);
  int rc = wire_find_block_response_decode(cbor, msg);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(msg->block_data_len, (size_t)16) << "decoder must take length from the bytestring, not array[10]";
  ASSERT_NE(msg->block_data, nullptr);
  EXPECT_EQ(msg->block_data[0], 0xAA);
  EXPECT_EQ(msg->block_data[15], 0xAA);
  wire_find_block_response_destroy(msg);
  cbor_decref(&cbor);
}

static cbor_item_t* _build_recall_accept_mismatched_len() {
  cbor_item_t* array = cbor_new_definite_array(8);
  cbor_item_t* item;
  item = cbor_build_uint8(WIRE_RECALL_ACCEPT); cbor_array_push(array, item); cbor_decref(&item);
  item = _build_node_id_array(); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(0); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(1); cbor_array_push(array, item); cbor_decref(&item);
  uint8_t hash[32] = {0}; item = cbor_build_bytestring(hash, 32); cbor_array_push(array, item); cbor_decref(&item);
  uint8_t data[16]; memset(data, 0xBB, 16);
  item = cbor_build_bytestring(data, 16); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(8192); cbor_array_push(array, item); cbor_decref(&item);  // LIE
  item = cbor_build_uint32(0); cbor_array_push(array, item); cbor_decref(&item);
  return array;
}

TEST(TestWireValidation, RecallAcceptUsesBytestringLengthNotDeclared) {
  cbor_item_t* cbor = _build_recall_accept_mismatched_len();
  wire_recall_accept_t* msg = (wire_recall_accept_t*)get_clear_memory(sizeof(wire_recall_accept_t));
  ASSERT_NE(msg, nullptr);
  int rc = wire_recall_accept_decode(cbor, msg);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(msg->block_data_len, (size_t)16);
  ASSERT_NE(msg->block_data, nullptr);
  EXPECT_EQ(msg->block_data[0], 0xBB);
  wire_recall_accept_destroy(msg);
  cbor_decref(&cbor);
}

// --- RelayChallenge / RelayChallengeResponse round-trip ---

static void _fill_test_node_id(node_id_t* node_id, uint8_t hash_byte, const char* str) {
  memset(node_id, 0, sizeof(*node_id));
  memset(node_id->hash, hash_byte, NODE_ID_HASH_SIZE);
  size_t str_len = strlen(str);
  size_t copy_len = str_len < NODE_ID_STRING_SIZE - 1 ? str_len : NODE_ID_STRING_SIZE - 1;
  memcpy(node_id->str, str, copy_len);
  node_id->str[copy_len] = '\0';
}

TEST(TestWireValidation, RelayChallengeRoundTrip) {
  wire_relay_challenge_t challenge;
  memset(&challenge, 0, sizeof(challenge));
  _fill_test_node_id(&challenge.challenger_id, 0xAB, "challenger-node");
  challenge.challenger_endpoint_id = 0xCAFEBABEu;
  for (size_t index = 0; index < 32; index++) {
    challenge.nonce[index] = (uint8_t)(0x10 + index);
  }

  cbor_item_t* cbor = wire_relay_challenge_encode(&challenge);
  ASSERT_NE(cbor, nullptr);
  EXPECT_EQ(wire_get_type(cbor), WIRE_RELAY_CHALLENGE);

  wire_relay_challenge_t* decoded =
      (wire_relay_challenge_t*)get_clear_memory(sizeof(wire_relay_challenge_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_challenge_decode(cbor, decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(memcmp(decoded->challenger_id.hash, challenge.challenger_id.hash,
                   NODE_ID_HASH_SIZE), 0);
  EXPECT_STREQ(decoded->challenger_id.str, challenge.challenger_id.str);
  EXPECT_EQ(decoded->challenger_endpoint_id, challenge.challenger_endpoint_id);
  EXPECT_EQ(memcmp(decoded->nonce, challenge.nonce, 32), 0);

  wire_relay_challenge_destroy(decoded);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RelayChallengeResponseRoundTrip) {
  wire_relay_challenge_response_t response;
  memset(&response, 0, sizeof(response));
  _fill_test_node_id(&response.responder_id, 0xCD, "responder-node");
  for (size_t index = 0; index < 32; index++) {
    response.nonce[index] = (uint8_t)(0x80 + index);
  }
  uint8_t public_key[32];
  uint8_t signature[64];
  for (size_t index = 0; index < 32; index++) public_key[index] = (uint8_t)(0x20 + index);
  for (size_t index = 0; index < 64; index++) signature[index] = (uint8_t)(0x40 + index);
  response.public_key = public_key;
  response.public_key_len = 32;
  response.signature = signature;
  response.signature_len = 64;

  cbor_item_t* cbor = wire_relay_challenge_response_encode(&response);
  ASSERT_NE(cbor, nullptr);
  EXPECT_EQ(wire_get_type(cbor), WIRE_RELAY_CHALLENGE_RESPONSE);

  wire_relay_challenge_response_t* decoded =
      (wire_relay_challenge_response_t*)get_clear_memory(sizeof(wire_relay_challenge_response_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_challenge_response_decode(cbor, decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(memcmp(decoded->responder_id.hash, response.responder_id.hash,
                   NODE_ID_HASH_SIZE), 0);
  EXPECT_STREQ(decoded->responder_id.str, response.responder_id.str);
  EXPECT_EQ(memcmp(decoded->nonce, response.nonce, 32), 0);
  ASSERT_NE(decoded->public_key, nullptr);
  EXPECT_EQ(decoded->public_key_len, (size_t)32);
  EXPECT_EQ(memcmp(decoded->public_key, public_key, 32), 0);
  ASSERT_NE(decoded->signature, nullptr);
  EXPECT_EQ(decoded->signature_len, (size_t)64);
  EXPECT_EQ(memcmp(decoded->signature, signature, 64), 0);

  wire_relay_challenge_response_destroy(decoded);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RelayChallengeRejectsWrongType) {
  cbor_item_t* cbor = cbor_new_definite_array(4);
  cbor_item_t* entry;
  entry = cbor_build_uint8(WIRE_PING); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = _build_node_id_array(); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = cbor_build_uint32(1); cbor_array_push(cbor, entry); cbor_decref(&entry);
  uint8_t nonce[32] = {0};
  entry = cbor_build_bytestring(nonce, 32); cbor_array_push(cbor, entry); cbor_decref(&entry);

  wire_relay_challenge_t* decoded =
      (wire_relay_challenge_t*)get_clear_memory(sizeof(wire_relay_challenge_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_challenge_decode(cbor, decoded);
  EXPECT_NE(rc, 0);
  wire_relay_challenge_destroy(decoded);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RelayChallengeRejectsTooShortArray) {
  cbor_item_t* cbor = cbor_new_definite_array(2);
  cbor_item_t* entry;
  entry = cbor_build_uint8(WIRE_RELAY_CHALLENGE); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = _build_node_id_array(); cbor_array_push(cbor, entry); cbor_decref(&entry);

  wire_relay_challenge_t* decoded =
      (wire_relay_challenge_t*)get_clear_memory(sizeof(wire_relay_challenge_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_challenge_decode(cbor, decoded);
  EXPECT_NE(rc, 0);
  wire_relay_challenge_destroy(decoded);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RelayChallengeResponseRejectsWrongType) {
  cbor_item_t* cbor = cbor_new_definite_array(5);
  cbor_item_t* entry;
  entry = cbor_build_uint8(WIRE_RELAY_RECEIVED); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = _build_node_id_array(); cbor_array_push(cbor, entry); cbor_decref(&entry);
  uint8_t nonce[32] = {0};
  entry = cbor_build_bytestring(nonce, 32); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = cbor_build_bytestring(nonce, 16); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = cbor_build_bytestring(nonce, 16); cbor_array_push(cbor, entry); cbor_decref(&entry);

  wire_relay_challenge_response_t* decoded =
      (wire_relay_challenge_response_t*)get_clear_memory(sizeof(wire_relay_challenge_response_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_challenge_response_decode(cbor, decoded);
  EXPECT_NE(rc, 0);
  wire_relay_challenge_response_destroy(decoded);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RelayChallengeResponseRejectsTooShortArray) {
  cbor_item_t* cbor = cbor_new_definite_array(3);
  cbor_item_t* entry;
  entry = cbor_build_uint8(WIRE_RELAY_CHALLENGE_RESPONSE); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = _build_node_id_array(); cbor_array_push(cbor, entry); cbor_decref(&entry);
  uint8_t nonce[32] = {0};
  entry = cbor_build_bytestring(nonce, 32); cbor_array_push(cbor, entry); cbor_decref(&entry);

  wire_relay_challenge_response_t* decoded =
      (wire_relay_challenge_response_t*)get_clear_memory(sizeof(wire_relay_challenge_response_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_challenge_response_decode(cbor, decoded);
  EXPECT_NE(rc, 0);
  wire_relay_challenge_response_destroy(decoded);
  cbor_decref(&cbor);
}

/* --- WIRE_RELAY_PUNCH (audit #18 — ICE simultaneous open) --- */

TEST(TestWireValidation, RelayPunchRoundTrip) {
  wire_relay_punch_t punch;
  memset(&punch, 0, sizeof(punch));
  _fill_test_node_id(&punch.sender_id, 0x42, "punch-sender");
  punch.reflexive_addr = 0xC0A80101u;  /* 192.168.1.1 */
  punch.reflexive_port = 4242;

  cbor_item_t* cbor = wire_relay_punch_encode(&punch);
  ASSERT_NE(cbor, nullptr);
  EXPECT_EQ(wire_get_type(cbor), WIRE_RELAY_PUNCH);

  wire_relay_punch_t* decoded =
      (wire_relay_punch_t*)get_clear_memory(sizeof(wire_relay_punch_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_punch_decode(cbor, decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(memcmp(decoded->sender_id.hash, punch.sender_id.hash,
                   NODE_ID_HASH_SIZE), 0);
  EXPECT_EQ(decoded->reflexive_addr, punch.reflexive_addr);
  EXPECT_EQ(decoded->reflexive_port, punch.reflexive_port);

  wire_relay_punch_destroy(decoded);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RelayPunchRejectsWrongType) {
  cbor_item_t* cbor = cbor_new_definite_array(4);
  cbor_item_t* entry;
  entry = cbor_build_uint8(WIRE_RELAY_CHALLENGE); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = _build_node_id_array(); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = cbor_build_uint32(0x01020304u); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = cbor_build_uint16(1234); cbor_array_push(cbor, entry); cbor_decref(&entry);

  wire_relay_punch_t* decoded =
      (wire_relay_punch_t*)get_clear_memory(sizeof(wire_relay_punch_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_punch_decode(cbor, decoded);
  EXPECT_NE(rc, 0);
  wire_relay_punch_destroy(decoded);
  cbor_decref(&cbor);
}

TEST(TestWireValidation, RelayPunchRejectsTooShortArray) {
  cbor_item_t* cbor = cbor_new_definite_array(3);
  cbor_item_t* entry;
  entry = cbor_build_uint8(WIRE_RELAY_PUNCH); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = _build_node_id_array(); cbor_array_push(cbor, entry); cbor_decref(&entry);
  entry = cbor_build_uint32(0x01020304u); cbor_array_push(cbor, entry); cbor_decref(&entry);

  wire_relay_punch_t* decoded =
      (wire_relay_punch_t*)get_clear_memory(sizeof(wire_relay_punch_t));
  ASSERT_NE(decoded, nullptr);
  int rc = wire_relay_punch_decode(cbor, decoded);
  EXPECT_NE(rc, 0);
  wire_relay_punch_destroy(decoded);
  cbor_decref(&cbor);
}
