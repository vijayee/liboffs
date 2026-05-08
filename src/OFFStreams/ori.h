//
// Created by victor on 5/7/26.
//

#ifndef OFFS_ORI_H
#define OFFS_ORI_H

#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "../BlockCache/block.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  refcounter_t refcounter;
  buffer_t* descriptor_hash;
  size_t descriptor_offset;
  block_size_e block_type;
  size_t tuple_size;
  buffer_t* file_hash;
  size_t file_offset;
  char* file_name;
  size_t final_byte;
} ori_t;

ori_t* ori_create(size_t final_byte);
void ori_destroy(ori_t* ori);

#endif //OFFS_ORI_H