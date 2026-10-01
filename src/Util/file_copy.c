//
// Created by victor on 9/28/26.
//
// See file_copy.h. The copy loop is lifted from Update/update_stage.c;
// the SHA256 streaming from Update/update_download.c — both lifted so the
// update stager and the cache-move command share one implementation.

#include "file_copy.h"
#include "../Platform/platform_file.h"
#include "../Util/log.h"

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

#include <openssl/evp.h>

#define FILE_COPY_CHUNK (64 * 1024)

int file_copy(const char* src, const char* dst) {
  FILE* in = fopen(src, "rb");
  if (in == NULL) {
    log_error("file_copy: failed to open source file %s", src);
    return -1;
  }
  FILE* out = fopen(dst, "wb");
  if (out == NULL) {
    log_error("file_copy: failed to create destination file %s", dst);
    fclose(in);
    return -1;
  }
  char buf[FILE_COPY_CHUNK];
  size_t bytes;
  int error = 0;
  while ((bytes = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, bytes, out) != bytes) {
      log_error("file_copy: short write copying %s -> %s", src, dst);
      error = -1;
      break;
    }
  }
  if (!error && ferror(in)) {
    log_error("file_copy: read error copying %s", src);
    error = -1;
  }
  fclose(in);
  if (fclose(out) != 0) {
    log_error("file_copy: close error writing %s", dst);
    error = -1;
  }
  if (error) {
    platform_file_unlink(dst);
  }
  return error;
}

int copy_tree(const char* src, const char* dst) {
  if (platform_mkdir(dst) != 0 && !platform_file_exists(dst)) {
    log_error("copy_tree: cannot create destination dir %s", dst);
    return -1;
  }
#ifdef _WIN32
  char pattern[OFFS_PATH_MAX];
  snprintf(pattern, sizeof(pattern), "%s\\*", src);
  WIN32_FIND_DATAA fd;
  HANDLE handle = FindFirstFileA(pattern, &fd);
  if (handle == INVALID_HANDLE_VALUE) {
    log_error("copy_tree: cannot open source dir %s", src);
    return -1;
  }
  int error = 0;
  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
    char src_child[OFFS_PATH_MAX];
    snprintf(src_child, sizeof(src_child), "%s\\%s", src, fd.cFileName);
    char dst_child[OFFS_PATH_MAX];
    snprintf(dst_child, sizeof(dst_child), "%s\\%s", dst, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (copy_tree(src_child, dst_child) != 0) { error = -1; break; }
    } else {
      if (file_copy(src_child, dst_child) != 0) { error = -1; break; }
    }
  } while (FindNextFileA(handle, &fd));
  FindClose(handle);
  return error;
#else
  DIR* dir = opendir(src);
  if (dir == NULL) {
    log_error("copy_tree: cannot open source dir %s", src);
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
      if (copy_tree(src_child, dst_child) != 0) { error = -1; break; }
    } else if (S_ISREG(st.st_mode)) {
      if (file_copy(src_child, dst_child) != 0) { error = -1; break; }
    }
  }
  closedir(dir);
  return error;
#endif
}

int dir_is_empty(const char* path) {
#ifdef _WIN32
  char pattern[OFFS_PATH_MAX];
  snprintf(pattern, sizeof(pattern), "%s\\*", path);
  WIN32_FIND_DATAA fd;
  HANDLE handle = FindFirstFileA(pattern, &fd);
  if (handle == INVALID_HANDLE_VALUE) {
    return platform_file_exists(path) ? 1 : -1;
  }
  int empty = 1;
  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
    empty = 0;
    break;
  } while (FindNextFileA(handle, &fd));
  FindClose(handle);
  return empty;
#else
  DIR* dir = opendir(path);
  if (dir == NULL) {
    return platform_file_exists(path) ? 1 : -1;
  }
  int empty = 1;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    empty = 0;
    break;
  }
  closedir(dir);
  return empty;
#endif
}

int file_sha256_hex(const char* path, char out_hex[65]) {
  if (path == NULL || out_hex == NULL) return -1;
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    return -1;
  }

  EVP_MD_CTX* md_context = EVP_MD_CTX_new();
  if (md_context == NULL) {
    fclose(file);
    return -1;
  }

  if (EVP_DigestInit_ex(md_context, EVP_sha256(), NULL) != 1) {
    EVP_MD_CTX_free(md_context);
    fclose(file);
    return -1;
  }

  unsigned char buffer[FILE_COPY_CHUNK];
  size_t bytes_read = 0;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    EVP_DigestUpdate(md_context, buffer, bytes_read);
  }
  if (ferror(file)) {
    /* A silent mid-stream read error would hash a prefix and "succeed". */
    log_error("file_sha256_hex: read error hashing %s", path);
    EVP_MD_CTX_free(md_context);
    fclose(file);
    return -1;
  }
  fclose(file);

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(md_context, digest, &digest_length) != 1) {
    EVP_MD_CTX_free(md_context);
    return -1;
  }
  EVP_MD_CTX_free(md_context);

  if (digest_length != 32) {
    return -1;
  }
  for (unsigned int index = 0; index < digest_length; index++) {
    snprintf(out_hex + (index * 2), 3, "%02x", digest[index]);
  }
  out_hex[64] = '\0';
  return 0;
}

int file_move_verified(const char* src, const char* dst, int verify_mode) {
  if (src == NULL || dst == NULL) return -1;
  if (file_copy(src, dst) != 0) {
    /* file_copy removes its own partial destination; the source is intact. */
    return -1;
  }

  if (verify_mode != FILE_VERIFY_NONE) {
    char src_hash[65];
    char dst_hash[65];
    if (file_sha256_hex(src, src_hash) != 0 ||
        file_sha256_hex(dst, dst_hash) != 0 ||
        strcmp(src_hash, dst_hash) != 0) {
      log_error("file_move_verified: verification failed moving %s -> %s",
                src, dst);
      /* The verified-copy contract: the source is never touched unless the
         copy is proven identical, so only the destination is removed. */
      platform_file_unlink(dst);
      return -1;
    }
  }

  if (platform_file_unlink(src) != 0) {
    log_error("file_move_verified: failed to remove source %s after a "
              "verified copy (destination %s remains)", src, dst);
    return -1;
  }
  return 0;
}