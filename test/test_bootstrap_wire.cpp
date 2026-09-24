//
// Created by victor on 9/24/26.
//

// Wire round-trip tests for the bootstrap peer client-API ops.
#include <gtest/gtest.h>
#include <cbor.h>
#include <cstring>

extern "C" {
#include "ClientAPI/client_api_wire.h"
}

TEST(BootstrapWire, AddRoundTrip) {
  client_api_bootstrap_add_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.endpoint = (char*)"[2001:db8::1]:8080";

  cbor_item_t* frame = client_api_bootstrap_add_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_bootstrap_add_t decoded;
  EXPECT_EQ(0, client_api_bootstrap_add_decode(frame, &decoded));
  EXPECT_STREQ("[2001:db8::1]:8080", decoded.endpoint);

  client_api_bootstrap_add_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(BootstrapWire, AddDecodeRejectsWrongTypeAndMissingField) {
  cbor_item_t* wrong = cbor_new_definite_array(1);
  cbor_item_t* t = cbor_build_uint8(CLIENT_API_FRIEND_ADD);
  cbor_array_push(wrong, t);
  cbor_decref(&t);
  client_api_bootstrap_add_t msg;
  memset(&msg, 0, sizeof(msg));
  EXPECT_NE(0, client_api_bootstrap_add_decode(wrong, &msg));
  cbor_decref(&wrong);

  cbor_item_t* short_frame = cbor_build_uint8(CLIENT_API_BOOTSTRAP_ADD);
  EXPECT_NE(0, client_api_bootstrap_add_decode(short_frame, &msg));
  cbor_decref(&short_frame);
}

TEST(BootstrapWire, RemoveRoundTrip) {
  client_api_bootstrap_remove_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.endpoint = (char*)"10.0.0.1:8080";

  cbor_item_t* frame = client_api_bootstrap_remove_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_bootstrap_remove_t decoded;
  EXPECT_EQ(0, client_api_bootstrap_remove_decode(frame, &decoded));
  EXPECT_STREQ("10.0.0.1:8080", decoded.endpoint);

  client_api_bootstrap_remove_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(BootstrapWire, ListResponseRoundTrip) {
  cbor_item_t* entries = cbor_new_definite_array(2);
  for (int iteration = 0; iteration < 2; iteration++) {
    cbor_item_t* entry = cbor_new_definite_array(3);
    cbor_item_t* host = cbor_build_string(iteration == 0 ? "10.0.0.1" : "2001:db8::1");
    cbor_item_t* port = cbor_build_uint16(iteration == 0 ? 8080 : 9090);
    cbor_item_t* source = cbor_build_uint8(iteration == 0
                                               ? CLIENT_API_BOOTSTRAP_SOURCE_CONFIG
                                               : CLIENT_API_BOOTSTRAP_SOURCE_MANAGED);
    cbor_array_push(entry, host);
    cbor_array_push(entry, port);
    cbor_array_push(entry, source);
    cbor_decref(&host);
    cbor_decref(&port);
    cbor_decref(&source);
    cbor_array_push(entries, entry);
    cbor_decref(&entry);
  }

  client_api_bootstrap_list_response_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.entries = entries;

  cbor_item_t* frame = client_api_bootstrap_list_response_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_bootstrap_list_response_t decoded;
  EXPECT_EQ(0, client_api_bootstrap_list_response_decode(frame, &decoded));
  ASSERT_TRUE(cbor_isa_array(decoded.entries));
  EXPECT_EQ(2, cbor_array_size(decoded.entries));

  client_api_bootstrap_list_response_destroy(&decoded);
  cbor_decref(&frame);
  // The test retains ownership of its locally built entries array; encode
  // only borrow-increfs it into the frame.
  cbor_decref(&entries);
}

TEST(BootstrapWire, AddDecodeRejectsOversizedAndEmptyEndpoint) {
  // Oversized endpoint (>256 bytes) must be rejected.
  std::string oversized(257, 'a');
  cbor_item_t* frame = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_ADD);
  cbor_item_t* endpoint_item = cbor_build_string(oversized.c_str());
  cbor_array_push(frame, type_item);
  cbor_array_push(frame, endpoint_item);
  cbor_decref(&type_item);
  cbor_decref(&endpoint_item);

  client_api_bootstrap_add_t msg;
  memset(&msg, 0, sizeof(msg));
  EXPECT_NE(0, client_api_bootstrap_add_decode(frame, &msg));
  cbor_decref(&frame);

  // Empty endpoint must be rejected.
  frame = cbor_new_definite_array(2);
  type_item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_ADD);
  endpoint_item = cbor_build_string("");
  cbor_array_push(frame, type_item);
  cbor_array_push(frame, endpoint_item);
  cbor_decref(&type_item);
  cbor_decref(&endpoint_item);
  EXPECT_NE(0, client_api_bootstrap_add_decode(frame, &msg));
  cbor_decref(&frame);
}

TEST(BootstrapWire, ListResponseEncodeNullEntriesYieldsEmptyArray) {
  client_api_bootstrap_list_response_t msg;
  memset(&msg, 0, sizeof(msg));

  cbor_item_t* frame = client_api_bootstrap_list_response_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_bootstrap_list_response_t decoded;
  EXPECT_EQ(0, client_api_bootstrap_list_response_decode(frame, &decoded));
  ASSERT_TRUE(cbor_isa_array(decoded.entries));
  EXPECT_EQ(0, cbor_array_size(decoded.entries));

  client_api_bootstrap_list_response_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(BootstrapWire, ListRequestEncodeHasTypeOnly) {
  cbor_item_t* frame = client_api_bootstrap_list_request_encode();
  ASSERT_NE(frame, nullptr);
  client_api_bootstrap_list_response_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  // A list request is not a response — decode must fail.
  EXPECT_NE(0, client_api_bootstrap_list_response_decode(frame, &decoded));
  cbor_decref(&frame);
}
