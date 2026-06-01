//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_DOWNLOAD_H
#define OFFS_UPDATE_DOWNLOAD_H

#include "update_check.h"
#include <stdbool.h>

bool update_download(const update_info_t* info,
                     const char* staging_dir,
                     const char* github_token);

#endif // OFFS_UPDATE_DOWNLOAD_H
