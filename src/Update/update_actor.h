//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_ACTOR_H
#define OFFS_UPDATE_ACTOR_H

#include "../Actor/actor.h"
#include "../Timer/timer_actor.h"
#include "../Scheduler/scheduler.h"
#include "../Version/version.h"
#include "update_check.h"
#include "../ClientAPI/update_status_handler.h"
#include <stdint.h>

typedef enum {
  update_state_idle = 0,
  update_state_checking,
  update_state_downloading,
  update_state_staged,
  update_state_draining,
  update_state_applying
} update_state_e;

typedef struct update_actor_t {
  actor_t actor;
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  update_check_config_t config;
  version_t current_version;
  update_state_e state;
  char staging_dir[512];
  char install_dir[512];
  char backup_dir[512];
  update_info_t* pending_update;
  ATOMIC(uint64_t) check_timer_id;
  uint64_t drain_start_ms;
  uint64_t drain_timeout_ms;
  uint8_t* draining_flag;
  ATOMIC(uint32_t)* open_stream_count;
  update_status_context_t* status_ctx;
} update_actor_t;

update_actor_t* update_actor_create(scheduler_pool_t* pool,
                                    timer_actor_t* timer,
                                    const update_check_config_t* config,
                                    const char* staging_dir,
                                    const char* install_dir,
                                    const char* backup_dir,
                                    uint8_t* draining_flag,
                                    ATOMIC(uint32_t)* open_stream_count,
                                    update_status_context_t* status_ctx);
void update_actor_destroy(update_actor_t* actor);
void update_actor_check_now(update_actor_t* actor);
update_state_e update_actor_get_state(update_actor_t* actor);
const version_t* update_actor_get_pending_version(update_actor_t* actor);

#endif // OFFS_UPDATE_ACTOR_H
