//
// Created by victor on 7/19/25.
//

#ifndef OFFS_SECTION_H
#define OFFS_SECTION_H
#include <stddef.h>
#include <stdio.h>
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include "block.h"

/* Bitmap-based free block tracking.
   Each bit represents one block slot: 1 = free, 0 = occupied.
   For sections with up to 32 blocks, a single uint32_t suffices.
   Larger sections use a dynamically allocated array of uint32_t. */
typedef struct {
  uint32_t* map;
  size_t map_capacity; /* number of uint32_t words */
  size_t total_blocks;
} free_map_t;

typedef struct {
  refcounter_t refcounter;
  PLATFORMLOCKTYPE(lock);
  int fd;
  size_t id;
  char* meta_path;
  char* path;
  free_map_t free_map;
  size_t size;
  block_size_e block_size;
} section_t;

section_t* section_create(char* path, char* meta_path, size_t size, size_t id, block_size_e type);
void section_destroy(section_t* section);
int section_write(section_t* section, buffer_t* data, size_t* index, uint8_t* full);
buffer_t* section_read(section_t* section, size_t index);
int section_deallocate(section_t* section, size_t index);
uint8_t section_full(section_t* section);
#endif //OFFS_SECTION_H