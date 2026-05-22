//
// Created by victor on 7/19/25.
//
#include "section.h"
#include "../Actor/message.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* ---- free_map helpers ---- */

static void free_map_init(free_map_t* fm, size_t total_blocks) {
  fm->total_blocks = total_blocks;
  fm->map_capacity = (total_blocks + 31) / 32;
  fm->map = get_clear_memory(fm->map_capacity * sizeof(uint32_t));
  /* Mark all blocks as free */
  for (size_t i = 0; i < total_blocks; i++) {
    size_t word = i / 32;
    size_t bit = i % 32;
    fm->map[word] |= ((uint32_t)1 << bit);
  }
}

static void free_map_destroy(free_map_t* fm) {
  if (fm->map != NULL) {
    free(fm->map);
    fm->map = NULL;
  }
}

/* Find the first free block and mark it occupied. Returns 0 on success. */
static int free_map_alloc(free_map_t* fm, size_t* index) {
  for (size_t word = 0; word < fm->map_capacity; word++) {
    if (fm->map[word] != 0) {
      int bit = PLATFORM_FFS(fm->map[word]) - 1;
      *index = word * 32 + (size_t)bit;
      fm->map[word] &= ~((uint32_t)1 << bit);
      return 0;
    }
  }
  return 1; /* no free blocks */
}

/* Mark a block as free. Returns 0 on success, 1 if already free or invalid. */
static int free_map_dealloc(free_map_t* fm, size_t index) {
  if (index >= fm->total_blocks) {
    return 1;
  }
  size_t word = index / 32;
  size_t bit = index % 32;
  uint32_t mask = (uint32_t)1 << bit;
  if (fm->map[word] & mask) {
    return 1; /* already free */
  }
  fm->map[word] |= mask;
  return 0;
}

static uint8_t free_map_is_full(free_map_t* fm) {
  for (size_t word = 0; word < fm->map_capacity; word++) {
    if (fm->map[word] != 0) {
      return 0;
    }
  }
  return 1;
}

/* Serialize free_map to a flat byte buffer.
   Format: [4 bytes total_blocks] [map_capacity * 4 bytes map data] */
static uint8_t* free_map_serialize(free_map_t* fm, size_t* out_size) {
  *out_size = 4 + fm->map_capacity * sizeof(uint32_t);
  uint8_t* buf = get_memory(*out_size);
  /* Write total_blocks as little-endian uint32 */
  buf[0] = (uint8_t)(fm->total_blocks & 0xFF);
  buf[1] = (uint8_t)((fm->total_blocks >> 8) & 0xFF);
  buf[2] = (uint8_t)((fm->total_blocks >> 16) & 0xFF);
  buf[3] = (uint8_t)((fm->total_blocks >> 24) & 0xFF);
  memcpy(buf + 4, fm->map, fm->map_capacity * sizeof(uint32_t));
  return buf;
}

/* Deserialize free_map from a byte buffer. Returns 0 on success. */
static int free_map_deserialize(free_map_t* fm, const uint8_t* buf, size_t buf_size) {
  if (buf_size < 4) {
    return 1;
  }
  size_t total_blocks = (size_t)buf[0] | ((size_t)buf[1] << 8) |
                        ((size_t)buf[2] << 16) | ((size_t)buf[3] << 24);
  size_t map_capacity = (total_blocks + 31) / 32;
  size_t expected_size = 4 + map_capacity * sizeof(uint32_t);
  if (buf_size < expected_size) {
    return 1;
  }
  fm->total_blocks = total_blocks;
  fm->map_capacity = map_capacity;
  fm->map = get_clear_memory(map_capacity * sizeof(uint32_t));
  memcpy(fm->map, buf + 4, map_capacity * sizeof(uint32_t));
  return 0;
}

/* ---- async payload destroy helpers ---- */

static void section_read_result_destroy(void* ptr) {
  section_read_result_payload_t* result = (section_read_result_payload_t*)ptr;
  if (result->data != NULL) {
    buffer_destroy(result->data);
  }
  free(result);
}

static void section_write_result_destroy(void* ptr) {
  free(ptr);
}

static void section_deallocate_result_destroy(void* ptr) {
  free(ptr);
}

/* ---- section dispatch ---- */

