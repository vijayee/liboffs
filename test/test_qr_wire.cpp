#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include <cbor.h>
}

namespace qr_wire_test {

TEST(PeerInfoRequestWire, BareFrameDecodesToFormatCbor) {
  cbor_item_t* frame = client_api_peer_info_request_encode();
  ASSERT_EQ(1u, cbor_array_size(frame));  /* original 1-element wire shape */
  uint8_t format = 99;
  ASSERT_EQ(0, client_api_peer_info_request_decode(frame, &format));
  EXPECT_EQ(0, format);  /* backward compatible default: raw CBOR */
  cbor_decref(&frame);
}

TEST(PeerInfoRequestWire, BareFrameAcceptedByDecode) {
  /* A hand-built 1-element frame decodes to format 0 — the shape old
     clients send and old daemons must keep accepting. */
  cbor_item_t* frame = cbor_new_definite_array(1);
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PEER_INFO_REQUEST);
  (void)cbor_array_push(frame, type);
  cbor_decref(&type);
  uint8_t format = 99;
  ASSERT_EQ(0, client_api_peer_info_request_decode(frame, &format));
  EXPECT_EQ(0, format);
  cbor_decref(&frame);
}

TEST(PeerInfoRequestWire, FormatTwoRoundTrips) {
  cbor_item_t* frame = client_api_peer_info_request_encode_format(2);
  uint8_t format = 0;
  ASSERT_EQ(0, client_api_peer_info_request_decode(frame, &format));
  EXPECT_EQ(2, format);
  cbor_decref(&frame);
}

TEST(PeerInfoRequestWire, FormatOneRoundTrips) {
  cbor_item_t* frame = client_api_peer_info_request_encode_format(1);
  uint8_t format = 0;
  ASSERT_EQ(0, client_api_peer_info_request_decode(frame, &format));
  EXPECT_EQ(1, format);
  cbor_decref(&frame);
}

TEST(PeerInfoRequestWire, UnknownFormatRejected) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PEER_INFO_REQUEST);
  cbor_item_t* fmt = cbor_build_uint8(5);
  (void)cbor_array_push(array, type);
  (void)cbor_array_push(array, fmt);
  cbor_decref(&type);
  cbor_decref(&fmt);
  uint8_t format = 0;
  EXPECT_NE(0, client_api_peer_info_request_decode(array, &format));
  cbor_decref(&array);
}

TEST(PeerInfoRequestWire, EncodeRejectsUnknownFormat) {
  EXPECT_TRUE(client_api_peer_info_request_encode_format(3) == NULL);
}

TEST(PeerInfoRequestWire, ExtraElementRejected) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PEER_INFO_REQUEST);
  cbor_item_t* fmt = cbor_build_uint8(2);
  cbor_item_t* extra = cbor_build_uint8(7);
  (void)cbor_array_push(array, type);
  (void)cbor_array_push(array, fmt);
  (void)cbor_array_push(array, extra);
  cbor_decref(&type);
  cbor_decref(&fmt);
  cbor_decref(&extra);
  uint8_t format = 0;
  EXPECT_NE(0, client_api_peer_info_request_decode(array, &format));
  cbor_decref(&array);
}

TEST(PeerConnectWire, FormatTwoPassesThrough) {
  /* PEER_CONNECT/FRIEND_ADD decoders already accept any format byte —
     pin that behavior so format 2 (PPM image) flows through unchanged. */
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PEER_CONNECT);
  cbor_item_t* fmt = cbor_build_uint8(2);
  const uint8_t image_bytes[] = {'P', '6', '\n'};
  cbor_item_t* data = cbor_build_bytestring(image_bytes, sizeof(image_bytes));
  (void)cbor_array_push(array, type);
  (void)cbor_array_push(array, fmt);
  (void)cbor_array_push(array, data);
  cbor_decref(&type);
  cbor_decref(&fmt);
  cbor_decref(&data);

  client_api_peer_connect_t msg;
  ASSERT_EQ(0, client_api_peer_connect_decode(array, &msg));
  EXPECT_EQ(2, msg.format);
  EXPECT_EQ(sizeof(image_bytes), msg.data_size);
  client_api_peer_connect_destroy(&msg);
  cbor_decref(&array);
}

}  // namespace qr_wire_test
