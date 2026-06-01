//
// Created by victor on 5/29/25.
//

#include "update_status_handler.h"
#include <cJSON.h>
#include <string.h>

void update_status_context_init(update_status_context_t* ctx) {
  memset(ctx, 0, sizeof(*ctx));
}

char* update_status_to_json(const update_status_context_t* ctx) {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddBoolToObject(root, "enabled", ctx->enabled ? 1 : 0);
  cJSON_AddStringToObject(root, "channel", ctx->channel);
  cJSON_AddNumberToObject(root, "check_interval_hours", ctx->check_interval_hours);

  if (ctx->state[0] != '\0') {
    cJSON_AddStringToObject(root, "state", ctx->state);
  } else {
    cJSON_AddStringToObject(root, "state", "idle");
  }

  if (ctx->current_version[0] != '\0') {
    cJSON_AddStringToObject(root, "current_version", ctx->current_version);
  } else {
    cJSON_AddStringToObject(root, "current_version", "0.0.0");
  }

  if (ctx->available_version[0] != '\0') {
    cJSON_AddStringToObject(root, "available_version", ctx->available_version);
  } else {
    cJSON_AddStringToObject(root, "available_version", "none");
  }

  char* json_str = cJSON_Print(root);
  cJSON_Delete(root);
  return json_str;
}
