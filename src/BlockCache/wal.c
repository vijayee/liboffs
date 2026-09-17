#include "wal.h"
#include "../Util/allocator.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include <stdio.h>
#include <xxh3.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
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
    if (wal->log == NULL) {
      /* Cannot log — drop the record rather than write through a dead handle. */
      return;
    }
    /* The lazily opened file can already hold records a prior session
       crash-recovered into it. Position at the end so this record appends
       after them instead of overwriting the recovered prefix from offset 0. */
    if (platform_file_seek(wal->log, 0, PLATFORM_SEEK_END) < 0) {
      platform_file_close(wal->log);
      wal->log = NULL;
      return;
    }
  }
  uint32_t crc =  htonl(XXH32(data->data,data->size, 0));
  platform_file_write(wal->log, &type, 1);
  platform_file_write(wal->log, &crc, 4);
  platform_file_write(wal->log, data->data, data->size);
}

int wal_sync(wal_t* wal) {
  if (wal == NULL || wal->log == NULL) return -1;
  return platform_file_sync(wal->log);
}

/* Read an entry ('a'/'i') record payload of `size` bytes starting just
   after the type + CRC header at `record_start`. Used to retry a record at
   the legacy 78-byte framing when the modern 86-byte framing fails to
   verify. On success fills *data, advances *cursor past the record and sets
   the sticky legacy flag when `legacy` is nonzero. On failure leaves *cursor
   at `record_start` and returns the same recoverable error codes as
   wal_read (WAL_ERR_SHORT_PAYLOAD / WAL_ERR_CRC) — the caller stops replay
   and keeps the prefix either way. */
static int _wal_read_entry_payload(wal_t* wal, buffer_t** data, uint64_t* cursor,
                                   uint32_t crc, uint64_t record_start,
                                   uint64_t size, uint8_t legacy) {
  if (platform_file_seek(wal->log, (int64_t)(record_start + 5), PLATFORM_SEEK_SET) < 0) {
    return WAL_ERR_SHORT_PAYLOAD;
  }
  uint8_t* buf = get_memory(size);
  size_t bytes = platform_file_read(wal->log, buf, size);
  if (bytes != size) {
    free(buf);
    return WAL_ERR_SHORT_PAYLOAD;
  }
  uint32_t crc2 = htonl(XXH32(buf, size, 0));
  if (crc != crc2) {
    free(buf);
    return WAL_ERR_CRC;
  }
  if (legacy) {
    /* A WAL file is written by a single binary version: one record that
       verifies at the 78-byte legacy payload size means the whole file
       predates the ephemeral_count/pin_count fields. */
    wal->legacy_entry_size = 1;
  }
  *data = buffer_create_from_existing_memory(buf, size);
  *cursor = record_start + 1 + 4 + size;
  return 0;
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
  uint64_t record_start = *cursor;
  platform_file_seek(wal->log, (int64_t)*cursor, PLATFORM_SEEK_SET);
  size_t bytes = platform_file_read(wal->log, type, 1);
  if (bytes != 1) {
    return WAL_ERR_SHORT_TYPE;
  }
  uint32_t crc;
  bytes = platform_file_read(wal->log, &crc, 4);
  if (bytes != 4) {
    return WAL_ERR_SHORT_CRC;
  }

  uint64_t size = 0;  // init — unknown type must not allocate garbage
  switch (*type) {
    case 'a':
    case 'i':
      /* Serialized index_entry_to_cbor output: array header (1) +
         fibonacci counter array (16) + hash bytestring (34) +
         section_index/section_id/ejection_date uint64s (27) +
         ephemeral_count uint16 (3) + pin_count uint32 (5) = 86.
         Older binaries wrote 78-byte payloads (no ephemeral/pin fields);
         which framing applies is sticky per WAL file. */
      size = wal->legacy_entry_size ? 78 : 86;
      break;
    case 'm':
      /* 'm' records carry the full 7-element entry CBOR (86 bytes) and were
         introduced with the new format — there is no legacy size for them. */
      size = 86;
      break;
    case 'e':
      size = 44;
      break;
    case 'r':
      size = 34;
      break;
    default:
      return WAL_ERR_UNKNOWN_TYPE;
  }
  uint8_t* buf = get_memory(size);
  bytes = platform_file_read(wal->log, buf, size);
  if (bytes != size) {
    free(buf);
    /* An unclassified 'a'/'i' record whose 86-byte read ran past the end
       (or into the next record) may simply be a legacy 78-byte record —
       rewind and retry at the legacy framing before reporting a short
       payload. */
    if (size == 86 && !wal->legacy_entry_size && (*type == 'a' || *type == 'i')) {
      return _wal_read_entry_payload(wal, data, cursor, crc, record_start, 78, 1);
    }
    return WAL_ERR_SHORT_PAYLOAD;
  }
  buffer_t* buffer = buffer_create_from_existing_memory(buf, size);
  *data = buffer;
  uint32_t crc2 = htonl(XXH32(buffer->data,buffer->size, 0));
  *cursor += 1;
  *cursor += 4;
  *cursor += size;
  if (crc == crc2) {
    return 0;
  }
  if (size == 86 && !wal->legacy_entry_size && (*type == 'a' || *type == 'i')) {
    /* CRC failed at the modern framing — this WAL may have been written by
       an older binary whose entry payloads were 78 bytes. Rewind to the
       record start and retry once at 78; a verified match makes the legacy
       framing sticky for the rest of this file. A genuine fault (torn or
       corrupt new-format record) fails both framings and still reports
       WAL_ERR_CRC. On the failed retry *data stays NULL and *cursor stays at
       the record start — both states are already handled by the replay
       caller, which stops at the last complete record either way. */
    buffer_destroy(buffer);
    *data = NULL;
    *cursor = record_start;
    return _wal_read_entry_payload(wal, data, cursor, crc, record_start, 78, 1);
  }
  return WAL_ERR_CRC;
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