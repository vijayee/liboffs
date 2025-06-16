//
// Created by victor on 5/7/25.
//

#ifndef OFFS_WAL_H
#define OFFS_WAL_H
#include <stdio.h>
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"

typedef enum wal_type_e {
  addition = 'a',
  removal = 'r',
  increment = 'i'
} wal_type_e;

typedef struct {
  refcounter_t refcounter;
  FILE* log;
  char* location;
  char* current_file;
  char* last_file;
} wal_t;

wal_t* wal_create(char* location);
void wal_write(wal_t* wal, wal_type_e type, buffer_t* data);
void wal_destroy(wal_t* wal);


#endif //OFFS_WAL_H
