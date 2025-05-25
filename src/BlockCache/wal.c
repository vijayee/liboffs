#include "wal.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include <stdio.h>
#include <xxhash.h>
#include <arpa/inet.h>

wal_t* wal_create(char* location) {
  wal_t* wal = get_clear_memory(sizeof(wal_t));
  refcounter_init((refcounter_t*) wal);
  wal->location = path_join(location, "wal");

  mkdir_p(wal->location);
  if (0) {
    //todo: parse directory for max number
  } else {
    char id[20];
    sprintf(id,"%lu", (uint64_t)1);
    wal->current_file = path_join(wal->location, id);
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
  refcounter_dereference((refcounter_t*) wal);
  if (refcounter_count((refcounter_t*) wal) == 0) {
    fclose(wal->log);
    free(wal->current_file);
    if (wal->last_file != NULL) {
      free(wal->last_file);
    }
    free(wal->location);
  }
}