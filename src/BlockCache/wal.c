#include "wal.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include <stdio.h>
#include <xxh3.h>
#include <arpa/inet.h>
#include "../Util/get_dir.h"
#include "../Util/vec.h"
#include <stdlib.h>
#include <stdio.h>



wal_t* wal_create(char* location) {
  wal_t* wal = get_clear_memory(sizeof(wal_t));
  wal->location = path_join(location, "wal");

  mkdir_p(wal->location);
  vec_str_t* files = get_dir(wal->location);

  if (files->length > 0) {
    char id[20];
    char* last = vec_last(files);
    uint64_t last_id = atol(last);
    last_id++;
    sprintf(id,"%lu", last_id);
    wal->current_file = path_join(wal->location, id);
    wal->last_file = path_join(wal->location, last);
    wal->next_id = last_id + 1;
  } else {
    char id[20];
    sprintf(id,"%lu", (uint64_t)1);
    wal->next_id = 2;
    wal->current_file = path_join(wal->location, id);
    wal->last_file = NULL;
  }

  wal->log = fopen(wal->current_file, "wb+");
  destroy_files(files);
  return wal;
}

wal_t* wal_create_next(char* location, uint64_t next_id, char* last_file) {
  wal_t* wal = get_clear_memory(sizeof(wal_t));
  wal->location = path_join(location, "wal");
  char id[20];

  sprintf(id,"%lu", next_id);
  wal->next_id = next_id + 1;
  wal->current_file = path_join(wal->location, id);
  if (last_file != NULL) {
    wal->last_file = strdup(last_file);
  } else {
    wal->last_file = NULL;
  }
  wal->log = fopen(wal->current_file, "wb+");
  return wal;
}

void wal_write(wal_t* wal,wal_type_e type, buffer_t* data) {
  uint32_t crc =  htonl(XXH32(data->data,data->size, 0));
  fwrite(&type, 1, 1,wal->log);
  fwrite(&crc, 4, 1, wal->log);
  fwrite(data->data, 1, data->size, wal->log);
  fflush(wal->log);
}

void wal_destroy(wal_t* wal) {
  fclose(wal->log);
  free(wal->current_file);
  if (wal->last_file != NULL) {
    free(wal->last_file);
  }
  free(wal->location);
  free(wal);
}