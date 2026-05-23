#include <gtest/gtest.h>
extern "C" {
#include <cbor.h>
}
#include <string.h>
#include <stdlib.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
}

TEST(TestAuthWire, EncodeDecodeRoundtrip) {
  client_api_auth_request_t auth;
  auth.api_key = (uint8_t*)"my-secret-key";
  auth.api_key_len = 13;

  cbor_item_t* encoded = client_api_auth_request_encode(&auth);
  ASSERT_NE(nullptr, encoded);

  client_api_auth_request_t decoded;
  int result = client_api_auth_request_decode(encoded, &decoded);
  EXPECT_EQ(0, result);
  EXPECT_EQ(13u, decoded.api_key_len);
  EXPECT_EQ(0, memcmp("my-secret-key", decoded.api_key, 13));

  client_api_auth_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(TestAuthWire, DecodeRejectsWrongType) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(1);
  (void)cbor_array_push(array, type_item);
  cbor_decref(&type_item);
  cbor_item_t* key_item = cbor_build_bytestring((const unsigned char*)"key", 3);
  (void)cbor_array_push(array, key_item);
  cbor_decref(&key_item);

  client_api_auth_request_t decoded;
  EXPECT_EQ(-1, client_api_auth_request_decode(array, &decoded));
  cbor_decref(&array);
}

TEST(TestAuthWire, DecodeRejectsNonArray) {
  cbor_item_t* number = cbor_build_uint8(12);
  client_api_auth_request_t decoded;
  EXPECT_EQ(-1, client_api_auth_request_decode(number, &decoded));
  cbor_decref(&number);
}

TEST(TestAuthWire, DecodeRejectsEmptyKey) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_AUTH_REQUEST);
  (void)cbor_array_push(array, type_item);
  cbor_decref(&type_item);
  cbor_item_t* key_item = cbor_build_bytestring(NULL, 0);
  (void)cbor_array_push(array, key_item);
  cbor_decref(&key_item);

  client_api_auth_request_t decoded;
  EXPECT_EQ(-1, client_api_auth_request_decode(array, &decoded));
  cbor_decref(&array);
}

TEST(TestAuthWire, DestroyHandlesNull) {
  client_api_auth_request_destroy(NULL);
  SUCCEED();
}

TEST(TestAuthWire, DestroyZeroesStruct) {
  client_api_auth_request_t auth;
  auth.api_key = (uint8_t*)malloc(5);
  memcpy(auth.api_key, "test", 5);
  auth.api_key_len = 5;
  client_api_auth_request_destroy(&auth);
  EXPECT_EQ(nullptr, auth.api_key);
  EXPECT_EQ(0u, auth.api_key_len);
}
