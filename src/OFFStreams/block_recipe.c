//
// Created by victor on 5/7/26.
//

#include "block_recipe.h"
#include "../Util/allocator.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Scheduler/scheduler.h"

// --- Generic recipe pull ---

void block_recipe_pull(block_recipe_t* recipe) {
  message_t msg;
  msg.type = READABLE_PULL;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&recipe->stream.actor, &msg);
  scheduler_inject(recipe->stream.pool, &recipe->stream.actor);
}

// --- NewBlocksRecipe ---

void new_blocks_recipe_dispatch(void* state, message_t* msg) {
  new_blocks_recipe_t* recipe = (new_blocks_recipe_t*)state;
  switch (msg->type) {
    case READABLE_PULL: {
      if (recipe->recipe.stream.is_deactivated) {
        break;
      }
      block_t* block = block_create_random_block_by_type(recipe->recipe.block_type);
      if (block == NULL) {
        stream_notify((stream_t*)recipe, error_event, NULL, NULL);
        stream_notify((stream_t*)recipe, close_event, NULL, NULL);
        recipe->recipe.stream.is_deactivated = 1;
        break;
      }
      block_cache_put(recipe->recipe.bc, block);
      stream_notify((stream_t*)recipe, data_event,
                    CONSUME(block, block_t), (void (*)(void*))block_destroy);
      break;
    }
    case CLOSE_STREAM: {
      stream_notify((stream_t*)recipe, close_event, NULL, NULL);
      recipe->recipe.stream.is_deactivated = 1;
      break;
    }
    default:
      break;
  }
}

new_blocks_recipe_t* new_blocks_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type) {
  new_blocks_recipe_t* recipe = get_clear_memory(sizeof(new_blocks_recipe_t));
  recipe->recipe.bc = bc;
  recipe->recipe.block_type = block_type;

  stream_init((stream_t*)recipe, pull, readable_stream, 0, pool,
              (void (*)(stream_t*))new_blocks_recipe_destroy);
  recipe->recipe.stream.actor.state = recipe;
  recipe->recipe.stream.actor.dispatch = new_blocks_recipe_dispatch;

  return recipe;
}

void new_blocks_recipe_destroy(new_blocks_recipe_t* recipe) {
  if (refcounter_dereference_is_zero((refcounter_t*)recipe)) {
    stream_deinit((stream_t*)recipe);
    free(recipe);
  }
}

void new_blocks_recipe_pull(new_blocks_recipe_t* recipe) {
  block_recipe_pull((block_recipe_t*)recipe);
}

// --- RecyclerRecipe ---

