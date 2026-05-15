//
// Created by victor on 5/14/25.
//

#ifndef OFFS_NODE_ID_H
#define OFFS_NODE_ID_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NODE_ID_HASH_SIZE 32
#define NODE_ID_STRING_SIZE 48

typedef struct node_id_t {
  uint8_t hash[NODE_ID_HASH_SIZE];
  char str[NODE_ID_STRING_SIZE];
} node_id_t;

int node_id_from_public_key(const uint8_t* public_key, size_t key_len, node_id_t* result);
int node_id_from_string(const char* str, node_id_t* result);
int node_id_compare(const node_id_t* left, const node_id_t* right);
bool node_id_equals(const node_id_t* left, const node_id_t* right);
bool node_id_is_null(const node_id_t* node_id);
void node_id_clear(node_id_t* node_id);
uint64_t node_id_hash(const node_id_t* node_id);
void node_id_generate(node_id_t* result);

#endif // OFFS_NODE_ID_H