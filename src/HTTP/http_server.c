//
// Created by victor on 5/7/26.
//
#include "http_server.h"
#include "http_request.h"
#include "http_response.h"
#include "http_connection.h"
#include "http_route.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include "../Actor/message.h"
#include <poll-dancer/poll-dancer.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                              pd_event_t events, void* user_data);

static void _destroy_stack_init(http_server_t* server) {
  platform_lock_init(&server->destroy_lock);
  server->destroy_head = NULL;
}

static void _destroy_stack_push(http_server_t* server, pd_watcher_t* watcher) {
  server_destroy_node_t* node = get_clear_memory(sizeof(server_destroy_node_t));
  node->watcher = watcher;
  platform_lock(&server->destroy_lock);
  node->next = server->destroy_head;
  server->destroy_head = node;
  platform_unlock(&server->destroy_lock);
  pd_loop_async_send(server->loop, NULL);
}

static void _destroy_stack_drain(http_server_t* server) {
  server_destroy_node_t* node;
  platform_lock(&server->destroy_lock);
  node = server->destroy_head;
  server->destroy_head = NULL;
  platform_unlock(&server->destroy_lock);
  while (node != NULL) {
    server_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(http_server_t* server) {
  _destroy_stack_drain(server);
  platform_lock_destroy(&server->destroy_lock);
}

/* Server actor dispatch — processes watcher lifecycle messages on scheduler threads. */
void _server_dispatch(void* state, message_t* msg) {
  http_server_t* server = (http_server_t*)state;
  switch (msg->type) {
    case HTTP_SERVER_UPDATE_WATCHER: {
      watcher_update_payload_t* payload = (watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_update(payload->watcher, payload->events);
      }
      break;
    }
    case HTTP_SERVER_STOP_WATCHER: {
      watcher_update_payload_t* payload = (watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_stop(payload->watcher);
        _destroy_stack_push(server, payload->watcher);
      }
      break;
    }
    default:
      break;
  }
}

http_server_t* http_server_create(scheduler_pool_t* pool, const char* host, uint16_t port) {
  http_server_t* server = get_clear_memory(sizeof(http_server_t));
  server->pool = pool;
  actor_init(&server->actor, server, _server_dispatch, server->pool);
  server->loop = pd_loop_create(NULL);
  server->ssl_ctx = NULL;
  vec_init(&server->routes);
  vec_init(&server->middlewares);
  vec_init(&server->connections);
  server->running = 0;
  server->listen_fd = -1;
  server->listen_watcher = NULL;
  server->max_connections = 0;
  atomic_store(&server->active_connections, 0);
  _destroy_stack_init(server);

  server->listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server->listen_fd < 0) {
    perror("socket");
    free(server);
    return NULL;
  }

  int opt = 1;
  setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    addr.sin_addr.s_addr = INADDR_ANY;
  }

  if (bind(server->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(server->listen_fd);
    free(server);
    return NULL;
  }

  if (listen(server->listen_fd, 128) < 0) {
    perror("listen");
    close(server->listen_fd);
    free(server);
    return NULL;
  }

  return server;
}

http_server_t* http_server_create_ssl(scheduler_pool_t* pool, const char* host, uint16_t port,
                                       const char* cert_path, const char* key_path) {
  http_server_t* server = http_server_create(pool, host, port);
  if (server == NULL) {
    return NULL;
  }

  SSL_load_error_strings();
  SSL_library_init();
  OpenSSL_add_all_algorithms();

  server->ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (server->ssl_ctx == NULL) {
    fprintf(stderr, "http_server_create_ssl: failed to create SSL_CTX\n");
    close(server->listen_fd);
    free(server);
    return NULL;
  }

  if (SSL_CTX_use_certificate_file(server->ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
    fprintf(stderr, "http_server_create_ssl: failed to load certificate from %s\n", cert_path);
    SSL_CTX_free(server->ssl_ctx);
    close(server->listen_fd);
    free(server);
    return NULL;
  }

  if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
    fprintf(stderr, "http_server_create_ssl: failed to load private key from %s\n", key_path);
    SSL_CTX_free(server->ssl_ctx);
    close(server->listen_fd);
    free(server);
    return NULL;
  }

  if (!SSL_CTX_check_private_key(server->ssl_ctx)) {
    fprintf(stderr, "http_server_create_ssl: private key does not match certificate\n");
    SSL_CTX_free(server->ssl_ctx);
    close(server->listen_fd);
    free(server);
    return NULL;
  }

  return server;
}

void http_server_destroy(http_server_t* server) {
  if (server == NULL) {
    return;
  }
  if (atomic_load(&server->running)) {
    http_server_stop(server);
  }
  if (server->listen_fd >= 0) {
    close(server->listen_fd);
  }
  for (int i = server->connections.length - 1; i >= 0; i--) {
    http_connection_t* conn = server->connections.data[i];
    conn->server = NULL;
    DESTROY(conn, http_connection);
  }
  vec_deinit(&server->connections);
  for (int i = 0; i < server->middlewares.length; i++) {
    http_middleware_entry_t* entry = &server->middlewares.data[i];
    if (entry->user_data_destroy != NULL && entry->user_data != NULL) {
      entry->user_data_destroy(entry->user_data);
    }
  }
  vec_deinit(&server->middlewares);
  for (int i = 0; i < server->routes.length; i++) {
    http_route_deinit(&server->routes.data[i]);
  }
  vec_deinit(&server->routes);
  if (server->ssl_ctx != NULL) {
    SSL_CTX_free(server->ssl_ctx);
  }
  actor_destroy(&server->actor);
  _destroy_stack_destroy(server);
  pd_loop_destroy(server->loop);
  free(server);
}

static int _add_route(http_server_t* server, int method, const char* pattern,
                      http_handler_t handler, void* user_data, void (*user_data_destroy)(void*)) {
  http_route_t route;
  int result = http_route_init(&route, method, pattern, handler, user_data, user_data_destroy);
  if (result != 0) {
    return result;
  }
  vec_push(&server->routes, route);
  return 0;
}

void http_server_get(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data) {
  _add_route(server, HTTP_GET, pattern, handler, user_data, NULL);
}

void http_server_put(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data) {
  _add_route(server, HTTP_PUT, pattern, handler, user_data, NULL);
}

void http_server_post(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data) {
  _add_route(server, HTTP_POST, pattern, handler, user_data, NULL);
}

void http_server_delete(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data) {
  _add_route(server, HTTP_DELETE, pattern, handler, user_data, NULL);
}

void http_server_get_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*)) {
  _add_route(server, HTTP_GET, pattern, handler, user_data, user_data_destroy);
}

