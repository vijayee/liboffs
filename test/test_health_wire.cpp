#include <gtest/gtest.h>
extern "C" {
#include <cbor.h>
#include "../src/ClientAPI/client_api_wire.h"
}
#include <cstring>

TEST(HealthWire, RequestEncode) {
  cbor_item_t* encoded = client_api_health_request_encode();
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_HEALTH_REQUEST);
  cbor_decref(&encoded);
}

TEST(HealthWire, ResponseEncodeDecode) {
  client_api_health_response_t msg;
  msg.json_data = (char*)"{\"status\":\"running\"}";

  cbor_item_t* encoded = client_api_health_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_HEALTH_RESPONSE);

  client_api_health_response_t decoded;
  int ret = client_api_health_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_STREQ(decoded.json_data, "{\"status\":\"running\"}");

  client_api_health_response_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(HealthWire, ResponseEncodeNullJson) {
  client_api_health_response_t msg;
  msg.json_data = NULL;

  cbor_item_t* encoded = client_api_health_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_health_response_t decoded;
  int ret = client_api_health_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_STREQ(decoded.json_data, "");

  client_api_health_response_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(HealthWire, ResponseEncodeEmptyJson) {
  client_api_health_response_t msg;
  msg.json_data = (char*)"";

  cbor_item_t* encoded = client_api_health_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_health_response_t decoded;
  int ret = client_api_health_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_STREQ(decoded.json_data, "");

  client_api_health_response_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(HealthWire, ResponseDecodeRejectsWrongType) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_AUTH_REQUEST);
  (void)cbor_array_push(array, type_item);
  cbor_decref(&type_item);
  cbor_item_t* json_item = cbor_build_string("{}");
  (void)cbor_array_push(array, json_item);
  cbor_decref(&json_item);

  client_api_health_response_t decoded;
  EXPECT_EQ(-1, client_api_health_response_decode(array, &decoded));
  cbor_decref(&array);
}

TEST(HealthWire, ResponseDecodeRejectsNonArray) {
  cbor_item_t* number = cbor_build_uint8(42);
  client_api_health_response_t decoded;
  EXPECT_EQ(-1, client_api_health_response_decode(number, &decoded));
  cbor_decref(&number);
}

TEST(HealthWire, ResponseDestroyHandlesNull) {
  client_api_health_response_destroy(NULL);
  SUCCEED();
}

TEST(HealthWire, ResponseDestroyZeroesStruct) {
  client_api_health_response_t msg;
  msg.json_data = (char*)malloc(16);
  strcpy(msg.json_data, "test");
  client_api_health_response_destroy(&msg);
  EXPECT_EQ(msg.json_data, nullptr);
}

TEST(HealthWire, ResponseLargeJson) {
  char large_json[4096];
  memset(large_json, 'x', sizeof(large_json) - 1);
  large_json[sizeof(large_json) - 1] = '\0';
  large_json[0] = '{';
  large_json[sizeof(large_json) - 2] = '}';

  client_api_health_response_t msg;
  msg.json_data = large_json;

  cbor_item_t* encoded = client_api_health_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_health_response_t decoded;
  int ret = client_api_health_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_STREQ(decoded.json_data, large_json);

  client_api_health_response_destroy(&decoded);
  cbor_decref(&encoded);
}
