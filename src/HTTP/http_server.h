//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_SERVER_H
#define OFFS_HTTP_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include <http_parser.h>
#include "../Actor/actor.h"
#include "../Util/vec.h"
#include <poll-dancer/poll-dancer.h>
#include "../Scheduler/scheduler.h"
#include "http_route.h"
#include "http_connection.h"

typedef struct http_server_t {
  actor_t actor;
  pd_loop_t* loop;
  PLATFORMTHREADTYPE thread;
  _Atomic uint8_t running;
  int listen_fd;
  pd_watcher_t* listen_watcher;
  vec_route_t routes;
  SSL_CTX* ssl_ctx;
  scheduler_pool_t* pool;
} http_server_t;

http_server_t* http_server_create(scheduler_pool_t* pool, const char* host, uint16_t port);
void http_server_destroy(http_server_t* server);

http_server_t* http_server_create_ssl(scheduler_pool_t* pool, const char* host, uint16_t port,
                                       const char* cert_path, const char* key_path);

void http_server_get(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);
void http_server_put(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);
void http_server_post(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);
void http_server_delete(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);

void http_server_listen(http_server_t* server);
void http_server_stop(http_server_t* server);

void http_server_dispatch(http_server_t* server, http_request_t* request, http_response_t* response);

#endif // OFFS_HTTP_SERVER_H