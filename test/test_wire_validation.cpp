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
