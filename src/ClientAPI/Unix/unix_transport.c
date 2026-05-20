//
// Created by victor on 5/20/26.
//
#include "unix_transport.h"
#include "../../Util/allocator.h"
#include "../../Util/threadding.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include <poll-dancer/poll-dancer.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                               pd_event_t events, void* user_data);

static void _destroy_stack_init(unix_transport_t* transport) {
  platform_lock_init(&transport->destroy_lock);
  transport->destroy_head = NULL;
}

static void _destroy_stack_push(unix_transport_t* transport, pd_watcher_t* watcher) {
  unix_transport_destroy_node_t* node = get_clear_memory(sizeof(unix_transport_destroy_node_t));
  node->watcher = watcher;
  platform_lock(&transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_unlock(&transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _destroy_stack_drain(unix_transport_t* transport) {
  unix_transport_destroy_node_t* node;
  platform_lock(&transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_unlock(&transport->destroy_lock);
  while (node != NULL) {
    unix_transport_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(unix_transport_t* transport) {
  _destroy_stack_drain(transport);
  platform_lock_destroy(&transport->destroy_lock);
}

void _unix_server_dispatch(void* state, message_t* msg) {
  unix_transport_t* transport = (unix_transport_t*)state;
  switch (msg->type) {
    case UNIX_SERVER_UPDATE_WATCHER: {
      watcher_update_payload_t* payload = (watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_update(payload->watcher, payload->events);
      }
      break;
    }
    case UNIX_SERVER_STOP_WATCHER: {
      watcher_update_payload_t* payload = (watcher_update_payload_t*)msg->payload;
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

unix_transport_t* unix_transport_create(scheduler_pool_t* pool,
                                         block_cache_t* bc,
                                         ofd_cache_t* ofd_cache,
                                         tuple_cache_t* tc,
                                         const char* socket_path) {
  unix_transport_t* transport = get_clear_memory(sizeof(unix_transport_t));
  transport->pool = pool;
  transport->bc = bc;
  transport->ofd_cache = ofd_cache;
  transport->tc = tc;
  actor_init(&transport->actor, transport, _unix_server_dispatch, transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  transport->listen_fd = -1;
  transport->listen_watcher = NULL;
  transport->max_connections = 0;
  atomic_store(&transport->active_connections, 0);
  _destroy_stack_init(transport);

  transport->socket_path = get_memory(strlen(socket_path) + 1);
  memcpy(transport->socket_path, socket_path, strlen(socket_path) + 1);

  transport->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (transport->listen_fd < 0) {
    perror("socket");
    free(transport->socket_path);
    free(transport);
    return NULL;
  }

  int flags = fcntl(transport->listen_fd, F_GETFL, 0);
  fcntl(transport->listen_fd, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  unlink(socket_path);

  if (bind(transport->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(transport->listen_fd);
    free(transport->socket_path);
    free(transport);
    return NULL;
  }

  if (listen(transport->listen_fd, 128) < 0) {
    perror("listen");
    close(transport->listen_fd);
    free(transport->socket_path);
    free(transport);
    return NULL;
  }

  return transport;
}

void unix_transport_destroy(unix_transport_t* transport) {
  if (transport == NULL) {
    return;
  }
  if (atomic_load(&transport->running)) {
    unix_transport_stop(transport);
  }
  if (transport->listen_fd >= 0) {
    close(transport->listen_fd);
  }
  for (int i = 0; i < transport->connections.length; i++) {
    unix_connection_t* conn = transport->connections.data[i];
    conn->is_closing = 1;
    if (conn->fd >= 0) {
      close(conn->fd);
      conn->fd = -1;
    }
    conn->transport = NULL;
  }
  for (int i = 0; i < transport->connections.length; i++) {
    unix_connection_t* conn = transport->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  if (!atomic_load_explicit(&transport->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(transport->pool);
  }
  for (int i = transport->connections.length - 1; i >= 0; i--) {
    unix_connection_t* conn = transport->connections.data[i];
    message_queue_destroy(&conn->actor.queue);
    if (ATOMIC_LOAD(&conn->watcher) != NULL) {
      pd_watcher_t* watcher = ATOMIC_EXCHANGE(&conn->watcher, NULL);
      if (watcher != NULL) {
        pd_watcher_stop(watcher);
        pd_watcher_destroy(watcher);
      }
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
  if (transport->socket_path != NULL) {
    unlink(transport->socket_path);
    free(transport->socket_path);
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
  unix_transport_t* transport = (unix_transport_t*)user_data;

  if (events & PD_EVENT_READ) {
    struct sockaddr_un client_addr;
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

    unix_connection_t* connection = unix_connection_create(transport, client_fd);
    if (connection == NULL) {
      close(client_fd);
      return;
    }
    vec_push(&transport->connections, connection);
    atomic_fetch_add(&transport->active_connections, 1);
  }
}

static void* _server_thread(void* arg) {
  unix_transport_t* transport = (unix_transport_t*)arg;
  platform_setup_thread_stack();

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

void unix_transport_start(unix_transport_t* transport) {
  atomic_store(&transport->running, 1);
#ifdef _WIN32
  transport->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)_server_thread, transport, 0, NULL);
#else
  pthread_create(&transport->thread, NULL, _server_thread, transport);
#endif
}

void unix_transport_stop(unix_transport_t* transport) {
  atomic_store(&transport->running, 0);
  pd_loop_async_send(transport->loop, transport);
#ifdef _WIN32
  WaitForSingleObject(transport->thread, INFINITE);
#else
  pthread_join(transport->thread, NULL);
#endif
}

void unix_transport_set_max_connections(unix_transport_t* transport, size_t max_connections) {
  transport->max_connections = max_connections;
}