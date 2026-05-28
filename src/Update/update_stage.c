//
// Created by victor on 5/28/25.
//

#include "update_stage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _mkdir_p(const char* path) {
  char cmd[1024];
  #ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", path);
  #else
  snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", path);
  #endif
  return system(cmd);
}

static int _remove_dir(const char* path) {
  char cmd[1024];
  #ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "rmdir /S /Q \"%s\" 2>nul", path);
  #else
  snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
  #endif
  return system(cmd);
}

static int _copy_tree(const char* src, const char* dst) {
  char cmd[2048];
  #ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "xcopy /E /I /Y \"%s\" \"%s\" > nul", src, dst);
  #else
  snprintf(cmd, sizeof(cmd), "cp -r \"%s\" \"%s\" 2>/dev/null || true", src, dst);
  #endif
  return system(cmd);
}

bool update_stage(const char* staging_dir,
                  const char* install_dir,
                  const char* backup_dir) {
  // Remove old backup if exists
  char backup_prev[1024];
  snprintf(backup_prev, sizeof(backup_prev), "%s/previous", backup_dir);
  _remove_dir(backup_prev);

  // Create backup dir
  _mkdir_p(backup_dir);

  // Backup current install
  _copy_tree(install_dir, backup_prev);

  return true;
}
