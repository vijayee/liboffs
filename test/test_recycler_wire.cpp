#include <gtest/gtest.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include <cbor.h>
}
#include <cstring>

TEST(TestRecyclerWire, TestPutRequestEncodeDecode_WithRecyclerAndTemporary) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"text/plain";
  msg.file_name = (char*)"test.txt";
  msg.stream_length = 1024;
  msg.server_address = (char*)"http://localhost:23402/offsystem/v3/";

  const char* urls[] = {
    "http://localhost:23402/offsystem/v3/text/plain/100/abc123/def456/file.txt",
    "http://localhost:23402/offsystem/v3/image/png/200/ghi789/jkl012/photo.png"
  };
  msg.recycler_urls = (char**)urls;
  msg.recycler_count = 2;
  msg.temporary = 1;

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(cbor_array_size(encoded), (size_t)8);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int result = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(result, 0);

  EXPECT_STREQ(decoded.content_type, "text/plain");
  EXPECT_STREQ(decoded.file_name, "test.txt");
  EXPECT_EQ(decoded.stream_length, (size_t)1024);
  EXPECT_STREQ(decoded.server_address, "http://localhost:23402/offsystem/v3/");
  EXPECT_EQ(decoded.recycler_count, (size_t)2);
  ASSERT_NE(decoded.recycler_urls, nullptr);
  EXPECT_STREQ(decoded.recycler_urls[0], urls[0]);
  EXPECT_STREQ(decoded.recycler_urls[1], urls[1]);
  EXPECT_EQ(decoded.temporary, 1);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(TestRecyclerWire, TestPutRequestEncodeDecode_NoRecycler) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"text/plain";
  msg.file_name = (char*)"test.txt";
  msg.stream_length = 1024;

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(cbor_array_size(encoded), (size_t)8);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int result = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(result, 0);

  EXPECT_STREQ(decoded.content_type, "text/plain");
  EXPECT_STREQ(decoded.file_name, "test.txt");
  EXPECT_EQ(decoded.stream_length, (size_t)1024);
  EXPECT_EQ(decoded.recycler_count, (size_t)0);
  EXPECT_EQ(decoded.recycler_urls, nullptr);
  EXPECT_EQ(decoded.temporary, 0);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(TestRecyclerWire, TestPutRequestEncodeDecode_RecyclerOnly) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"application/octet-stream";
  msg.file_name = (char*)"data.bin";
  msg.stream_length = 4096;

  const char* urls[] = {
    "http://localhost:23402/offsystem/v3/app/octet-stream/500/aaa111/bbb222/old.bin"
  };
  msg.recycler_urls = (char**)urls;
  msg.recycler_count = 1;
  msg.temporary = 0;

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(cbor_array_size(encoded), (size_t)8);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int result = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(result, 0);

  EXPECT_STREQ(decoded.content_type, "application/octet-stream");
  EXPECT_STREQ(decoded.file_name, "data.bin");
  EXPECT_EQ(decoded.stream_length, (size_t)4096);
  EXPECT_EQ(decoded.recycler_count, (size_t)1);
  ASSERT_NE(decoded.recycler_urls, nullptr);
  EXPECT_STREQ(decoded.recycler_urls[0], urls[0]);
  EXPECT_EQ(decoded.temporary, 0);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}
