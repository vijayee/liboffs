#include <gtest/gtest.h>
extern "C" {
#include "ClientAPI/client_api_wire.h"
#include "Util/base58.h"
}

/* --- Block PUT Request --- */

TEST(BlockCacheAPIWire, PutRequestEncodeDecode) {
  uint8_t data[] = {'h', 'e', 'l', 'l', 'o'};
  client_api_block_put_request_t msg;
  msg.data = data;
  msg.data_size = 5;
  msg.encoding = 0;

  cbor_item_t* encoded = client_api_block_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_BLOCK_PUT_REQUEST);

  client_api_block_put_request_t decoded;
  int ret = client_api_block_put_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.data_size, 5u);
  EXPECT_EQ(memcmp(decoded.data, "hello", 5), 0);
  EXPECT_EQ(decoded.encoding, 0);

  client_api_block_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, PutRequestEncodeDecodeBase58) {
  uint8_t data[] = {'w', 'o', 'r', 'l', 'd'};
  client_api_block_put_request_t msg;
  msg.data = data;
  msg.data_size = 5;
  msg.encoding = 1;

  cbor_item_t* encoded = client_api_block_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_put_request_t decoded;
  int ret = client_api_block_put_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.encoding, 1);

  client_api_block_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, PutRequestDecodeMinimal) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_BLOCK_PUT_REQUEST);
  cbor_array_push(array, type_item); cbor_decref(&type_item);
  cbor_item_t* data_item = cbor_build_bytestring((const uint8_t*)"test", 4);
  cbor_array_push(array, data_item); cbor_decref(&data_item);

  client_api_block_put_request_t decoded;
  int ret = client_api_block_put_request_decode(array, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.encoding, 0);
  EXPECT_EQ(decoded.data_size, 4u);

  client_api_block_put_request_destroy(&decoded);
  cbor_decref(&array);
}

/* --- Block PUT Response --- */

TEST(BlockCacheAPIWire, PutResponseEncodeDecodeRaw) {
  uint8_t hash[32];
  memset(hash, 0xAB, 32);

  client_api_block_put_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;
  msg.hash_data = hash;
  msg.hash_len = 32;
  msg.hash_is_text = 0;

  cbor_item_t* encoded = client_api_block_put_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_put_response_t decoded;
  int ret = client_api_block_put_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);
  EXPECT_EQ(decoded.hash_is_text, 0);
  EXPECT_EQ(decoded.hash_len, 32u);
  EXPECT_EQ(memcmp(decoded.hash_data, hash, 32), 0);

  client_api_block_put_response_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, PutResponseEncodeDecodeBase58) {
  const char* hash_str = "2gPvUwFKjMqX5N7rR3tZ8h1B4vC6xW9yA0bD";
  size_t str_len = strlen(hash_str);

  client_api_block_put_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;
  msg.hash_data = (uint8_t*)hash_str;
  msg.hash_len = str_len;
  msg.hash_is_text = 1;

  cbor_item_t* encoded = client_api_block_put_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_put_response_t decoded;
  int ret = client_api_block_put_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);
  EXPECT_EQ(decoded.hash_is_text, 1);
  EXPECT_EQ(decoded.hash_len, str_len);
  EXPECT_EQ(memcmp(decoded.hash_data, hash_str, str_len), 0);

  client_api_block_put_response_destroy(&decoded);
  cbor_decref(&encoded);
}

/* --- Block GET Request --- */

TEST(BlockCacheAPIWire, GetRequestEncodeDecode) {
  uint8_t hash[32];
  memset(hash, 0x42, 32);

  client_api_block_get_request_t msg;
  msg.hash_data = hash;
  msg.hash_len = 32;

  cbor_item_t* encoded = client_api_block_get_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_BLOCK_GET_REQUEST);

  client_api_block_get_request_t decoded;
  int ret = client_api_block_get_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.hash_len, 32u);
  EXPECT_EQ(memcmp(decoded.hash_data, hash, 32), 0);

  client_api_block_get_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, GetRequestDecodeInvalidHashSize) {
  uint8_t short_hash[16];
  memset(short_hash, 0x42, 16);

  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_BLOCK_GET_REQUEST);
  cbor_array_push(array, type_item); cbor_decref(&type_item);
  cbor_item_t* hash_item = cbor_build_bytestring(short_hash, 16);
  cbor_array_push(array, hash_item); cbor_decref(&hash_item);

  client_api_block_get_request_t decoded;
  int ret = client_api_block_get_request_decode(array, &decoded);
  EXPECT_EQ(ret, -1);

  cbor_decref(&array);
}

/* --- Block GET Response --- */

TEST(BlockCacheAPIWire, GetResponseEncodeDecode) {
  uint8_t block_data[128000];
  memset(block_data, 0xCC, sizeof(block_data));

  client_api_block_get_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;
  msg.data = block_data;
  msg.data_size = sizeof(block_data);

  cbor_item_t* encoded = client_api_block_get_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_get_response_t decoded;
  int ret = client_api_block_get_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);
  EXPECT_EQ(decoded.data_size, 128000u);

  client_api_block_get_response_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, GetResponseNotFound) {
  client_api_block_get_response_t msg;
  msg.status = CLIENT_API_STATUS_NOT_FOUND;
  msg.data = NULL;
  msg.data_size = 0;

  cbor_item_t* encoded = client_api_block_get_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_get_response_t decoded;
  int ret = client_api_block_get_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_NOT_FOUND);
  EXPECT_EQ(decoded.data_size, 0u);

  client_api_block_get_response_destroy(&decoded);
  cbor_decref(&encoded);
}

/* --- Block DELETE Request --- */

TEST(BlockCacheAPIWire, DeleteRequestEncodeDecode) {
  uint8_t hash[32];
  memset(hash, 0xFF, 32);

  client_api_block_delete_request_t msg;
  msg.hash_data = hash;
  msg.hash_len = 32;

  cbor_item_t* encoded = client_api_block_delete_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_BLOCK_DELETE_REQUEST);

  client_api_block_delete_request_t decoded;
  int ret = client_api_block_delete_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.hash_len, 32u);

  client_api_block_delete_request_destroy(&decoded);
  cbor_decref(&encoded);
}

/* --- Block DELETE Response --- */

TEST(BlockCacheAPIWire, DeleteResponseEncodeDecode) {
  client_api_block_delete_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;

  cbor_item_t* encoded = client_api_block_delete_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_delete_response_t decoded;
  int ret = client_api_block_delete_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);

  cbor_decref(&encoded);
}

/* --- Base58 encode/decode round trip --- */

TEST(BlockCacheAPIWire, HashBase58RoundTrip) {
  uint8_t original_hash[32];
  for (int i = 0; i < 32; i++) {
    original_hash[i] = (uint8_t)(i * 7 + 13);
  }

  char encoded[64] = {0};
  int ret = base58_encode(original_hash, 32, encoded, sizeof(encoded));
  ASSERT_GT(ret, 0);
  EXPECT_LE(ret, (int)sizeof(encoded));
  encoded[ret] = '\0';

  uint8_t decoded[32];
  size_t decoded_len = 0;
  ret = base58_decode(encoded, decoded, 32, &decoded_len);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded_len, 32u);
  EXPECT_EQ(memcmp(original_hash, decoded, 32), 0);
}
