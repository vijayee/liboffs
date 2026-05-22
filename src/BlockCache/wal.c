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


wal_t* wal_create(char* location, uint64_t id) {
  wal_t* wal = get_clear_memory(sizeof(wal_t));
  wal->location = path_join(location, "wal");

  mkdir_p(wal->location);

  char id_str[20];

  sprintf(id_str,"%lu", id);
  wal->next_id = id + 1;
  wal->current_file = path_join(wal->location, id_str);
  if (id > 1) {
    char last_id_str[20];
    sprintf(last_id_str,"%lu", (uint64_t)(id - 1));
    wal->last_file = path_join(wal->location, last_id_str);
  } else {
    wal->last_file = NULL;
  }
  return wal;
}
wal_t* wal_load(char* location, uint64_t id) {
  wal_t* wal = get_clear_memory(sizeof(wal_t));
  wal->location = path_join(location, "wal");
  char current[20];
  sprintf(current, "%lu", id);
  wal->current_file = path_join(wal->location, current);
  if (id > 1) {
    char last[20];
    sprintf(last, "%lu", (id - 1));
    wal->last_file = path_join(wal->location, last);
  } else {
    wal->last_file = NULL;
  }
  wal->next_id = id + 1;
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
  return wal;
}

void wal_write(wal_t* wal,wal_type_e type, buffer_t* data) {
  if (wal->log == NULL) {
    wal->log = platform_file_open(wal->current_file, PLATFORM_O_RDWR | PLATFORM_O_CREAT, 0644);
  }
  uint32_t crc =  htonl(XXH32(data->data,data->size, 0));
  platform_file_write(wal->log, &type, 1);
  platform_file_write(wal->log, &crc, 4);
  platform_file_write(wal->log, data->data, data->size);
}

int wal_read(wal_t* wal, wal_type_e* type, buffer_t** data, uint64_t* cursor, int32_t* wal_size) {
  if (wal->log == NULL) {
    *cursor = 0;
    wal->log = platform_file_open(wal->current_file, PLATFORM_O_RDWR | PLATFORM_O_CREAT, 0644);
    int64_t file_size = platform_file_seek(wal->log, 0, PLATFORM_SEEK_END);
    if (file_size < 0) {
      return -1;
    }
    *wal_size = (int32_t)file_size;
    if (platform_file_seek(wal->log, 0, PLATFORM_SEEK_SET) < 0) {
      return -2;
    }
  } else if (*cursor >= (uint64_t)*wal_size) {
      return -3;
  }
  platform_file_seek(wal->log, (int64_t)*cursor, PLATFORM_SEEK_SET);
  size_t bytes = platform_file_read(wal->log, type, 1);
  if (bytes != 1) {
    return 1;
  }
  uint32_t crc;
  bytes = platform_file_read(wal->log, &crc, 4);
  if (bytes != 4) {
    return 2;
  }

  uint64_t size;
  switch (*type) {
    case 'a':
    case 'i':
      size = 78;
      break;
    case 'e':
      size = 44;
      break;
    case 'r':
      size = 34;
      break;
  }
  uint8_t* buf = get_memory(size);
  bytes = platform_file_read(wal->log, buf, size);
  if (bytes != size) {
    free(buf);
    return 3;
  }
  buffer_t* buffer = buffer_create_from_existing_memory(buf, size);
  *data = buffer;
  uint32_t crc2 = htonl(XXH32(buffer->data,buffer->size, 0));
  *cursor += 1;
  *cursor += 4;
  *cursor += size;
  if (crc == crc2) {
    return 0;
  } else {
    return 4;
  }
}
void wal_destroy(wal_t* wal) {
  if (wal->log != NULL) {
    platform_file_close(wal->log);
  }
  free(wal->current_file);
  if (wal->last_file != NULL) {
    free(wal->last_file);
  }
  free(wal->location);
  free(wal);
}