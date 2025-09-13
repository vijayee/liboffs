//
// Created by victor on 4/29/25.
//

#ifndef OFFS_HASH_H
#define OFFS_HASH_H
#include <stdlib.h>
#include <stdint.h>
#include "../BlockCache/block.h"
#include "../Buffer/buffer.h"
size_t hash_pointer(const void* ptr);
size_t hash_uint32(const void* data);
size_t hash_uint64(const void* data);
int compare_uint32(const void* data1, const void* data2);
int compare_uint64(const void* data1, const void* data2);
uint32_t* duplicate_uint32(const uint32_t* key);
uint64_t* duplicate_uint64(const uint64_t* key);
size_t* duplicate_size_t(const size_t* key);
size_t hash_buffer(const buffer_t* data);
size_t hash_block(const block_t* data);
size_t hash_size_t(const void* data);
int compare_size_t(const void* data1, const void* data2);
int compare_buffer(const void* data1, const void* data2);
#endif //OFFS_HASH_H
