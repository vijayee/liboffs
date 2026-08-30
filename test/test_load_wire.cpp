#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include <cbor.h>
}

namespace load_wire_test {

static client_api_load_request_t _make_req(const char* ori, uint8_t has_range,
                                           size_t start, size_t end) {
  client_api_load_request_t req;
  memset(&req, 0, sizeof(req));
  /* strdup, not the literal: _destroy frees ori_string, and free() on a
     string literal aborts. decode copies the string, so semantics are intact. */
  req.ori_string = strdup(ori);
  req.has_range = has_range;
  req.range_start = start;
  req.range_end = end;
  return req;
}

TEST(LoadRequestWire, EncodeDecodeRoundTripNoRange) {
  client_api_load_request_t req = _make_req("http://n/offsystem/v3/standard/10/a/b/f", 0, 0, 0);
  cbor_item_t* frame = client_api_load_request_encode(&req);
  ASSERT_NE(frame, nullptr);

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, client_api_load_request_decode(frame, &decoded));
  EXPECT_STREQ(decoded.ori_string, req.ori_string);
  EXPECT_EQ(0, decoded.has_range);

  client_api_load_request_destroy(&decoded);
  client_api_load_request_destroy(&req);
  cbor_decref(&frame);
}

TEST(LoadRequestWire, EncodeDecodeRoundTripWithRange) {
  client_api_load_request_t req = _make_req("ori-string", 1, 128000, 256000);
  cbor_item_t* frame = client_api_load_request_encode(&req);
  ASSERT_NE(frame, nullptr);

  /* Ranged shape mirrors GET_REQUEST: [39, ori, 1, start, end] — 5 elements
     with the literal has_range flag in position 2. */
  ASSERT_TRUE(cbor_isa_array(frame));
  EXPECT_EQ(5u, cbor_array_size(frame));
  cbor_item_t* flag = cbor_array_get(frame, 2);
  ASSERT_TRUE(cbor_isa_uint(flag));
  EXPECT_EQ(1u, cbor_get_uint8(flag));
  cbor_decref(&flag);

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, client_api_load_request_decode(frame, &decoded));
  EXPECT_EQ(1, decoded.has_range);
  EXPECT_EQ(128000u, decoded.range_start);
  EXPECT_EQ(256000u, decoded.range_end);

  client_api_load_request_destroy(&decoded);
  client_api_load_request_destroy(&req);
  cbor_decref(&frame);
}

TEST(LoadRequestWire, DecodeFiveElementFrameSetsHasRangeByShape) {
  /* Hand-build the same 5-element shape GET_REQUEST produces:
     [39, ori, 1, start, end]. */
  cbor_item_t* frame = cbor_new_definite_array(5);
  cbor_item_t* item = cbor_build_uint8(39);
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);

  item = cbor_build_string("http://n/offsystem/v3/standard/10/a/b/f");
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);

  item = cbor_build_uint8(1); /* literal has_range flag */
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);

  item = cbor_build_uint64(1000);
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);

  item = cbor_build_uint64(2000);
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, client_api_load_request_decode(frame, &decoded));
  EXPECT_EQ(1, decoded.has_range);
  EXPECT_EQ(1000u, decoded.range_start);
  EXPECT_EQ(2000u, decoded.range_end);
  EXPECT_STREQ("http://n/offsystem/v3/standard/10/a/b/f", decoded.ori_string);

  client_api_load_request_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(LoadProgressWire, EncodesCounts) {
  /* [40, tuples_loaded, tuples_total] */
  cbor_item_t* frame = client_api_load_progress_encode(7, 20);
  ASSERT_NE(frame, nullptr);
  size_t loaded = 0, total = 0;
  ASSERT_EQ(0, client_api_load_progress_decode(frame, &loaded, &total));
  EXPECT_EQ(7u, loaded);
  EXPECT_EQ(20u, total);
  cbor_decref(&frame);
}

TEST(LoadEndWire, EncodesFullTally) {
  /* [41, status, tuples_loaded, tuples_total] */
  cbor_item_t* frame = client_api_load_end_encode(1, 180, 200);
  ASSERT_NE(frame, nullptr);

  uint8_t status = 99;
  size_t loaded = 0, total = 0;
  ASSERT_EQ(0, client_api_load_end_decode(frame, &status, &loaded, &total));
  EXPECT_EQ(1, status);
  EXPECT_EQ(180u, loaded);
  EXPECT_EQ(200u, total);
  cbor_decref(&frame);
}

TEST(LoadWire, FrameTypesDoNotCollide) {
  EXPECT_EQ(39, CLIENT_API_LOAD_REQUEST);
  EXPECT_EQ(40, CLIENT_API_LOAD_PROGRESS);
  EXPECT_EQ(41, CLIENT_API_LOAD_END);
}

}  // namespace load_wire_test