//
// Created by victor on 5/7/26.
//

#include "readable_off_stream.h"
#include "../Util/allocator.h"
#include "../Util/error.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Scheduler/scheduler.h"
#include "../Network/network.h"

static size_t _block_size_for_type(block_size_e type) {
  switch (type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

static void _render_origin_data(readable_off_stream_t* stream, buffer_t* data) {
  size_t start;
  size_t length;
  if (!stream->offset_applied && stream->offset_remainder > 0) {
    size_t available = data->size - stream->offset_remainder;
    if (stream->sent_bytes + available > stream->ori->final_byte) {
      length = stream->ori->final_byte - stream->sent_bytes;
    } else {
      length = available;
    }
    start = stream->offset_remainder;
    stream->offset_applied = 1;
  } else {
    if (stream->sent_bytes + data->size > stream->ori->final_byte) {
      length = stream->ori->final_byte - stream->sent_bytes;
    } else {
      length = data->size;
    }
    start = 0;
  }

  buffer_t* slice = buffer_slice(data, start, start + length);
  if (slice != NULL) {
    stream_notify((stream_t*)stream, data_event, CONSUME(slice, buffer_t), (void (*)(void*))buffer_destroy);
  }

  stream->sent_bytes += length;

  if (stream->sent_bytes >= stream->ori->final_byte) {
    stream_notify((stream_t*)stream, finished_event, NULL, NULL);
    stream_notify((stream_t*)stream, complete_event, NULL, NULL);
    stream_notify((stream_t*)stream, close_event, NULL, NULL);
  }
}

static void _start_tuple_cache_lookup(readable_off_stream_t* stream, tuple_t* tuple);
static void _drain_tuple_queue(readable_off_stream_t* stream);

static void _finish_decode_and_render(readable_off_stream_t* stream) {
  if (stream->xor_accumulator == NULL) {
    DESTROY(stream->pending_tuple, tuple);
    stream->pending_tuple = NULL;
    return;
  }

  tuple_cache_put(stream->tc, stream->pending_tuple, stream->xor_accumulator);
  _render_origin_data(stream, stream->xor_accumulator);
  DESTROY(stream->xor_accumulator, buffer);
  stream->xor_accumulator = NULL;
  DESTROY(stream->pending_tuple, tuple);
  stream->pending_tuple = NULL;
  stream->blocks_expected = 0;
  stream->blocks_received = 0;

  /* Clean up any remaining pending fetches (shouldn't happen normally) */
  pending_block_fetch_t* fetch = stream->pending_fetches;
  while (fetch != NULL) {
    pending_block_fetch_t* next = fetch->next;
    DESTROY(fetch->hash, buffer);
    free(fetch);
    fetch = next;
  }
  stream->pending_fetches = NULL;

  /* Process next queued tuple if available */
  _drain_tuple_queue(stream);
}

static void _start_block_fetches(readable_off_stream_t* stream) {
  size_t count = tuple_size(stream->pending_tuple);
  stream->blocks_expected = count;
  stream->blocks_received = 0;
  stream->xor_accumulator = NULL;
  stream->pending_fetches = NULL;

  for (size_t i = 0; i < count; i++) {
    buffer_t* hash = tuple_get(stream->pending_tuple, i);
    pending_block_fetch_t* fetch = get_clear_memory(sizeof(pending_block_fetch_t));
    fetch->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
    fetch->index = i;
    fetch->next = stream->pending_fetches;
    stream->pending_fetches = fetch;
    block_cache_get(stream->bc, hash, &stream->stream.actor);
  }
}

static void _start_tuple_cache_lookup(readable_off_stream_t* stream, tuple_t* tuple) {
  stream->pending_tuple = (tuple_t*)refcounter_reference((refcounter_t*)tuple);
  tuple_cache_get(stream->tc, tuple, &stream->stream.actor);
}

/* Process next tuple from the queue if available. Called after a tuple finishes processing. */
static void _drain_tuple_queue(readable_off_stream_t* stream) {
  if (stream->stream.is_deactivated || stream->pending_tuple != NULL) {
    return;
  }
  if (stream->tuple_queue != NULL) {
    /* Dequeue from front (FIFO) */
    pending_tuple_t* head = stream->tuple_queue;
    stream->tuple_queue = head->next;
    tuple_t* tuple = head->tuple;
    free(head);
    _start_tuple_cache_lookup(stream, tuple);
    DESTROY(tuple, tuple);
  }
}

void readable_off_stream_dispatch(void* state, message_t* msg) {
  readable_off_stream_t* stream = (readable_off_stream_t*)state;
  switch (msg->type) {
    case OFF_STREAM_WRITE: {
      tuple_t* tuple = (tuple_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      /* If we're already fetching blocks for a previous tuple, queue this one. */
      if (stream->pending_tuple != NULL) {
        pending_tuple_t* queued = get_clear_memory(sizeof(pending_tuple_t));
        queued->tuple = (tuple_t*)refcounter_reference((refcounter_t*)tuple);
        queued->next = NULL;
        /* Append to tail (FIFO) */
        if (stream->tuple_queue == NULL) {
          stream->tuple_queue = queued;
        } else {
          pending_tuple_t* tail = stream->tuple_queue;
          while (tail->next != NULL) {
            tail = tail->next;
          }
          tail->next = queued;
        }
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      _start_tuple_cache_lookup(stream, tuple);
      DESTROY(tuple, tuple);
      msg->payload = NULL;
      break;
    }
    case TUPLE_CACHE_GET_RESULT: {
      tuple_cache_get_result_payload_t* result = (tuple_cache_get_result_payload_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        if (result->value != NULL) {
          DESTROY(result->value, buffer);
        }
        if (result->key != NULL) {
          DESTROY(result->key, tuple);
        }
        break;
      }
      if (result->value != NULL) {
        /* Cache hit — render directly */
        _render_origin_data(stream, result->value);
        DESTROY(result->value, buffer);
        DESTROY(stream->pending_tuple, tuple);
        stream->pending_tuple = NULL;
        _drain_tuple_queue(stream);
      } else {
        /* Cache miss — start fetching blocks */
        _start_block_fetches(stream);
      }
      if (result->key != NULL) {
        DESTROY(result->key, tuple);
      }
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        if (result->block != NULL) {
          DESTROY(result->block, block);
        }
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
        }
        break;
      }

      if (result->block == NULL) {
        /* Block not found in cache */
        if (stream->network != NULL) {
          /* Network-aware: send NETWORK_LOCAL_FIND_BLOCK */
          stream->state = OFF_STREAM_AWAITING_NETWORK;
          stream->pending_fetch_hash = REFERENCE(result->hash, buffer_t);
          network_local_find_block_payload_t payload;
          payload.hash = stream->pending_fetch_hash;
          payload.reply_to = &stream->stream.actor;
          message_t msg;
          msg.type = NETWORK_LOCAL_FIND_BLOCK;
          msg.payload = &payload;
          msg.payload_destroy = NULL;
          actor_send(&stream->network->actor, &msg);
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
          }
        } else {
          /* Local-only: deactivate as before */
          if (stream->xor_accumulator != NULL) {
            DESTROY(stream->xor_accumulator, buffer);
            stream->xor_accumulator = NULL;
          }
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
          }
          DESTROY(stream->pending_tuple, tuple);
          stream->pending_tuple = NULL;
          stream_deactivate((stream_t*)stream, ERROR("Block not found in cache"));
        }
        break;
      }

      /* Accumulate block into XOR result */
      if (stream->xor_accumulator == NULL) {
        stream->xor_accumulator = buffer_copy(result->block->data);
      } else {
        buffer_t* xored = buffer_xor(stream->xor_accumulator, result->block->data);
        DESTROY(stream->xor_accumulator, buffer);
        stream->xor_accumulator = xored;
      }

      DESTROY(result->block, block);
      if (result->hash != NULL) {
        DESTROY(result->hash, buffer);
      }

      stream->blocks_received++;

      if (stream->blocks_received >= stream->blocks_expected) {
        _finish_decode_and_render(stream);
      }
      break;
    }
    case NETWORK_FIND_BLOCK_RESULT: {
      network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
      if (result->found) {
        /* Block found on network — re-issue cache_get (it should now be in cache) */
        block_cache_get(stream->bc, stream->pending_fetch_hash, &stream->stream.actor);
        stream->state = OFF_STREAM_FETCHING_BLOCKS;
      } else {
        /* Block not found on network — deactivate */
        if (stream->xor_accumulator != NULL) {
          DESTROY(stream->xor_accumulator, buffer);
          stream->xor_accumulator = NULL;
        }
        DESTROY(stream->pending_tuple, tuple);
        stream->pending_tuple = NULL;
        stream_deactivate((stream_t*)stream, ERROR("Block not found on network"));
      }
      if (stream->pending_fetch_hash != NULL) {
        DESTROY(stream->pending_fetch_hash, buffer);
        stream->pending_fetch_hash = NULL;
      }
      break;
    }
    case CLOSE_STREAM: {
      if (stream->pending_tuple != NULL) {
        DESTROY(stream->pending_tuple, tuple);
        stream->pending_tuple = NULL;
      }
      if (stream->pending_fetch_hash != NULL) {
        DESTROY(stream->pending_fetch_hash, buffer);
        stream->pending_fetch_hash = NULL;
      }
      if (stream->xor_accumulator != NULL) {
        DESTROY(stream->xor_accumulator, buffer);
        stream->xor_accumulator = NULL;
      }
      pending_tuple_t* queued = stream->tuple_queue;
      while (queued != NULL) {
        pending_tuple_t* next = queued->next;
        DESTROY(queued->tuple, tuple);
        free(queued);
        queued = next;
      }
      stream->tuple_queue = NULL;
      pending_block_fetch_t* fetch = stream->pending_fetches;
      while (fetch != NULL) {
        pending_block_fetch_t* next = fetch->next;
        DESTROY(fetch->hash, buffer);
        free(fetch);
        fetch = next;
      }
      stream->pending_fetches = NULL;
      stream_notify((stream_t*)stream, close_event, NULL, NULL);
      stream->stream.is_deactivated = 1;
      break;
    }
    default:
      stream_dispatch(state, msg);
      break;
  }
}

