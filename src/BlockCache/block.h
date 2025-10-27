//
// Created by victor on 3/20/25.
//

#ifndef LIBOFFS_BLOCK_H
#define LIBOFFS_BLOCK_H
#include "../Buffer/buffer.h"
#include "../RefCounter/refcounter.h"
#include <cbor.h>

typedef enum {
  mega = 1000000,
  standard = 128000,
  mini = 64000,
  nano = 136
} block_size_e;

typedef struct {
  refcounter_t refcounter;
  buffer_t* data;
  buffer_t* hash;
} block_t;

buffer_t* hash_data(buffer_t* data);
block_t* block_create(buffer_t* data);
block_t* block_create_by_type(buffer_t* data, block_size_e type);
block_t* block_create_random_block();
block_t* block_create_random_block_by_type(block_size_e type);
block_t* block_create_existing_data(buffer_t* data);
block_t* block_create_existing_data_by_type(buffer_t* data, block_size_e type);
block_t* block_create_existing_data_hash(buffer_t* data, buffer_t* hash);
block_t* block_create_existing_data_hash_by_type(buffer_t* data, buffer_t* hash, block_size_e type);
block_t* block_xor(block_t* block1, block_t* block2);
cbor_item_t* block_to_cbor(block_t* block);
block_t* cbor_to_block(cbor_item_t* cbor);
void block_destroy(block_t* block);

#endif //LIBOFFS_BLOCK_H
