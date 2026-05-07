//
// Created by victor on 5/7/26.
//

#include "writeable_off_stream.h"
#include "../Util/allocator.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Scheduler/scheduler.h"
#include <string.h>
#include "../../deps/BLAKE3/c/blake3.h"

static size_t _block_size_for_type(block_size_e type) {
  switch (type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

static off_stream_tuple_entry_t* _entry_create(block_t* origin, size_t tuple_size) {
  off_stream_tuple_entry_t* entry = get_clear_memory(sizeof(off_stream_tuple_entry_t));
  entry->origin = (block_t*)refcounter_reference((refcounter_t*)origin);
  entry->random_blocks = get_clear_memory(sizeof(block_t*) * (tuple_size - 1));
  entry->random_count = 0;
  entry->random_capacity = tuple_size - 1;
  return entry;
}

static void _entry_destroy(off_stream_tuple_entry_t* entry) {
  if (entry->origin != NULL) {
    block_destroy(entry->origin);
  }
  for (size_t i = 0; i < entry->random_count; i++) {
    if (entry->random_blocks[i] != NULL) {
      block_destroy(entry->random_blocks[i]);
    }
  }
  free(entry->random_blocks);
  free(entry);
}

static void _push_entry(writeable_off_stream_t* stream, off_stream_tuple_entry_t* entry) {
  if (stream->entry_count >= stream->entry_capacity) {
    size_t new_cap = stream->entry_capacity == 0 ? 15 : stream->entry_capacity * 2;
    stream->entries = realloc(stream->entries, sizeof(off_stream_tuple_entry_t*) * new_cap);
    stream->entry_capacity = new_cap;
  }
  stream->entries[stream->entry_count++] = entry;
}

static off_stream_tuple_entry_t* _shift_entry(writeable_off_stream_t* stream) {
  if (stream->entry_count == 0) {
    return NULL;
  }
  off_stream_tuple_entry_t* entry = stream->entries[0];
  stream->entry_count--;
  memmove(stream->entries, stream->entries + 1, stream->entry_count * sizeof(off_stream_tuple_entry_t*));
  return entry;
}

static void _create_tuple(writeable_off_stream_t* stream, off_stream_tuple_entry_t* entry) {
  buffer_t* origin_data = entry->origin->data;
  buffer_t* off_data = buffer_copy(origin_data);

  for (size_t i = 0; i < entry->random_count; i++) {
    buffer_t* xored = buffer_xor(off_data, entry->random_blocks[i]->data);
    DESTROY(off_data, buffer);
    off_data = xored;
  }

  block_t* off_block = block_create_existing_data_by_type(off_data, stream->block_type);
  DESTROY(off_data, buffer);
  if (off_block == NULL) {
    return;
  }

  tuple_t* tuple = tuple_create(stream->tuple_size);
  for (size_t i = 0; i < entry->random_count; i++) {
    tuple_push(tuple, entry->random_blocks[i]->hash);
  }
  tuple_push(tuple, off_block->hash);

  for (size_t i = 0; i < entry->random_count; i++) {
    block_cache_put(stream->bc, entry->random_blocks[i]);
  }
  block_cache_put(stream->bc, off_block);

  tuple_cache_update(stream->tc, tuple, origin_data);

  buffer_t* tuple_val = (buffer_t*)refcounter_reference((refcounter_t*)tuple);
  stream_notify((stream_t*)stream, data_event, CONSUME(tuple_val, buffer_t), NULL);
  /* Note: tuple is emitted as data payload. Subscriber takes ownership via type-specific handling.
     Since data_event carries void* and tuples are refcounted, the subscriber must handle lifecycle. */

  if (stream->final_block != NULL &&
      entry->origin->hash != NULL &&
      entry->origin->hash->size == stream->final_block->hash->size &&
      memcmp(entry->origin->hash->data, stream->final_block->hash->data, entry->origin->hash->size) == 0) {
    stream->final_block = NULL;
    stream_notify((stream_t*)stream, finished_event, NULL, NULL);
    stream_notify((stream_t*)stream, complete_event, NULL, NULL);
    stream_close((stream_t*)stream);
  } else if (stream->entry_count > 0) {
    off_stream_tuple_entry_t* next = stream->entries[0];
    for (size_t i = 0; i < next->random_capacity; i++) {
      block_t* random = block_create_random_block_by_type(stream->block_type);
      if (random == NULL) {
        break;
      }
      next->random_blocks[next->random_count++] = random;
    }
    if (next->random_count >= next->random_capacity) {
      _shift_entry(stream);
      _create_tuple(stream, next);
    }
  }

  block_destroy(off_block);
  tuple_destroy(tuple);
}

void writeable_off_stream_dispatch(void* state, message_t* msg) {
  writeable_off_stream_t* stream = (writeable_off_stream_t*)state;
  switch (msg->type) {
    case WRITEABLE_WRITE: {
      buffer_t* data = (buffer_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        DESTROY(data, buffer);
        break;
      }
      blake3_hasher hasher;
      memcpy(&hasher, stream->hash_state, sizeof(blake3_hasher));
      blake3_hasher_update(&hasher, data->data, data->size);
      memcpy(stream->hash_state, &hasher, sizeof(blake3_hasher));

      size_t remaining = data->size;
      size_t offset = 0;
      while (remaining > 0) {
        size_t accum_space = stream->block_size - stream->accumulator->size;
        size_t to_append = remaining < accum_space ? remaining : accum_space;
        memcpy(stream->accumulator->data + stream->accumulator->size, data->data + offset, to_append);
        stream->accumulator->size += to_append;
        offset += to_append;
        remaining -= to_append;

        if (stream->accumulator->size >= stream->block_size) {
          buffer_t* block_data = buffer_create_from_pointer_copy(stream->accumulator->data, stream->block_size);
          block_t* origin = block_create_existing_data_by_type(block_data, stream->block_type);
          DESTROY(block_data, buffer);
          if (origin != NULL) {
            off_stream_tuple_entry_t* entry = _entry_create(origin, stream->tuple_size);
            _push_entry(stream, entry);
            block_destroy(origin);

            for (size_t i = 0; i < entry->random_capacity; i++) {
              block_t* random = block_create_random_block_by_type(stream->block_type);
              if (random == NULL) break;
              entry->random_blocks[entry->random_count++] = random;
            }

            if (!stream->is_readable) {
              stream->is_readable = 1;
              stream_notify((stream_t*)stream, readable_event, NULL, NULL);
            }

            if (stream->entry_count == 1 && entry->random_count >= entry->random_capacity) {
              off_stream_tuple_entry_t* first = _shift_entry(stream);
              _create_tuple(stream, first);
            }
          }
          stream->accumulator->size = 0;
        }
      }

      DESTROY(data, buffer);
      break;
    }
    default:
      break;
  }
}

static void _writeable_off_stream_on_write(stream_t* s, void* data) {
  (void)s;
  (void)data;
}

writeable_off_stream_t* writeable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    block_size_e block_type, size_t tuple_size, size_t digest_size) {
  (void)digest_size;
  writeable_off_stream_t* stream = get_clear_memory(sizeof(writeable_off_stream_t));
  stream->bc = bc;
  stream->tc = tc;
  stream->block_type = block_type;
  stream->block_size = _block_size_for_type(block_type);
  stream->tuple_size = tuple_size;
  stream->final_block = NULL;
  stream->is_readable = 0;
  stream->entry_count = 0;
  stream->entry_capacity = 0;
  stream->entries = NULL;

  stream->accumulator = buffer_create(stream->block_size);
  stream->hash_state = get_clear_memory(sizeof(blake3_hasher));
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  memcpy(stream->hash_state, &hasher, sizeof(blake3_hasher));

  stream_init((stream_t*)stream, push, writeable_stream, 1, pool,
              (void (*)(stream_t*))writeable_off_stream_destroy);
  stream->stream.on_write = _writeable_off_stream_on_write;

  actor_init(&stream->stream.actor, stream, writeable_off_stream_dispatch);

  return stream;
}

void writeable_off_stream_destroy(writeable_off_stream_t* stream) {
  refcounter_dereference((refcounter_t*)stream);
  if (refcounter_count((refcounter_t*)stream) == 0) {
    for (size_t i = 0; i < stream->entry_count; i++) {
      _entry_destroy(stream->entries[i]);
    }
    free(stream->entries);
    if (stream->final_block != NULL) {
      block_destroy(stream->final_block);
    }
    DESTROY(stream->accumulator, buffer);
    free(stream->hash_state);
    stream_deinit((stream_t*)stream);
    free(stream);
  }
}

void writeable_off_stream_write(writeable_off_stream_t* stream, buffer_t* data) {
  buffer_t* ref = REFERENCE(data, buffer_t);
  message_t msg;
  msg.type = WRITEABLE_WRITE;
  msg.payload = ref;
  msg.payload_destroy = (void (*)(void*))buffer_destroy;

  actor_send(&stream->stream.actor, &msg);
  scheduler_inject(stream->stream.pool, &stream->stream.actor);
}

void writeable_off_stream_finalize(writeable_off_stream_t* stream) {
  blake3_hasher hasher;
  memcpy(&hasher, stream->hash_state, sizeof(blake3_hasher));
  uint8_t* digest = get_clear_memory(32);
  blake3_hasher_finalize(&hasher, digest, 32);
  buffer_t* file_hash = buffer_create_from_existing_memory(digest, 32);
  stream_notify((stream_t*)stream, data_event, CONSUME(file_hash, buffer_t), (void (*)(void*))buffer_destroy);

  if (stream->accumulator->size > 0) {
    block_t* origin = block_create_existing_data_by_type(stream->accumulator, stream->block_type);
    if (origin != NULL) {
      stream->final_block = (block_t*)refcounter_reference((refcounter_t*)origin);
      off_stream_tuple_entry_t* entry = _entry_create(origin, stream->tuple_size);
      _push_entry(stream, entry);
      block_destroy(origin);

      for (size_t i = 0; i < entry->random_capacity; i++) {
        block_t* random = block_create_random_block_by_type(stream->block_type);
        if (random == NULL) break;
        entry->random_blocks[entry->random_count++] = random;
      }

      if (stream->entry_count == 1 && entry->random_count >= entry->random_capacity) {
        off_stream_tuple_entry_t* first = _shift_entry(stream);
        _create_tuple(stream, first);
      }
    }
    stream->accumulator->size = 0;
  } else {
    stream_notify((stream_t*)stream, finished_event, NULL, NULL);
    stream_notify((stream_t*)stream, complete_event, NULL, NULL);
    stream_close((stream_t*)stream);
  }
}