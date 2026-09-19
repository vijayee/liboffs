#include "path_join.h"


//
// path-join.c
//
// Copyright (c) 2013 Stephen Mathieson
// MIT licensed
//

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#define PATH_JOIN_SEPERATOR   "\\"
#else
#define PATH_JOIN_SEPERATOR   "/"
#endif

/*
 * Join `dir` with `file`
 */
int str_ends_with(const char *str, const char *end);
int str_starts_with(const char *str, const char *start);

char* path_join(const char *dir, const char *file) {
  if (dir == NULL || file == NULL) return NULL;
  size_t dir_len = strlen(dir);
  size_t file_len = strlen(file);
  /* Skip a leading separator in file if present so we don't double up when
     dir already ends with one. */
  const char* file_part = file;
  if (str_starts_with(file, PATH_JOIN_SEPERATOR)) {
    file_part = file + strlen(PATH_JOIN_SEPERATOR);
    file_len = strlen(file_part);
  }
  int need_sep = !str_ends_with(dir, PATH_JOIN_SEPERATOR);
  size_t size = dir_len + (need_sep ? strlen(PATH_JOIN_SEPERATOR) : 0) + file_len + 1;
  char *buf = malloc(size);
  if (NULL == buf) return NULL;

  /* Build with snprintf so the copy is bounded by the allocated size even if
     the length computation above is wrong. The original used strcpy/strcat
     with no overflow check. */
  if (need_sep) {
    snprintf(buf, size, "%s%s%s", dir, PATH_JOIN_SEPERATOR, file_part);
  } else {
    snprintf(buf, size, "%s%s", dir, file_part);
  }
  return buf;
}

int str_ends_with(const char *str, const char *end) {
  int end_len;
  int str_len;

  if (NULL == str || NULL == end) {
    return 0;
  }

  end_len = strlen(end);
  str_len = strlen(str);

  return str_len < end_len
         ? 0
         : !strcmp(str + str_len - end_len, end);
}

int str_starts_with(const char *str, const char *start) {
  for (; ; str++, start++)
    if (!*start) {
      return 1;
    } else if (*str != *start) {
      return 0;
    }
}