void http_server_put_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*)) {
  _add_route(server, HTTP_PUT, pattern, handler, user_data, user_data_destroy);
}

void http_server_post_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*)) {
  _add_route(server, HTTP_POST, pattern, handler, user_data, user_data_destroy);
}

void http_server_delete_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*)) {
  _add_route(server, HTTP_DELETE, pattern, handler, user_data, user_data_destroy);
}

void http_server_dispatch(http_server_t* server, http_request_t* request, http_response_t* response) {
  for (int i = 0; i < server->middlewares.length; i++) {
    http_middleware_entry_t* entry = &server->middlewares.data[i];
    int result = entry->handler(request, response, entry->user_data);
    if (result != 0) return;
  }

  const char* path = request->path != NULL ? request->path : request->url;
  if (path == NULL) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  for (int i = 0; i < server->routes.length; i++) {
    vec_capture_t captures;
    if (http_route_match(&server->routes.data[i], request->method, path, &captures)) {
      if (request->params.data != NULL) {
        vec_capture_deinit(&request->params);
      }
      request->params = captures;
      http_route_dispatch(&server->routes.data[i], request, response);
      return;
    }
  }

  http_response_set_status(response, HTTP_STATUS_NOT_FOUND);
  http_response_end(response);
}

http_route_t* http_server_match_route(http_server_t* server, int method, const char* path) {
  for (int i = 0; i < server->routes.length; i++) {
    vec_capture_t captures;
    if (http_route_match(&server->routes.data[i], method, path, &captures)) {
      vec_capture_deinit(&captures);
      return &server->routes.data[i];
    }
  }
  return NULL;
}

void http_server_use(http_server_t* server, http_middleware_t middleware, void* user_data, void (*user_data_destroy)(void*)) {
  http_middleware_entry_t entry;
  entry.handler = middleware;
  entry.user_data = user_data;
  entry.user_data_destroy = user_data_destroy;
  vec_push(&server->middlewares, entry);
}

static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                             pd_event_t events, void* user_data) {
  http_server_t* server = (http_server_t*)user_data;

  if (events & PD_EVENT_READ) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server->listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
      perror("accept");
      return;
    }

    if (server->max_connections > 0 &&
        atomic_load(&server->active_connections) >= server->max_connections) {
      close(client_fd);
      return;
    }

    http_connection_t* connection = http_connection_create(server, client_fd);
    if (connection == NULL) {
      close(client_fd);
      return;
    }
    vec_push(&server->connections, connection);
    atomic_fetch_add(&server->active_connections, 1);

    if (server->ssl_ctx != NULL) {
      connection->ssl = SSL_new(server->ssl_ctx);
      SSL_set_fd(connection->ssl, client_fd);
      SSL_set_accept_state(connection->ssl);
      connection->is_ssl = 1;
    }
  }
}

static void* _server_thread(void* arg) {
  http_server_t* server = (http_server_t*)arg;
  platform_setup_thread_stack();

  server->listen_watcher = pd_watcher_create(server->loop, server->listen_fd,
    PD_EVENT_READ, _accept_callback, server);
  if (server->listen_watcher != NULL) {
    pd_watcher_start(server->listen_watcher);
  }

  while (atomic_load(&server->running)) {
    _destroy_stack_drain(server);
    pd_loop_run_once(server->loop, 100);
  }

  if (server->listen_watcher != NULL) {
    pd_watcher_stop(server->listen_watcher);
    pd_watcher_destroy(server->listen_watcher);
  }
  pd_loop_stop(server->loop);

  return NULL;
}

void http_server_listen(http_server_t* server) {
  atomic_store(&server->running, 1);
#ifdef _WIN32
  server->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)_server_thread, server, 0, NULL);
#else
  pthread_create(&server->thread, NULL, _server_thread, server);
#endif
}

void http_server_stop(http_server_t* server) {
  atomic_store(&server->running, 0);
  pd_loop_async_send(server->loop, server);
#ifdef _WIN32
  WaitForSingleObject(server->thread, INFINITE);
#else
  pthread_join(server->thread, NULL);
#endif
}

void http_server_set_max_connections(http_server_t* server, size_t max_connections) {
  server->max_connections = max_connections;
}