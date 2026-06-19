//
// Created by victor on 6/14/25.
//
#include "get_dir.h"
#include "allocator.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dirent.h>
#endif


int sortstring( const void *str1, const void *str2 ){
  char *const *pp1 = str1;
  char *const *pp2 = str2;
  return strcmp(*pp1, *pp2);
}
vec_str_t* get_dir(const char* directory) {
  vec_str_t* files = get_clear_memory(sizeof(vec_char_t));
  vec_init(files);
  vec_reserve(files, 2);

#ifdef _WIN32
  char search_path[MAX_PATH];
  snprintf(search_path, sizeof(search_path), "%s\\*", directory);
  WIN32_FIND_DATAA find_data;
  HANDLE find_handle = FindFirstFileA(search_path, &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    vec_deinit(files);
    free(files);
    return NULL;
  }
  do {
    if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      char* str = strdup(find_data.cFileName);
      vec_push(files, str);
    }
  } while (FindNextFileA(find_handle, &find_data) != 0);
  FindClose(find_handle);
#else
  DIR *dir;
  struct dirent *ent;

  dir = opendir(directory);
  if (dir == NULL) {
    vec_deinit(files);
    free(files);
    return NULL;
  }
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_type != DT_DIR) {
      char *str = strdup(ent->d_name);
      vec_push(files, str);
    }
  }
  closedir(dir);
#endif
  vec_sort(files, sortstring);
  return files;
}
void destroy_files(vec_str_t* files) {
  if (files == NULL) {
    return;
  }
  int i; char* file;
  vec_foreach(files, file, i) {
      free(file);
  }
  vec_deinit(files);
  free(files);
}
