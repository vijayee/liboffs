//
// Created by victor on 5/7/26.
//

#include "ori.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include <string.h>

ori_t* ori_create(size_t final_byte) {
  ori_t* ori = get_clear_memory(sizeof(ori_t));
  refcounter_init((refcounter_t*)ori);
  ori->final_byte = final_byte;
  ori->descriptor_offset = 0;
  ori->block_type = standard;
  ori->tuple_size = 0;
  ori->file_offset = 0;
  return ori;
}

void ori_destroy(ori_t* ori) {
  if (ori == NULL) {
    return;
  }
  refcounter_dereference((refcounter_t*)ori);
  if (refcounter_count((refcounter_t*)ori) == 0) {
    if (ori->descriptor_hash != NULL) {
      DESTROY(ori->descriptor_hash, buffer);
    }
    if (ori->file_hash != NULL) {
      DESTROY(ori->file_hash, buffer);
    }
    if (ori->file_name != NULL) {
      free(ori->file_name);
    }
    free(ori);
  }
}