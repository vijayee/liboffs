//
// Created by victor on 7/30/25.
//
#include <stdio.h>
#include <string.h>
#include "../Platform/platform.h"
#include "allocator.h"
#include "log.h"
#include "rm_rf.h"

#ifdef _WIN32
  /* WIN32_LEAN_AND_MEAN is set globally by the MSVC CMake configuration. */
  #include <windows.h>
  #include <io.h>
  #include <sys/stat.h>
  #include <direct.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif


int rm_rf(const char *path) {
#ifdef _WIN32
  /* MAX_PATH is the legacy 260-char limit. liboffs only uses paths
   * constructed via path_join from short segments (off-system tempdir,
   * block hashes, log names), so the bound is comfortable. Use a
   * generous heap buffer to be safe with deep nesting. */
  size_t path_len_full = strlen(path);
  if (path_len_full + 2 > MAX_PATH) {
    log_error("rm_rf: path too long for Windows legacy APIs (%zu > %d)",
              path_len_full, MAX_PATH);
    return -1;
  }
  char search_path[MAX_PATH];
  int path_len = (int)path_len_full;
  int r = 0;

  snprintf(search_path, sizeof(search_path), "%s\\*", path);
  WIN32_FIND_DATAA find_data;
  HANDLE find_handle = FindFirstFileA(search_path, &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    /* Either the path does not exist or it is a file (not a directory). */
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
      if (platform_file_unlink(path) == -1) {
        log_error("failed to remove file");
        return -1;
      }
      return 0;
    }
    log_error("failed to open directory");
    return -1;
  }

  do {
    if (!strcmp(find_data.cFileName, ".") || !strcmp(find_data.cFileName, "..")) {
      continue;
    }
    char* buf = get_memory(path_len + (int)strlen(find_data.cFileName) + 2);
    if (!buf) {
      log_error("failed to allocate buffer");
      r = -1;
      break;
    }
    sprintf(buf, "%s\\%s", path, find_data.cFileName);

    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (rm_rf(buf) == -1) {
        r = -1;
        free(buf);
        break;
      }
    } else {
      if (platform_file_unlink(buf) == -1) {
        log_error("failed to unlink");
        r = -1;
        free(buf);
        break;
      }
    }
    free(buf);
  } while (FindNextFileA(find_handle, &find_data) != 0);
  FindClose(find_handle);

  if (r == 0 && _rmdir(path) == -1) {
    log_error("failed to remove directory");
    r = -1;
  }
  return r;
#else
  DIR *d = opendir(path);
  size_t path_len = strlen(path);
  int r = -1; // Return value, -1 for error, 0 for success

  if (!d) {
    log_error("failed to open directory");
    return -1;
  }

  struct dirent *p;
  r = 0; // Assume success initially

  while ((p = readdir(d)) != NULL) {
    // Skip "." and ".."
    if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
      continue;
    }

    char *buf = get_memory(path_len + strlen(p->d_name) + 2); // +2 for '/' and null terminator
    if (!buf) {
      log_error("failed to allocate buffer");
      r = -1;
      break;
    }
    sprintf(buf, "%s/%s", path, p->d_name);

    struct stat statbuf;
    if (lstat(buf, &statbuf) == -1) {
      log_error("failed to stat");
      free(buf);
      r = -1;
      break;
    }

    if (S_ISLNK(statbuf.st_mode)) {
      // Remove symlink — do not follow it
      if (platform_file_unlink(buf) == -1) {
        log_error("failed to unlink symlink");
        free(buf);
        r = -1;
        break;
      }
    } else if (S_ISDIR(statbuf.st_mode)) {
      // Recursively remove subdirectory
      if (rm_rf(buf) == -1) {
        r = -1;
        free(buf);
        break;
      }
    } else {
      // Remove file
      if (platform_file_unlink(buf) == -1) {
        log_error("failed to unlink");
        free(buf);
        r = -1;
        break;
      }
    }
    free(buf);
  }

  if (closedir(d) == -1) {
    log_error("failed to close directory");
    r = -1;
  }

  // Remove the now-empty directory
  if (r == 0 && rmdir(path) == -1) {
    log_error("failed to remove directory");
    r = -1;
  }

  return r;
#endif
}
