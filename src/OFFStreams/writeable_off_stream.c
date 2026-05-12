//
// Created by victor on 5/7/26.
//

#include "writeable_off_stream.h"
#include "block_recipe.h"
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
  vec_init(&entry->random_blocks);
  vec_reserve(&entry->random_blocks, tuple_size - 1);
  entry->random_capacity = tuple_size - 1;
  return entry;
}

static void _entry_destroy(off_stream_tuple_entry_t* entry) {
  if (entry->origin != NULL) {
    block_destroy(entry->origin);
  }
  for (int i = 0; i < entry->random_blocks.length; i++) {
    if (entry->random_blocks.data[i] != NULL) {
      block_destroy(entry->random_blocks.data[i]);
    }
  }
  vec_deinit(&entry->random_blocks);
  free(entry);
}

static void _get_random_blocks(writeable_off_stream_t* stream);

static void _maybe_finalize(writeable_off_stream_t* stream) {
  if (stream->pending_finalize && stream->entries.length == 0 && !stream->has_pulled) {
    stream->pending_finalize = 0;
    stream_notify((stream_t*)stream, finished_event, NULL, NULL);
    stream_notify((stream_t*)stream, complete_event, NULL, NULL);
    stream_notify((stream_t*)stream, close_event, NULL, NULL);
    stream->stream.is_deactivated = 1;
  }
}

static void _get_random_blocks(writeable_off_stream_t* stream) {
  if (stream->has_pulled || stream->current_recipe == NULL) {
    return;
  }
  stream->has_pulled = 1;
  block_recipe_pull(stream->current_recipe);
}

static void _create_tuple(writeable_off_stream_t* stream, off_stream_tuple_entry_t* entry) {
  buffer_t* origin_data = entry->origin->data;
  buffer_t* off_data = buffer_copy(origin_data);

  for (int i = 0; i < entry->random_blocks.length; i++) {
    buffer_t* xored = buffer_xor(off_data, entry->random_blocks.data[i]->data);
    DESTROY(off_data, buffer);
    off_data = xored;
  }

  block_t* off_block = block_create_existing_data_by_type(off_data, stream->block_type);
  DESTROY(off_data, buffer);
  if (off_block == NULL) {
    return;
  }

  tuple_t* tuple = tuple_create(stream->tuple_size);
  for (int i = 0; i < entry->random_blocks.length; i++) {
    tuple_push(tuple, entry->random_blocks.data[i]->hash);
  }
  tuple_push(tuple, off_block->hash);

  for (int i = 0; i < entry->random_blocks.length; i++) {
    block_cache_put(stream->bc, entry->random_blocks.data[i]);
  }
  block_cache_put(stream->bc, off_block);

  tuple_cache_update(stream->tc, tuple, origin_data);

  buffer_t* tuple_val = (buffer_t*)refcounter_reference((refcounter_t*)tuple);
  stream_notify((stream_t*)stream, data_event, CONSUME(tuple_val, buffer_t), (void (*)(void*))tuple_destroy);

  uint8_t is_final = stream->final_block != NULL &&
      entry->origin->hash != NULL &&
      entry->origin->hash->size == stream->final_block->hash->size &&
      memcmp(entry->origin->hash->data, stream->final_block->hash->data, entry->origin->hash->size) == 0;

  block_destroy(off_block);
  tuple_destroy(tuple);
  _entry_destroy(entry);


  if (is_final) {
    block_destroy(stream->final_block);
    stream->final_block = NULL;
    stream_notify((stream_t*)stream, finished_event, NULL, NULL);
    stream_notify((stream_t*)stream, complete_event, NULL, NULL);
    stream_notify((stream_t*)stream, close_event, NULL, NULL);
    stream->stream.is_deactivated = 1;
  } else if (stream->entries.length > 0 && !stream->stream.is_deactivated) {
    _get_random_blocks(stream);
  } else {
    _maybe_finalize(stream);
  }
}

