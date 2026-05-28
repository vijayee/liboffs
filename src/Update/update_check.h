//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_CHECK_H
#define OFFS_UPDATE_CHECK_H

#include "../Version/version.h"
#include <stdbool.h>

typedef struct {
  version_t version;
  char tag_name[64];
  char download_url[512];
  char sha256[65];
  bool available;
  bool prerelease;
} update_info_t;

typedef struct {
  char github_repo[128];
  char github_api_url[256];
  char github_token[128];
  update_channel_e channel;
} update_check_config_t;

update_info_t* update_check_fetch(const update_check_config_t* config,
                                  const version_t* current_version);
void update_info_free(update_info_t* info);

#endif // OFFS_UPDATE_CHECK_H
