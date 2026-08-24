#include "update_extract.h"

#include "../Util/log.h"
#include "../Util/mkdir_p.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <windows.h>
  #define OFFS_EXTRACT_PATH_MAX 4096
#else
  #include <limits.h>
  #include <unistd.h>
  #define OFFS_EXTRACT_PATH_MAX 4096
#endif

/* USTAR (POSIX.1-1988) header field offsets and lengths, in bytes from the
   start of the 512-byte block. */
#define USTAR_BLOCK_SIZE     512
#define USTAR_NAME_OFFSET    0
#define USTAR_NAME_LEN       100
#define USTAR_SIZE_OFFSET    124
#define USTAR_SIZE_LEN       12
#define USTAR_TYPEFLAG_OFFSET 156
#define USTAR_LINKNAME_OFFSET 157
#define USTAR_LINKNAME_LEN   100
#define USTAR_MAGIC_OFFSET   257
#define USTAR_PREFIX_OFFSET  345
#define USTAR_PREFIX_LEN     155

/* True if the 512-byte block is entirely NUL — used to detect the
   end-of-archive marker (one or two zero blocks). */
static int _is_zero_block(const uint8_t* block) {
  for (size_t index = 0; index < USTAR_BLOCK_SIZE; index++) {
    if (block[index] != 0) {
      return 0;
    }
  }
  return 1;
}

/* Parse a USTAR numeric field (octal ASCII, possibly with trailing space or
   NUL). Stops at the first non-octal digit. */
static uint64_t _parse_octal(const uint8_t* field, size_t len) {
  uint64_t value = 0;
  for (size_t index = 0; index < len; index++) {
    char digit = (char)field[index];
    if (digit < '0' || digit > '7') {
      break;
    }
    value = (value << 3) | (uint64_t)(digit - '0');
  }
  return value;
}

/* Bounded strlen — like strnlen, but portable to MSVC without extra flags. */
static size_t _bounded_strlen(const char* str, size_t max) {
  for (size_t index = 0; index < max; index++) {
    if (str[index] == '\0') {
      return index;
    }
  }
  return max;
}

/* True if any path component of `path` is exactly "..". The path is split on
   '/'. Leading and consecutive slashes are skipped. */
static int _path_has_dotdot(const char* path) {
  const char* cursor = path;
  while (*cursor != '\0') {
    while (*cursor == '/') {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }
    const char* segment_start = cursor;
    while (*cursor != '\0' && *cursor != '/') {
      cursor++;
    }
    size_t segment_len = (size_t)(cursor - segment_start);
    if (segment_len == 2 &&
        segment_start[0] == '.' &&
        segment_start[1] == '.') {
      return 1;
    }
  }
  return 0;
}

/* Join dir + name with a single '/' separator into `out` (bounded by
   out_size). Returns 0 on success, -1 if the result would overflow. */
static int _join_path(char* out, size_t out_size,
                      const char* dir, const char* name) {
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  int need_sep = (dir_len > 0 && dir[dir_len - 1] != '/');
  if (dir_len + (need_sep ? 1u : 0u) + name_len + 1u > out_size) {
    return -1;
  }
  memcpy(out, dir, dir_len);
  size_t position = dir_len;
  if (need_sep) {
    out[position++] = '/';
  }
  memcpy(out + position, name, name_len);
  position += name_len;
  out[position] = '\0';
  return 0;
}

