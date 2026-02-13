//
// Created by victor on 2/4/26.
//

#ifndef OFFS_FILE_STREAM_H
#define OFFS_FILE_STREAM_H
#include "stream.h"
#include <stdio.h>
#include "../Buffer/buffer.h"

#define DEFAULT_CHUNK_SIZE 64000

typedef struct {
  stream_t stream;
  size_t chunk_size;
  int32_t fd;
  char* filename;
  int32_t file_size;
  int32_t cursor;
} readable_file_stream_t;
readable_file_stream_t* readable_file_stream_create(char* filename, size_t chunk_size, int* error_code);
void readable_file_stream_destroy(readable_file_stream_t* stream);

void readable_file_stream_push(readable_file_stream_t* stream);

typedef struct {
  stream_t stream;
  int32_t fd;
  char* filename;
  int32_t cursor;
} writeable_file_stream_t;

writeable_file_stream_t* writeable_file_stream_create(char* filename);
void writeable_file_stream_write(writeable_file_stream_t* stream, buffer_t* data);
#endif //OFFS_FILE_STREAM_H
