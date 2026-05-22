//
// Created by victor on 5/20/26.
//
#include "tcp_transport.h"
#include "../../Util/allocator.h"
#include "../../Platform/platform.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include <poll-dancer/poll-dancer.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                               pd_event_t events, void* user_data);

static void _destroy_stack_init(tcp_transport_t* transport) {
  transport->destroy_lock = platform_mutex_create();
  transport->destroy_head = NULL;
}

static void _destroy_stack_push(tcp_transport_t* transport, pd_watcher_t* watcher) {
  tcp_transport_destroy_node_t* node = get_clear_memory(sizeof(tcp_transport_destroy_node_t));
  node->watcher = watcher;
  platform_mutex_lock(transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_mutex_unlock(transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _destroy_stack_drain(tcp_transport_t* transport) {
  tcp_transport_destroy_node_t* node;
  platform_mutex_lock(transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_mutex_unlock(transport->destroy_lock);
  while (node != NULL) {
    tcp_transport_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(tcp_transport_t* transport) {
  _destroy_stack_drain(transport);
  platform_mutex_destroy(transport->destroy_lock);
}

void _tcp_server_dispatch(void* state, message_t* msg) {
  tcp_transport_t* transport = (tcp_transport_t*)state;
  switch (msg->type) {
    case TCP_SERVER_UPDATE_WATCHER: {
      tcp_watcher_update_payload_t* payload = (tcp_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_update(payload->watcher, payload->events);
      }
      break;
    }
    case TCP_SERVER_STOP_WATCHER: {
      tcp_watcher_update_payload_t* payload = (tcp_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_stop(payload->watcher);
        _destroy_stack_push(transport, payload->watcher);
      }
      break;
    }
    default:
      break;
  }
}

tcp_transport_t* tcp_transport_create(scheduler_pool_t* pool,
                                       block_cache_t* bc,
                                       ofd_cache_t* ofd_cache,
                                       tuple_cache_t* tc,
                                       const char* host,
                                       uint16_t port,
                                       const char* cert_path,
                                       const char* key_path) {
  tcp_transport_t* transport = get_clear_memory(sizeof(tcp_transport_t));
  transport->pool = pool;
  transport->bc = bc;
  transport->ofd_cache = ofd_cache;
  transport->tc = tc;
  actor_init(&transport->actor, transport, _tcp_server_dispatch, transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  transport->listen_fd = -1;
  transport->listen_watcher = NULL;
  transport->max_connections = 0;
  atomic_store(&transport->active_connections, 0);
  _destroy_stack_init(transport);

  transport->host = get_memory(strlen(host) + 1);
  memcpy(transport->host, host, strlen(host) + 1);
  transport->port = port;
  transport->ssl_ctx = NULL;

  /* Set up SSL_CTX if cert_path and key_path are provided */
  if (cert_path != NULL && key_path != NULL) {
    OPENSSL_init_ssl(0, NULL);
    transport->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (transport->ssl_ctx == NULL) {
      fprintf(stderr, "tcp_transport_create: failed to create SSL_CTX\n");
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
    if (SSL_CTX_use_certificate_file(transport->ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
      fprintf(stderr, "tcp_transport_create: failed to load certificate from %s\n", cert_path);
      SSL_CTX_free(transport->ssl_ctx);
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(transport->ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
      fprintf(stderr, "tcp_transport_create: failed to load private key from %s\n", key_path);
      SSL_CTX_free(transport->ssl_ctx);
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
    if (!SSL_CTX_check_private_key(transport->ssl_ctx)) {
      fprintf(stderr, "tcp_transport_create: private key does not match certificate\n");
      SSL_CTX_free(transport->ssl_ctx);
      pd_loop_destroy(transport->loop);
      _destroy_stack_destroy(transport);
      actor_destroy(&transport->actor);
      free(transport->host);
      free(transport);
      return NULL;
    }
  }

  transport->listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (transport->listen_fd < 0) {
    perror("socket");
    if (transport->ssl_ctx != NULL) {
      SSL_CTX_free(transport->ssl_ctx);
    }
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->host);
    free(transport);
    return NULL;
  }

  int flags = fcntl(transport->listen_fd, F_GETFL, 0);
  fcntl(transport->listen_fd, F_SETFL, flags | O_NONBLOCK);

  int opt = 1;
  setsockopt(transport->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    addr.sin_addr.s_addr = INADDR_ANY;
  }

  if (bind(transport->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(transport->listen_fd);
    if (transport->ssl_ctx != NULL) {
      SSL_CTX_free(transport->ssl_ctx);
    }
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->host);
    free(transport);
    return NULL;
  }

  if (listen(transport->listen_fd, 128) < 0) {
    perror("listen");
    close(transport->listen_fd);
    if (transport->ssl_ctx != NULL) {
      SSL_CTX_free(transport->ssl_ctx);
    }
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->host);
    free(transport);
    return NULL;
  }

  return transport;
}

void tcp_transport_destroy(tcp_transport_t* transport) {
  if (transport == NULL) {
    return;
  }
  if (atomic_load(&transport->running)) {
    tcp_transport_stop(transport);
  }
  if (transport->listen_fd >= 0) {
    close(transport->listen_fd);
  }
  for (int i = 0; i < transport->connections.length; i++) {
    tcp_connection_t* conn = transport->connections.data[i];
    conn->is_closing = 1;
    if (conn->fd >= 0) {
      close(conn->fd);
      conn->fd = -1;
    }
    conn->transport = NULL;
  }
  for (int i = 0; i < transport->connections.length; i++) {
    tcp_connection_t* conn = transport->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  if (!atomic_load_explicit(&transport->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(transport->pool);
  }
  for (int i = transport->connections.length - 1; i >= 0; i--) {
    tcp_connection_t* conn = transport->connections.data[i];
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
    if (conn->framer != NULL) {
      stream_framer_destroy(conn->framer);
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
  actor_destroy(&transport->actor);
  _destroy_stack_destroy(transport);
  pd_loop_destroy(transport->loop);
  free(transport);
}

static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                               pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  tcp_transport_t* transport = (tcp_transport_t*)user_data;

  if (events & PD_EVENT_READ) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(transport->listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
      perror("accept");
      return;
    }

    if (transport->max_connections > 0 &&
        atomic_load(&transport->active_connections) >= transport->max_connections) {
      close(client_fd);
      return;
    }

    tcp_connection_t* connection = tcp_connection_create(transport, client_fd);
    if (connection == NULL) {
      close(client_fd);
      return;
    }
    vec_push(&transport->connections, connection);
    atomic_fetch_add(&transport->active_connections, 1);
  }
}

static void* _server_thread(void* arg) {
  tcp_transport_t* transport = (tcp_transport_t*)arg;
  platform_thread_setup_stack();

  transport->listen_watcher = pd_watcher_create(transport->loop, transport->listen_fd,
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
}

void tcp_transport_start(tcp_transport_t* transport) {
  atomic_store(&transport->running, 1);
  transport->thread = platform_thread_create(_server_thread, transport);
}

void tcp_transport_stop(tcp_transport_t* transport) {
  atomic_store(&transport->running, 0);
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);
}

void tcp_transport_set_max_connections(tcp_transport_t* transport, size_t max_connections) {
  transport->max_connections = max_connections;
}