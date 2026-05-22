#define _DEFAULT_SOURCE

#include "platform_file.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <string.h>
  #include <errno.h>

  extern int mkdir_p(char* path);

  struct platform_file_t {
    int fd;
  };

  platform_file_t* platform_file_open(const char* path, int flags, int mode) {
    int fd = open(path, flags, mode);
    if (fd < 0) {
      return NULL;
    }
    platform_file_t* file = (platform_file_t*)calloc(1, sizeof(platform_file_t));
    if (file == NULL) {
      close(fd);
      return NULL;
    }
    file->fd = fd;
    return file;
  }

  void platform_file_close(platform_file_t* file) {
    if (file == NULL) {
      return;
    }
    close(file->fd);
    free(file);
  }

  ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count) {
    return read(file->fd, buf, count);
  }

  ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count) {
    return write(file->fd, buf, count);
  }

  ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset) {
    return pread(file->fd, buf, count, (off_t)offset);
  }

  ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset) {
    return pwrite(file->fd, buf, count, (off_t)offset);
  }

  int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence) {
    int w = (whence == PLATFORM_SEEK_SET) ? SEEK_SET :
            (whence == PLATFORM_SEEK_CUR) ? SEEK_CUR : SEEK_END;
    off_t result = lseek(file->fd, (off_t)offset, w);
    return (int64_t)result;
  }

  int platform_file_exists(const char* path) {
    return access(path, F_OK) == 0 ? 1 : 0;
  }

  int platform_file_unlink(const char* path) {
    return unlink(path);
  }

  int platform_mkdir(const char* path) {
    return mkdir_p((char*)path);
  }
#else
  /* Windows implementation deferred */
#endif