static void _on_recipe_block(void* ctx, void* data) {
  writeable_off_stream_t* stream = (writeable_off_stream_t*)ctx;
  block_t* block = (block_t*)refcounter_reference((refcounter_t*)data);
  message_t msg;
  msg.type = RECIPE_BLOCK_DATA;
  msg.payload = block;
  msg.payload_destroy = (void (*)(void*))block_destroy;
  actor_send(&stream->stream.actor, &msg);
}

static void _on_recipe_close(void* ctx, void* data) {
  (void)data;
  writeable_off_stream_t* stream = (writeable_off_stream_t*)ctx;
  message_t msg;
  msg.type = RECIPE_ROTATE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&stream->stream.actor, &msg);
}

static void _register_recipe(writeable_off_stream_t* stream) {
  stream->recipe_data_sub_id = stream_subscribe(
      (stream_t*)stream->current_recipe, data_event, stream,
      _on_recipe_block, NULL);
  stream->recipe_close_sub_id = stream_subscribe(
      (stream_t*)stream->current_recipe, close_event, stream,
      _on_recipe_close, NULL);
  stream->recipe_error_sub_id = stream_subscribe(
      (stream_t*)stream->current_recipe, error_event, stream,
      _on_recipe_close, NULL);
}

static void _unregister_recipe(writeable_off_stream_t* stream) {
  if (stream->current_recipe == NULL) {
    return;
  }
  stream_unsubscribe((stream_t*)stream->current_recipe, data_event, stream->recipe_data_sub_id);
  stream_unsubscribe((stream_t*)stream->current_recipe, close_event, stream->recipe_close_sub_id);
  stream_unsubscribe((stream_t*)stream->current_recipe, error_event, stream->recipe_error_sub_id);
}