static void _readable_off_stream_on_write(stream_t* stream, void* data) {
  (void)stream;
  (void)data;
}

readable_off_stream_t* readable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network) {
  readable_off_stream_t* stream = get_clear_memory(sizeof(readable_off_stream_t));
  stream->bc = bc;
  stream->tc = tc;
  stream->ori = ori;
  stream->network = network;
  stream->pending_fetch_hash = NULL;
  stream->descriptor_pad = descriptor_pad;
  stream->offset_applied = 0;
  stream->state = OFF_STREAM_FETCHING_BLOCKS;

  size_t block_size = _block_size_for_type(ori->block_type);
  stream->sent_bytes = ori->file_offset;
  stream->offset_remainder = ori->file_offset % block_size;

  stream_init((stream_t*)stream, push, transform_stream, 0, pool,
              (void (*)(stream_t*))readable_off_stream_destroy);
  stream->stream.on_write = _readable_off_stream_on_write;

  stream->stream.actor.state = stream;
  stream->stream.actor.dispatch = readable_off_stream_dispatch;

  return stream;
}

void readable_off_stream_destroy(readable_off_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*)stream)) {
    pending_tuple_t* queued = stream->tuple_queue;
    while (queued != NULL) {
      pending_tuple_t* next = queued->next;
      DESTROY(queued->tuple, tuple);
      free(queued);
      queued = next;
    }
    if (stream->pending_tuple != NULL) {
      DESTROY(stream->pending_tuple, tuple);
    }
    if (stream->pending_fetch_hash != NULL) {
      DESTROY(stream->pending_fetch_hash, buffer);
    }
    stream_deinit((stream_t*)stream);
    free(stream);
  }
}

void readable_off_stream_write(readable_off_stream_t* stream, tuple_t* tuple) {
  tuple_t* ref = REFERENCE(tuple, tuple_t);
  message_t msg;
  msg.type = OFF_STREAM_WRITE;
  msg.payload = ref;
  msg.payload_destroy = (void (*)(void*))tuple_destroy;

  actor_send(&stream->stream.actor, &msg);
}