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
  client_api_load_request_t req = _make_req("https://n/offsystem/v3/standard/10/a/b/f", 1, 128000, 256000);
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

/* Build a LOAD-shaped frame of arbitrary element count with per-slot types. */
static cbor_item_t* _build_load_frame(uint8_t type_byte, size_t element_count) {
  cbor_item_t* frame = cbor_new_definite_array(element_count);
  cbor_item_t* item = cbor_build_uint8(type_byte);
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);
  return frame;
}

static void _push_string(cbor_item_t* frame, const char* value) {
  cbor_item_t* item = cbor_build_string(value);
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);
}

static void _push_uint64(cbor_item_t* frame, uint64_t value) {
  cbor_item_t* item = cbor_build_uint64(value);
  (void)cbor_array_push(frame, item);
  cbor_decref(&item);
}

TEST(LoadRequestWire, DecodeRejectsWrongTypeByte) {
  /* Well-shaped 5-element frame carrying a non-LOAD type byte (38 is
     CONFIG_RELOAD_RESPONSE). */
  cbor_item_t* frame = _build_load_frame(38, 5);
  _push_string(frame, "http://n/offsystem/v3/standard/10/a/b/f");
  _push_uint64(frame, 1);
  _push_uint64(frame, 0);
  _push_uint64(frame, 0);

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(-1, client_api_load_request_decode(frame, &decoded));
  cbor_decref(&frame);
}

TEST(LoadRequestWire, DecodeRejectsNonArrayInput) {
  cbor_item_t* not_array = cbor_build_string("not a frame");
  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(-1, client_api_load_request_decode(not_array, &decoded));
  cbor_decref(&not_array);
}

TEST(LoadRequestWire, DecodeRejectsOneElementFrame) {
  cbor_item_t* frame = _build_load_frame(39, 1);
  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(-1, client_api_load_request_decode(frame, &decoded));
  cbor_decref(&frame);
}

TEST(LoadRequestWire, DecodeRejectsNonStringOri) {
  cbor_item_t* frame = _build_load_frame(39, 2);
  _push_uint64(frame, 5); /* ori slot holds a uint, not a string */

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(-1, client_api_load_request_decode(frame, &decoded));
  cbor_decref(&frame);
}

TEST(LoadRequestWire, DecodeRejectsNonStringOriSafeCallerCleanup) {
  /* SAFE-CALLER pattern: the client treats every decode failure the same way
     — destroy the struct and move on. With a non-uint range_start the decoder
     fails after freeing ori_string; destroy must not double-free. */
  cbor_item_t* frame = _build_load_frame(39, 5);
  _push_string(frame, "http://n/offsystem/v3/standard/10/a/b/f");
  _push_uint64(frame, 1);
  _push_string(frame, "not-a-number"); /* range_start: tstr instead of uint */
  _push_uint64(frame, 2000);

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(-1, client_api_load_request_decode(frame, &decoded));
  client_api_load_request_destroy(&decoded);
  /* A second destroy is safe: decode leaves the struct zeroed after cleanup. */
  client_api_load_request_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(LoadRequestWire, DecodeRejectsInvalidOriString) {
  cbor_item_t* frame = _build_load_frame(39, 2);
  _push_string(frame, "not-an-off-url");

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(-1, client_api_load_request_decode(frame, &decoded));
  cbor_decref(&frame);
}

TEST(LoadRequestWire, DecodeShortFramesTreatAsUnranged) {
  /* 3- and 4-element LOAD frames are not the ranged shape; decode pins them
     as unranged requests with has_range == 0. */
  for (size_t element_count = 3; element_count <= 4; element_count++) {
    cbor_item_t* frame = _build_load_frame(39, element_count);
    _push_string(frame, "http://n/offsystem/v3/standard/10/a/b/f");
    if (element_count >= 3) _push_uint64(frame, 1);
    if (element_count >= 4) _push_uint64(frame, 1000);

    client_api_load_request_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    ASSERT_EQ(0, client_api_load_request_decode(frame, &decoded));
    EXPECT_EQ(0, decoded.has_range);
    EXPECT_EQ(0u, decoded.range_start);
    EXPECT_EQ(0u, decoded.range_end);
    EXPECT_STREQ("http://n/offsystem/v3/standard/10/a/b/f", decoded.ori_string);

    client_api_load_request_destroy(&decoded);
    cbor_decref(&frame);
  }
}

TEST(LoadProgressWire, DecodeRejectsNonUintCount) {
  cbor_item_t* frame = _build_load_frame(40, 3);
  _push_string(frame, "not-a-number"); /* tuples_loaded slot */
  _push_uint64(frame, 20);

  size_t loaded = 0, total = 0;
  EXPECT_EQ(-1, client_api_load_progress_decode(frame, &loaded, &total));
  cbor_decref(&frame);
}

TEST(LoadEndWire, DecodeRejectsNonUintCount) {
  cbor_item_t* frame = _build_load_frame(41, 4);
  _push_uint64(frame, 0);
  _push_string(frame, "not-a-number"); /* tuples_loaded slot */
  _push_uint64(frame, 20);

  uint8_t status = 99;
  size_t loaded = 0, total = 0;
  EXPECT_EQ(-1, client_api_load_end_decode(frame, &status, &loaded, &total));
  cbor_decref(&frame);
}

}  // namespace load_wire_test

