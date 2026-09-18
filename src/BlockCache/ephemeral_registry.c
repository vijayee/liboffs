//
// Ephemeral registry — actor-owned persistent elastic bloom filter of
// ephemeral representation descriptor hashes.
//

#include "ephemeral_registry.h"
#include "../Platform/platform_file.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/path_join.h"
#include <cbor.h>
#include <stdlib.h>
#include <string.h>

#define EPHEMERAL_REGISTRY_FILE_NAME "ephemeral_registry.bf"

/* ---- persistence ---- */

/* Serialize the filter to CBOR and write it through the two-file rotation:
   temp (write + fsync) → old backup removed → current renamed to backup →
   temp renamed to current. A lost flush is safe — the filter is advisory and
   the block index is authoritative — so failures are logged and skipped
   rather than propagated. */
static void _registry_flush(ephemeral_registry_t* registry) {
  cbor_item_t* encoded = elastic_bloom_filter_encode(registry->filter);
  if (encoded == NULL) {
    log_error("ephemeral registry: filter encode failed — flush skipped");
    return;
  }
  uint8_t* data = NULL;
  size_t data_size = 0;
  size_t serialized = cbor_serialize_alloc(encoded, &data, &data_size);
  cbor_decref(&encoded);
  if (data == NULL || serialized == 0 || serialized != data_size) {
    log_error("ephemeral registry: filter serialization failed — flush skipped");
    free(data);
    return;
  }

  platform_file_t* file =
      platform_file_open(registry->temp_file, PLATFORM_O_WRONLY | PLATFORM_O_CREAT | PLATFORM_O_TRUNC, 0644);
  if (file == NULL) {
    log_error("ephemeral registry: cannot open %s for flush", registry->temp_file);
    free(data);
    return;
  }
  ssize_t written = platform_file_write(file, data, data_size);
  int synced = platform_file_sync(file);
  platform_file_close(file);
  free(data);
  if (written != (ssize_t)data_size || synced != 0) {
    log_error("ephemeral registry: incomplete or unsynced write to %s — flush abandoned",
              registry->temp_file);
    platform_file_unlink(registry->temp_file);
    return;
  }

  /* Rotate. A missing current file on the first flush is normal — the backup
     simply stays absent until the next flush has a current to rotate. */
  if (platform_file_exists(registry->backup_file)) {
    platform_file_unlink(registry->backup_file);
  }
  if (platform_file_exists(registry->current_file)) {
    if (platform_file_rename(registry->current_file, registry->backup_file) != 0) {
      log_error("ephemeral registry: rotate %s → %s failed — flush abandoned",
                registry->current_file, registry->backup_file);
      return;
    }
  }
  if (platform_file_rename(registry->temp_file, registry->current_file) != 0) {
    log_error("ephemeral registry: rotate %s → %s failed",
              registry->temp_file, registry->current_file);
  }
}

/* Read the whole filter file and decode the CBOR filter from it. Returns
   NULL when the file is missing, unreadable, or corrupt — callers fall back
   to the next file in the load order. */
static elastic_bloom_filter_t* _registry_load_filter(const char* filename) {
  platform_file_t* file = platform_file_open(filename, PLATFORM_O_RDONLY, 0);
  if (file == NULL) {
    return NULL;
  }
  size_t capacity = 4096;
  size_t used = 0;
  uint8_t* data = get_clear_memory(capacity);
  for (;;) {
    if (used == capacity) {
      uint8_t* grown = (uint8_t*)realloc(data, capacity * 2);
      if (grown == NULL) {
        free(data);
        platform_file_close(file);
        return NULL;
      }
      data = grown;
      capacity *= 2;
    }
    ssize_t read_count = platform_file_read(file, data + used, capacity - used);
    if (read_count < 0) {
      free(data);
      platform_file_close(file);
      return NULL;
    }
    if (read_count == 0) {
      break;
    }
    used += (size_t)read_count;
  }
  platform_file_close(file);
  if (used == 0) {
    free(data);
    return NULL;
  }

  struct cbor_load_result load_result;
  cbor_item_t* item = cbor_load(data, used, &load_result);
  free(data);
  if (item == NULL) {
    return NULL;
  }
  elastic_bloom_filter_t* filter = elastic_bloom_filter_decode(item);
  cbor_decref(&item);
  return filter;
}

/* ---- actor ---- */

