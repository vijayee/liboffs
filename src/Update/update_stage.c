//
// Created by victor on 5/28/25.
//
// Stages an update: backs up the current install directory in-process,
// without invoking a shell. The previous implementation built shell commands
// via snprintf and called system(); a staging/install path containing '"'
// or '$()' could escape the quoting. Paths here come from local config and
// the GitHub release API, so removing the shell closes that injection
// vector regardless of path content.

#include "update_stage.h"
#include "../Platform/platform_file.h"
#include "../Util/file_copy.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/rm_rf.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #define OFFS_PATH_MAX MAX_PATH
#else
  #include <dirent.h>
  #include <limits.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #define OFFS_PATH_MAX 4096
#endif

bool update_stage(const char* staging_dir,
                  const char* install_dir,
                  const char* backup_dir) {
  (void)staging_dir;
  char backup_prev[OFFS_PATH_MAX];
  snprintf(backup_prev, sizeof(backup_prev), "%s/previous", backup_dir);

  if (rm_rf(backup_prev) != 0) {
    log_warn("update_stage: could not remove previous backup %s (continuing)", backup_prev);
  }
  if (mkdir_p(backup_prev) != 0) {
    log_error("update_stage: cannot create backup dir %s", backup_prev);
    return false;
  }
  /* If the install dir doesn't exist yet (fresh install, nothing to back up),
     skip the copy — this is success, not failure. The old shell path used
     "cp -r ... || true" which silently treated a missing source as success. */
  if (!platform_file_exists(install_dir)) {
    log_info("update_stage: install dir %s does not exist — nothing to back up", install_dir);
    return true;
  }
  if (copy_tree(install_dir, backup_prev) != 0) {
    log_error("update_stage: backup copy %s -> %s failed", install_dir, backup_prev);
    return false;
  }
  return true;
}