//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_CONNECTION_H
#define OFFS_HTTP_CONNECTION_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <http_parser.h>
#include "../../RefCounter/refcounter.h"
#include "../../Buffer/buffer.h"
#include "../../Util/vec.h"
#include "../../Actor/actor.h"
#include "../../Platform/platform.h"
#include "http_route.h"
#include "../../Util/atomic_compat.h"
#include <poll-dancer/poll-dancer.h>

typedef struct http_server_t http_server_t;
typedef struct http_request_t http_request_t;
typedef struct http_response_t http_response_t;

typedef struct {
  pd_watcher_t* watcher;
  pd_event_t events;
} watcher_update_payload_t;

typedef struct http_connection_t {
  refcounter_t refcounter;
  actor_t actor;
  http_server_t* server;
  platform_socket_t* sock;
  ATOMIC(pd_watcher_t*) watcher;
  SSL* ssl;
  http_parser parser;
  http_request_t* request;
  buffer_t* write_buffer;
  uint8_t write_pending;
  uint8_t read_pending;      /* I/O thread has sent READABLE; actor hasn't processed it yet */
  char* header_field;
  size_t header_field_len;
  size_t header_field_cap;
  char* header_value;
  size_t header_value_len;
  size_t header_value_cap;
  uint8_t headers_complete;
  uint8_t request_complete;
  uint8_t is_ssl;
  uint8_t piped_pending;
  uint8_t is_closing;
  http_route_t* streaming_route;
} http_connection_t;

http_connection_t* http_connection_create(http_server_t* server, platform_socket_t* sock);
void http_connection_destroy(http_connection_t* connection);

void http_connection_dispatch(void* state, message_t* msg);
void http_connection_write(http_connection_t* connection, const char* data, size_t length);
void http_connection_close(http_connection_t* connection);

#endif // OFFS_HTTP_CONNECTION_H