void ephemeral_registry_dispatch(void* state, message_t* msg) {
  ephemeral_registry_t* registry = (ephemeral_registry_t*)state;
  if (registry == NULL) {
    log_error("ephemeral_registry_dispatch: registry is NULL — dropping msg type %d",
              (int)msg->type);
    return;
  }
  if (registry->filter == NULL) {
    log_error("ephemeral_registry_dispatch: filter is NULL — dropping msg type %d",
              (int)msg->type);
    return;
  }
  switch (msg->type) {
    case EPHEMERAL_REGISTRY_ADD: {
      buffer_t* hash = (buffer_t*)msg->payload;
      if (hash == NULL || hash->data == NULL) {
        break;
      }
      elastic_bloom_filter_add(registry->filter, hash->data, hash->size);
      _registry_flush(registry);
      break;
    }
    case EPHEMERAL_REGISTRY_REMOVE: {
      buffer_t* hash = (buffer_t*)msg->payload;
      if (hash == NULL || hash->data == NULL) {
        break;
      }
      elastic_bloom_filter_remove(registry->filter, hash->data, hash->size);
      _registry_flush(registry);
      break;
    }
    case EPHEMERAL_REGISTRY_CHECK: {
      registry_check_request_payload_t* request = (registry_check_request_payload_t*)msg->payload;
      if (request == NULL) {
        break;
      }
      uint8_t present = request->hash != NULL && request->hash->data != NULL &&
                        elastic_bloom_filter_contains(registry->filter,
                                                      request->hash->data,
                                                      request->hash->size);
      if (request->reply_to != NULL) {
        ephemeral_registry_check_result_payload_t* result =
            get_clear_memory(sizeof(ephemeral_registry_check_result_payload_t));
        result->present = present;
        result->reply_to = NULL;
        message_t reply;
        reply.type = EPHEMERAL_REGISTRY_CHECK_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(request->reply_to, &reply);
      }
      break;
    }
    default:
      break;
  }
}

/* ---- message payload destroyers ---- */

/* ADD/REMOVE payloads are referenced buffer_t hashes */
static void _registry_buffer_destroy(void* ptr) {
  if (ptr != NULL) {
    buffer_destroy((buffer_t*)ptr);
  }
}

/* CHECK payload — release the referenced hash, free the request shell
   (mirrors cache_ephemeral_payload_destroy in block_cache.c) */
static void _registry_check_request_destroy(void* ptr) {
  registry_check_request_payload_t* payload = (registry_check_request_payload_t*)ptr;
  if (payload == NULL) {
    return;
  }
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}

/* ---- async API — send message, actor injected by actor_send ---- */

void ephemeral_registry_add(ephemeral_registry_t* registry, buffer_t* descriptor_hash) {
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)descriptor_hash);
  message_t msg;
  msg.type = EPHEMERAL_REGISTRY_ADD;
  msg.payload = hash;
  msg.payload_destroy = _registry_buffer_destroy;
  actor_send(&registry->actor, &msg);
}

void ephemeral_registry_remove(ephemeral_registry_t* registry, buffer_t* descriptor_hash) {
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)descriptor_hash);
  message_t msg;
  msg.type = EPHEMERAL_REGISTRY_REMOVE;
  msg.payload = hash;
  msg.payload_destroy = _registry_buffer_destroy;
  actor_send(&registry->actor, &msg);
}

void ephemeral_registry_check(ephemeral_registry_t* registry, buffer_t* descriptor_hash, actor_t* reply_to) {
  registry_check_request_payload_t* payload =
      get_clear_memory(sizeof(registry_check_request_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)descriptor_hash);
  payload->reply_to = reply_to;

  message_t msg;
  msg.type = EPHEMERAL_REGISTRY_CHECK;
  msg.payload = payload;
  msg.payload_destroy = _registry_check_request_destroy;
  actor_send(&registry->actor, &msg);
}

/* ---- lifecycle ---- */

ephemeral_registry_t* ephemeral_registry_create(const char* location, config_t config, scheduler_pool_t* pool) {
  if (location == NULL) {
    log_error("ephemeral_registry_create: location is NULL");
    return NULL;
  }
  ephemeral_registry_t* registry = get_clear_memory(sizeof(ephemeral_registry_t));
  registry->pool = pool;
  registry->current_file = path_join(location, EPHEMERAL_REGISTRY_FILE_NAME);
  registry->backup_file = path_join(location, EPHEMERAL_REGISTRY_FILE_NAME ".last");
  registry->temp_file = path_join(location, EPHEMERAL_REGISTRY_FILE_NAME ".tmp");

  /* Load order: current → backup → fresh empty filter. */
  registry->filter = _registry_load_filter(registry->current_file);
  if (registry->filter == NULL) {
    registry->filter = _registry_load_filter(registry->backup_file);
    if (registry->filter != NULL) {
      log_warn("ephemeral registry: %s is missing or corrupt — starting from the backup",
               registry->current_file);
    }
  }
  if (registry->filter == NULL) {
    log_warn("ephemeral registry: no usable filter file — starting empty");
    registry->filter = elastic_bloom_filter_create(config.ephemeral_registry_size,
                                                   config.ephemeral_registry_hash_count,
                                                   config.ephemeral_registry_omega,
                                                   config.ephemeral_registry_fp_bits);
  }
  if (registry->filter == NULL) {
    log_error("ephemeral_registry_create: filter creation failed");
    free(registry->current_file);
    free(registry->backup_file);
    free(registry->temp_file);
    free(registry);
    return NULL;
  }
  actor_init(&registry->actor, registry, ephemeral_registry_dispatch, pool);
  return registry;
}

void ephemeral_registry_destroy(ephemeral_registry_t* registry) {
  if (registry == NULL) {
    return;
  }
  /* Let any in-flight mutations (and their flushes) drain before tearing the
     actor down — this must be called from outside the registry's own
     dispatch, never inline from it. */
  if (registry->pool != NULL) {
    scheduler_pool_wait_for_idle(registry->pool);
  }
  actor_destroy(&registry->actor);
  elastic_bloom_filter_destroy(registry->filter);
  free(registry->current_file);
  free(registry->backup_file);
  free(registry->temp_file);
  free(registry);
}
