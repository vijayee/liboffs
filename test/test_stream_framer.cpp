//
// Created by victor on 5/16/25.
//

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "Network/stream_framer.h"
#include "Util/allocator.h"
}

// === stream_frame_encode tests ===

TEST(StreamFramer, EncodeSingleMessage) {
  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  size_t out_len = 0;
  uint8_t* frame = stream_frame_encode(payload, sizeof(payload), &out_len);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(out_len, (size_t)(4 + sizeof(payload)));

  // Verify length prefix (big-endian)
  uint32_t length = ((uint32_t)frame[0] << 24) |
                    ((uint32_t)frame[1] << 16) |
                    ((uint32_t)frame[2] << 8) |
                    (uint32_t)frame[3];
  EXPECT_EQ(length, (uint32_t)sizeof(payload));

  // Verify payload
  EXPECT_EQ(memcmp(frame + 4, payload, sizeof(payload)), 0);

  free(frame);
}

TEST(StreamFramer, EncodeEmptyPayload) {
  size_t out_len = 0;
  uint8_t* frame = stream_frame_encode(NULL, 0, &out_len);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(out_len, (size_t)4);

  uint32_t length = ((uint32_t)frame[0] << 24) |
                    ((uint32_t)frame[1] << 16) |
                    ((uint32_t)frame[2] << 8) |
                    (uint32_t)frame[3];
  EXPECT_EQ(length, (uint32_t)0);

  free(frame);
}

TEST(StreamFramer, EncodeNullDataWithPositiveLength) {
  size_t out_len = 0;
  uint8_t* frame = stream_frame_encode(NULL, 10, &out_len);
  EXPECT_EQ(frame, nullptr);
}

// === stream_framer accumulator tests ===

TEST(StreamFramer, SingleCompleteMessage) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(payload, sizeof(payload), &frame_len);
  ASSERT_NE(frame, nullptr);

  int result = stream_framer_feed(framer, frame, frame_len);
  EXPECT_EQ(result, 0);

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, sizeof(payload));
  EXPECT_EQ(memcmp(msg, payload, sizeof(payload)), 0);

  free(msg);
  free(frame);

  // No more messages available
  uint8_t* msg2 = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg2, nullptr);

  stream_framer_destroy(framer);
}

TEST(StreamFramer, SplitMessage) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55};
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(payload, sizeof(payload), &frame_len);
  ASSERT_NE(frame, nullptr);

  // Feed first half
  size_t first_half = frame_len / 2;
  int result = stream_framer_feed(framer, frame, first_half);
  EXPECT_EQ(result, 0);

  // Not enough data yet
  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg, nullptr);

  // Feed second half
  result = stream_framer_feed(framer, frame + first_half, frame_len - first_half);
  EXPECT_EQ(result, 0);

  msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, sizeof(payload));
  EXPECT_EQ(memcmp(msg, payload, sizeof(payload)), 0);

  free(msg);
  free(frame);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, MultipleMessagesInOneFeed) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  uint8_t payload1[] = {0x01, 0x02};
  uint8_t payload2[] = {0x03, 0x04, 0x05};

  size_t frame1_len = 0;
  size_t frame2_len = 0;
  uint8_t* frame1 = stream_frame_encode(payload1, sizeof(payload1), &frame1_len);
  uint8_t* frame2 = stream_frame_encode(payload2, sizeof(payload2), &frame2_len);
  ASSERT_NE(frame1, nullptr);
  ASSERT_NE(frame2, nullptr);

  // Concatenate both frames into a single buffer
  size_t combined_len = frame1_len + frame2_len;
  uint8_t* combined = (uint8_t*)malloc(combined_len);
  ASSERT_NE(combined, nullptr);
  memcpy(combined, frame1, frame1_len);
  memcpy(combined + frame1_len, frame2, frame2_len);

  int result = stream_framer_feed(framer, combined, combined_len);
  EXPECT_EQ(result, 0);

  // Extract first message
  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, sizeof(payload1));
  EXPECT_EQ(memcmp(msg, payload1, sizeof(payload1)), 0);
  free(msg);

  // Extract second message
  msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, sizeof(payload2));
  EXPECT_EQ(memcmp(msg, payload2, sizeof(payload2)), 0);
  free(msg);

  // No more messages
  msg = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg, nullptr);

  free(combined);
  free(frame1);
  free(frame2);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, EmptyFeed) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  int result = stream_framer_feed(framer, NULL, 0);
  EXPECT_EQ(result, 0);

  uint8_t empty_data[] = {0x01};
  result = stream_framer_feed(framer, empty_data, 0);
  EXPECT_EQ(result, 0);

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg, nullptr);

  stream_framer_destroy(framer);
}

