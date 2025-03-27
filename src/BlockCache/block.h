//
// Created by victor on 3/20/25.
//

#ifndef LIBOFFS_BLOCK_H
#define LIBOFFS_BLOCK_H
#include "../Buffer/buffer.h"
#include "../RefCounter/refcounter.h"
#include "../RefCounter/refcounter.p.h"

#define STANDARD_BLOCK_SIZE 128000

typedef struct {
  refcounter_t refcounter;
  const buffer_t* data;
  const buffer_t* hash;
} block_t;

block_t* create_block(buffer_t* data);
block_t* create_block_existing_data(buffer_t* data);
block_t* create_block_existing_data_hash(buffer_t* data, buffer_t* hash);
#endif //LIBOFFS_BLOCK_H
