//
// Created by victor on 5/20/26.
//
#include "ws_transport.h"
#include "../../Util/allocator.h"
#include "../../Platform/platform.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include "../../Util/log.h"
#include <poll-dancer/poll-dancer.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                              pd_event_t events, void* user_data);

static void _destroy_stack_init(ws_transport_t* transport) {
  transport->destroy_lock = platform_mutex_create();
  transport->destroy_head = NULL;
}

static void _destroy_stack_push(ws_transport_t* transport, pd_watcher_t* watcher) {
  ws_transport_destroy_node_t* node = get_clear_memory(sizeof(ws_transport_destroy_node_t));
  node->watcher = watcher;
  platform_mutex_lock(transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_mutex_unlock(transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _destroy_stack_drain(ws_transport_t* transport) {
  ws_transport_destroy_node_t* node;
  platform_mutex_lock(transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_mutex_unlock(transport->destroy_lock);
  while (node != NULL) {
    ws_transport_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(ws_transport_t* transport) {
  _destroy_stack_drain(transport);
  platform_mutex_destroy(transport->destroy_lock);
}

void _ws_server_dispatch(void* state, message_t* msg) {
  ws_transport_t* transport = (ws_transport_t*)state;
  switch (msg->type) {
    case WS_SERVER_UPDATE_WATCHER: {
      ws_watcher_update_payload_t* payload = (ws_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_update(payload->watcher, payload->events);
      }
      break;
    }
    case WS_SERVER_STOP_WATCHER: {
      ws_watcher_update_payload_t* payload = (ws_watcher_update_payload_t*)msg->payload;
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
           safe) and on Windows in ws_transport_destroy, after the I/O thread
           has been joined — so by the time pd_watcher_destroy runs the real
           stop+free there is no concurrent reader, and any still-queued abort
           completion is discarded when the IOCP handle closes. */
        _destroy_stack_push(transport, payload->watcher);
      }
      break;
    }
    default:
      break;
  }
}

ws_transport_t* ws_transport_create(scheduler_pool_t* pool,
                                     block_cache_t* bc,
                                     ofd_cache_t* ofd_cache,
                                     tuple_cache_t* tc,
                                     const char* host,
                                     uint16_t port,
                                     const char* cert_path,
                                     const char* key_path,
                                     size_t max_connections,
                                     const char* api_key_hash,
                                     health_context_t* health_ctx) {
  ws_transport_t* transport = get_clear_memory(sizeof(ws_transport_t));
  transport->pool = pool;
  transport->bc = bc;
  transport->ofd_cache = ofd_cache;
  transport->tc = tc;
  actor_init(&transport->actor, transport, _ws_server_dispatch, transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  transport->listen_sock = NULL;
  transport->listen_watcher = NULL;
  transport->max_connections = max_connections;
  atomic_store(&transport->active_connections, 0);
  _destroy_stack_init(transport);

  transport->host = get_memory(strlen(host) + 1);
  memcpy(transport->host, host, strlen(host) + 1);
  transport->port = port;
  transport->ssl_ctx = NULL;

  if (api_key_hash != NULL) {
    transport->api_key_hash = get_memory(strlen(api_key_hash) + 1);
    memcpy(transport->api_key_hash, api_key_hash, strlen(api_key_hash) + 1);
  } else {
    transport->api_key_hash = NULL;
  }
  transport->health_ctx = health_ctx;

  /* Set up SSL_CTX if cert_path and key_path are provided */
  if (cert_path != NULL && key_path != NULL) {
    OPENSSL_init_ssl(0, NULL);
    transport->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (transport->ssl_ctx == NULL) {
      fprintf(stderr, "ws_transport_create: failed to create SSL_CTX\n");
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
    if (SSL_CTX_use_certificate_file(transport->ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
      fprintf(stderr, "ws_transport_create: failed to load certificate from %s\n", cert_path);
      SSL_CTX_free(transport->ssl_ctx);
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(transport->ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
      fprintf(stderr, "ws_transport_create: failed to load private key from %s\n", key_path);
      SSL_CTX_free(transport->ssl_ctx);
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
    if (!SSL_CTX_check_private_key(transport->ssl_ctx)) {
      fprintf(stderr, "ws_transport_create: private key does not match certificate\n");
      SSL_CTX_free(transport->ssl_ctx);
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
  }

  transport->listen_sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (transport->listen_sock == NULL) {
    perror("socket");
    if (transport->ssl_ctx != NULL) {
      SSL_CTX_free(transport->ssl_ctx);
    }
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->api_key_hash);
    free(transport->host);
    free(transport);
    return NULL;
  }

  platform_socket_set_nonblocking(transport->listen_sock);
  platform_socket_set_reuseaddr(transport->listen_sock);

  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET;
  addr.inet.port = port;
  if (platform_address_parse(&addr, host, port) != 0) {
    addr.inet.addr = 0; /* INADDR_ANY */
  }

  if (platform_socket_bind(transport->listen_sock, &addr) < 0) {
    perror("bind");
    platform_socket_destroy(transport->listen_sock);
    if (transport->ssl_ctx != NULL) {
      SSL_CTX_free(transport->ssl_ctx);
    }
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->api_key_hash);
    free(transport->host);
    free(transport);
    return NULL;
  }

  if (platform_socket_listen(transport->listen_sock, 128) < 0) {
    perror("listen");
    platform_socket_destroy(transport->listen_sock);
    if (transport->ssl_ctx != NULL) {
      SSL_CTX_free(transport->ssl_ctx);
    }
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->api_key_hash);
    free(transport->host);
    free(transport);
    return NULL;
  }

  return transport;
}

void ws_transport_destroy(ws_transport_t* transport) {
  if (transport == NULL) {
    return;
  }
  if (atomic_load(&transport->running)) {
    ws_transport_stop(transport);
  }
  if (transport->listen_sock != NULL) {
    platform_socket_destroy(transport->listen_sock);
    transport->listen_sock = NULL;
  }
  for (int i = 0; i < transport->connections.length; i++) {
    ws_connection_t* conn = transport->connections.data[i];
    conn->is_closing = 1;
    if (conn->sock != NULL) {
      platform_socket_destroy(conn->sock);
      conn->sock = NULL;
    }
    conn->transport = NULL;
  }
  for (int i = 0; i < transport->connections.length; i++) {
    ws_connection_t* conn = transport->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  if (!atomic_load_explicit(&transport->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(transport->pool);
  }
  for (int i = transport->connections.length - 1; i >= 0; i--) {
    ws_connection_t* conn = transport->connections.data[i];
    message_queue_destroy(&conn->actor.queue);
    if (ATOMIC_LOAD(&conn->watcher) != NULL) {
      pd_watcher_t* watcher = ATOMIC_EXCHANGE(&conn->watcher, NULL);
      if (watcher != NULL) {
        pd_watcher_stop(watcher);
        pd_watcher_destroy(watcher);
      }
    }
    if (conn->ssl != NULL) {
      SSL_free(conn->ssl);
      conn->ssl = NULL;
    }
    if (conn->upgrade_buf != NULL) {
      DESTROY(conn->upgrade_buf, buffer);
    }
    if (conn->recv_buf != NULL) {
      DESTROY(conn->recv_buf, buffer);
    }
    if (conn->write_buffer != NULL) {
      DESTROY(conn->write_buffer, buffer);
    }
    if (conn->put_content_type != NULL) {
      free(conn->put_content_type);
    }
    if (conn->put_file_name != NULL) {
      free(conn->put_file_name);
    }
    if (conn->put_server_address != NULL) {
      free(conn->put_server_address);
    }
    if (conn->put_file_hash != NULL) {
      DESTROY(conn->put_file_hash, buffer);
    }
    if (conn->put_descriptor_hash != NULL) {
      DESTROY(conn->put_descriptor_hash, buffer);
    }
    if (conn->resolve_url != NULL) {
      off_url_destroy(conn->resolve_url);
    }
    if (conn->resolve_path != NULL) {
      free(conn->resolve_path);
    }
    atomic_fetch_sub(&transport->active_connections, 1);
    free(conn);
  }
  vec_deinit(&transport->connections);
  if (transport->ssl_ctx != NULL) {
    SSL_CTX_free(transport->ssl_ctx);
  }
  if (transport->host != NULL) {
    free(transport->host);
  }
  free(transport->api_key_hash);
  actor_destroy(&transport->actor);
  _destroy_stack_destroy(transport);
  pd_loop_destroy(transport->loop);
  free(transport);
}

/* Accept one pending connection (shared by the POSIX listen-watcher callback
   and the Windows accept-poll loop). Honours the max_connections cap. */
static void _ws_transport_accept_one(ws_transport_t* transport) {
  platform_socket_t* client_sock = platform_socket_accept(transport->listen_sock, NULL);
  if (client_sock == NULL) {
#ifndef _WIN32
    /* On POSIX the listen-watcher only fires accept when READ is ready, so a
       NULL here is a real (permanent) error worth logging. On Windows the
       poll loop calls accept every iteration and NULL means WSAEWOULDBLOCK
       (no pending connection), which is normal; errno is not set by the
       Winsock path, so the check would log against stale errno. */
    if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK || errno == EOPNOTSUPP) {
      log_error("ws_transport: accept failed permanently (errno=%d)", errno);
    }
#endif
    return;
  }

  if (transport->max_connections > 0 &&
      atomic_load(&transport->active_connections) >= transport->max_connections) {
    platform_socket_destroy(client_sock);
    return;
  }

  ws_connection_t* connection = ws_connection_create(transport, client_sock);
  if (connection == NULL) {
    platform_socket_destroy(client_sock);
    return;
  }
  vec_push(&transport->connections, connection);
  atomic_fetch_add(&transport->active_connections, 1);
}

static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                              pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  ws_transport_t* transport = (ws_transport_t*)user_data;

  if (events & PD_EVENT_READ) {
    _ws_transport_accept_one(transport);
  }
}

static void* _server_thread(void* arg) {
  ws_transport_t* transport = (ws_transport_t*)arg;
  platform_thread_setup_stack();

#ifdef _WIN32
  /* Windows: IOCP is completion-based and cannot deliver a READ event on a
     listening socket (WSARecv on a listen socket fails WSAENOTCONN), so a
     poll-dancer READ watcher can't drive accept here — the same constraint
     the AF_UNIX transport works around. The listen socket is non-blocking
     (see ws_transport_create), so poll platform_socket_accept each loop
     iteration on this same thread, which also services per-connection I/O
     via pd_loop_run_once. platform_socket_accept returns NULL silently when
     no connection is pending (WSAEWOULDBLOCK), so the poll costs only the
     loop wait. The short (10ms) loop timeout bounds accept latency and makes
     shutdown prompt: ws_transport_stop clears `running` and posts
     pd_loop_async_send, so the next iteration exits.

     Unlike the POSIX branch we do NOT drain the destroy stack here: a
     stopped connection watcher's overlapped may still be referenced by an
     in-flight ERROR_OPERATION_ABORTED completion this same loop will process
     next; freeing it now (via _destroy_stack_drain) would free that overlapped
     out from under the pending completion — a use-after-free. Watcher
     destruction is deferred to ws_transport_destroy (run after this loop
     thread is joined and the IOCP is closed, so pending completions are
     discarded). */
  while (atomic_load(&transport->running)) {
    _ws_transport_accept_one(transport);
    pd_loop_run_once(transport->loop, 10);
  }
  pd_loop_stop(transport->loop);
  return NULL;
#else
  transport->listen_watcher = pd_watcher_create(transport->loop, platform_socket_fd(transport->listen_sock),
    PD_EVENT_READ, _accept_callback, transport);
  if (transport->listen_watcher != NULL) {
    pd_watcher_start(transport->listen_watcher);
  }

  while (atomic_load(&transport->running)) {
    _destroy_stack_drain(transport);
    pd_loop_run_once(transport->loop, 100);
  }

  if (transport->listen_watcher != NULL) {
    pd_watcher_stop(transport->listen_watcher);
    pd_watcher_destroy(transport->listen_watcher);
  }
  pd_loop_stop(transport->loop);

  return NULL;
#endif
}

void ws_transport_start(ws_transport_t* transport) {
  atomic_store(&transport->running, 1);
  transport->thread = platform_thread_create(_server_thread, transport);
}

void ws_transport_stop(ws_transport_t* transport) {
  atomic_store(&transport->running, 0);
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);
}