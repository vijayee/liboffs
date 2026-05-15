//
// Created by victor on 5/14/25.
//

#include "node_id.h"
#include "../Util/base58.h"
#include "../../deps/BLAKE3/c/blake3.h"
#include <string.h>
#include <stdlib.h>

int node_id_from_public_key(const uint8_t* public_key, size_t key_len, node_id_t* result) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, public_key, key_len);
  blake3_hasher_finalize(&hasher, result->hash, NODE_ID_HASH_SIZE);
  base58_encode(result->hash, NODE_ID_HASH_SIZE, result->str, NODE_ID_STRING_SIZE);
  return 0;
}

int node_id_from_string(const char* str, node_id_t* result) {
  size_t bytes_written = 0;
  int rc = base58_decode(str, result->hash, NODE_ID_HASH_SIZE, &bytes_written);
  if (rc != 0 || bytes_written != NODE_ID_HASH_SIZE) {
    node_id_clear(result);
    return -1;
  }
  strncpy(result->str, str, NODE_ID_STRING_SIZE - 1);
  result->str[NODE_ID_STRING_SIZE - 1] = '\0';
  return 0;
}

int node_id_compare(const node_id_t* left, const node_id_t* right) {
  return memcmp(left->hash, right->hash, NODE_ID_HASH_SIZE);
}

bool node_id_equals(const node_id_t* left, const node_id_t* right) {
  return memcmp(left->hash, right->hash, NODE_ID_HASH_SIZE) == 0;
}

bool node_id_is_null(const node_id_t* node_id) {
  for (size_t index = 0; index < NODE_ID_HASH_SIZE; index++) {
    if (node_id->hash[index] != 0) {
      return false;
    }
  }
  return true;
}

void node_id_clear(node_id_t* node_id) {
  memset(node_id->hash, 0, NODE_ID_HASH_SIZE);
  memset(node_id->str, 0, NODE_ID_STRING_SIZE);
}

uint64_t node_id_hash(const node_id_t* node_id) {
  uint64_t result;
  memcpy(&result, node_id->hash, sizeof(uint64_t));
  return result;
}

void node_id_generate(node_id_t* result) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  uint64_t randomness[4];
  for (size_t index = 0; index < 4; index++) {
    randomness[index] = (uint64_t)rand() | ((uint64_t)rand() << 32);
  }
  blake3_hasher_update(&hasher, randomness, sizeof(randomness));
  blake3_hasher_finalize(&hasher, result->hash, NODE_ID_HASH_SIZE);
  base58_encode(result->hash, NODE_ID_HASH_SIZE, result->str, NODE_ID_STRING_SIZE);
}