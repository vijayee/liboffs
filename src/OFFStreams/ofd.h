//
// Created by victor on 5/8/25.
//

#ifndef OFFS_OFD_H
#define OFFS_OFD_H

#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "ori.h"
#include "../Util/vec.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  OFD_ENTRY_FILE,
  OFD_ENTRY_DIRECTORY
} ofd_entry_type_t;

typedef struct {
  char* name;
  ofd_entry_type_t type;
  union {
    ori_t* file_ori;
    buffer_t* dir_hash;
  };
} ofd_entry_t;

typedef vec_t(ofd_entry_t) vec_ofd_entry_t;

typedef struct {
  refcounter_t refcounter;
  vec_ofd_entry_t entries;
  buffer_t* hash;
  uint64_t expires_at;
} ofd_t;

ofd_t* ofd_create(void);
void ofd_destroy(ofd_t* ofd);

void ofd_add_file(ofd_t* ofd, const char* name, ori_t* file_ori);
void ofd_add_directory(ofd_t* ofd, const char* name, buffer_t* dir_hash);
ofd_entry_t* ofd_find(ofd_t* ofd, const char* name);

buffer_t* ofd_encode(ofd_t* ofd);
ofd_t* ofd_decode(buffer_t* data);

#ifdef __cplusplus
}
#endif

#endif //OFFS_OFD_H