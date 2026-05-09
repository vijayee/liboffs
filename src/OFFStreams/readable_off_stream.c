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
    stream->stream.is_deactivated = 1;
  }
}

static void _check_cache_and_decode(readable_off_stream_t* stream, tuple_t* tuple) {
  buffer_t* cached = tuple_cache_apply(stream->tc, tuple);
  if (cached != NULL) {
    _render_origin_data(stream, cached);
    DESTROY(cached, buffer);
    return;
  }

  size_t count = tuple_size(tuple);
  if (count == 0) {
    return;
  }

  buffer_t* origin_data = NULL;
  for (size_t i = 0; i < count; i++) {
    buffer_t* hash = tuple_get(tuple, i);
    block_t* block = block_cache_get(stream->bc, hash);
    if (block == NULL) {
      if (origin_data != NULL) {
        DESTROY(origin_data, buffer);
      }
      stream_notify((stream_t*)stream, error_event, ERROR("Block not found in cache"), (void (*)(void*))error_destroy);
      stream_notify((stream_t*)stream, close_event, NULL, NULL);
      stream->stream.is_deactivated = 1;
      return;
    }
    if (i == 0) {
      origin_data = buffer_copy(block->data);
    } else {
      buffer_t* xored = buffer_xor(origin_data, block->data);
      DESTROY(origin_data, buffer);
      origin_data = xored;
    }
    block_destroy(block);
  }

  if (origin_data == NULL) {
    return;
  }

  tuple_cache_update(stream->tc, tuple, origin_data);

  _render_origin_data(stream, origin_data);
  DESTROY(origin_data, buffer);

  if (stream->stream.is_deactivated) {
    return;
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
      _check_cache_and_decode(stream, tuple);
      if (stream->stream.is_deactivated) {
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      DESTROY(tuple, tuple);
      msg->payload = NULL;
      break;
    }
    case CLOSE_STREAM: {
      stream_notify((stream_t*)stream, close_event, NULL, NULL);
      stream->stream.is_deactivated = 1;
      break;
    }
    default:
      break;
  }
}

static void _readable_off_stream_on_write(stream_t* stream, void* data) {
  (void)stream;
  (void)data;
}

readable_off_stream_t* readable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad) {
  readable_off_stream_t* stream = get_clear_memory(sizeof(readable_off_stream_t));
  stream->bc = bc;
  stream->tc = tc;
  stream->ori = ori;
  stream->descriptor_pad = descriptor_pad;
  stream->offset_applied = 0;

  size_t block_size = _block_size_for_type(ori->block_type);
  stream->sent_bytes = ori->file_offset;
  stream->offset_remainder = ori->file_offset % block_size;

  stream_init((stream_t*)stream, push, readable_stream, 1, pool,
              (void (*)(stream_t*))readable_off_stream_destroy);
  stream->stream.on_write = _readable_off_stream_on_write;

  stream->stream.actor.state = stream;
  stream->stream.actor.dispatch = readable_off_stream_dispatch;

  return stream;
}

void readable_off_stream_destroy(readable_off_stream_t* stream) {
  refcounter_dereference((refcounter_t*)stream);
  if (refcounter_count((refcounter_t*)stream) == 0) {
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
  scheduler_inject(stream->stream.pool, &stream->stream.actor);
}