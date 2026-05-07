#include <gtest/gtest.h>
extern "C" {
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/ori.h"
#include "../src/Buffer/buffer.h"
}

TEST(TestTuple, TestCreateDestroy) {
  tuple_t* tuple = tuple_create(3);
  ASSERT_NE(tuple, nullptr);
  EXPECT_EQ(tuple_size(tuple), 0u);
  EXPECT_EQ(tuple->capacity, 3u);
  tuple_destroy(tuple);
}

TEST(TestTuple, TestPushAndGet) {
  tuple_t* tuple = tuple_create(3);
  uint8_t data1[] = {0x01, 0x02, 0x03};
  uint8_t data2[] = {0x04, 0x05, 0x06};
  buffer_t* hash1 = buffer_create_from_pointer_copy(data1, 3);
  buffer_t* hash2 = buffer_create_from_pointer_copy(data2, 3);

  tuple_push(tuple, hash1);
  tuple_push(tuple, hash2);

  EXPECT_EQ(tuple_size(tuple), 2u);

  buffer_t* got0 = tuple_get(tuple, 0);
  buffer_t* got1 = tuple_get(tuple, 1);
  ASSERT_NE(got0, nullptr);
  ASSERT_NE(got1, nullptr);
  EXPECT_EQ(memcmp(got0->data, data1, 3), 0);
  EXPECT_EQ(memcmp(got1->data, data2, 3), 0);

  tuple_destroy(tuple);
  DESTROY(hash1, buffer);
  DESTROY(hash2, buffer);
}

TEST(TestTuple, TestShift) {
  tuple_t* tuple = tuple_create(3);
  uint8_t data1[] = {0xAA};
  uint8_t data2[] = {0xBB};
  buffer_t* hash1 = buffer_create_from_pointer_copy(data1, 1);
  buffer_t* hash2 = buffer_create_from_pointer_copy(data2, 1);

  tuple_push(tuple, hash1);
  tuple_push(tuple, hash2);

  EXPECT_EQ(tuple_size(tuple), 2u);

  buffer_t* shifted = tuple_shift(tuple);
  ASSERT_NE(shifted, nullptr);
  EXPECT_EQ(shifted->data[0], 0xAA);
  EXPECT_EQ(tuple_size(tuple), 1u);

  DESTROY(shifted, buffer);
  tuple_destroy(tuple);
  DESTROY(hash2, buffer);
}

TEST(TestTuple, TestHash) {
  tuple_t* a = tuple_create(2);
  tuple_t* b = tuple_create(2);
  uint8_t data1[] = {0x01};
  uint8_t data2[] = {0x02};
  buffer_t* h1a = buffer_create_from_pointer_copy(data1, 1);
  buffer_t* h2a = buffer_create_from_pointer_copy(data2, 1);
  buffer_t* h1b = buffer_create_from_pointer_copy(data1, 1);
  buffer_t* h2b = buffer_create_from_pointer_copy(data2, 1);

  tuple_push(a, h1a);
  tuple_push(a, h2a);
  tuple_push(b, h1b);
  tuple_push(b, h2b);

  EXPECT_EQ(tuple_hash(a), tuple_hash(b));
  EXPECT_TRUE(tuple_equals(a, b));

  tuple_destroy(a);
  tuple_destroy(b);
  DESTROY(h1a, buffer);
  DESTROY(h2a, buffer);
  DESTROY(h1b, buffer);
  DESTROY(h2b, buffer);
}

TEST(TestTuple, TestNotEquals) {
  tuple_t* a = tuple_create(2);
  tuple_t* b = tuple_create(2);
  uint8_t data1[] = {0x01};
  uint8_t data2[] = {0x02};
  uint8_t data3[] = {0x03};
  buffer_t* h1 = buffer_create_from_pointer_copy(data1, 1);
  buffer_t* h2 = buffer_create_from_pointer_copy(data2, 1);
  buffer_t* h3 = buffer_create_from_pointer_copy(data3, 1);

  tuple_push(a, h1);
  tuple_push(a, h2);
  tuple_push(b, h1);
  tuple_push(b, h3);

  EXPECT_FALSE(tuple_equals(a, b));

  tuple_destroy(a);
  tuple_destroy(b);
  DESTROY(h1, buffer);
  DESTROY(h2, buffer);
  DESTROY(h3, buffer);
}

TEST(TestORI, TestCreateDestroy) {
  ori_t* ori = ori_create(1024);
  ASSERT_NE(ori, nullptr);
  EXPECT_EQ(ori->final_byte, 1024u);
  EXPECT_EQ(ori->descriptor_offset, 0u);
  EXPECT_EQ(ori->block_type, standard);
  EXPECT_EQ(ori->tuple_size, 0u);
  EXPECT_EQ(ori->file_offset, 0u);
  EXPECT_EQ(ori->descriptor_hash, nullptr);
  EXPECT_EQ(ori->file_hash, nullptr);
  EXPECT_EQ(ori->file_name, nullptr);
  ori_destroy(ori);
}

TEST(TestORI, TestSetFields) {
  ori_t* ori = ori_create(2048);
  uint8_t hash_data[] = {0xAB, 0xCD, 0xEF};
  ori->descriptor_hash = buffer_create_from_pointer_copy(hash_data, 3);
  ori->file_hash = buffer_create_from_pointer_copy(hash_data, 3);
  ori->file_name = strdup("test.pdf");
  ori->tuple_size = 5;
  ori->descriptor_offset = 100;

  EXPECT_EQ(ori->tuple_size, 5u);
  EXPECT_EQ(ori->descriptor_offset, 100u);
  EXPECT_STREQ(ori->file_name, "test.pdf");

  ori_destroy(ori);
}