int update_extract(const char* archive_path, const char* dest_dir, uint64_t max_bytes) {
  if (archive_path == NULL || dest_dir == NULL) {
    return -1;
  }

  FILE* archive = fopen(archive_path, "rb");
  if (archive == NULL) {
    log_error("update_extract: cannot open archive %s: %s",
              archive_path, strerror(errno));
    return -1;
  }

  /* Canonicalize dest_dir once. On POSIX, realpath resolves symlinks so the
     per-file parent containment check below can compare a canonical prefix.
     On Windows, realpath is unavailable; rely on the string-level checks
     (no absolute path, no ".." component) plus the fact that the staging
     directory is created fresh by the updater. Windows doesn't follow
     symlinks the same way and Stage 3 only supports regular files + dirs. */
  char dest_real[OFFS_EXTRACT_PATH_MAX];
#ifndef _WIN32
  if (realpath(dest_dir, dest_real) == NULL) {
    log_error("update_extract: cannot canonicalize dest_dir %s: %s",
              dest_dir, strerror(errno));
    fclose(archive);
    return -1;
  }
#else
  if (strlen(dest_dir) >= sizeof(dest_real)) {
    log_error("update_extract: dest_dir too long");
    fclose(archive);
    return -1;
  }
  strncpy(dest_real, dest_dir, sizeof(dest_real) - 1);
  dest_real[sizeof(dest_real) - 1] = '\0';
#endif

  uint64_t total_extracted = 0;
  uint8_t header[USTAR_BLOCK_SIZE];

  while (1) {
    size_t got = fread(header, 1, USTAR_BLOCK_SIZE, archive);
    if (got == 0 && feof(archive)) {
      /* Clean EOF — some tarballs omit the trailing zero blocks. */
      break;
    }
    if (got != USTAR_BLOCK_SIZE) {
      log_error("update_extract: short read on header (%zu bytes)", got);
      fclose(archive);
      return -1;
    }

    if (_is_zero_block(header)) {
      /* End-of-archive marker. */
      break;
    }

    /* Parse name[100] + prefix[155]. USTAR long paths are stored as
       prefix + "/" + name. */
    char name[USTAR_NAME_LEN + 1];
    char prefix[USTAR_PREFIX_LEN + 1];
    memcpy(name, header + USTAR_NAME_OFFSET, USTAR_NAME_LEN);
    name[USTAR_NAME_LEN] = '\0';
    name[_bounded_strlen(name, USTAR_NAME_LEN)] = '\0';

    memcpy(prefix, header + USTAR_PREFIX_OFFSET, USTAR_PREFIX_LEN);
    prefix[USTAR_PREFIX_LEN] = '\0';
    prefix[_bounded_strlen(prefix, USTAR_PREFIX_LEN)] = '\0';

    char full_name[USTAR_PREFIX_LEN + 1 + USTAR_NAME_LEN + 1];
    if (prefix[0] != '\0') {
      snprintf(full_name, sizeof(full_name), "%s/%s", prefix, name);
    } else {
      snprintf(full_name, sizeof(full_name), "%s", name);
    }

    /* Containment pre-check (string level): reject absolute paths and any
       ".." component. This runs on every platform. */
    if (full_name[0] == '/') {
      log_error("update_extract: rejecting absolute path %s", full_name);
      fclose(archive);
      return -1;
    }
    if (_path_has_dotdot(full_name)) {
      log_error("update_extract: rejecting path with .. component: %s", full_name);
      fclose(archive);
      return -1;
    }

    /* Parse typeflag. Stage 3 supports regular files ('0' or NUL) and
       directories ('5'). Reject everything else — symlinks ('2'), hardlinks
       ('1'), char/block devices ('3'/'4'), FIFOs ('6'), and unknown types. */
    char typeflag = (char)header[USTAR_TYPEFLAG_OFFSET];
    if (typeflag != '0' && typeflag != '\0' && typeflag != '5') {
      log_error("update_extract: rejecting entry type '%c' (0x%02x) for %s",
                (typeflag >= 32 && typeflag < 127) ? typeflag : '?',
                (unsigned char)typeflag,
                full_name);
      fclose(archive);
      return -1;
    }

    /* Parse size (octal, 12 bytes). */
    uint64_t entry_size = _parse_octal(header + USTAR_SIZE_OFFSET, USTAR_SIZE_LEN);

    /* Total-size cap — applies to regular-file content only (directories
       contribute no data blocks). */
    if (typeflag == '0' || typeflag == '\0') {
      if (total_extracted > max_bytes ||
          entry_size > (max_bytes - total_extracted)) {
        log_error("update_extract: archive exceeds max_bytes (%llu + %llu > %llu)",
                  (unsigned long long)total_extracted,
                  (unsigned long long)entry_size,
                  (unsigned long long)max_bytes);
        fclose(archive);
        return -1;
      }
      total_extracted += entry_size;
    }

    /* Build dest_dir + "/" + full_name. */
    char dest_path[OFFS_EXTRACT_PATH_MAX];
    if (_join_path(dest_path, sizeof(dest_path), dest_dir, full_name) != 0) {
      log_error("update_extract: destination path too long: %s/%s", dest_dir, full_name);
      fclose(archive);
      return -1;
    }

    if (typeflag == '5') {
      /* Directory entry. mkdir_p is recursive and a no-op if the dir exists. */
      if (mkdir_p(dest_path) != 0) {
        log_error("update_extract: mkdir_p failed for %s: %s",
                  dest_path, strerror(errno));
        fclose(archive);
        return -1;
      }
      continue;
    }

    /* Regular file. Resolve and create the parent directory, then verify
       (POSIX) that the canonical parent is inside dest_real. */
    char parent_path[OFFS_EXTRACT_PATH_MAX];
    if (_join_path(parent_path, sizeof(parent_path), dest_dir, full_name) != 0) {
      log_error("update_extract: parent path too long");
      fclose(archive);
      return -1;
    }
    char* last_sep = strrchr(parent_path, '/');
    if (last_sep == NULL) {
      /* No separator means full_name had no subdirectory — parent is dest_dir. */
      if (_join_path(parent_path, sizeof(parent_path), dest_dir, "") != 0) {
        fclose(archive);
        return -1;
      }
    } else {
      *last_sep = '\0';
      if (parent_path[0] == '\0') {
        /* Should not happen: full_name is relative, so dest_dir is always the
           root of the join. Guard defensively. */
        log_error("update_extract: resolved to root — rejecting %s", full_name);
        fclose(archive);
        return -1;
      }
    }

    if (mkdir_p(parent_path) != 0) {
      log_error("update_extract: mkdir_p failed for parent %s: %s",
                parent_path, strerror(errno));
      fclose(archive);
      return -1;
    }

#ifndef _WIN32
    /* Defense-in-depth: canonicalize the parent and confirm it is under
       dest_real. This catches pre-existing symlinks in dest_dir that the
       string-level check above cannot see. */
    char parent_real[PATH_MAX];
    if (realpath(parent_path, parent_real) == NULL) {
      log_error("update_extract: cannot canonicalize parent %s: %s",
                parent_path, strerror(errno));
      fclose(archive);
      return -1;
    }
    size_t dest_real_len = strlen(dest_real);
    int under_dest =
        (strncmp(parent_real, dest_real, dest_real_len) == 0) &&
        (parent_real[dest_real_len] == '/' || parent_real[dest_real_len] == '\0');
    if (!under_dest) {
      log_error("update_extract: parent %s (real %s) escapes dest %s",
                parent_path, parent_real, dest_real);
      fclose(archive);
      return -1;
    }
#endif

    /* Open the output file and stream `entry_size` bytes from the archive
       in 512-byte blocks, discarding the NUL padding. */
    FILE* out_file = fopen(dest_path, "wb");
    if (out_file == NULL) {
      log_error("update_extract: cannot open %s for writing: %s",
                dest_path, strerror(errno));
      fclose(archive);
      return -1;
    }

    uint64_t remaining = entry_size;
    uint8_t data_block[USTAR_BLOCK_SIZE];
    int write_failed = 0;
    while (remaining > 0) {
      if (fread(data_block, 1, USTAR_BLOCK_SIZE, archive) != USTAR_BLOCK_SIZE) {
        log_error("update_extract: short read on data for %s", full_name);
        write_failed = 1;
        break;
      }
      size_t to_write = (remaining < (uint64_t)USTAR_BLOCK_SIZE)
                            ? (size_t)remaining
                            : USTAR_BLOCK_SIZE;
      if (fwrite(data_block, 1, to_write, out_file) != to_write) {
        log_error("update_extract: write failed for %s: %s",
                  dest_path, strerror(errno));
        write_failed = 1;
        break;
      }
      remaining -= to_write;
    }
    fclose(out_file);

    if (write_failed) {
      /* Remove the partial file so a retry doesn't see stale content. */
      unlink(dest_path);
      fclose(archive);
      return -1;
    }
  }

  fclose(archive);
  return 0;
}
