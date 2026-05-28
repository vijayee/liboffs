#include <gtest/gtest.h>
extern "C" {
#include <cbor.h>
}
#include <string.h>
#include <stdlib.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
}

// Helper: serialize item to buffer, then parse it back — verifies
// the full CBOR encoding/decoding pipeline.
static cbor_item_t* roundtrip(cbor_item_t* input) {
  size_t bufsize = cbor_serialized_size(input);
  unsigned char* buf = (unsigned char*)malloc(bufsize);
  size_t written = cbor_serialize(input, buf, bufsize);
  EXPECT_EQ(bufsize, written);
  struct cbor_load_result result;
  cbor_item_t* output = cbor_load(buf, bufsize, &result);
  free(buf);
  return output;
}

TEST(TestClientApiWireConfig, ConfigGetRequestRoundtrip) {
  client_api_config_get_request_t req = {0};
  req.key = (char*)"workers";
  cbor_item_t* encoded = client_api_config_get_request_encode(&req);
  ASSERT_NE(nullptr, encoded);

  cbor_item_t* decoded_item = roundtrip(encoded);
  ASSERT_NE(nullptr, decoded_item);

  EXPECT_EQ(CLIENT_API_CONFIG_GET_REQUEST, client_api_wire_get_type(decoded_item));

  client_api_config_get_request_t decoded = {0};
  EXPECT_EQ(0, client_api_config_get_request_decode(decoded_item, &decoded));
  EXPECT_STREQ("workers", decoded.key);

  client_api_config_get_request_destroy(&decoded);
  cbor_decref(&decoded_item);
  cbor_decref(&encoded);
}

TEST(TestClientApiWireConfig, ConfigSetRequestRoundtrip) {
  client_api_config_set_request_t req = {0};
  req.op = CLIENT_API_CONFIG_OP_SET;
  req.key = (char*)"workers";
  req.value_json = (char*)"8";
  cbor_item_t* encoded = client_api_config_set_request_encode(&req);
  ASSERT_NE(nullptr, encoded);

  cbor_item_t* decoded_item = roundtrip(encoded);
  ASSERT_NE(nullptr, decoded_item);

  EXPECT_EQ(CLIENT_API_CONFIG_SET_REQUEST, client_api_wire_get_type(decoded_item));

  client_api_config_set_request_t decoded = {0};
  EXPECT_EQ(0, client_api_config_set_request_decode(decoded_item, &decoded));
  EXPECT_EQ(CLIENT_API_CONFIG_OP_SET, decoded.op);
  EXPECT_STREQ("workers", decoded.key);
  EXPECT_STREQ("8", decoded.value_json);

  client_api_config_set_request_destroy(&decoded);
  cbor_decref(&decoded_item);
  cbor_decref(&encoded);
}

TEST(TestClientApiWireConfig, ShutdownRequestRoundtrip) {
  cbor_item_t* encoded = client_api_shutdown_request_encode();
  ASSERT_NE(nullptr, encoded);

  cbor_item_t* decoded = roundtrip(encoded);
  ASSERT_NE(nullptr, decoded);

  EXPECT_EQ(CLIENT_API_SHUTDOWN_REQUEST, client_api_wire_get_type(decoded));

  cbor_decref(&decoded);
  cbor_decref(&encoded);
}

TEST(TestClientApiWireConfig, ConfigGenerateAuthRoundtrip) {
  client_api_config_generate_auth_response_t resp = {0};
  resp.token = (char*)"new-token-abc123";
  cbor_item_t* encoded = client_api_config_generate_auth_response_encode(&resp);
  ASSERT_NE(nullptr, encoded);

  cbor_item_t* decoded_item = roundtrip(encoded);
  ASSERT_NE(nullptr, decoded_item);

  client_api_config_generate_auth_response_t decoded = {0};
  EXPECT_EQ(0, client_api_config_generate_auth_response_decode(decoded_item, &decoded));
  EXPECT_STREQ("new-token-abc123", decoded.token);

  client_api_config_generate_auth_response_destroy(&decoded);
  cbor_decref(&decoded_item);
  cbor_decref(&encoded);
}
