//
// Created by victor on 5/7/26.
//

#include "readable_descriptor.h"
#include "../Util/allocator.h"
#include "../Util/error.h"
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

static void _move_to_offset(readable_descriptor_t* desc, buffer_t* descriptor,
                            buffer_t** out_key, buffer_t** out_remaining) {
  buffer_t* buf = descriptor;
  int combined = 0;

  if (desc->offset_remainder != NULL && desc->offset_remainder->size > 0) {
    buffer_t* combined_buf = buffer_create(desc->offset_remainder->size + descriptor->size);
    memcpy(combined_buf->data, desc->offset_remainder->data, desc->offset_remainder->size);
    memcpy(combined_buf->data + desc->offset_remainder->size, descriptor->data, descriptor->size);
    combined_buf->size = desc->offset_remainder->size + descriptor->size;
    DESTROY(desc->offset_remainder, buffer);
    desc->offset_remainder = NULL;
    buf = combined_buf;
    combined = 1;
  }

  size_t entry_size = desc->descriptor_pad * desc->ori->tuple_size;

  if (desc->offset_tuple > 0 && desc->tuple_counter < desc->offset_tuple) {
    if (desc->offset_tuple > ((buf->size % entry_size) + desc->tuple_counter)) {
      desc->tuple_counter = (buf->size - desc->descriptor_pad) / entry_size;
      size_t cut = (buf->size - desc->descriptor_pad) -
                   ((buf->size - desc->descriptor_pad) % entry_size);
      desc->offset_remainder = buffer_slice(buf, cut, buf->size - desc->descriptor_pad);
      *out_key = buffer_slice(buf, buf->size - desc->descriptor_pad, buf->size);
      *out_remaining = NULL;
    } else {
      size_t cut = ((desc->offset_tuple - desc->tuple_counter) *
                    desc->ori->tuple_size) * desc->descriptor_pad;
      desc->tuple_counter += cut / entry_size;
      *out_key = buffer_slice(buf, cut, cut + desc->descriptor_pad);
      if (cut + desc->descriptor_pad < buf->size) {
        *out_remaining = buffer_slice(buf, cut + desc->descriptor_pad, buf->size);
      } else {
        *out_remaining = NULL;
      }
    }
  } else {
    *out_key = buffer_slice(buf, 0, desc->descriptor_pad);
    if (desc->descriptor_pad < buf->size) {
      *out_remaining = buffer_slice(buf, desc->descriptor_pad, buf->size);
    } else {
      *out_remaining = NULL;
    }
  }

  if (combined) {
    DESTROY(buf, buffer);
  }
}

static void _process_descriptor(readable_descriptor_t* desc, buffer_t* block_data) {
  buffer_t* current_descriptor;

  if (desc->current_descriptor == NULL) {
    current_descriptor = buffer_slice(block_data, 0, desc->cut_point);
    if (desc->ori->descriptor_offset > 0 && desc->ori->descriptor_hash != NULL) {
      buffer_t* sliced = buffer_slice(current_descriptor, desc->ori->descriptor_offset,
                                       current_descriptor->size);
      DESTROY(current_descriptor, buffer);
      current_descriptor = sliced;
    }
  } else {
    current_descriptor = desc->current_descriptor;
    desc->current_descriptor = NULL;
  }

  while (current_descriptor != NULL && current_descriptor->size > 0 &&
         !desc->stream.is_deactivated) {
    if (desc->current_tuple == NULL) {
      desc->current_tuple = tuple_create(desc->ori->tuple_size);
    }

    buffer_t* key = NULL;
    buffer_t* remaining = NULL;
    _move_to_offset(desc, current_descriptor, &key, &remaining);
    DESTROY(current_descriptor, buffer);
    current_descriptor = remaining;

    while (tuple_size(desc->current_tuple) < desc->ori->tuple_size) {
      if (current_descriptor == NULL || current_descriptor->size <= 0) {
        if (key != NULL) {
          block_t* next_block = block_cache_get(desc->bc, key);
          DESTROY(key, buffer);
          key = NULL;
          if (next_block != NULL) {
            current_descriptor = buffer_slice(next_block->data, 0, desc->cut_point);
            block_destroy(next_block);
            buffer_t* next_key = NULL;
            buffer_t* next_remaining = NULL;
            _move_to_offset(desc, current_descriptor, &next_key, &next_remaining);
            DESTROY(current_descriptor, buffer);
            current_descriptor = next_remaining;
            key = next_key;
            continue;
          } else {
            stream_notify((stream_t*)desc, error_event, ERROR("Descriptor block not found"), (void (*)(void*))error_destroy);
            stream_notify((stream_t*)desc, close_event, NULL, NULL);
            desc->stream.is_deactivated = 1;
            if (current_descriptor != NULL) {
              DESTROY(current_descriptor, buffer);
            }
            if (desc->current_tuple != NULL) {
              tuple_destroy(desc->current_tuple);
              desc->current_tuple = NULL;
            }
            return;
          }
        }
        desc->current_descriptor = NULL;
        if (key != NULL) {
          desc->next_descriptor_hash = key;
          key = NULL;
        }
        return;
      }

      tuple_push(desc->current_tuple, key);
      DESTROY(key, buffer);
      key = NULL;

      if (tuple_size(desc->current_tuple) == desc->ori->tuple_size) {
        break;
      }

      buffer_t* next_key = NULL;
      buffer_t* next_remaining = NULL;
      _move_to_offset(desc, current_descriptor, &next_key, &next_remaining);
      DESTROY(current_descriptor, buffer);
      current_descriptor = next_remaining;
      key = next_key;
    }

    if (tuple_size(desc->current_tuple) == desc->ori->tuple_size) {
      tuple_t* tuple = desc->current_tuple;
      desc->current_tuple = NULL;
      tuple_t* ref = REFERENCE(tuple, tuple_t);
      stream_notify((stream_t*)desc, data_event, ref, (void (*)(void*))tuple_destroy);
      desc->tuple_counter++;

      if (desc->tuple_counter >= desc->tuple_count) {
        stream_notify((stream_t*)desc, complete_event, NULL, NULL);
        stream_notify((stream_t*)desc, close_event, NULL, NULL);
        desc->stream.is_deactivated = 1;
        if (current_descriptor != NULL) {
          DESTROY(current_descriptor, buffer);
        }
        return;
      }
    }
  }

  if (current_descriptor != NULL) {
    DESTROY(current_descriptor, buffer);
  }
}

