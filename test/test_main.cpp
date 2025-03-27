//
// Created by victor on 3/22/25.
//

#include <gtest/gtest.h>
extern "C" {
#include "../src/RefCounter/refcounter.h"
#include "../src/RefCounter/refcounter.p.h"
#include "../src/Buffer/buffer.h"
}

TEST(TestRefCounter, TestRefCounterFunctions) {
  refcounter_t* refc1 = (refcounter_t*) calloc(sizeof(refcounter_t), 1);
  refcounter_init(refc1);

  refcounter_t* refc2 = (refcounter_t*) refcounter_reference(refc1);
  EXPECT_EQ(refc1, refc2);

  EXPECT_EQ(refcounter_count(refc1), 2);
  refcounter_yield((refcounter_t*) refc2);
  refc2 = NULL;
  refcounter_t* refc3 = (refcounter_t*) refcounter_reference(refc1);
  EXPECT_EQ(refcounter_count(refc3), 2);
  EXPECT_EQ(refc1, refc3);
  refcounter_dereference(refc3);
  refc3 = NULL;
  EXPECT_EQ(refcounter_count(refc1), 1);
}

TEST(TestBuffer, TestBufferCreation) {
  buffer_t* buf = buffer_create(25);
  ASSERT_EQ(buf->size, 25);
  for (size_t i = 0; i < buf->size; i++) {
    EXPECT_EQ(buffer_get_index(buf, i), 0);
  }
  uint8_t data[10] = {1,2,3,4,5,6,7,8};
  buffer_t* buf2 = buffer_create_from_pointer_copy((uint8_t*) &data, 8);
  for (size_t i = 0; i < 8; i++) {
    EXPECT_EQ(buffer_get_index(buf2, i), data[i]);
    EXPECT_NE(&buf2->data[i], &data[i]);
  }
  uint8_t* data2 = (uint8_t*) calloc(8, sizeof(uint8_t));
  memcpy(&data, data2,8);
  buffer_t* buf3 = buffer_create_from_existing_memory(data2, 8);
  for (size_t i = 0; i < 8; i++) {
    EXPECT_EQ(buffer_get_index(buf3, i), data[i]);
    EXPECT_EQ(&buf3->data[i], &data2[i]);
  }
  buffer_destroy(buf3);
  buffer_destroy(buf2);
  buffer_destroy(buf);
}

TEST(TestBuffer, TestBufferBitwise) {
  uint8_t data1[4] = {255, 255, 255,255};
  buffer_t* buf1 = buffer_create_from_pointer_copy((uint8_t*) data1, 4);
  uint8_t data2[4] = {0,0,0,0};
  buffer_t* buf2 = buffer_create_from_pointer_copy(data2, 4);
  buffer_t* notResult = buffer_not(buf1);
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(buffer_get_index(buf2, i), 0);
  }
  EXPECT_EQ(buffer_compare(notResult, buf2), 0);
  buffer_t* xorResult = buffer_xor(buf1, buf2);
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(buffer_get_index(xorResult, i), 255);
  }
  buffer_t* xorResult2 = buffer_xor(xorResult, buf1);
  EXPECT_EQ(buffer_compare(xorResult2, buf2), 0);
  uint8_t data3[5] ={0,0,0,0,1};
  buffer_t* buf3 = buffer_create_from_pointer_copy(data3, 5);
  EXPECT_EQ(buffer_compare(buf3, buf2), 1);
  EXPECT_EQ(buffer_compare(buf2, buf3), -1);

  buffer_t* andResult = buffer_and(buf1, buf2);
  EXPECT_EQ(buffer_compare(andResult, buf2), 0);
  buffer_t* orResult = buffer_or(buf1, buf2);
  EXPECT_EQ(buffer_compare(orResult, buf1), 0);
  buffer_t* sliceResult = buffer_slice(buf3, 3,5);
  EXPECT_EQ(sliceResult->size, 2);
  EXPECT_EQ(buffer_get_index(sliceResult, 0), 0);
  EXPECT_EQ(buffer_get_index(sliceResult, 1), 1);
  buffer_destroy(buf1);
  buffer_destroy(buf2);
  buffer_destroy(buf3);
  buffer_destroy(notResult);
  buffer_destroy(xorResult);
  buffer_destroy(xorResult2);
  buffer_destroy(andResult);
  buffer_destroy(orResult);
  buffer_destroy(sliceResult);
}