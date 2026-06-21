//
// Created by victor on 5/7/26.
//
#include "http_server.h"
#include "http_request.h"
#include "http_response.h"
#include "http_connection.h"
#include "http_route.h"
#include "../../Util/allocator.h"
#include "../../Platform/platform.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include "../../Util/log.h"
#include <poll-dancer/poll-dancer.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                              pd_event_t events, void* user_data);

static void _destroy_stack_init(http_server_t* server) {
  server->destroy_lock = platform_mutex_create();
  server->destroy_head = NULL;
}

static void _destroy_stack_push(http_server_t* server, pd_watcher_t* watcher) {
  server_destroy_node_t* node = get_clear_memory(sizeof(server_destroy_node_t));
  node->watcher = watcher;
  platform_mutex_lock(server->destroy_lock);
  node->next = server->destroy_head;
  server->destroy_head = node;
  platform_mutex_unlock(server->destroy_lock);
  pd_loop_async_send(server->loop, NULL);
}

static void _destroy_stack_drain(http_server_t* server) {
  server_destroy_node_t* node;
  platform_mutex_lock(server->destroy_lock);
  node = server->destroy_head;
  server->destroy_head = NULL;
  platform_mutex_unlock(server->destroy_lock);
  while (node != NULL) {
    server_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(http_server_t* server) {
  _destroy_stack_drain(server);
  platform_mutex_destroy(server->destroy_lock);
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
        /* Do NOT call pd_watcher_stop here. On a completion-based backend
           (Windows IOCP), pd_watcher_stop -> iocp_watcher_unregister frees the
           watcher's overlapped (platform_data) synchronously. But a re-armed
           WSARecv may still be outstanding — the connection's CLOSE path runs
           on a worker and closes the socket, which cancels that pending
           WSARecv and posts a STATUS_CANCELLED completion referencing the
           overlapped. Freeing the overlapped now would leave that completion
           pointing at freed memory; when the I/O thread dequeues it, it reads
           a stale/garbage Internal field, the cancel-filter misses, and the
           callback runs against freed watcher_data (use-after-free -> crash).
           Deferring the whole stop+destroy to the destroy stack keeps the
           overlapped alive while the loop is running, so the abort completion
           is correctly filtered (real STATUS_CANCELLED from a live overlapped)
           and no UAF occurs. The destroy stack is drained on POSIX each loop
           iteration (epoll has no outstanding overlapped, so a late stop is
           safe) and on Windows in http_server_destroy, after the I/O thread
           has been joined — so by the time pd_watcher_destroy runs the real
           stop+free there is no concurrent reader, and any still-queued abort
           completion is discarded when the IOCP handle closes. */
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
  server->listen_sock = NULL;
  server->listen_watcher = NULL;
  server->max_connections = 0;
  atomic_store(&server->active_connections, 0);
  atomic_store(&server->draining, 0);
  server->is_local_binding = 0;
  _destroy_stack_init(server);

  server->listen_sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (server->listen_sock == NULL) {
    log_error("http_server_create: socket creation failed");
    perror("socket");
    free(server);
    return NULL;
  }

  platform_socket_set_nonblocking(server->listen_sock);
  platform_socket_set_reuseaddr(server->listen_sock);

  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET;
  addr.inet.port = port;
  if (platform_address_parse(&addr, host, port) != 0) {
    addr.inet.addr = 0; /* INADDR_ANY */
  }

  if (platform_socket_bind(server->listen_sock, &addr) < 0) {
    log_error("http_server_create: bind failed for port %u", port);
    perror("bind");
    platform_socket_destroy(server->listen_sock);
    free(server);
    return NULL;
  }

  if (platform_socket_listen(server->listen_sock, 128) < 0) {
    log_error("http_server_create: listen failed");
    perror("listen");
    platform_socket_destroy(server->listen_sock);
    free(server);
    return NULL;
  }

  if (host != NULL) {
    server->is_local_binding = (strcmp(host, "127.0.0.1") == 0 ||
                                strcmp(host, "localhost") == 0 ||
                                strcmp(host, "::1") == 0);
  }

  return server;
}

http_server_t* http_server_create_ssl(scheduler_pool_t* pool, const char* host, uint16_t port,
                                       const char* cert_path, const char* key_path) {
  http_server_t* server = http_server_create(pool, host, port);
  if (server == NULL) {
    return NULL;
  }

  OPENSSL_init_ssl(0, NULL);

  server->ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (server->ssl_ctx == NULL) {
    fprintf(stderr, "http_server_create_ssl: failed to create SSL_CTX\n");
    platform_socket_destroy(server->listen_sock);
    free(server);
    return NULL;
  }

  if (SSL_CTX_use_certificate_file(server->ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
    fprintf(stderr, "http_server_create_ssl: failed to load certificate from %s\n", cert_path);
    SSL_CTX_free(server->ssl_ctx);
    platform_socket_destroy(server->listen_sock);
    free(server);
    return NULL;
  }

  if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
    fprintf(stderr, "http_server_create_ssl: failed to load private key from %s\n", key_path);
    SSL_CTX_free(server->ssl_ctx);
    platform_socket_destroy(server->listen_sock);
    free(server);
    return NULL;
  }

  if (!SSL_CTX_check_private_key(server->ssl_ctx)) {
    fprintf(stderr, "http_server_create_ssl: private key does not match certificate\n");
    SSL_CTX_free(server->ssl_ctx);
    platform_socket_destroy(server->listen_sock);
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
  if (server->listen_sock != NULL) {
    platform_socket_destroy(server->listen_sock);
    server->listen_sock = NULL;
  }
  /* Mark all connections as closing, close their fds so actors drain quickly,
   * and null out the server pointer so connection callbacks won't send
   * messages to the server actor. */
  for (int i = 0; i < server->connections.length; i++) {
    http_connection_t* conn = server->connections.data[i];
    conn->is_closing = 1;
    if (conn->sock != NULL) {
      platform_socket_destroy(conn->sock);
      conn->sock = NULL;
    }
    conn->server = NULL;
  }
  /* Mark all connection actors as destroyed so the scheduler skips them
   * and actor_send drops any new messages. This MUST happen before
   * scheduler_pool_wait_for_idle, otherwise a worker thread could still
   * be dispatching on a connection when we free it below. */
  for (int i = 0; i < server->connections.length; i++) {
    http_connection_t* conn = server->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  /* If the scheduler pool is still running, wait for all workers to go idle
   * so no worker is dispatching on a connection actor. If the pool is
   * already stopped (workers joined), no workers are running so it's safe
   * to skip this. */
  if (!atomic_load_explicit(&server->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(server->pool);
  }
  /* Drain each connection's message queue and free its resources.
   * We must NOT call actor_destroy here because it triggers
   * backpressure_release which wakes worker threads — those workers
   * could then access the connection actor's memory via actor_send,
   * causing a use-after-free. Since ACTOR_FLAG_DESTROY is already set,
   * actor_send will drop any queued messages, so we only need to
   * free the queue nodes. The backpressure senders will remain muted
   * until the scheduler pool is stopped, which is fine during shutdown. */
  for (int i = server->connections.length - 1; i >= 0; i--) {
    http_connection_t* conn = server->connections.data[i];
    /* Drain and free the actor's message queue without triggering
     * backpressure_release (which would wake workers). */
    message_queue_destroy(&conn->actor.queue);
    /* Clean up the watcher — the I/O thread is already stopped so we
     * can stop and destroy it directly on the main thread. */
    if (ATOMIC_LOAD(&conn->watcher) != NULL) {
      pd_watcher_t* watcher = ATOMIC_EXCHANGE(&conn->watcher, NULL);
      if (watcher != NULL) {
        pd_watcher_stop(watcher);
        pd_watcher_destroy(watcher);
      }
    }
    if (conn->request != NULL) {
      DESTROY(conn->request, http_request);
    }
    if (conn->write_buffer != NULL) {
      DESTROY(conn->write_buffer, buffer);
    }
    if (conn->header_field != NULL) {
      free(conn->header_field);
    }
    if (conn->header_value != NULL) {
      free(conn->header_value);
    }
    if (conn->ssl != NULL) {
      SSL_free(conn->ssl);
    }
    atomic_fetch_sub(&server->active_connections, 1);
    free(conn);
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

/* Accept one pending connection (shared by the POSIX listen-watcher callback
   and the Windows accept-poll loop). Honours the drain flag, the
   max_connections cap, and SSL setup. */
static void _http_server_accept_one(http_server_t* server) {
  if (atomic_load(&server->draining)) {
    return;
  }

  platform_socket_t* client_sock = platform_socket_accept(server->listen_sock, NULL);
  if (client_sock == NULL) {
#ifndef _WIN32
    /* On POSIX the listen-watcher only fires accept when READ is ready, so a
       NULL here is a real (permanent) error worth logging. On Windows the
       poll loop calls accept every iteration and NULL means WSAEWOULDBLOCK
       (no pending connection), which is normal; errno is not set by the
       Winsock path, so the check would log against stale errno. */
    if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK || errno == EOPNOTSUPP) {
      log_error("http_server: accept failed permanently (errno=%d)", errno);
    }
#endif
    return;
  }

  if (server->max_connections > 0 &&
      atomic_load(&server->active_connections) >= server->max_connections) {
    platform_socket_destroy(client_sock);
    return;
  }

  http_connection_t* connection = http_connection_create(server, client_sock);
  if (connection == NULL) {
    platform_socket_destroy(client_sock);
    return;
  }
  vec_push(&server->connections, connection);
  atomic_fetch_add(&server->active_connections, 1);

  if (server->ssl_ctx != NULL) {
    connection->ssl = SSL_new(server->ssl_ctx);
    SSL_set_fd(connection->ssl, platform_socket_fd(client_sock));
    SSL_set_accept_state(connection->ssl);
    connection->is_ssl = 1;
  }
}

static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                             pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  http_server_t* server = (http_server_t*)user_data;

  if (events & PD_EVENT_READ) {
    _http_server_accept_one(server);
  }
}

static void* _server_thread(void* arg) {
  http_server_t* server = (http_server_t*)arg;
  platform_thread_setup_stack();

#ifdef _WIN32
  /* Windows: IOCP is completion-based and cannot deliver a READ event on a
     listening socket (WSARecv on a listen socket fails WSAENOTCONN), so a
     poll-dancer READ watcher can't drive accept here — the same constraint
     the AF_UNIX transport works around. The listen socket is non-blocking
     (see http_server_create), so poll platform_socket_accept each loop
     iteration on this same thread, which also services per-connection I/O
     via pd_loop_run_once. platform_socket_accept returns NULL silently when
     no connection is pending (WSAEWOULDBLOCK), so the poll costs only the
     loop wait. The short (10ms) loop timeout bounds accept latency and makes
     shutdown prompt: http_server_stop clears `running` and posts
     pd_loop_async_send, so the next iteration exits.

     Unlike the POSIX branch we do NOT drain the destroy stack here: a
     stopped connection watcher's overlapped may still be referenced by an
     in-flight ERROR_OPERATION_ABORTED completion this same loop will process
     next; freeing it now (via _destroy_stack_drain) would free that overlapped
     out from under the pending completion — a use-after-free. Watcher
     destruction is deferred to http_server_destroy (run after this loop
     thread is joined and the IOCP is closed, so pending completions are
     discarded). */
  while (atomic_load(&server->running)) {
    _http_server_accept_one(server);
    pd_loop_run_once(server->loop, 10);
  }
  pd_loop_stop(server->loop);
  return NULL;
#else
  server->listen_watcher = pd_watcher_create(server->loop, platform_socket_fd(server->listen_sock),
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
#endif
}

void http_server_listen(http_server_t* server) {
  log_info("HTTP server starting");
  atomic_store(&server->running, 1);
  server->thread = platform_thread_create(_server_thread, server);
}

void http_server_stop(http_server_t* server) {
  atomic_store(&server->running, 0);
  pd_loop_async_send(server->loop, server);
  platform_thread_join(server->thread);
}

void http_server_drain(http_server_t* server) {
  if (server == NULL) return;
  atomic_store(&server->draining, 1);
}

void http_server_set_max_connections(http_server_t* server, size_t max_connections) {
  server->max_connections = max_connections;
}

uint8_t http_server_is_local_binding(const http_server_t* server) {
  return server != NULL ? server->is_local_binding : 0;
}