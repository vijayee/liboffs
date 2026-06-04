//
// Created by victor on 7/19/25.
//
#include "section.h"
#include "../Actor/message.h"
#include "../Platform/platform_compiler.h"
#include "../Platform/platform_file.h"
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

static size_t free_map_count_free(free_map_t* fm) {
  size_t count = 0;
  for (size_t word = 0; word < fm->map_capacity; word++) {
    count += (size_t)PLATFORM_POPCOUNT(fm->map[word]);
  }
  return count;
}

static size_t free_map_highest_used(free_map_t* fm) {
  for (size_t word = fm->map_capacity; word > 0; word--) {
    size_t w = word - 1;
    if (fm->map[w] != 0xFFFFFFFFU) {
      uint32_t occupied = ~fm->map[w];
      int bit = 31 - PLATFORM_CLZ(occupied);
      return w * 32 + (size_t)bit;
    }
  }
  return fm->total_blocks;
}

/* Compact occupied slots toward the front (slot 0, 1, 2, ...).
   Returns relocation array where result[old_index] = new_index,
   or (size_t)-1 if the slot was free. The free_map is updated in place:
   slots 0..new_count-1 are occupied, new_count..total_blocks-1 are free. */
static free_map_defrag_result_t free_map_defragment(free_map_t* fm) {
  free_map_defrag_result_t result;
  result.relocation = get_memory(fm->total_blocks * sizeof(size_t));
  memset(result.relocation, 0xFF, fm->total_blocks * sizeof(size_t));

  size_t write_pos = 0;
  for (size_t i = 0; i < fm->total_blocks; i++) {
    size_t word = i / 32;
    size_t bit = i % 32;
    uint8_t is_free = (fm->map[word] >> bit) & 1;
    if (!is_free) {
      result.relocation[i] = write_pos;
      write_pos++;
    }
  }
  result.new_count = write_pos;

  /* Rebuild the free_map: slots 0..new_count-1 occupied, rest free */
  for (size_t word = 0; word < fm->map_capacity; word++) {
    fm->map[word] = 0;
  }
  for (size_t i = write_pos; i < fm->total_blocks; i++) {
    size_t word = i / 32;
    size_t bit = i % 32;
    fm->map[word] |= ((uint32_t)1 << bit);
  }

  return result;
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

/* ---- relocation plan file helpers ---- */

/* Build the path for a section's relocation plan file.
   Format: <meta_path>.defrag */
static char* _section_defrag_plan_path(section_t* section) {
  size_t len = strlen(section->meta_path) + 8;
  char* path = get_memory(len);
  snprintf(path, len, "%s.defrag", section->meta_path);
  return path;
}

/* Write the relocation plan to a file for crash recovery.
   Format: [4B total_blocks LE] [4B new_count LE] [total_blocks * 8B relocation LE] */
static int _section_defrag_plan_write(section_t* section,
                                       free_map_defrag_result_t* defrag) {
  char* plan_path = _section_defrag_plan_path(section);
  size_t buf_size = 8 + section->free_map.total_blocks * 8;
  uint8_t* buf = get_memory(buf_size);
  /* total_blocks as LE uint32 */
  buf[0] = (uint8_t)(section->free_map.total_blocks & 0xFF);
  buf[1] = (uint8_t)((section->free_map.total_blocks >> 8) & 0xFF);
  buf[2] = (uint8_t)((section->free_map.total_blocks >> 16) & 0xFF);
  buf[3] = (uint8_t)((section->free_map.total_blocks >> 24) & 0xFF);
  /* new_count as LE uint32 */
  buf[4] = (uint8_t)(defrag->new_count & 0xFF);
  buf[5] = (uint8_t)((defrag->new_count >> 8) & 0xFF);
  buf[6] = (uint8_t)((defrag->new_count >> 16) & 0xFF);
  buf[7] = (uint8_t)((defrag->new_count >> 24) & 0xFF);
  /* relocation array as LE uint64 */
  for (size_t i = 0; i < section->free_map.total_blocks; i++) {
    size_t off = 8 + i * 8;
    uint64_t val = (uint64_t)defrag->relocation[i];
    buf[off + 0] = (uint8_t)(val & 0xFF);
    buf[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((val >> 24) & 0xFF);
    buf[off + 4] = (uint8_t)((val >> 32) & 0xFF);
    buf[off + 5] = (uint8_t)((val >> 40) & 0xFF);
    buf[off + 6] = (uint8_t)((val >> 48) & 0xFF);
    buf[off + 7] = (uint8_t)((val >> 56) & 0xFF);
  }
  platform_file_t* file = platform_file_open(plan_path,
      PLATFORM_O_WRONLY | PLATFORM_O_CREAT | PLATFORM_O_TRUNC, 0644);
  if (file == NULL) {
    free(buf);
    free(plan_path);
    return -1;
  }
  ssize_t written = platform_file_write(file, buf, buf_size);
  platform_file_sync(file);
  platform_file_close(file);
  free(buf);
  free(plan_path);
  return (written == (ssize_t)buf_size) ? 0 : -1;
}

/* Delete the relocation plan file after successful defragmentation. */
static void _section_defrag_plan_delete(section_t* section) {
  char* plan_path = _section_defrag_plan_path(section);
  platform_file_unlink(plan_path);
  free(plan_path);
}

/* Check for and recover from an incomplete defragmentation.
   If a .defrag plan file exists, roll forward: move data, update free_map,
   truncate, delete plan. Returns 0 if recovery was performed or not needed. */
static int _section_defrag_plan_recover(section_t* section) {
  char* plan_path = _section_defrag_plan_path(section);
  if (!platform_file_exists(plan_path)) {
    free(plan_path);
    return 0;
  }

  log_info("Recovering from incomplete defragmentation for section %zu", section->id);

  platform_file_t* file = platform_file_open(plan_path, PLATFORM_O_RDONLY, 0644);
  if (file == NULL) {
    log_error("Failed to open defrag plan file for recovery");
    free(plan_path);
    return -1;
  }

  int64_t file_size = platform_file_seek(file, 0, PLATFORM_SEEK_END);
  platform_file_seek(file, 0, PLATFORM_SEEK_SET);
  if (file_size < 8) {
    platform_file_close(file);
    free(plan_path);
    return -1;
  }

  uint8_t* buf = get_memory((size_t)file_size);
  ssize_t read_size = platform_file_read(file, buf, (size_t)file_size);
  platform_file_close(file);
  if (read_size != file_size) {
    free(buf);
    free(plan_path);
    return -1;
  }

  size_t total_blocks = (size_t)buf[0] | ((size_t)buf[1] << 8) |
                        ((size_t)buf[2] << 16) | ((size_t)buf[3] << 24);
  size_t new_count = (size_t)buf[4] | ((size_t)buf[5] << 8) |
                     ((size_t)buf[6] << 16) | ((size_t)buf[7] << 24);

  if (total_blocks != section->free_map.total_blocks ||
      8 + total_blocks * 8 != (size_t)file_size) {
    log_error("Defrag plan file has invalid format");
    free(buf);
    free(plan_path);
    return -1;
  }

  /* Parse relocation array */
  size_t* relocation = get_memory(total_blocks * sizeof(size_t));
  for (size_t i = 0; i < total_blocks; i++) {
    size_t off = 8 + i * 8;
    uint64_t val = 0;
    for (int byte = 0; byte < 8; byte++) {
      val |= (uint64_t)buf[off + byte] << (byte * 8);
    }
    relocation[i] = (size_t)val;
  }
  free(buf);

  /* Open data file if needed */
  if (section->file == NULL) {
    section->file = platform_file_open(section->path,
        PLATFORM_O_RDWR | PLATFORM_O_CREAT, 0644);
    if (section->file == NULL) {
      free(relocation);
      free(plan_path);
      return -1;
    }
  }

  /* Move data for each relocation where old != new */
  uint8_t* block_buf = get_memory(section->block_size);
  for (size_t i = 0; i < total_blocks; i++) {
    if (relocation[i] != (size_t)-1 && relocation[i] != i) {
      size_t old_offset = i * section->block_size;
      size_t new_offset = relocation[i] * section->block_size;
      ssize_t rd = platform_file_pread(section->file, block_buf,
                                        section->block_size, (uint64_t)old_offset);
      if (rd == (ssize_t)section->block_size) {
        platform_file_pwrite(section->file, block_buf,
                              section->block_size, (uint64_t)new_offset);
      }
    }
  }
  free(block_buf);

  /* Truncate file to new_count * block_size */
  platform_file_truncate(section->file, (uint64_t)(new_count * section->block_size));
  platform_file_sync(section->file);

  /* Rebuild free_map: slots 0..new_count-1 occupied, rest free */
  for (size_t word = 0; word < section->free_map.map_capacity; word++) {
    section->free_map.map[word] = 0;
  }
  for (size_t i = new_count; i < total_blocks; i++) {
    size_t word = i / 32;
    size_t bit = i % 32;
    section->free_map.map[word] |= ((uint32_t)1 << bit);
  }

  /* Save metadata and delete plan */
  section_save_meta(section);
  free(relocation);
  platform_file_unlink(plan_path);
  free(plan_path);

  log_info("Defragmentation recovery complete for section %zu", section->id);
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

static void section_defragment_result_destroy(void* ptr) {
  section_defragment_result_payload_t* result = (section_defragment_result_payload_t*)ptr;
  if (result->defrag.relocation != NULL) {
    free(result->defrag.relocation);
  }
  free(result);
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
    case SECTION_DEFRAGMENT: {
      section_defragment_payload_t* p = (section_defragment_payload_t*)msg->payload;
      p->result = -1;
      p->section_id = section->id;
      p->defrag.relocation = NULL;
      p->defrag.new_count = 0;

      size_t total = section->free_map.total_blocks;
      size_t free_count = free_map_count_free(&section->free_map);
      size_t used_count = total - free_count;

      /* Skip if section is empty, fully occupied, or no gaps to fill */
      if (used_count == 0 || free_count == 0 ||
          free_map_highest_used(&section->free_map) < used_count) {
        /* Already compact or nothing to defragment */
        p->result = 0;
        p->defrag.relocation = NULL;
        p->defrag.new_count = used_count;
        break;
      }

      /* Compact the free_map and get the relocation map */
      free_map_defrag_result_t defrag = free_map_defragment(&section->free_map);

      /* Write relocation plan file for crash recovery before moving data */
      if (_section_defrag_plan_write(section, &defrag) != 0) {
        log_error("Failed to write defrag plan file for section %zu", section->id);
        /* Restore free_map by re-init from scratch */
        free(defrag.relocation);
        free_map_destroy(&section->free_map);
        free_map_init(&section->free_map, total);
        /* Mark occupied slots based on what was there before — we lost the
           original map. Best effort: mark 0..used_count-1 as occupied. */
        for (size_t i = 0; i < used_count; i++) {
          size_t word = i / 32;
          size_t bit = i % 32;
          section->free_map.map[word] &= ~((uint32_t)1 << bit);
        }
        break;
      }

      /* Open data file if needed */
      if (section->file == NULL) {
        section->file = platform_file_open(section->path,
            PLATFORM_O_RDWR | PLATFORM_O_CREAT, 0644);
        if (section->file == NULL) {
          log_error("Failed to open section file for defragmentation");
          free(defrag.relocation);
          break;
        }
      }

      /* Move data for each relocation where old != new */
      uint8_t* block_buf = get_memory(section->block_size);
      for (size_t i = 0; i < total; i++) {
        if (defrag.relocation[i] != (size_t)-1 && defrag.relocation[i] != i) {
          size_t old_offset = i * section->block_size;
          size_t new_offset = defrag.relocation[i] * section->block_size;
          ssize_t rd = platform_file_pread(section->file, block_buf,
              section->block_size, (uint64_t)old_offset);
          if (rd == (ssize_t)section->block_size) {
            platform_file_pwrite(section->file, block_buf,
                section->block_size, (uint64_t)new_offset);
          }
        }
      }
      free(block_buf);

      /* Truncate file to the compacted size */
      uint64_t new_file_size = (uint64_t)(defrag.new_count * section->block_size);
      platform_file_truncate(section->file, new_file_size);
      platform_file_sync(section->file);

      /* Save metadata and delete plan file */
      atomic_store(&section->dirty, 1);
      section_save_meta(section);
      _section_defrag_plan_delete(section);

      p->result = 0;
      p->section_id = section->id;
      p->defrag = defrag;

      /* Send completion message if async (reply_to is set) */
      if (p->reply_to != NULL) {
        section_defragment_result_payload_t* result =
            get_clear_memory(sizeof(section_defragment_result_payload_t));
        result->result = p->result;
        result->section_id = p->section_id;
        result->defrag.relocation = get_memory(total * sizeof(size_t));
        memcpy(result->defrag.relocation, defrag.relocation, total * sizeof(size_t));
        result->defrag.new_count = defrag.new_count;
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTION_DEFRAGMENT_RESULT;
        reply.payload = result;
        reply.payload_destroy = section_defragment_result_destroy;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    default:
      break;
  }
}

/* ---- section implementation ---- */

section_t* section_create(char* path, char* meta_path, size_t size, size_t id, block_size_e type, scheduler_pool_t* pool) {
  section_t* section = get_clear_memory(sizeof(section_t));
  refcounter_init((refcounter_t*) section);
  actor_init(&section->actor, section, section_dispatch, pool);
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

  /* Check for and recover from incomplete defragmentation */
  _section_defrag_plan_recover(section);

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

size_t section_count_free(section_t* section) {
  return free_map_count_free(&section->free_map);
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

void section_defragment(section_t* section, actor_t* reply_to) {
  section_defragment_payload_t* payload = get_clear_memory(sizeof(section_defragment_payload_t));
  payload->reply_to = reply_to;
  payload->result = -1;
  payload->section_id = section->id;
  payload->defrag.relocation = NULL;
  payload->defrag.new_count = 0;

  message_t msg;
  msg.type = SECTION_DEFRAGMENT;
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