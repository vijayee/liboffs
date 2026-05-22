#include <gtest/gtest.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include "../src/Util/validation.h"
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

TEST(TestWireValidation, RejectsPutWithOversizedStreamLength) {
  cbor_item_t* cbor = _build_put_request("text/plain", "file.txt",
      (uint64_t)OFFS_MAX_CBOR_MESSAGE_SIZE + 1);
  client_api_put_request_t msg;
  int rc = client_api_put_request_decode(cbor, &msg);
  if (rc == 0) client_api_put_request_destroy(&msg);
  cbor_decref(&cbor);
  EXPECT_NE(rc, 0);
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
