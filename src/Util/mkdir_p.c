#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <sys/stat.h>
  #include <direct.h>
  /* MSVC's <sys/stat.h> does not define S_ISDIR; provide a local definition. */
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
  #endif
#else
  #include <libgen.h>
  #include <sys/stat.h>
#endif

#ifndef _WIN32
static char* _offs_dirname(char* path) {
  return dirname(path);
}
#endif

int mkdir_p(char* path) {
  int len = strlen(path);
  if (len == 0) {
    return -1;
  }
#ifdef _WIN32
  /* Accept either slash form on Windows. */
  if (len == 1 && (path[0] == '.' || path[0] == '/' || path[0] == '\\')) {
    return 0;
  }
#else
  if (len == 1 && (path[0] == '.' || path[0] == '/')) {
    return 0;
  }
#endif

  int ret = 0;
  for (int i = 0; i < 2; ++i) {
#ifdef _WIN32
    ret = _mkdir(path);
#else
    ret = mkdir(path, S_IRWXU);
#endif
    if (ret == 0) {
      return 0;
    } else if (errno == EEXIST) {
      struct stat st;
      ret = stat(path, &st);
      if (ret != 0) {
        return ret;
      } else if (S_ISDIR(st.st_mode)) {
        return 0;
      } else {
        return -1;
      }
    } else if (errno != ENOENT) {
      return -1;
    }
    char* copy = strdup(path);
#ifdef _WIN32
    /* Strip trailing slashes so the parent directory is not just separators. */
    size_t copy_len = strlen(copy);
    while (copy_len > 1 && (copy[copy_len - 1] == '/' || copy[copy_len - 1] == '\\')) {
      copy[--copy_len] = '\0';
    }
    char* last_sep = strrchr(copy, '\\');
    if (last_sep == NULL) {
      last_sep = strrchr(copy, '/');
    }
    if (last_sep == NULL) {
      free(copy);
      return -1;
    }
    *last_sep = '\0';
    ret = mkdir_p(copy);
#else
    ret = mkdir_p(_offs_dirname(copy));
#endif
    free(copy);
    if (ret != 0) {
      return ret;
    }
  }
  return ret;
}
