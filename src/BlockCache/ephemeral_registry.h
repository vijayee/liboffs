//
// Ephemeral registry — actor-owned persistent elastic bloom filter of
// ephemeral representation descriptor hashes.
//

#ifndef OFFS_EPHEMERAL_REGISTRY_H
#define OFFS_EPHEMERAL_REGISTRY_H

#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Bloom/elastic_bloom_filter.h"
#include "../Configuration/config.h"
#include "../Scheduler/scheduler.h"
#include <stdint.h>

/* Actor-owned registry of ephemeral representation descriptor hashes, backed
   by a persistent elastic bloom filter (native add/remove — no rebuild).
   Advisory in both directions: the recycler's fetch-time index check is exact;
   a filter miss self-heals via ADD when ephemeral blocks are discovered.
   Persistence: two-file rotation (current + .last backup) flushed on every
   mutation; load falls back to the backup, then to a fresh empty filter. */
typedef struct ephemeral_registry_t {
  actor_t actor;
  scheduler_pool_t* pool;
  elastic_bloom_filter_t* filter;
  char* current_file;
  char* backup_file;
  char* temp_file;
} ephemeral_registry_t;

ephemeral_registry_t* ephemeral_registry_create(const char* location, config_t config, scheduler_pool_t* pool);
void ephemeral_registry_destroy(ephemeral_registry_t* registry);
void ephemeral_registry_dispatch(void* state, message_t* msg);

void ephemeral_registry_add(ephemeral_registry_t* registry, buffer_t* descriptor_hash);
void ephemeral_registry_remove(ephemeral_registry_t* registry, buffer_t* descriptor_hash);
void ephemeral_registry_check(ephemeral_registry_t* registry, buffer_t* descriptor_hash, actor_t* reply_to);

#endif // OFFS_EPHEMERAL_REGISTRY_H
