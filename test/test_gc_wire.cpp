//
// Wire round-trip tests for the keep-list GC pair (58/59) and the cache
// capacity pair (56/57) — codec only, no daemon.
//
#include <gtest/gtest.h>
#include <cbor.h>
#include <cstring>
#include <string>
extern "C" {
#include "ClientAPI/client_api_wire.h"
#include "../src/BlockCache/block_gc.h"
}

/* The response-type pairing asserts are compiled in-header for both C and C++
   translation units — this TU exercises them via inclusion (renumbering one
   code fails the build here too; nothing further to assert at runtime). */

/* push item; take a fresh item built inline and hand it to the array */
static void gw_push(cbor_item_t* array, cbor_item_t* item) {
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
}

TEST(GcWire, RequestRoundTrip) {
  client_api_gc_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.urls = (char*)"http://localhost/offsystem/v3/a/1/AAA/AAA/f.bin\n"
                     "http://localhost/offsystem/v3/a/1/BBB/BBB/g.bin";
  msg.force = 1;
  msg.defrag = 1;

  cbor_item_t* frame = client_api_gc_request_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_gc_request_t decoded;
  EXPECT_EQ(0, client_api_gc_request_decode(frame, &decoded));
  EXPECT_STREQ(msg.urls, decoded.urls);
  EXPECT_EQ(1, decoded.force);
  EXPECT_EQ(1, decoded.defrag);

  client_api_gc_request_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(GcWire, RequestDefaults) {
  client_api_gc_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.urls = (char*)"http://localhost/offsystem/v3/a/1/AAA/AAA/f.bin";

  cbor_item_t* frame = client_api_gc_request_encode(&msg);
  ASSERT_NE(frame, nullptr);
  client_api_gc_request_t decoded;
  EXPECT_EQ(0, client_api_gc_request_decode(frame, &decoded));
  EXPECT_EQ(0, decoded.force);
  EXPECT_EQ(0, decoded.defrag);
  client_api_gc_request_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(GcWire, RequestDecodeRejections) {
  client_api_gc_request_t msg;
  memset(&msg, 0, sizeof(msg));

  /* Not an array. */
  cbor_item_t* scalar = cbor_build_uint8(CLIENT_API_GC_REQUEST);
  EXPECT_NE(0, client_api_gc_request_decode(scalar, &msg));
  client_api_gc_request_destroy(&msg);
  cbor_decref(&scalar);

  /* Wrong element count: 3 then 5. */
  for (int size = 3; size <= 5; size += 2) {
    cbor_item_t* frame = cbor_new_definite_array(size);
    gw_push(frame, cbor_build_uint8(CLIENT_API_GC_REQUEST));
    gw_push(frame, cbor_build_string("x"));
    for (int idx = 0; idx < size - 2; idx++) {
      gw_push(frame, cbor_build_uint8(0));
    }
    EXPECT_NE(0, client_api_gc_request_decode(frame, &msg)) << "size " << size;
    cbor_decref(&frame);
  }

  /* Force out of range (2). */
  cbor_item_t* frame = cbor_new_definite_array(4);
  gw_push(frame, cbor_build_uint8(CLIENT_API_GC_REQUEST));
  gw_push(frame, cbor_build_string("x"));
  gw_push(frame, cbor_build_uint8(2));
  gw_push(frame, cbor_build_uint8(0));
  EXPECT_NE(0, client_api_gc_request_decode(frame, &msg));
  client_api_gc_request_destroy(&msg);
  cbor_decref(&frame);

  /* Defrag out of range (2). */
  frame = cbor_new_definite_array(4);
  gw_push(frame, cbor_build_uint8(CLIENT_API_GC_REQUEST));
  gw_push(frame, cbor_build_string("x"));
  gw_push(frame, cbor_build_uint8(0));
  gw_push(frame, cbor_build_uint8(2));
  EXPECT_NE(0, client_api_gc_request_decode(frame, &msg));
  client_api_gc_request_destroy(&msg);
  cbor_decref(&frame);

  /* Empty urls tstr. */
  frame = cbor_new_definite_array(4);
  gw_push(frame, cbor_build_uint8(CLIENT_API_GC_REQUEST));
  gw_push(frame, cbor_build_string(""));
  gw_push(frame, cbor_build_uint8(0));
  gw_push(frame, cbor_build_uint8(0));
  EXPECT_NE(0, client_api_gc_request_decode(frame, &msg));
  client_api_gc_request_destroy(&msg);
  cbor_decref(&frame);

  /* Oversized urls (> 4 MiB cap). */
  std::string oversized(CLIENT_API_GC_MAX_URLS_TEXT + 1, 'u');
  frame = cbor_new_definite_array(4);
  gw_push(frame, cbor_build_uint8(CLIENT_API_GC_REQUEST));
  gw_push(frame, cbor_build_string(oversized.c_str()));
  gw_push(frame, cbor_build_uint8(0));
  gw_push(frame, cbor_build_uint8(0));
  EXPECT_NE(0, client_api_gc_request_decode(frame, &msg));
  client_api_gc_request_destroy(&msg);
  cbor_decref(&frame);
}

/* Response round trip with failed rows. The daemon side borrows the rows
   array into the frame and destroys its reference after encoding; the decode
   side deep-copies. */
TEST(GcWire, ResponseRoundTripWithFailedRows) {
  cbor_item_t* failed_rows = cbor_new_definite_array(2);
  cbor_item_t* row = cbor_new_definite_array(3);
  gw_push(row, cbor_build_uint32(7));
  gw_push(row, cbor_build_uint8(GC_LINE_MISSING_DESCRIPTOR));
  gw_push(row, cbor_build_string("http://localhost/offsystem/v3/a/1/ZZZ/ZZZ/ghost.bin"));
  (void)cbor_array_push(failed_rows, row);
  cbor_decref(&row);
  row = cbor_new_definite_array(3);
  gw_push(row, cbor_build_uint32(9));
  gw_push(row, cbor_build_uint8(GC_LINE_MALFORMED_URL));
  gw_push(row, cbor_build_string("not-a-url"));
  (void)cbor_array_push(failed_rows, row);
  cbor_decref(&row);

  client_api_gc_response_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.status = 0;
  msg.urls_request = 12;
  msg.urls_collected = 10;
  msg.blocks_deleted = 4820;
  msg.blocks_kept = 173;
  msg.skipped_pinned = 4;
  msg.skipped_claimed = 2;
  msg.defrag_applied = 1;
  msg.failed = failed_rows;
  msg.defrag_sections = 3;
  msg.defrag_blocks_relocated = 96;

  cbor_item_t* frame = client_api_gc_response_encode(&msg);
  ASSERT_NE(frame, nullptr);
  cbor_decref(&failed_rows);  /* the daemon encode path's own destroy */

  client_api_gc_response_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(0, client_api_gc_response_decode(frame, &decoded));
  EXPECT_EQ(0, decoded.status);
  EXPECT_EQ(12u, decoded.urls_request);
  EXPECT_EQ(10u, decoded.urls_collected);
  EXPECT_EQ(4820u, decoded.blocks_deleted);
  EXPECT_EQ(173u, decoded.blocks_kept);
  EXPECT_EQ(4u, decoded.skipped_pinned);
  EXPECT_EQ(2u, decoded.skipped_claimed);
  EXPECT_EQ(1, decoded.defrag_applied);
  EXPECT_EQ(3u, decoded.defrag_sections);
  EXPECT_EQ(96u, decoded.defrag_blocks_relocated);
  ASSERT_TRUE(cbor_isa_array(decoded.failed));
  ASSERT_EQ(2, cbor_array_size(decoded.failed));
  cbor_item_t* first = cbor_array_get(decoded.failed, 0);
  cbor_item_t* line_item = cbor_array_get(first, 0);
  ASSERT_TRUE(cbor_isa_uint(line_item));
  EXPECT_EQ(7u, cbor_get_uint64(line_item));
  cbor_decref(&line_item);
  cbor_item_t* reason_item = cbor_array_get(first, 1);
  ASSERT_TRUE(cbor_isa_uint(reason_item));
  EXPECT_EQ((uint64_t)GC_LINE_MISSING_DESCRIPTOR, cbor_get_uint64(reason_item));
  cbor_decref(&reason_item);
  cbor_item_t* text_item = cbor_array_get(first, 2);
  ASSERT_TRUE(cbor_isa_string(text_item));
  EXPECT_STREQ("http://localhost/offsystem/v3/a/1/ZZZ/ZZZ/ghost.bin",
               (const char*)cbor_string_handle(text_item));
  cbor_decref(&text_item);
  cbor_decref(&first);

  client_api_gc_response_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(GcWire, ResponseRoundTripNoFailedRows) {
  client_api_gc_response_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.status = 1;
  msg.urls_request = 2;
  msg.urls_collected = 0;
  msg.failed = NULL;

  cbor_item_t* frame = client_api_gc_response_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_gc_response_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(0, client_api_gc_response_decode(frame, &decoded));
  EXPECT_EQ(1, decoded.status);
  EXPECT_EQ(2u, decoded.urls_request);
  EXPECT_EQ(0u, decoded.urls_collected);
  EXPECT_EQ(0u, decoded.blocks_deleted);
  EXPECT_EQ((cbor_item_t*)NULL, decoded.failed);  /* empty rows -> NULL */

  client_api_gc_response_destroy(&decoded);
  cbor_decref(&frame);
}

/* A failed row that is malformed in any slot must fail the whole decode (the
   struct owns a partial failed reference at that point — the destroy path
   runs inside decode, so nothing to release here). */
TEST(GcWire, ResponseDecodeRejections) {
  client_api_gc_response_t msg;
  memset(&msg, 0, sizeof(msg));

  /* Short array (< 12). */
  cbor_item_t* frame = cbor_new_definite_array(11);
  gw_push(frame, cbor_build_uint8(CLIENT_API_GC_RESPONSE));
  EXPECT_NE(0, client_api_gc_response_decode(frame, &msg));
  cbor_decref(&frame);

  /* Three malformed-row shapes: too short, non-uint line, non-string text. */
  cbor_item_t* rows[3];
  rows[0] = cbor_new_definite_array(1);
  cbor_item_t* row = cbor_new_definite_array(2);
  gw_push(row, cbor_build_uint32(1));
  gw_push(row, cbor_build_uint8(1));
  (void)cbor_array_push(rows[0], row);
  cbor_decref(&row);

  rows[1] = cbor_new_definite_array(1);
  row = cbor_new_definite_array(3);
  gw_push(row, cbor_build_string("seven"));
  gw_push(row, cbor_build_uint8(1));
  gw_push(row, cbor_build_string("text"));
  (void)cbor_array_push(rows[1], row);
  cbor_decref(&row);

  rows[2] = cbor_new_definite_array(1);
  row = cbor_new_definite_array(3);
  gw_push(row, cbor_build_uint32(1));
  gw_push(row, cbor_build_uint8(1));
  gw_push(row, cbor_build_uint8(99));
  (void)cbor_array_push(rows[2], row);
  cbor_decref(&row);

  for (int v = 0; v < 3; v++) {
    frame = cbor_new_definite_array(12);
    gw_push(frame, cbor_build_uint8(CLIENT_API_GC_RESPONSE));
    for (int idx = 0; idx < 8; idx++) {
      gw_push(frame, cbor_build_uint8(0));
    }
    (void)cbor_array_push(frame, rows[v]);
    cbor_decref(&rows[v]);
    gw_push(frame, cbor_build_uint8(0));
    gw_push(frame, cbor_build_uint8(0));
    memset(&msg, 0, sizeof(msg));
    EXPECT_NE(0, client_api_gc_response_decode(frame, &msg)) << "variant " << v;
    client_api_gc_response_destroy(&msg);
    cbor_decref(&frame);
  }
}

TEST(GcWire, ResponseDefragFlagNormalizesToBit) {
  /* defrag_applied is a bit on the wire: any nonzero uint normalizes to 1. */
  cbor_item_t* frame = cbor_new_definite_array(12);
  gw_push(frame, cbor_build_uint8(CLIENT_API_GC_RESPONSE));
  for (int idx = 0; idx < 7; idx++) {
    gw_push(frame, cbor_build_uint8(0));
  }
  gw_push(frame, cbor_build_uint8(3));  /* defrag_applied: truthy */
  gw_push(frame, cbor_new_definite_array(0));  /* failed: empty */
  gw_push(frame, cbor_build_uint8(0));
  gw_push(frame, cbor_build_uint8(0));

  client_api_gc_response_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(0, client_api_gc_response_decode(frame, &decoded));
  EXPECT_EQ(1, decoded.defrag_applied);
  client_api_gc_response_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(GcWire, RequestTypeByteIsReadable) {
  client_api_gc_request_t msg;
  memset(&msg, 0, sizeof(msg));
  /* The struct's destroy frees urls — it must be heap memory, not a literal. */
  msg.urls = strdup("u");
  cbor_item_t* frame = client_api_gc_request_encode(&msg);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(CLIENT_API_GC_REQUEST, client_api_wire_get_type(frame));
  client_api_gc_request_destroy(&msg);
  cbor_decref(&frame);
}

/* --- cache capacity pair 56/57 (Phase 0 scope) --- */

TEST(CacheResizeWire, RequestRoundTrip) {
  client_api_cache_resize_request_t msg;
  msg.capacity_bytes = 10ull * 1024ull * 1024ull;

  cbor_item_t* frame = client_api_cache_resize_request_encode(&msg);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(CLIENT_API_CACHE_RESIZE_REQUEST, client_api_wire_get_type(frame));

  client_api_cache_resize_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(0, client_api_cache_resize_request_decode(frame, &decoded));
  EXPECT_EQ(10ull * 1024ull * 1024ull, decoded.capacity_bytes);

  cbor_decref(&frame);

  /* Zero capacity is rejected at decode. */
  msg.capacity_bytes = 0;
  frame = client_api_cache_resize_request_encode(&msg);
  ASSERT_NE(frame, nullptr);
  EXPECT_NE(0, client_api_cache_resize_request_decode(frame, &decoded));
  cbor_decref(&frame);
}

TEST(CacheResizeWire, ResponseRoundTrip) {
  client_api_cache_resize_response_t msg;
  msg.status = 0;
  msg.applied_live = 1;
  msg.max_capacity_bytes = 4096;
  msg.current_bytes = 512;

  cbor_item_t* frame = client_api_cache_resize_response_encode(&msg);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(CLIENT_API_CACHE_RESIZE_RESPONSE, client_api_wire_get_type(frame));

  client_api_cache_resize_response_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  EXPECT_EQ(0, client_api_cache_resize_response_decode(frame, &decoded));
  EXPECT_EQ(0, decoded.status);
  EXPECT_EQ(1, decoded.applied_live);
  EXPECT_EQ(4096u, decoded.max_capacity_bytes);
  EXPECT_EQ(512u, decoded.current_bytes);

  client_api_cache_resize_response_destroy(&decoded);
  cbor_decref(&frame);

  /* Short response array is rejected. */
  frame = cbor_new_definite_array(4);
  gw_push(frame, cbor_build_uint8(CLIENT_API_CACHE_RESIZE_RESPONSE));
  EXPECT_NE(0, client_api_cache_resize_response_decode(frame, &decoded));
  cbor_decref(&frame);
}