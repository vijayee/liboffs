//
// Created by victor on 5/7/26.
//

#ifndef OFFS_WRITEABLE_DESCRIPTOR_H
#define OFFS_WRITEABLE_DESCRIPTOR_H

#include "../Streams/stream.h"
#include "tuple.h"
#include "ori.h"
#include "../BlockCache/block_cache.h"
#include "../Buffer/buffer.h"
#include "../Network/network.h"

typedef struct {
  stream_t stream;
  block_cache_t* bc;
  network_t* network;               /* NULL = local-only mode */
  buffer_t* descriptor;
  size_t tuple_size;
  size_t cut_point;
  size_t block_size;
  size_t data_length;
  size_t block_count;
  size_t descriptor_pad;
  uint8_t sent_descriptor;
  block_size_e block_type;
} writeable_descriptor_t;

writeable_descriptor_t* writeable_descriptor_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type,
    size_t descriptor_pad, size_t tuple_size, size_t data_length,
    network_t* network);
void writeable_descriptor_destroy(writeable_descriptor_t* desc);
void writeable_descriptor_dispatch(void* state, message_t* msg);
void writeable_descriptor_write(writeable_descriptor_t* desc, tuple_t* tuple);
void writeable_descriptor_close(writeable_descriptor_t* desc);

#endif //OFFS_WRITEABLE_DESCRIPTOR_H