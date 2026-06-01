//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_STAGE_H
#define OFFS_UPDATE_STAGE_H

#include <stdbool.h>

bool update_stage(const char* staging_dir,
                  const char* install_dir,
                  const char* backup_dir);

#endif // OFFS_UPDATE_STAGE_H