void section_dispatch(void* state, message_t* msg) {
  section_t* section = (section_t*)state;
  switch (msg->type) {
    case SECTION_WRITE: {
      section_write_payload_t* p = (section_write_payload_t*)msg->payload;
      p->result = -1;
      p->index = 0;
      p->full = 0;
      if (p->data->size != section->block_size) {
        p->result = 1;
        p->full = free_map_is_full(&section->free_map);
        break;
      }
      size_t alloc_index;
      if (free_map_alloc(&section->free_map, &alloc_index) != 0) {
        p->result = 2;
        p->full = 1;
        break;
      }
      if (section->file == NULL) {
        section->file = platform_file_open(section->path, PLATFORM_O_RDWR | PLATFORM_O_CREAT, 0644);
        if (section->file == NULL) {
          free_map_dealloc(&section->free_map, alloc_index);
          p->result = 3;
          p->full = free_map_is_full(&section->free_map);
          break;
        }
      }
      size_t byte_offset = p->data->size * alloc_index;
      ssize_t written = platform_file_pwrite(section->file, p->data->data, p->data->size, (uint64_t)byte_offset);
      if (written < (ssize_t)section->block_size) {
        free_map_dealloc(&section->free_map, alloc_index);
        p->result = 5;
        p->full = free_map_is_full(&section->free_map);
        break;
      }
      atomic_store(&section->dirty, 1);
      if (section->on_dirty != NULL) {
        section->on_dirty(section->on_dirty_context, section);
      }
      p->result = 0;
      p->index = alloc_index;
      p->full = free_map_is_full(&section->free_map);
      /* Send completion message if async (reply_to is set) */
      if (p->reply_to != NULL) {
        section_write_result_payload_t* result = get_clear_memory(sizeof(section_write_result_payload_t));
        result->result = p->result;
        result->index = p->index;
        result->full = p->full;
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTION_WRITE_RESULT;
        reply.payload = result;
        reply.payload_destroy = section_write_result_destroy;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case SECTION_READ: {
      section_read_payload_t* p = (section_read_payload_t*)msg->payload;
      p->result = NULL;
      platform_file_t* read_file = section->file;
      if (read_file == NULL) {
        read_file = platform_file_open(section->path, PLATFORM_O_RDONLY, 0644);
      }
      if (read_file == NULL) {
        break;
      }
      size_t byte_offset = p->index * section->block_size;
      uint8_t* data = get_memory(section->block_size);
      ssize_t read_size = platform_file_pread(read_file, data, section->block_size, (uint64_t)byte_offset);
      if (read_file != section->file) platform_file_close(read_file);
      if (read_size < (ssize_t)section->block_size) {
        free(data);
        break;
      }
      if (p->reply_to != NULL) {
        /* Async: send result via SECTION_READ_RESULT message */
        section_read_result_payload_t* result = get_clear_memory(sizeof(section_read_result_payload_t));
        result->index = p->index;
        result->data = buffer_create_from_existing_memory(data, section->block_size);
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTION_READ_RESULT;
        reply.payload = result;
        reply.payload_destroy = section_read_result_destroy;
        actor_send(p->reply_to, &reply);
      } else {
        /* Sync: store result in payload for caller */
        p->result = buffer_create_from_existing_memory(data, section->block_size);
      }
      break;
    }
    case SECTION_DEALLOCATE: {
      section_deallocate_payload_t* p = (section_deallocate_payload_t*)msg->payload;
      p->result = free_map_dealloc(&section->free_map, p->index);
      if (p->result == 0) {
        atomic_store(&section->dirty, 1);
        if (section->on_dirty != NULL) {
          section->on_dirty(section->on_dirty_context, section);
        }
      }
      /* Send completion message if async (reply_to is set) */
      if (p->reply_to != NULL) {
        section_deallocate_result_payload_t* result = get_clear_memory(sizeof(section_deallocate_result_payload_t));
        result->result = p->result;
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTION_DEALLOCATE_RESULT;
        reply.payload = result;
        reply.payload_destroy = section_deallocate_result_destroy;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case SECTION_SAVE_META: {
      section_save_meta(section);
      break;
    }
    case SECTION_CLOSE: {
      if (section->file != NULL) {
        platform_file_close(section->file);
        section->file = NULL;
      }
      break;
    }
    default:
      break;
  }
}

/* ---- section implementation ---- */

section_t* section_create(char* path, char* meta_path, size_t size, size_t id, block_size_e type) {
  section_t* section = get_clear_memory(sizeof(section_t));
  refcounter_init((refcounter_t*) section);
  actor_init(&section->actor, section, section_dispatch, NULL);
  char section_id[20];
  sprintf(section_id, "%lu", id);
  section->file = NULL;
  section->path = path_join(path, section_id);
  section->meta_path = path_join(meta_path, section_id);
  section->size = size;
  section->id = id;
  section->block_size = (size_t)type;

  if (platform_file_exists(section->meta_path)) {
    /* Existing section -- load metadata */
    platform_file_t* meta_file = platform_file_open(section->meta_path, PLATFORM_O_RDONLY, 0644);
    if (meta_file == NULL) {
      log_error("Failed to open section meta file");
      actor_destroy(&section->actor);
      free(section->path);
      free(section->meta_path);
      free(section);
      return NULL;
    }

    int64_t file_size = platform_file_seek(meta_file, 0, PLATFORM_SEEK_END);
    if (file_size < 0) {
      log_error("Failed to read section meta file size");
      platform_file_close(meta_file);
      actor_destroy(&section->actor);
      free(section->path);
      free(section->meta_path);
      free(section);
      return NULL;
    }

    if (platform_file_seek(meta_file, 0, PLATFORM_SEEK_SET) < 0) {
      log_error("Failed to seek section meta file");
      platform_file_close(meta_file);
      actor_destroy(&section->actor);
      free(section->path);
      free(section->meta_path);
      free(section);
      return NULL;
    }

    uint8_t* buffer = get_memory((size_t)file_size);
    ssize_t read_size = platform_file_read(meta_file, buffer, (size_t)file_size);
    platform_file_close(meta_file);

    if (read_size != file_size) {
      log_error("Failed to read section meta file");
      free(buffer);
      actor_destroy(&section->actor);
      free(section->path);
      free(section->meta_path);
      free(section);
      return NULL;
    }

    if (free_map_deserialize(&section->free_map, buffer, (size_t)file_size) != 0) {
      log_error("Failed to parse section meta file: malformed data");
      free(buffer);
      actor_destroy(&section->actor);
      free(section->path);
      free(section->meta_path);
      free(section);
      return NULL;
    }
    free(buffer);
  } else {
    /* New section -- all blocks are free */
    free_map_init(&section->free_map, size);
  }
  return section;
}

void section_destroy(section_t* section) {
  if (refcounter_dereference_is_zero((refcounter_t*) section)) {
    /* Flush dirty metadata before destroying */
    if (atomic_load(&section->dirty)) {
      section_save_meta(section);
    }
    refcounter_destroy_lock((refcounter_t*) section);
    actor_destroy(&section->actor);
    if (section->file != NULL) {
      platform_file_close(section->file);
      section->file = NULL;
    }
    free_map_destroy(&section->free_map);
    free(section->path);
    free(section->meta_path);
    free(section);
  }
}

/* Direct read of free_map state. Safe because the actor serializes all
   modifications. May return stale data if called from outside the actor
   context while modifications are pending. */
uint8_t section_full(section_t* section) {
  return free_map_is_full(&section->free_map);
}

/* Async API -- send message to section actor.
   The caller must also inject the section's actor into the scheduler
   after calling these functions. Results arrive as
   SECTION_READ_RESULT / SECTION_WRITE_RESULT / SECTION_DEALLOCATE_RESULT
   messages on the reply_to actor. */
void section_read(section_t* section, size_t index, actor_t* reply_to) {
  section_read_payload_t* payload = get_clear_memory(sizeof(section_read_payload_t));
  payload->index = index;
  payload->reply_to = reply_to;
  payload->result = NULL;

  message_t msg;
  msg.type = SECTION_READ;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&section->actor, &msg);
}

void section_write(section_t* section, buffer_t* data, actor_t* reply_to) {
  section_write_payload_t* payload = get_clear_memory(sizeof(section_write_payload_t));
  payload->data = data;
  payload->reply_to = reply_to;
  payload->result = -1;
  payload->index = 0;
  payload->full = 0;

  message_t msg;
  msg.type = SECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&section->actor, &msg);
}

void section_deallocate(section_t* section, size_t index, actor_t* reply_to) {
  section_deallocate_payload_t* payload = get_clear_memory(sizeof(section_deallocate_payload_t));
  payload->index = index;
  payload->reply_to = reply_to;
  payload->result = -1;

  message_t msg;
  msg.type = SECTION_DEALLOCATE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&section->actor, &msg);
}


void section_save_meta(section_t* section) {
  size_t size;
  uint8_t* data = free_map_serialize(&section->free_map, &size);
  platform_file_t* meta_file = platform_file_open(section->meta_path,
      PLATFORM_O_WRONLY | PLATFORM_O_TRUNC | PLATFORM_O_CREAT, 0644);
  if (meta_file == NULL) {
    log_error("Failed to save section meta data");
    free(data);
    return;
  }
  ssize_t written = platform_file_write(meta_file, data, size);
  if (written < (ssize_t)size) {
    log_error("Failed to write section meta data");
  }
  platform_file_close(meta_file);
  free(data);
}