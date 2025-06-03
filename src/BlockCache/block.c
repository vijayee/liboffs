//
// Created by victor on 3/30/25.
//
#include "block.h"
#include "frand.h"
#include "../Util/allocator.h"
#include "../../deps/BLAKE3/c/blake3.h"
#include <blake3.h>

buffer_t* hash_data(buffer_t* data) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data->data, data->size);
  uint8_t* digest = get_clear_memory(BLAKE3_OUT_LEN);
  blake3_hasher_finalize(&hasher, digest, BLAKE3_OUT_LEN);
  return buffer_create_from_existing_memory(digest, BLAKE3_OUT_LEN);
}

block_t* block_create(buffer_t* data) {
  return block_create_by_type(data, standard);
}

block_t* block_create_by_type(buffer_t* data, block_size_e type) {
  size_t size = type;
  if (data->size > size) {
    return NULL;
  }
  buffer_t* final = NULL;
  if (data->size < size) {
    size_t diff = size - data->size;
    buffer_t* randBuf = buffer_create_from_existing_memory( frand(diff), diff);
    final = buffer_concat(data, randBuf);
    buffer_destroy(randBuf);
  } else {
    final = data;
  }

  block_t* block = get_clear_memory(sizeof(block_t));
  block->data= final;
  block->hash = hash_data(final);
  refcounter_init((refcounter_t*) block);
  return block;
}

block_t* block_create_random_block() {
  return block_create_random_block_by_type(standard);
}

block_t* block_create_random_block_by_type(block_size_e type) {
  size_t size = type;
  buffer_t* randBuf = buffer_create_from_existing_memory( frand(size), size);

  block_t* block = get_clear_memory(sizeof(block_t));
  block->data= randBuf;
  block->hash = hash_data(randBuf);

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
  block->data = (buffer_t*) refcounter_reference((refcounter_t*)data);
  block->hash = hash_data(block->data);
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
  block->hash = refcounter_reference((refcounter_t*)hash);
  block->data = refcounter_reference((refcounter_t*)data);
  refcounter_init((refcounter_t*) block);
  return block;
}

void block_destroy(block_t* block) {
  buffer_destroy(block->hash);
  buffer_destroy(block->data);
  refcounter_dereference((refcounter_t*) block);
  if (refcounter_count((refcounter_t*) block) == 0) {
    refcounter_destroy_lock((refcounter_t*) block);
    free(block);
  }
}

block_t* block_xor(block_t* block1, block_t* block2) {
  buffer_t* data = buffer_xor(block1->data, block2->data);
  block_t* block = block_create_existing_data_by_type(data, data->size);
  buffer_destroy(data);
  return block;
}

cbor_item_t* block_to_cbor(block_t* block) {
  cbor_item_t* array = cbor_new_definite_array(2);
  bool success = cbor_array_push(array, cbor_move(buffer_to_cbor(block->hash)));
  success &= cbor_array_push(array, cbor_move(buffer_to_cbor(block->data)));
  if (!success) {
    cbor_decref(&array);
    return NULL;
  } else {
    return array;
  }
}

block_t* cbor_to_block(cbor_item_t* cbor) {
  cbor_item_t* cbor_hash = cbor_move(cbor_array_get(cbor, 0));
  cbor_item_t* cbor_data = cbor_move(cbor_array_get(cbor, 1));
  buffer_t* hash = cbor_to_buffer(cbor_hash);
  buffer_t* data = cbor_to_buffer(cbor_data);
  refcounter_yield((refcounter_t*) hash);
  refcounter_yield((refcounter_t*) data);
  block_t* block = block_create_existing_data_hash_by_type(data, hash, data->size);
  return block;
}