//
// Created by victor on 7/19/25.
//

#ifndef OFFS_SECTION_H
#define OFFS_SECTION_H
#include <stddef.h>
#include <stdio.h>
#include "../Actor/actor.h"
#include "../RefCounter/refcounter.h"
#include "../Util/atomic_compat.h"
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

/* Payload for SECTION_WRITE message.
   When reply_to is NULL (sync), result/index/full are filled by dispatch.
   When reply_to is set (async), a SECTION_WRITE_COMPLETE is sent back. */
typedef struct {
  buffer_t* data;
  actor_t* reply_to;
  int result;
  size_t index;
  uint8_t full;
} section_write_payload_t;

/* Payload for SECTION_READ message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a SECTION_READ_COMPLETE is sent back. */
typedef struct {
  size_t index;
  actor_t* reply_to;
  buffer_t* result;
} section_read_payload_t;

/* Payload for SECTION_DEALLOCATE message.
   When reply_to is NULL (sync), result is filled by dispatch.
   When reply_to is set (async), a SECTION_DEALLOCATE_COMPLETE is sent back. */
typedef struct {
  size_t index;
  actor_t* reply_to;
  int result;
} section_deallocate_payload_t;

/* Completion result for SECTION_WRITE_COMPLETE */
typedef struct {
  int result;
  size_t index;
  uint8_t full;
} section_write_result_t;

/* Completion result for SECTION_READ_COMPLETE */
typedef struct {
  buffer_t* data;
} section_read_result_t;

/* Completion result for SECTION_DEALLOCATE_COMPLETE */
typedef struct {
  int result;
} section_deallocate_result_t;

/* Async result payload for SECTION_READ_RESULT */
typedef struct {
  size_t index;
  buffer_t* data;
  actor_t* reply_to;
} section_read_result_payload_t;

/* Async result payload for SECTION_WRITE_RESULT */
typedef struct {
  int result;
  size_t index;
  uint8_t full;
  actor_t* reply_to;
} section_write_result_payload_t;

/* Async result payload for SECTION_DEALLOCATE_RESULT */
typedef struct {
  int result;
  actor_t* reply_to;
} section_deallocate_result_payload_t;

typedef struct section_t {
  refcounter_t refcounter;
  actor_t actor;
  int fd;
  size_t id;
  char* meta_path;
  char* path;
  free_map_t free_map;
  size_t size;
  block_size_e block_size;
  ATOMIC(uint8_t) dirty;
  /* Callback invoked when section metadata changes (write/deallocate).
     The context is typically a sections_t pointer. Used for debounced saves. */
  void (*on_dirty)(void* context, struct section_t* section);
  void* on_dirty_context;
} section_t;

section_t* section_create(char* path, char* meta_path, size_t size, size_t id, block_size_e type);
void section_destroy(section_t* section);
uint8_t section_full(section_t* section);
void section_save_meta(section_t* section);
void section_dispatch(void* state, message_t* msg);

/* Async API — send message and inject actor into scheduler. */
void section_read_async(section_t* section, size_t index, actor_t* reply_to);
void section_write_async(section_t* section, buffer_t* data, actor_t* reply_to);
void section_deallocate_async(section_t* section, size_t index, actor_t* reply_to);

/* Sync API — direct dispatch. Temporary, will be removed as callers convert. */
buffer_t* section_read(section_t* section, size_t index);
int section_write(section_t* section, buffer_t* data, size_t* out_index, uint8_t* out_full);
int section_deallocate(section_t* section, size_t index);

#endif //OFFS_SECTION_H