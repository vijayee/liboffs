//
// Created by victor on 5/28/25.
//

#include "update_actor.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "update_download.h"
#include "update_stage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum {
  UPDATE_MSG_CHECK = 1,
  UPDATE_MSG_DRAIN_TICK = 2,
  UPDATE_MSG_APPLY = 3
} update_msg_e;

static const char* _state_to_string(update_state_e state) {
  switch (state) {
    case update_state_idle:        return "idle";
    case update_state_checking:    return "checking";
    case update_state_downloading: return "downloading";
    case update_state_staged:      return "staged";
    case update_state_draining:    return "draining";
    case update_state_applying:    return "applying";
    default:                       return "unknown";
  }
}

static void _update_status_ctx(update_actor_t* ua) {
  if (ua->status_ctx == NULL) return;
  snprintf(ua->status_ctx->state, sizeof(ua->status_ctx->state),
           "%s", _state_to_string(ua->state));
  version_to_string(&ua->current_version, ua->status_ctx->current_version,
                    sizeof(ua->status_ctx->current_version));
  if (ua->pending_update != NULL) {
    version_to_string(&ua->pending_update->version,
                      ua->status_ctx->available_version,
                      sizeof(ua->status_ctx->available_version));
  } else {
    ua->status_ctx->available_version[0] = '\0';
  }
}

static void _start_check(update_actor_t* ua);
static void _drain_tick(update_actor_t* ua);
static void _apply_update(update_actor_t* ua);

static void _dispatch(void* state, message_t* msg) {
  update_actor_t* ua = (update_actor_t*)state;
  switch (msg->type) {
    case UPDATE_MSG_CHECK:
      _start_check(ua);
      break;
    case UPDATE_MSG_DRAIN_TICK:
      _drain_tick(ua);
      break;
    case UPDATE_MSG_APPLY:
      _apply_update(ua);
      break;
  }
}

static void _send_msg(update_actor_t* ua, uint32_t type) {
  message_t msg;
  msg.type = type;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&ua->actor, &msg);
}

static void _start_check(update_actor_t* ua) {
  update_info_t* info;

  ua->state = update_state_checking;
  _update_status_ctx(ua);
  info = update_check_fetch(&ua->config, &ua->current_version);
  if (info == NULL) {
    ua->state = update_state_idle;
    _update_status_ctx(ua);
    return;
  }
  if (!info->available) {
    update_info_free(info);
    ua->state = update_state_idle;
    _update_status_ctx(ua);
    return;
  }

  ua->state = update_state_downloading;
  _update_status_ctx(ua);
  ua->pending_update = info;

  if (!update_download(info, ua->staging_dir, ua->config.github_token)) {
    update_info_free(info);
    ua->pending_update = NULL;
    ua->state = update_state_idle;
    _update_status_ctx(ua);
    return;
  }

  if (!update_stage(ua->staging_dir, ua->install_dir, ua->backup_dir)) {
    update_info_free(info);
    ua->pending_update = NULL;
    ua->state = update_state_idle;
    _update_status_ctx(ua);
    return;
  }

  ua->state = update_state_staged;
  _update_status_ctx(ua);

  /* Begin draining */
  {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ua->drain_start_ms = (uint64_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
  }
  *ua->draining_flag = 1;
  ua->state = update_state_draining;
  _update_status_ctx(ua);

  timer_actor_set(ua->timer, 1000, 0, &ua->actor, UPDATE_MSG_DRAIN_TICK, NULL);
}

static void _drain_tick(update_actor_t* ua) {
  uint32_t open_count;
  struct timespec now;
  uint64_t now_ms;

  /* Check if all streams are closed */
  open_count = ua->open_stream_count ? ATOMIC_LOAD(ua->open_stream_count) : 0;
  if (open_count == 0) {
    _send_msg(ua, UPDATE_MSG_APPLY);
    return;
  }

  /* Check drain timeout */
  clock_gettime(CLOCK_MONOTONIC, &now);
  now_ms = (uint64_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
  if (now_ms - ua->drain_start_ms > ua->drain_timeout_ms) {
    _send_msg(ua, UPDATE_MSG_APPLY);
    return;
  }

  /* Still draining — reschedule */
  timer_actor_set(ua->timer, 1000, 0, &ua->actor, UPDATE_MSG_DRAIN_TICK, NULL);
}

static void _apply_update(update_actor_t* ua) {
  ua->state = update_state_applying;
  _update_status_ctx(ua);
  pid_t pid = fork();
  if (pid == 0) {
    /* Child: replace self with updater binary */
    execlp("offs-updater", "offs-updater",
           ua->staging_dir, ua->install_dir, ua->backup_dir, NULL);
    _exit(127);
  } else if (pid > 0) {
    /* Parent: terminate cleanly to let child take over */
    exit(0);
  }
}

update_actor_t* update_actor_create(scheduler_pool_t* pool,
                                    timer_actor_t* timer,
                                    const update_check_config_t* config,
                                    const char* staging_dir,
                                    const char* install_dir,
                                    const char* backup_dir,
                                    uint8_t* draining_flag,
                                    ATOMIC(uint32_t)* open_stream_count,
                                    update_status_context_t* status_ctx) {
  update_actor_t* ua = get_clear_memory(sizeof(update_actor_t));
  actor_init(&ua->actor, ua, _dispatch, pool);
  ua->pool = pool;
  ua->timer = timer;
  ua->config = *config;
  version_parse(OFFS_VERSION, &ua->current_version);
  ua->state = update_state_idle;
  ua->draining_flag = draining_flag;
  ua->open_stream_count = open_stream_count;
  ua->status_ctx = status_ctx;
  ua->drain_timeout_ms = 300000;
  snprintf(ua->staging_dir, sizeof(ua->staging_dir), "%s", staging_dir);
  snprintf(ua->install_dir, sizeof(ua->install_dir), "%s", install_dir);
  snprintf(ua->backup_dir, sizeof(ua->backup_dir), "%s", backup_dir);

  ua->check_timer_id = 0;
  timer_actor_set(timer, 5000, 6 * 60 * 60 * 1000,
                  &ua->actor, UPDATE_MSG_CHECK, &ua->check_timer_id);
  return ua;
}

void update_actor_destroy(update_actor_t* ua) {
  timer_actor_cancel(ua->timer, atomic_load(&ua->check_timer_id));
  if (ua->pending_update != NULL) {
    update_info_free(ua->pending_update);
    ua->pending_update = NULL;
  }
  actor_destroy(&ua->actor);
  free(ua);
}

void update_actor_check_now(update_actor_t* ua) {
  _send_msg(ua, UPDATE_MSG_CHECK);
}

update_state_e update_actor_get_state(update_actor_t* ua) {
  return ua->state;
}

const version_t* update_actor_get_pending_version(update_actor_t* ua) {
  if (ua->pending_update != NULL) {
    return &ua->pending_update->version;
  }
  return NULL;
}