void writeable_off_stream_dispatch(void* state, message_t* msg) {
  writeable_off_stream_t* stream = (writeable_off_stream_t*)state;
  switch (msg->type) {
    case WRITEABLE_WRITE: {
      buffer_t* data = (buffer_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        DESTROY(data, buffer);
        msg->payload = NULL;
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
            vec_push(&stream->entries, entry);
            block_destroy(origin);

            if (!stream->is_readable) {
              stream->is_readable = 1;
              stream_notify((stream_t*)stream, readable_event, NULL, NULL);
            }

            if (!stream->has_pulled) {
              _get_random_blocks(stream);
            }
          }
          stream->accumulator->size = 0;
        }
      }

      DESTROY(data, buffer);
      msg->payload = NULL;
      break;
    }
    case WRITEABLE_FINALIZE: {
      if (stream->stream.is_deactivated) {
        break;
      }
      blake3_hasher hasher;
      memcpy(&hasher, stream->hash_state, sizeof(blake3_hasher));
      uint8_t* digest = get_clear_memory(32);
      blake3_hasher_finalize(&hasher, digest, 32);
      buffer_t* file_hash = buffer_create_from_existing_memory(digest, 32);
      stream_notify((stream_t*)stream, data_event, CONSUME(file_hash, buffer_t), (void (*)(void*))buffer_destroy);

      if (stream->accumulator->size > 0) {
        block_t* origin = block_create_by_type(stream->accumulator, stream->block_type);
        if (origin != NULL) {
          stream->final_block = (block_t*)refcounter_reference((refcounter_t*)origin);
          off_stream_tuple_entry_t* entry = _entry_create(origin, stream->tuple_size);
          vec_push(&stream->entries, entry);
          block_destroy(origin);

          if (!stream->has_pulled) {
            _get_random_blocks(stream);
          }
        }
        stream->accumulator->size = 0;
      }

      if (stream->entries.length == 0 && !stream->has_pulled) {
        stream_notify((stream_t*)stream, finished_event, NULL, NULL);
        stream_notify((stream_t*)stream, complete_event, NULL, NULL);
        stream_notify((stream_t*)stream, close_event, NULL, NULL);
        stream->stream.is_deactivated = 1;
      } else {
        stream->pending_finalize = 1;
      }
      break;
    }
    case RECIPE_BLOCK_DATA: {
      block_t* block = (block_t*)msg->payload;
      stream->has_pulled = 0;

      if (stream->entries.length == 0 || stream->stream.is_deactivated) {
        block_destroy(block);
        msg->payload = NULL;
        break;
      }

      off_stream_tuple_entry_t* entry = stream->entries.data[0];
      vec_push(&entry->random_blocks, block);

      if (entry->random_blocks.length >= entry->random_capacity) {
        vec_splice(&stream->entries, 0, 1);
        _create_tuple(stream, entry);
      } else {
        _get_random_blocks(stream);
      }

      msg->payload = NULL;
      break;
    }
    case RECIPE_ROTATE: {
      _unregister_recipe(stream);
      if (stream->current_recipe != NULL) {
        refcounter_dereference((refcounter_t*)stream->current_recipe);
      }
      stream->current_recipe_index++;
      if (stream->current_recipe_index >= stream->recipes.length) {
        stream_deactivate((stream_t*)stream, ERROR("Write error"));
        stream->stream.is_deactivated = 1;
        break;
      }

      stream->current_recipe = stream->recipes.data[stream->current_recipe_index];
      refcounter_reference((refcounter_t*)stream->current_recipe);
      _register_recipe(stream);

      if (stream->has_pulled) {
        _get_random_blocks(stream);
      }
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

static void _writeable_off_stream_on_write(stream_t* stream, void* data) {
  (void)stream;
  (void)data;
}

writeable_off_stream_t* writeable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    block_size_e block_type, size_t tuple_size, size_t digest_size,
    vec_block_recipe_t recipes) {
  (void)digest_size;
  writeable_off_stream_t* stream = get_clear_memory(sizeof(writeable_off_stream_t));
  stream->bc = bc;
  stream->tc = tc;
  stream->block_type = block_type;
  stream->block_size = _block_size_for_type(block_type);
  stream->tuple_size = tuple_size;
  stream->final_block = NULL;
  stream->is_readable = 0;
  stream->pending_finalize = 0;

  vec_init(&stream->entries);

  stream->accumulator = buffer_create(stream->block_size);
  stream->accumulator->size = 0;
  stream->hash_state = get_clear_memory(sizeof(blake3_hasher));
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  memcpy(stream->hash_state, &hasher, sizeof(blake3_hasher));

  stream->recipes = recipes;
  for (int i = 0; i < stream->recipes.length; i++) {
    refcounter_reference((refcounter_t*)stream->recipes.data[i]);
  }
  stream->current_recipe_index = 0;
  stream->current_recipe = stream->recipes.length > 0 ? stream->recipes.data[0] : NULL;
  if (stream->current_recipe != NULL) {
    refcounter_reference((refcounter_t*)stream->current_recipe);
  }
  stream->has_pulled = 0;

  stream_init((stream_t*)stream, push, writeable_stream, 1, pool,
              (void (*)(stream_t*))writeable_off_stream_destroy);
  stream->stream.on_write = _writeable_off_stream_on_write;

  stream->stream.actor.state = stream;
  stream->stream.actor.dispatch = writeable_off_stream_dispatch;

  if (stream->current_recipe != NULL) {
    _register_recipe(stream);
  }

  return stream;
}

void writeable_off_stream_destroy(writeable_off_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*)stream)) {
    for (int i = 0; i < stream->entries.length; i++) {
      _entry_destroy(stream->entries.data[i]);
    }
    vec_deinit(&stream->entries);
    if (stream->final_block != NULL) {
      block_destroy(stream->final_block);
    }
    DESTROY(stream->accumulator, buffer);
    free(stream->hash_state);

    _unregister_recipe(stream);
    if (stream->current_recipe != NULL) {
      refcounter_dereference((refcounter_t*)stream->current_recipe);
    }
    for (int i = 0; i < stream->recipes.length; i++) {
      refcounter_dereference((refcounter_t*)stream->recipes.data[i]);
    }
    vec_deinit(&stream->recipes);

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
}

void writeable_off_stream_finalize(writeable_off_stream_t* stream) {
  message_t msg;
  msg.type = WRITEABLE_FINALIZE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&stream->stream.actor, &msg);
}