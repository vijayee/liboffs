//
// Created by victor on 5/8/25.
//

#ifndef OFFS_OFF_URL_H
#define OFFS_OFF_URL_H

#include "../Buffer/buffer.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char* server_address;
  char* content_type;
  size_t stream_length;
  size_t stream_offset;
  buffer_t* file_hash;
  buffer_t* descriptor_hash;
  char* file_name;
} off_url_t;

off_url_t* off_url_create(void);
off_url_t* off_url_parse(const char* url_string);
char* off_url_to_string(off_url_t* url);
off_url_t* off_url_from_headers(const char* type, const char* file_name, size_t stream_length, const char* server_address);
void off_url_destroy(off_url_t* url);

#ifdef __cplusplus
}
#endif

#endif //OFFS_OFF_URL_H