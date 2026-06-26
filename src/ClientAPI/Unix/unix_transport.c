//
// Created by victor on 5/20/26.
//
#include "unix_transport.h"
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
/* _accept_callback drives accept via the poll-dancer readiness listen watcher,
 * used only on readiness backends (epoll/kqueue). Windows IOCP can't deliver a
 * READ event on a listening socket, so the Windows AF_UNIX path polls accept()
 * in _server_thread instead and never references this callback. */
#ifndef _WIN32
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                               pd_event_t events, void* user_data);
#endif
static void* _pipe_loop_thread(void* arg);

static void _destroy_stack_init(unix_transport_t* transport) {
  transport->destroy_lock = platform_mutex_create();
  transport->destroy_head = NULL;
}

static void _destroy_stack_push(unix_transport_t* transport, pd_watcher_t* watcher) {
  unix_transport_destroy_node_t* node = get_clear_memory(sizeof(unix_transport_destroy_node_t));
  node->watcher = watcher;
  platform_mutex_lock(transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_mutex_unlock(transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _destroy_stack_drain(unix_transport_t* transport) {
  unix_transport_destroy_node_t* node;
  platform_mutex_lock(transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_mutex_unlock(transport->destroy_lock);
  while (node != NULL) {
    unix_transport_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(unix_transport_t* transport) {
  _destroy_stack_drain(transport);
  platform_mutex_destroy(transport->destroy_lock);
}

void _unix_server_dispatch(void* state, message_t* msg) {
  unix_transport_t* transport = (unix_transport_t*)state;
  switch (msg->type) {
    case UNIX_SERVER_UPDATE_WATCHER: {
      unix_watcher_update_payload_t* payload = (unix_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_update(payload->watcher, payload->events);
      }
      break;
    }
    case UNIX_SERVER_STOP_WATCHER: {
      unix_watcher_update_payload_t* payload = (unix_watcher_update_payload_t*)msg->payload;
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
                                         const char* socket_path,
                                         const char* api_key_hash,
                                         health_context_t* health_ctx) {
  unix_transport_t* transport = get_clear_memory(sizeof(unix_transport_t));
  transport->pool = pool;
  transport->bc = bc;
  transport->ofd_cache = ofd_cache;
  transport->tc = tc;
  actor_init(&transport->actor, transport, _unix_server_dispatch, transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  transport->listen_sock = NULL;
  transport->listen_watcher = NULL;
  transport->loop_thread = NULL;
  transport->max_connections = 0;
  atomic_store(&transport->active_connections, 0);
  atomic_store(&transport->loop_running, 0);
  _destroy_stack_init(transport);

  transport->socket_path = get_memory(strlen(socket_path) + 1);
  memcpy(transport->socket_path, socket_path, strlen(socket_path) + 1);

  if (api_key_hash != NULL) {
    transport->api_key_hash = get_memory(strlen(api_key_hash) + 1);
    memcpy(transport->api_key_hash, api_key_hash, strlen(api_key_hash) + 1);
  } else {
    transport->api_key_hash = NULL;
  }
  transport->health_ctx = health_ctx;

  transport->listen_sock = platform_local_listen(socket_path);
  if (transport->listen_sock == NULL) {
    log_error("unix_transport_create: platform_local_listen failed for %s", socket_path);
    perror("platform_local_listen");
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->socket_path);
    free(transport->api_key_hash);
    free(transport->config_data_dir);
    free(transport);
    return NULL;
  }

  platform_socket_set_nonblocking(transport->listen_sock);

  return transport;
}

void unix_transport_destroy(unix_transport_t* transport) {
  if (transport == NULL) {
    return;
  }
  if (atomic_load(&transport->running)) {
    unix_transport_stop(transport);
  }
  /* The pipe path runs a separate _pipe_loop_thread; it should already
   * have been joined inside unix_transport_stop. If unix_transport_stop
   * was never called (running was never set), we still need to make
   * sure the loop thread isn't running. */
  atomic_store(&transport->loop_running, 0);
  if (transport->loop_thread != NULL) {
    pd_loop_async_send(transport->loop, transport);
    platform_thread_join(transport->loop_thread);
    transport->loop_thread = NULL;
  }
  /* Drain the destroy stack BEFORE we tear down connection watchers. The
   * destroy stack holds STOP_WATCHER pushes (via _connection_stop_watcher)
   * that were enqueued to the transport's actor and may still be pending
   * when the server thread exits. If we destroy those watchers again from
   * the per-connection cleanup loop below, we double-free their pd_watcher_t
   * and corrupt the heap (a real source of test flakiness — the next test's
   * allocation hits the corrupted tcache/fastbin). */
  _destroy_stack_drain(transport);
  if (transport->listen_sock != NULL) {
    platform_socket_destroy(transport->listen_sock);
    transport->listen_sock = NULL;
  }
  for (int i = 0; i < transport->connections.length; i++) {
    unix_connection_t* conn = transport->connections.data[i];
    conn->is_closing = 1;
    if (conn->sock != NULL) {
      platform_socket_destroy(conn->sock);
      conn->sock = NULL;
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
    /* Detach from the pool registry before freeing conn, so no dangling
     * registry pointer remains for a recovery scan / pool-destroy detach. */
    actor_detach_pool(&conn->actor);
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
    platform_local_cleanup(transport->socket_path);
    free(transport->socket_path);
  }
  free(transport->api_key_hash);
  free(transport->config_data_dir);
  actor_destroy(&transport->actor);
  _destroy_stack_destroy(transport);
  pd_loop_destroy(transport->loop);
  free(transport);
}

#ifndef _WIN32
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                               pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  unix_transport_t* transport = (unix_transport_t*)user_data;

  if (events & PD_EVENT_READ) {
    platform_socket_t* client_sock = platform_local_accept(transport->listen_sock);
    if (client_sock == NULL) {
      /* Only log permanent accept failures (EBADF, EINVAL, ENOTSOCK, EOPNOTSUPP).
         Transient errors like EAGAIN, ECONNABORTED, EINTR are normal. */
      if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK || errno == EOPNOTSUPP) {
        log_error("unix_transport: accept failed permanently (errno=%d)", errno);
      }
      return;
    }

    if (transport->max_connections > 0 &&
        atomic_load(&transport->active_connections) >= transport->max_connections) {
      platform_socket_destroy(client_sock);
      return;
    }

    unix_connection_t* connection = unix_connection_create(transport, client_sock);
    if (connection == NULL) {
      platform_socket_destroy(client_sock);
      return;
    }
    vec_push(&transport->connections, connection);
    atomic_fetch_add(&transport->active_connections, 1);

    /* For pipe-backed listeners the rearm happens on the accept thread
     * (see _server_thread); on the AF_UNIX path the listener is a
     * regular socket and rearm is a no-op. */
    platform_local_rearm(transport->listen_sock);
  }
}
#endif

/* The pipe loop thread: services the transport's loop (IOCP) so that
 * per-connection read/watch completions get processed. This is a
 * separate thread from the accept thread because the accept path
 * blocks in GetOverlappedResult; on the same thread the loop would
 * never get a chance to dispatch connection I/O. */
static void* _pipe_loop_thread(void* arg) {
  unix_transport_t* transport = (unix_transport_t*)arg;
  platform_thread_setup_stack();
  while (atomic_load(&transport->loop_running)) {
    int rc = pd_loop_run_once(transport->loop, 50);
    (void)rc;
  }
  return NULL;
}

static void* _server_thread(void* arg) {
  unix_transport_t* transport = (unix_transport_t*)arg;
  platform_thread_setup_stack();

  /* For a Windows named-pipe listener, a ReadFile on the listener handle
   * never completes (a client connecting only signals via ConnectNamedPipe).
   * Run a blocking accept loop on this thread; a separate
   * _pipe_loop_thread services the IOCP for per-connection watchers.
   * For AF_UNIX listeners the strategy is platform-dependent: POSIX uses the
   * poll-dancer readiness listen watcher (epoll/kqueue deliver READ when a
   * connection is pending); Windows IOCP is completion-based and cannot
   * deliver a READ event on a listening socket, so it polls accept() in the
   * loop below instead. */
  if (platform_socket_is_pipe(transport->listen_sock)) {
    /* Spawn the loop thread. It services transport->loop in the
     * background so connection I/O is not starved by the blocking
     * accept on this thread. */
    atomic_store(&transport->loop_running, 1);
    transport->loop_thread = platform_thread_create(_pipe_loop_thread, transport);

    while (atomic_load(&transport->running)) {
      platform_socket_t* client_sock = platform_local_accept(transport->listen_sock);
      if (client_sock == NULL) {
        /* platform_local_accept is blocking; a NULL return means
         * ConnectNamedPipe/GetOverlappedResult failed terminally OR
         * the pending I/O was cancelled by unix_transport_stop's
         * CancelIoEx. Re-check running — if we're shutting down,
         * exit the accept loop cleanly without an error log. */
        if (!atomic_load(&transport->running)) {
          break;
        }
        log_error("unix_transport: pipe accept failed; stopping server thread");
        break;
      }

      if (transport->max_connections > 0 &&
          atomic_load(&transport->active_connections) >= transport->max_connections) {
        platform_socket_destroy(client_sock);
        platform_local_rearm(transport->listen_sock);
        continue;
      }

      unix_connection_t* connection = unix_connection_create(transport, client_sock);
      if (connection == NULL) {
        platform_socket_destroy(client_sock);
        platform_local_rearm(transport->listen_sock);
        continue;
      }
      vec_push(&transport->connections, connection);
      atomic_fetch_add(&transport->active_connections, 1);

      /* Rearm: DisconnectNamedPipe + new overlapped ConnectNamedPipe
       * inside platform_local_rearm. The listener HANDLE remains
       * valid; the same wrapper socket is reused for the next accept. */
      platform_local_rearm(transport->listen_sock);
    }

    /* Tell the loop thread to exit and join it. */
    atomic_store(&transport->loop_running, 0);
    pd_loop_async_send(transport->loop, transport);
    if (transport->loop_thread != NULL) {
      platform_thread_join(transport->loop_thread);
      transport->loop_thread = NULL;
    }
    return NULL;
  }

#ifdef _WIN32
  /* Windows AF_UNIX: IOCP is completion-based and cannot deliver a READ
   * event on a listening socket — issuing WSARecv on a listen socket fails
   * with WSAENOTCONN, so a poll-dancer READ watcher can't drive accept here.
   * The listen socket is non-blocking (see unix_transport_create); poll
   * accept() each loop iteration on this same thread, which also services
   * per-connection I/O via pd_loop_run_once. The short loop timeout bounds
   * accept latency and makes shutdown prompt: unix_transport_stop clears
   * `running` and posts via pd_loop_async_send, so the next iteration exits.
   * platform_local_accept returns NULL silently when no connection is
   * pending (WSAEWOULDBLOCK), so the poll costs only the loop wait.
   *
   * Unlike the POSIX branch we do NOT drain the destroy stack here: a
   * stopped connection watcher's overlapped is still referenced by an
   * in-flight ERROR_OPERATION_ABORTED completion that this same loop will
   * process next. Freeing the watcher now (via _destroy_stack_drain) would
   * free that overlapped out from under the pending completion — a use-
   * after-free that corrupts the heap. We defer watcher destruction to
   * unix_transport_destroy (run after this loop thread is joined and the
   * IOCP is closed, so pending completions are discarded), mirroring the
   * named-pipe path. Stopped watchers accumulate until teardown, which is
   * the same trade-off the pipe path already makes. */
  while (atomic_load(&transport->running)) {
    platform_socket_t* client_sock = platform_local_accept(transport->listen_sock);
    if (client_sock != NULL) {
      if (transport->max_connections > 0 &&
          atomic_load(&transport->active_connections) >= transport->max_connections) {
        platform_socket_destroy(client_sock);
      } else {
        unix_connection_t* connection = unix_connection_create(transport, client_sock);
        if (connection == NULL) {
          platform_socket_destroy(client_sock);
        } else {
          vec_push(&transport->connections, connection);
          atomic_fetch_add(&transport->active_connections, 1);
        }
      }
    }
    pd_loop_run_once(transport->loop, 10);
  }
  pd_loop_stop(transport->loop);
  return NULL;
#else
  transport->listen_watcher = platform_socket_watcher_create(transport->loop, transport->listen_sock,
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

void unix_transport_start(unix_transport_t* transport) {
  log_info("unix_transport_start: transport starting on %s", transport->socket_path);
  atomic_store(&transport->running, 1);
  transport->thread = platform_thread_create(_server_thread, transport);
}

void unix_transport_stop(unix_transport_t* transport) {
  atomic_store(&transport->running, 0);
  /* For pipe-backed listeners, the server thread is blocked in
   * _pipe_accept's GetOverlappedResult. Cancel the pending I/O so the
   * blocking call returns NULL and the thread can exit its accept
   * loop. For AF_UNIX listeners, async_send wakes the event loop. */
#ifdef _WIN32
  if (transport->listen_sock != NULL && platform_socket_is_pipe(transport->listen_sock)) {
    HANDLE h = (HANDLE)platform_socket_handle(transport->listen_sock);
    if (h != NULL && h != INVALID_HANDLE_VALUE) {
      CancelIoEx(h, NULL);
    }
  }
#endif
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);
}

void unix_transport_set_max_connections(unix_transport_t* transport, size_t max_connections) {
  transport->max_connections = max_connections;
}

void unix_transport_set_update_status_ctx(unix_transport_t* transport,
                                           update_status_context_t* ctx) {
  transport->update_status_ctx = ctx;
}

void unix_transport_set_config_ctx(unix_transport_t* transport,
                                    offs_node_t* node, const char* data_dir,
                                    config_trigger_restart_fn trigger_restart,
                                    void* restart_user_data) {
  transport->config_node = node;
  if (transport->config_data_dir != NULL) {
    free(transport->config_data_dir);
    transport->config_data_dir = NULL;
  }
  if (data_dir != NULL) {
    transport->config_data_dir = get_memory(strlen(data_dir) + 1);
    memcpy(transport->config_data_dir, data_dir, strlen(data_dir) + 1);
  }
  transport->trigger_restart = trigger_restart;
  transport->restart_user_data = restart_user_data;
}