TEST(StreamFramer, PartialLengthHeader) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(payload, sizeof(payload), &frame_len);
  ASSERT_NE(frame, nullptr);

  // Feed only first 2 bytes (partial length header)
  int result = stream_framer_feed(framer, frame, 2);
  EXPECT_EQ(result, 0);

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg, nullptr);

  // Feed remaining bytes (2 more header bytes + payload)
  result = stream_framer_feed(framer, frame + 2, frame_len - 2);
  EXPECT_EQ(result, 0);

  msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, sizeof(payload));
  EXPECT_EQ(memcmp(msg, payload, sizeof(payload)), 0);

  free(msg);
  free(frame);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, ZeroLengthMessage) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  // Encode a zero-length payload (just the 4-byte length prefix)
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(NULL, 0, &frame_len);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame_len, (size_t)4);

  int result = stream_framer_feed(framer, frame, frame_len);
  EXPECT_EQ(result, 0);

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, (size_t)0);

  free(msg);
  free(frame);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, DestroyCleanup) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  uint8_t payload[] = {0x01, 0x02, 0x03};
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(payload, sizeof(payload), &frame_len);
  ASSERT_NE(frame, nullptr);

  // Feed data but do not extract it
  int result = stream_framer_feed(framer, frame, frame_len);
  EXPECT_EQ(result, 0);

  // Destroy should free internal buffer without leaks
  stream_framer_destroy(framer);
  free(frame);
}

TEST(StreamFramer, NullFramer) {
  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(NULL, &msg_len);
  EXPECT_EQ(msg, nullptr);

  int result = stream_framer_feed(NULL, NULL, 0);
  EXPECT_EQ(result, -1);
}

TEST(StreamFramer, LargePayload) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  // Create a large payload (1KB)
  size_t large_size = 1024;
  uint8_t* large_payload = (uint8_t*)malloc(large_size);
  ASSERT_NE(large_payload, nullptr);
  for (size_t index = 0; index < large_size; index++) {
    large_payload[index] = (uint8_t)(index & 0xFF);
  }

  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(large_payload, large_size, &frame_len);
  ASSERT_NE(frame, nullptr);

  int result = stream_framer_feed(framer, frame, frame_len);
  EXPECT_EQ(result, 0);

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, large_size);
  EXPECT_EQ(memcmp(msg, large_payload, large_size), 0);

  free(msg);
  free(frame);
  free(large_payload);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, BufferGrowth) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  // Feed data larger than initial capacity (256 bytes)
  size_t payload_size = 512;
  uint8_t* payload = (uint8_t*)malloc(payload_size);
  ASSERT_NE(payload, nullptr);
  for (size_t index = 0; index < payload_size; index++) {
    payload[index] = (uint8_t)(index & 0xFF);
  }

  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(payload, payload_size, &frame_len);
  ASSERT_NE(frame, nullptr);

  int result = stream_framer_feed(framer, frame, frame_len);
  EXPECT_EQ(result, 0);

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, payload_size);
  EXPECT_EQ(memcmp(msg, payload, payload_size), 0);

  free(msg);
  free(frame);
  free(payload);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, ThreePartialFeeds) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(payload, sizeof(payload), &frame_len);
  ASSERT_NE(frame, nullptr);

  // Feed 1 byte, then 2 bytes, then the rest
  size_t offset = 0;
  int result = stream_framer_feed(framer, frame, 1);
  EXPECT_EQ(result, 0);
  offset = 1;

  result = stream_framer_feed(framer, frame + offset, 2);
  EXPECT_EQ(result, 0);
  offset = 3;

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg, nullptr);  // Still not enough data (only 3 bytes of header)

  result = stream_framer_feed(framer, frame + offset, frame_len - offset);
  EXPECT_EQ(result, 0);

  msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, sizeof(payload));
  EXPECT_EQ(memcmp(msg, payload, sizeof(payload)), 0);

  free(msg);
  free(frame);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, MessageSpanningMultipleFeeds) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);

  uint8_t payload[] = {0xAA, 0xBB};
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(payload, sizeof(payload), &frame_len);
  ASSERT_NE(frame, nullptr);

  // Feed 1 byte at a time
  for (size_t index = 0; index < frame_len; index++) {
    int result = stream_framer_feed(framer, frame + index, 1);
    EXPECT_EQ(result, 0);
  }

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, sizeof(payload));
  EXPECT_EQ(memcmp(msg, payload, sizeof(payload)), 0);

  free(msg);
  free(frame);
  stream_framer_destroy(framer);
}