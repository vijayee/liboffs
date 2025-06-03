//
// Created by victor on 5/26/25.
//

#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "../src/BlockCache/block.h"
#include <cbor.h>
}


TEST(TestBlock, TestBlockOperations) {
  uint8_t data[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*) &data, 20);
  block_t* block1 = block_create(buf);
  block_t* block2 = block_create_random_block();
  block_t* block3 = block_xor(block1, block2);
  block_t* block4 = block_xor(block3, block2);
  EXPECT_EQ(buffer_compare(block1->data, block4->data), 0);

  buffer_t* hash1 = hash_data(block1->data);
  buffer_t* hash2 = hash_data(block1->data);
  buffer_t* hash3 = hash_data(block4->data);
  EXPECT_EQ(buffer_compare(hash1, hash2), 0);
  EXPECT_EQ(buffer_compare(hash3, hash2), 0);
  buffer_destroy(hash1);
  buffer_destroy(hash2);
  buffer_destroy(hash3);
  for (size_t i = 0; i < block1->data->size; i++) {
    EXPECT_EQ(buffer_get_index(block1->data, i), buffer_get_index(block4->data, i));
  }

  cbor_item_t* cbor = block_to_cbor(block1);
  EXPECT_EQ(cbor == NULL, false);
  uint8_t* cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  struct cbor_load_result result;
  cbor_item_t* cbor2 = cbor_load(cbor_data, cbor_size, &result);

  EXPECT_EQ(result.error.code == CBOR_ERR_NONE, true);
  EXPECT_EQ(cbor_isa_array(cbor2), true);
  block_t* from_cbor = cbor_to_block(cbor2);

  EXPECT_EQ(from_cbor == NULL, false);
  EXPECT_EQ(buffer_compare(from_cbor->hash, block1->hash), 0);
  EXPECT_EQ(buffer_compare(from_cbor->data, block1->data), 0);

  EXPECT_EQ(buffer_compare(block1->data, block4->data), 0);
  EXPECT_EQ(buffer_compare(block1->hash, block4->hash), 0);
  free(cbor_data);
  cbor_decref(&cbor);
  cbor_decref(&cbor2);
  block_destroy(from_cbor);
  block_destroy(block4);
  block_destroy(block3);
  block_destroy(block2);
  block_destroy(block1);
  buffer_destroy(buf);
}
