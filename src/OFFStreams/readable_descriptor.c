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
#include "../Network/network.h"
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
    if (desc->offset_tuple > (((buf->size - desc->descriptor_pad) / entry_size) + desc->tuple_counter)) {
      desc->tuple_counter += (buf->size - desc->descriptor_pad) / entry_size;
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

/* Process descriptor data that we already have in memory.
   Returns 1 if we need another block (stored in desc->next_descriptor_hash),
   or 0 if processing is complete or we emitted all tuples. */
static int _process_descriptor(readable_descriptor_t* desc, buffer_t* block_data) {
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
          /* Need to fetch another descriptor block — request async */
          desc->next_descriptor_hash = key;
          if (current_descriptor != NULL) {
            DESTROY(current_descriptor, buffer);
          }
          return 1; /* need another block */
        }
        desc->current_descriptor = NULL;
        return 0;
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
      stream_notify((stream_t*)desc, data_event, CONSUME(tuple, tuple_t), (void (*)(void*))tuple_destroy);
      desc->tuple_counter++;

      if (desc->tuple_counter >= desc->tuple_count) {
        stream_notify((stream_t*)desc, complete_event, NULL, NULL);
        stream_notify((stream_t*)desc, close_event, NULL, NULL);
        desc->stream.is_deactivated = 1;
        if (current_descriptor != NULL) {
          DESTROY(current_descriptor, buffer);
        }
        return 0;
      }
    }
  }

  if (current_descriptor != NULL) {
    DESTROY(current_descriptor, buffer);
  }
  return 0;
}

/* Request a descriptor block from block_cache. Result arrives as CACHE_GET_RESULT. */
static void _fetch_descriptor_block(readable_descriptor_t* desc, buffer_t* hash) {
  if (desc->expected_hash != NULL) {
    DESTROY(desc->expected_hash, buffer);
  }
  desc->expected_hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  block_cache_get(desc->bc, hash, &desc->stream.actor);
}

void readable_descriptor_dispatch(void* state, message_t* msg) {
  readable_descriptor_t* desc = (readable_descriptor_t*)state;
  switch (msg->type) {
    case READABLE_PUSH: {
      if (desc->stream.is_deactivated) {
        break;
      }
      if (desc->current_descriptor == NULL) {
        if (desc->ori->descriptor_hash == NULL && desc->next_descriptor_hash == NULL) {
          stream_notify((stream_t*)desc, error_event, NULL, NULL);
          desc->stream.is_deactivated = 1;
          break;
        }
        buffer_t* hash = desc->next_descriptor_hash != NULL
                             ? desc->next_descriptor_hash
                             : (buffer_t*)refcounter_reference((refcounter_t*)desc->ori->descriptor_hash);
        desc->next_descriptor_hash = NULL;
        _fetch_descriptor_block(desc, hash);
        DESTROY(hash, buffer);
      }
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      if (desc->stream.is_deactivated) {
        if (result->block != NULL) {
          DESTROY(result->block, block);
          result->block = NULL;
        }
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
          result->hash = NULL;
        }
        break;
      }

      /* Verify the result is for the hash we requested — ignore stale results
         from concurrent requests that may arrive at our actor */
      if (desc->expected_hash != NULL && result->hash != NULL &&
          buffer_compare(desc->expected_hash, result->hash) != 0) {
        if (result->block != NULL) {
          DESTROY(result->block, block);
          result->block = NULL;
        }
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
          result->hash = NULL;
        }
        break;
      }

      if (desc->expected_hash != NULL) {
        DESTROY(desc->expected_hash, buffer);
        desc->expected_hash = NULL;
      }

      if (result->block == NULL) {
        /* Block not found */
        if (desc->network != NULL) {
          /* Network-aware: send NETWORK_LOCAL_FIND_BLOCK.
           * Use result->hash directly — the network's wanted_list deduplicates. */
          desc->state = DESCRIPTOR_AWAITING_NETWORK;
          network_local_find_block_payload_t* payload = get_clear_memory(sizeof(network_local_find_block_payload_t));
          payload->hash = REFERENCE(result->hash, buffer_t);
          payload->reply_to = &desc->stream.actor;
          message_t msg;
          msg.type = NETWORK_LOCAL_FIND_BLOCK;
          msg.payload = payload;
          msg.payload_destroy = network_local_find_block_payload_destroy;
          actor_send(&desc->network->actor, &msg);
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
            result->hash = NULL;
          }
        } else {
          /* Local-only: deactivate */
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
            result->hash = NULL;
          }
          stream_deactivate((stream_t*)desc, OFFS_ERROR("Descriptor block not found"));
          desc->stream.is_deactivated = 1;
        }
        break;
      }

      buffer_t* block_data = result->block->data;
      int need_more = _process_descriptor(desc, block_data);
      DESTROY(result->block, block);
      result->block = NULL;
      if (result->hash != NULL) {
        DESTROY(result->hash, buffer);
        result->hash = NULL;
      }

      if (need_more && desc->next_descriptor_hash != NULL && !desc->stream.is_deactivated) {
        /* Need another descriptor block — fetch it */
        buffer_t* hash = desc->next_descriptor_hash;
        desc->next_descriptor_hash = NULL;
        _fetch_descriptor_block(desc, hash);
        DESTROY(hash, buffer);
      }
      break;
    }
    case NETWORK_FIND_BLOCK_RESULT: {
      network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
      if (result->found) {
        if (result->block != NULL) {
          /* Direct-return: network provided the block. Process it the same
           * way as CACHE_GET_RESULT success (lines 256-272). */
          buffer_t* block_data = result->block->data;
          int need_more = _process_descriptor(desc, block_data);
          DESTROY(result->block, block);
          result->block = NULL;
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
            result->hash = NULL;
          }
          if (need_more && desc->next_descriptor_hash != NULL && !desc->stream.is_deactivated) {
            buffer_t* hash = desc->next_descriptor_hash;
            desc->next_descriptor_hash = NULL;
            _fetch_descriptor_block(desc, hash);
            DESTROY(hash, buffer);
          }
        } else {
          /* Local path: block is in the cache. Re-fetch as before. */
          if (result->hash != NULL) {
            block_cache_get(desc->bc, result->hash, &desc->stream.actor);
          }
          desc->state = DESCRIPTOR_FETCHING_BLOCK;
        }
      } else {
        /* Block not found on network — deactivate */
        stream_deactivate((stream_t*)desc, OFFS_ERROR("Descriptor block not found on network"));
        desc->stream.is_deactivated = 1;
      }
      break;
    }
    case CLOSE_STREAM: {
      stream_notify((stream_t*)desc, close_event, NULL, NULL);
      desc->stream.is_deactivated = 1;
      break;
    }
    default:
      stream_dispatch(state, msg);
      break;
  }
}

readable_descriptor_t* readable_descriptor_create(
    scheduler_pool_t* pool, block_cache_t* bc, ori_t* ori, size_t descriptor_pad,
    network_t* network) {
  readable_descriptor_t* desc = get_clear_memory(sizeof(readable_descriptor_t));
  desc->bc = bc;
  desc->ori = ori;
  desc->network = network;
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
  desc->state = DESCRIPTOR_FETCHING_BLOCK;
  desc->is_readable = 1;

  stream_init((stream_t*)desc, push, readable_stream, 0, pool,
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
    if (desc->expected_hash != NULL) {
      DESTROY(desc->expected_hash, buffer);
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
}