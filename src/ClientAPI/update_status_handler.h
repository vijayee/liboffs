//
// Created by victor on 5/29/25.
//

#ifndef OFFS_UPDATE_STATUS_HANDLER_H
#define OFFS_UPDATE_STATUS_HANDLER_H

#include <stdint.h>

typedef struct update_status_context_t {
  int enabled;
  char channel[32];
  int check_interval_hours;
  char state[32];
  char current_version[64];
  char available_version[64];
} update_status_context_t;

void update_status_context_init(update_status_context_t* ctx);
char* update_status_to_json(const update_status_context_t* ctx);

#endif // OFFS_UPDATE_STATUS_HANDLER_H
