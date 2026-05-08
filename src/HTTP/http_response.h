//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_RESPONSE_H
#define OFFS_HTTP_RESPONSE_H

#include "../Streams/stream.h"
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "http_headers.h"
#include "http_status.h"

typedef struct http_connection_t http_connection_t;

typedef struct http_response_t {
  refcounter_t refcounter;
  stream_t stream;
  uint16_t status_code;
  http_headers_t headers;
  uint8_t headers_sent;
  size_t body_length;
  http_connection_t* connection;
} http_response_t;

http_response_t* http_response_create(scheduler_pool_t* pool, http_connection_t* connection);
void http_response_destroy(http_response_t* response);

void http_response_set_status(http_response_t* response, uint16_t status);
void http_response_set_header(http_response_t* response, const char* name, const char* value);
void http_response_write(http_response_t* response, const char* data, size_t length);
void http_response_end(http_response_t* response);

#endif // OFFS_HTTP_RESPONSE_H