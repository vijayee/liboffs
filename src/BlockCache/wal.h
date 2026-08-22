//
// Created by victor on 5/7/25.
//

#ifndef OFFS_WAL_H
#define OFFS_WAL_H
#include <stdio.h>
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "../Platform/platform.h"
#include <stdint.h>

typedef enum wal_type_e {
  addition = 'a',
  removal = 'r',
  increment = 'i',
  ejection = 'e'
} wal_type_e;

// wal_read return codes. -3 is clean EOF; 0 is a complete valid record.
// Positive codes are recoverable errors — the caller should stop replay at
// the last complete record and keep the prefix, NOT bail to an empty index.
#define WAL_ERR_SHORT_TYPE      1
#define WAL_ERR_SHORT_CRC       2
#define WAL_ERR_SHORT_PAYLOAD   3
#define WAL_ERR_CRC             4
#define WAL_ERR_UNKNOWN_TYPE    5

typedef struct {
  platform_file_t* log;
  char* location;
  char* current_file;
  char* last_file;
  uint64_t next_id;
} wal_t;

wal_t* wal_create(char* location, uint64_t id);
wal_t* wal_create_next(char* location, uint64_t next_id, char* last_file);
wal_t* wal_load(char* location, uint64_t id);
void wal_write(wal_t* wal, wal_type_e type, buffer_t* data);
int wal_sync(wal_t* wal);
int wal_read(wal_t* wal, wal_type_e* type, buffer_t** data, uint64_t* cursor, int32_t* size);
void wal_destroy(wal_t* wal);


#endif //OFFS_WAL_H
