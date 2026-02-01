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
#include <fcntl.h>
#include <unistd.h>


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
  if (wal->log == 0) {
#ifdef _WIN32
    wal->log = open(wal->current_file, _O_RDWR | _O_BINARY | _O_CREAT, 0644);
#else
    wal->log = open(wal->current_file, O_RDWR | O_CREAT, 0644);
#endif
  }
  uint32_t crc =  htonl(XXH32(data->data,data->size, 0));
  write(wal->log, &type, 1);
  write(wal->log, &crc, 4);
  write(wal->log, data->data, data->size);
}
int wal_read(wal_t* wal, wal_type_e* type, buffer_t** data, uint64_t* cursor, int32_t* wal_size) {
  if (wal->log == 0) {
    *cursor = 0;
#ifdef _WIN32
    wal->log = open(wal->current_file, _O_RDWR | _O_BINARY | _O_CREAT, 0644);
#else
    wal->log = open(wal->current_file, O_RDWR | O_CREAT, 0644);
#endif
    *wal_size = lseek(wal->log, 0,SEEK_END);
    if(wal_size < 0) {
      return -1;
    }
    if (lseek(wal->log, 0, SEEK_SET) < 0) {
      return -2;
    }
  } else if (*cursor >= *wal_size) {
      return -3;
  }
  lseek(wal->log, *cursor, SEEK_SET);
  size_t bytes = read(wal->log, type, 1);
  if (bytes != 1) {
    return 1;
  }
  uint32_t crc;
  bytes = read(wal->log, &crc, 4);
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
  bytes = read(wal->log, buf, size);
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
  close(wal->log);
  free(wal->current_file);
  if (wal->last_file != NULL) {
    free(wal->last_file);
  }
  free(wal->location);
  free(wal);
}