static size_t _block_size_for_type(block_size_e type) {
  switch (type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

static void _load_descriptor(recycler_recipe_t* recipe) {
  if (recipe->endcap != NULL) {
    DESTROY(recipe->endcap, buffer);
    recipe->endcap = NULL;
  }

  if (recipe->ori_index >= recipe->oris.length) {
    stream_notify((stream_t*)recipe, complete_event, NULL, NULL);
    stream_notify((stream_t*)recipe, close_event, NULL, NULL);
    recipe->recipe.stream.is_deactivated = 1;
    return;
  }

  ori_t* current_ori = recipe->oris.data[recipe->ori_index];
  if (current_ori->descriptor_hash == NULL) {
    recipe->ori_index++;
    _load_descriptor(recipe);
    return;
  }

  block_t* descriptor_block = block_cache_get(recipe->recipe.bc, current_ori->descriptor_hash);
  if (descriptor_block == NULL) {
    recipe->ori_index++;
    _load_descriptor(recipe);
    return;
  }

  size_t block_size = _block_size_for_type(current_ori->block_type);
  size_t descriptor_pad = current_ori->file_hash != NULL ? current_ori->file_hash->size : 32;
  size_t cut_point = (block_size / descriptor_pad) * descriptor_pad;

  vec_buffer_t front_hashes;
  vec_buffer_t back_hashes;
  vec_init(&front_hashes);
  vec_init(&back_hashes);

  buffer_t* current_data = descriptor_block->data;
  size_t offset = 0;

  if (current_ori->descriptor_offset > 0 && recipe->descriptor_index == 0) {
    size_t skip_hashes = current_ori->descriptor_offset / descriptor_pad;
    offset = skip_hashes * descriptor_pad;
    recipe->descriptor_index = skip_hashes;
  }

  while (offset + descriptor_pad <= current_data->size) {
    buffer_t* hash = buffer_slice(current_data, offset, offset + descriptor_pad);
    if (hash == NULL) {
      offset += descriptor_pad;
      continue;
    }

    size_t tuple_position = (offset / descriptor_pad) % current_ori->tuple_size;
    if (tuple_position < current_ori->tuple_size - 1) {
      vec_push(&front_hashes, hash);
    } else {
      vec_push(&back_hashes, hash);
    }

    offset += descriptor_pad;
  }

  size_t remaining_start = current_data->size - descriptor_pad;
  buffer_t* next_hash = NULL;
  if (remaining_start > offset - descriptor_pad) {
    next_hash = buffer_slice(current_data, remaining_start, current_data->size);
  }

  block_destroy(descriptor_block);

  while (next_hash != NULL) {
    block_t* next_block = block_cache_get(recipe->recipe.bc, next_hash);
    DESTROY(next_hash, buffer);
    if (next_block == NULL) {
      break;
    }

    current_data = next_block->data;
    offset = 0;

    while (offset + descriptor_pad <= current_data->size) {
      buffer_t* hash = buffer_slice(current_data, offset, offset + descriptor_pad);
      if (hash == NULL) {
        offset += descriptor_pad;
        continue;
      }

      size_t tuple_position = (offset / descriptor_pad) % current_ori->tuple_size;
      if (tuple_position < current_ori->tuple_size - 1) {
        vec_push(&front_hashes, hash);
      } else {
        vec_push(&back_hashes, hash);
      }

      offset += descriptor_pad;
    }

    remaining_start = current_data->size - descriptor_pad;
    next_hash = NULL;
    if (remaining_start > offset - descriptor_pad) {
      next_hash = buffer_slice(current_data, remaining_start, current_data->size);
    }

    block_destroy(next_block);
  }

  if (next_hash != NULL) {
    DESTROY(next_hash, buffer);
  }

  if (back_hashes.length > 0) {
    recipe->endcap = back_hashes.data[back_hashes.length - 1];
    back_hashes.length--;
  }

  for (int i = 0; i < back_hashes.length; i++) {
    vec_push(&front_hashes, back_hashes.data[i]);
  }
  vec_deinit(&back_hashes);

  recipe->descriptor = front_hashes;
  recipe->descriptor_loaded = 1;
}

void recycler_recipe_dispatch(void* state, message_t* msg) {
  recycler_recipe_t* recipe = (recycler_recipe_t*)state;
  switch (msg->type) {
    case READABLE_PULL: {
      if (recipe->recipe.stream.is_deactivated) {
        break;
      }

      if (!recipe->descriptor_loaded) {
        _load_descriptor(recipe);
        if (recipe->recipe.stream.is_deactivated) {
          break;
        }
      }

      if (recipe->descriptor_index >= recipe->descriptor.length) {
        recipe->ori_index++;
        for (int i = 0; i < recipe->descriptor.length; i++) {
          DESTROY(recipe->descriptor.data[i], buffer);
        }
        vec_deinit(&recipe->descriptor);
        vec_init(&recipe->descriptor);
        if (recipe->endcap != NULL) {
          DESTROY(recipe->endcap, buffer);
          recipe->endcap = NULL;
        }
        recipe->descriptor_index = 0;
        recipe->descriptor_loaded = 0;

        if (recipe->ori_index >= recipe->oris.length) {
          stream_notify((stream_t*)recipe, complete_event, NULL, NULL);
          stream_notify((stream_t*)recipe, close_event, NULL, NULL);
          recipe->recipe.stream.is_deactivated = 1;
          break;
        }

        _load_descriptor(recipe);
        if (recipe->recipe.stream.is_deactivated) {
          break;
        }
      }

      if (recipe->descriptor_index < recipe->descriptor.length) {
        buffer_t* hash = recipe->descriptor.data[recipe->descriptor_index];
        recipe->descriptor_index++;

        block_t* block = block_cache_get(recipe->recipe.bc, hash);
        if (block != NULL) {
          stream_notify((stream_t*)recipe, data_event,
                        CONSUME(block, block_t), (void (*)(void*))block_destroy);
        } else {
          stream_notify((stream_t*)recipe, error_event, NULL, NULL);
          stream_notify((stream_t*)recipe, close_event, NULL, NULL);
          recipe->recipe.stream.is_deactivated = 1;
        }
      }
      break;
    }
    case CLOSE_STREAM: {
      stream_notify((stream_t*)recipe, close_event, NULL, NULL);
      recipe->recipe.stream.is_deactivated = 1;
      break;
    }
    default:
      break;
  }
}

recycler_recipe_t* recycler_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type,
    vec_ori_t oris) {
  recycler_recipe_t* recipe = get_clear_memory(sizeof(recycler_recipe_t));
  recipe->recipe.bc = bc;
  recipe->recipe.block_type = block_type;

  recipe->oris = oris;
  for (int i = 0; i < recipe->oris.length; i++) {
    REFERENCE(recipe->oris.data[i], ori_t);
  }
  recipe->ori_index = 0;

  vec_init(&recipe->descriptor);
  recipe->descriptor_index = 0;
  recipe->endcap = NULL;
  recipe->descriptor_loaded = 0;

  stream_init((stream_t*)recipe, pull, readable_stream, 0, pool,
              (void (*)(stream_t*))recycler_recipe_destroy);
  recipe->recipe.stream.actor.state = recipe;
  recipe->recipe.stream.actor.dispatch = recycler_recipe_dispatch;

  return recipe;
}

void recycler_recipe_destroy(recycler_recipe_t* recipe) {
  if (refcounter_dereference_is_zero((refcounter_t*)recipe)) {
    for (int i = 0; i < recipe->oris.length; i++) {
      DESTROY(recipe->oris.data[i], ori);
    }
    vec_deinit(&recipe->oris);

    for (int i = 0; i < recipe->descriptor.length; i++) {
      DESTROY(recipe->descriptor.data[i], buffer);
    }
    vec_deinit(&recipe->descriptor);

    if (recipe->endcap != NULL) {
      DESTROY(recipe->endcap, buffer);
    }

    stream_deinit((stream_t*)recipe);
    free(recipe);
  }
}

void recycler_recipe_pull(recycler_recipe_t* recipe) {
  block_recipe_pull((block_recipe_t*)recipe);
}