//
// Created by victor on 3/30/25.
//
#include "block.h"
#include "frand.h"
#include "../Util/allocator.h"
#include "../../deps/BLAKE3/c/blake3.h"
#include <blake3.h>

block_t* block_create(buffer_t* data) {
  return block_create_by_type(data, standard);
}

block_t* block_created_by_type(buffer_t* data, block_size_e type) {
  size_t size = type;
  if (data->size > size) {
    return NULL;
  }
  buffer_t* final;
  if (data->size < size) {
    size_t diff = size - data->size;
    buffer_t* randBuf = buffer_create_from_existing_memory( frand(diff), diff);
    buffer_t* final = buffer_concat(data, randBuf);
    buffer_destroy(randBuf);
  } else {
    final = data;
  }

  block_t* block = get_clear_memory(sizeof(block_t));
  block->data= final;
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, final, final->size);
  uint8_t* digest = get_memory(BLAKE3_OUT_LEN);
  blake3_hasher_finalize(&hasher, &digest, BLAKE3_OUT_LEN);
  block->hash = buffer_create_from_existing_memory(digest, BLAKE3_OUT_LEN);

  refcounter_init((refcounter_t*) block);
  return block;
}

block_t* block_create_existing_data(buffer_t* data) {
  return block_create_existing_data_by_type(data,standard);
}

block_t* block_create_existing_data_by_type(buffer_t* data, block_size_e type) {
  if ((data->size != standard) && (data->size != mega) && (data->size != nano) && (data->size != mini)) {
    return NULL;
  }
  block_t* block = get_clear_memory(sizeof(block_t));
  block->data = refcounter_reference(data);
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data, data->size);
  uint8_t* digest = get_memory(BLAKE3_OUT_LEN);
  blake3_hasher_finalize(&hasher, &digest, BLAKE3_OUT_LEN);
  block->hash = buffer_create_from_existing_memory(digest, BLAKE3_OUT_LEN);
  refcounter_init((refcounter_t*) block);
  return block;
}

block_t* block_create_existing_data_hash(buffer_t* data, buffer_t* hash) {
  return block_create_existing_data_hash_by_type(data, hash,standard);
}

block_t* block_create_existing_data_hash_by_type(buffer_t* data, buffer_t* hash, block_size_e type) {
  if ((data->size != standard) && (data->size != mega) && (data->size != nano) && (data->size != mini)) {
    return NULL;
  }
  if (hash->size != BLAKE3_OUT_LEN) {
    return NULL;
  }

  block_t* block = get_clear_memory(sizeof(block_t));
  block->hash = refcounter_reference(hash);
  block->data = refcounter_reference(data);
  refcounter_init((refcounter_t*) block);
  return block;
}

void block_destroy(buffer_t* block) {
  buffer_destroy(block->hash);
  buffer_destroy(block->data);
  refcounter_dereference((refcounter_t*) block);
  if (refcounter_count(block) == 0) {
    free(block);
  }
}

block_t* block_xor(block_t* block1, block_t* block2) {
  buffer_t* data = buffer_xor(block1->data, block2->data);
  block_t* block = block_create_existing_data_by_type(data, data->size);
  buffer_destroy(data);
  return block;
}

