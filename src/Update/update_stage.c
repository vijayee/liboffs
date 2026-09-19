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

static int _copy_file(const char* src, const char* dst) {
  FILE* in = fopen(src, "rb");
  if (in == NULL) {
    log_error("update_stage: failed to open source file %s", src);
    return -1;
  }
  FILE* out = fopen(dst, "wb");
  if (out == NULL) {
    log_error("update_stage: failed to create destination file %s", dst);
    fclose(in);
    return -1;
  }
  char buf[64 * 1024];
  size_t bytes;
  int error = 0;
  while ((bytes = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, bytes, out) != bytes) {
      log_error("update_stage: short write copying %s -> %s", src, dst);
      error = -1;
      break;
    }
  }
  if (!error && ferror(in)) {
    log_error("update_stage: read error copying %s", src);
    error = -1;
  }
  fclose(in);
  if (fclose(out) != 0) {
    log_error("update_stage: close error writing %s", dst);
    error = -1;
  }
  return error;
}

static int _copy_tree(const char* src, const char* dst) {
  if (platform_mkdir(dst) != 0 && !platform_file_exists(dst)) {
    log_error("update_stage: cannot create destination dir %s", dst);
    return -1;
  }
#ifdef _WIN32
  char pattern[OFFS_PATH_MAX];
  snprintf(pattern, sizeof(pattern), "%s\\*", src);
  WIN32_FIND_DATAA fd;
  HANDLE handle = FindFirstFileA(pattern, &fd);
  if (handle == INVALID_HANDLE_VALUE) return 0;
  int error = 0;
  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
    char src_child[OFFS_PATH_MAX];
    snprintf(src_child, sizeof(src_child), "%s\\%s", src, fd.cFileName);
    char dst_child[OFFS_PATH_MAX];
    snprintf(dst_child, sizeof(dst_child), "%s\\%s", dst, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (_copy_tree(src_child, dst_child) != 0) { error = -1; break; }
    } else {
      if (_copy_file(src_child, dst_child) != 0) { error = -1; break; }
    }
  } while (FindNextFileA(handle, &fd));
  FindClose(handle);
  return error;
#else
  DIR* dir = opendir(src);
  if (dir == NULL) {
    log_error("update_stage: cannot open source dir %s", src);
    return -1;
  }
  struct dirent* entry;
  int error = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char src_child[OFFS_PATH_MAX];
    snprintf(src_child, sizeof(src_child), "%s/%s", src, entry->d_name);
    char dst_child[OFFS_PATH_MAX];
    snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, entry->d_name);
    struct stat st;
    if (stat(src_child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (_copy_tree(src_child, dst_child) != 0) { error = -1; break; }
    } else if (S_ISREG(st.st_mode)) {
      if (_copy_file(src_child, dst_child) != 0) { error = -1; break; }
    }
  }
  closedir(dir);
  return error;
#endif
}

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
  if (_copy_tree(install_dir, backup_prev) != 0) {
    log_error("update_stage: backup copy %s -> %s failed", install_dir, backup_prev);
    return false;
  }
  return true;
}