void readable_descriptor_dispatch(void* state, message_t* msg) {
  readable_descriptor_t* desc = (readable_descriptor_t*)state;
  switch (msg->type) {
    case READABLE_PUSH: {
      if (desc->stream.is_deactivated) {
        break;
      }
      if (desc->current_descriptor == NULL) {
        buffer_t* hash = desc->next_descriptor_hash != NULL
                             ? desc->next_descriptor_hash
                             : desc->ori->descriptor_hash;
        desc->next_descriptor_hash = NULL;
        block_t* block = block_cache_get(desc->bc, hash);
        if (block != NULL) {
          _process_descriptor(desc, block->data);
          block_destroy(block);
        } else {
          stream_notify((stream_t*)desc, error_event, ERROR("Descriptor block not found"), (void (*)(void*))error_destroy);
          stream_notify((stream_t*)desc, close_event, NULL, NULL);
          desc->stream.is_deactivated = 1;
        }
      }
      break;
    }
    case CLOSE_STREAM: {
      stream_notify((stream_t*)desc, close_event, NULL, NULL);
      desc->stream.is_deactivated = 1;
      break;
    }
    default:
      break;
  }
}

readable_descriptor_t* readable_descriptor_create(
    scheduler_pool_t* pool, block_cache_t* bc, ori_t* ori, size_t descriptor_pad) {
  readable_descriptor_t* desc = get_clear_memory(sizeof(readable_descriptor_t));
  desc->bc = bc;
  desc->ori = ori;
  desc->block_size = _block_size_for_type(ori->block_type);
  desc->tuple_count = (ori->final_byte / desc->block_size) +
                      ((ori->final_byte % desc->block_size) > 0 ? 1 : 0);
  desc->cut_point = (desc->block_size / descriptor_pad) * descriptor_pad;
  desc->offset_tuple = ori->file_offset / desc->block_size;
  desc->descriptor_pad = descriptor_pad;
  desc->tuple_counter = 0;
  desc->current_descriptor = NULL;
  desc->current_tuple = NULL;
  desc->offset_remainder = NULL;
  desc->next_descriptor_hash = NULL;
  desc->is_readable = 1;

  stream_init((stream_t*)desc, push, readable_stream, 1, pool,
              (void (*)(stream_t*))readable_descriptor_destroy);

  desc->stream.actor.state = desc;
  desc->stream.actor.dispatch = readable_descriptor_dispatch;

  return desc;
}

void readable_descriptor_destroy(readable_descriptor_t* desc) {
  if (refcounter_dereference_is_zero((refcounter_t*)desc)) {
    if (desc->current_descriptor != NULL) {
      DESTROY(desc->current_descriptor, buffer);
    }
    if (desc->current_tuple != NULL) {
      tuple_destroy(desc->current_tuple);
    }
    if (desc->offset_remainder != NULL) {
      DESTROY(desc->offset_remainder, buffer);
    }
    if (desc->next_descriptor_hash != NULL) {
      DESTROY(desc->next_descriptor_hash, buffer);
    }
    stream_deinit((stream_t*)desc);
    free(desc);
  }
}

void readable_descriptor_push(readable_descriptor_t* desc) {
  message_t msg;
  msg.type = READABLE_PUSH;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&desc->stream.actor, &msg);
  scheduler_inject(desc->stream.pool, &desc->stream.actor);
}