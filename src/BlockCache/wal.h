//
// Created by victor on 5/7/25.
//

#ifndef OFFS_WAL_H
#define OFFS_WAL_H
#include <stdio.h>
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include <stdint.h>

typedef enum wal_type_e {
  addition = 'a',
  removal = 'r',
  increment = 'i'
} wal_type_e;

typedef struct {
  FILE* log;
  char* location;
  char* current_file;
  char* last_file;
  uint64_t next_id;
} wal_t;

wal_t* wal_create(char* location);
wal_t* wal_create_next(char* location, uint64_t next_id, char* last_file);
void wal_write(wal_t* wal, wal_type_e type, buffer_t* data);
void wal_destroy(wal_t* wal);


#endif //OFFS_WAL_H
