//
// Created by victor on 5/7/26.
//

#include "writeable_descriptor.h"
#include "../Util/allocator.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Scheduler/scheduler.h"
#include <string.h>

static size_t _block_size_for_type(block_size_e type) {
  switch (type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

static void _build_descriptor_blocks(writeable_descriptor_t* desc) {
  size_t chunk_size = desc->cut_point - desc->descriptor_pad;

  size_t chunk_count = 0;
  size_t remaining = desc->descriptor->size;
  size_t cap = 8;
  buffer_t** chunks = get_clear_memory(sizeof(buffer_t*) * cap);

  while (remaining > desc->cut_point) {
    if (chunk_count >= cap) {
      cap *= 2;
      chunks = realloc(chunks, sizeof(buffer_t*) * cap);
    }
    size_t offset = chunk_count * chunk_size;
    chunks[chunk_count] = buffer_slice(desc->descriptor, offset, offset + chunk_size);
    chunk_count++;
    remaining -= chunk_size;
  }

  if (remaining > 0) {
    if (chunk_count >= cap) {
      cap *= 2;
      chunks = realloc(chunks, sizeof(buffer_t*) * cap);
    }
    size_t offset = chunk_count * chunk_size;
    buffer_t* last_chunk = buffer_slice(desc->descriptor, offset,
                                          desc->descriptor->size);
    if (last_chunk->size < desc->block_size) {
      buffer_t* padded = buffer_create(desc->block_size);
      memcpy(padded->data, last_chunk->data, last_chunk->size);
      padded->size = desc->block_size;
      DESTROY(last_chunk, buffer);
      last_chunk = padded;
    }
    chunks[chunk_count] = last_chunk;
    chunk_count++;
  }

  buffer_t* prior_hash = NULL;

  for (size_t i = chunk_count; i > 0; i--) {
    size_t idx = i - 1;
    buffer_t* chunk = chunks[idx];

    if (prior_hash != NULL) {
      buffer_t* combined = buffer_create(chunk->size + prior_hash->size);
      memcpy(combined->data, chunk->data, chunk->size);
      memcpy(combined->data + chunk->size, prior_hash->data, prior_hash->size);
      combined->size = chunk->size + prior_hash->size;
      DESTROY(chunk, buffer);
      chunk = combined;
    }

    block_t* block = block_create_existing_data_by_type(chunk, desc->block_type);
    DESTROY(chunk, buffer);

    if (block == NULL) {
      for (size_t j = 0; j < idx; j++) {
        DESTROY(chunks[j], buffer);
      }
      free(chunks);
      if (prior_hash != NULL) {
        DESTROY(prior_hash, buffer);
      }
      stream_deactivate((stream_t*)desc, ERROR("Write descriptor error"));
      desc->stream.is_deactivated = 1;
      return;
    }

    block_cache_put(desc->bc, block);
    if (prior_hash != NULL) {
      DESTROY(prior_hash, buffer);
    }
    prior_hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
    block_destroy(block);
  }

  free(chunks);

  if (prior_hash != NULL) {
    stream_notify((stream_t*)desc, data_event, CONSUME(prior_hash, buffer_t),
                  (void (*)(void*))buffer_destroy);
    desc->sent_descriptor = 1;
  }

  stream_notify((stream_t*)desc, finished_event, NULL, NULL);
  stream_notify((stream_t*)desc, complete_event, NULL, NULL);
  desc->stream.is_deactivated = 1;
  stream_notify((stream_t*)desc, close_event, NULL, NULL);
}

void writeable_descriptor_dispatch(void* state, message_t* msg) {
  writeable_descriptor_t* desc = (writeable_descriptor_t*)state;
  switch (msg->type) {
    case WRITEABLE_DESCRIPTOR_WRITE: {
      tuple_t* tuple = (tuple_t*)msg->payload;
      if (desc->stream.is_deactivated) {
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      if (tuple_size(tuple) != desc->tuple_size) {
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      uint8_t valid = 1;
      for (size_t i = 0; i < tuple_size(tuple); i++) {
        buffer_t* hash = tuple_get(tuple, i);
        if (hash == NULL || hash->size != desc->descriptor_pad) {
          valid = 0;
          break;
        }
      }
      if (!valid) {
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      size_t append_size = desc->tuple_size * desc->descriptor_pad;
      size_t needed = desc->descriptor->size + append_size;
      buffer_ensure_capacity(desc->descriptor, needed);
      for (size_t i = 0; i < tuple_size(tuple); i++) {
        buffer_t* hash = tuple_get(tuple, i);
        memcpy(desc->descriptor->data + desc->descriptor->size, hash->data,
               hash->size);
        desc->descriptor->size += hash->size;
      }
      DESTROY(tuple, tuple);
      msg->payload = NULL;
      break;
    }
    case CLOSE_STREAM: {
      if (desc->stream.is_deactivated) {
        break;
      }
      if (desc->sent_descriptor) {
        stream_notify((stream_t*)desc, close_event, NULL, NULL);
        desc->stream.is_deactivated = 1;
      } else {
        _build_descriptor_blocks(desc);
      }
      break;
    }
    default:
      break;
  }
}

writeable_descriptor_t* writeable_descriptor_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type,
    size_t descriptor_pad, size_t tuple_size, size_t data_length) {
  writeable_descriptor_t* desc = get_clear_memory(sizeof(writeable_descriptor_t));
  desc->bc = bc;
  desc->block_type = block_type;
  desc->block_size = _block_size_for_type(block_type);
  desc->tuple_size = tuple_size;
  desc->descriptor_pad = descriptor_pad;
  desc->cut_point = (desc->block_size / descriptor_pad) * descriptor_pad;
  desc->data_length = data_length;
  desc->block_count = data_length / desc->block_size;
  desc->descriptor = buffer_create(0);
  desc->sent_descriptor = 0;

  stream_init((stream_t*)desc, push, writeable_stream, 1, pool,
              (void (*)(stream_t*))writeable_descriptor_destroy);

  desc->stream.actor.state = desc;
  desc->stream.actor.dispatch = writeable_descriptor_dispatch;

  return desc;
}

void writeable_descriptor_destroy(writeable_descriptor_t* desc) {
  if (refcounter_dereference_is_zero((refcounter_t*)desc)) {
    if (desc->descriptor != NULL) {
      DESTROY(desc->descriptor, buffer);
    }
    stream_deinit((stream_t*)desc);
    free(desc);
  }
}

void writeable_descriptor_write(writeable_descriptor_t* desc, tuple_t* tuple) {
  tuple_t* ref = REFERENCE(tuple, tuple_t);
  message_t msg;
  msg.type = WRITEABLE_DESCRIPTOR_WRITE;
  msg.payload = ref;
  msg.payload_destroy = (void (*)(void*))tuple_destroy;

  actor_send(&desc->stream.actor, &msg);
  scheduler_inject(desc->stream.pool, &desc->stream.actor);
}

void writeable_descriptor_close(writeable_descriptor_t* desc) {
  message_t msg;
  msg.type = CLOSE_STREAM;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&desc->stream.actor, &msg);
  scheduler_inject(desc->stream.pool, &desc->stream.actor);
}