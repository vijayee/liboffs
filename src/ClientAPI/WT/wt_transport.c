//
// Created by victor on 5/20/26.
//
#include "wt_transport.h"

#ifdef HAS_MSQUIC

#include "../../Util/allocator.h"
#include "../../Platform/platform.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include <poll-dancer/poll-dancer.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void* _server_thread(void* arg);

/* Forward declarations for MsQuic callbacks */
static QUIC_STATUS QUIC_API _wt_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event);
static QUIC_STATUS QUIC_API _wt_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event);
static QUIC_STATUS QUIC_API _wt_listener_callback(
    HQUIC listener, void* context, QUIC_LISTENER_EVENT* event);

static void _destroy_stack_init(wt_transport_t* transport) {
  transport->destroy_lock = platform_mutex_create();
  transport->destroy_head = NULL;
}

static void _destroy_stack_push(wt_transport_t* transport, void* item) {
  wt_transport_destroy_node_t* node = get_clear_memory(sizeof(wt_transport_destroy_node_t));
  node->item = item;
  platform_mutex_lock(transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_mutex_unlock(transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _destroy_stack_drain(wt_transport_t* transport) {
  wt_transport_destroy_node_t* node;
  platform_mutex_lock(transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_mutex_unlock(transport->destroy_lock);
  while (node != NULL) {
    wt_transport_destroy_node_t* next = node->next;
    /* Items in the destroy stack are either HQUIC streams or connections
     * that were deferred for safe cleanup. We close them here on the
     * server thread. */
    HQUIC handle = (HQUIC)node->item;
    if (handle != NULL) {
      transport->msquic->StreamClose(handle);
    }
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(wt_transport_t* transport) {
  _destroy_stack_drain(transport);
  platform_mutex_destroy(transport->destroy_lock);
}

void _wt_server_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
  /* WT server actor currently only drains the destroy stack via pd_loop.
   * Connection-level dispatch is handled by wt_connection_dispatch. */
}

wt_transport_t* wt_transport_create(scheduler_pool_t* pool,
                                      block_cache_t* bc,
                                      ofd_cache_t* ofd_cache,
                                      tuple_cache_t* tc,
                                      const char* host,
                                      uint16_t port,
                                      const char* cert_path,
                                      const char* key_path,
                                      size_t max_connections,
                                      const char* api_key_hash) {
  wt_transport_t* transport = get_clear_memory(sizeof(wt_transport_t));
  transport->pool = pool;
  transport->bc = bc;
  transport->ofd_cache = ofd_cache;
  transport->tc = tc;
  actor_init(&transport->actor, transport, _wt_server_dispatch, transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  atomic_store(&transport->listening, 0);
  transport->listener = NULL;
  transport->registration = NULL;
  transport->configuration = NULL;
  transport->max_connections = max_connections;
  atomic_store(&transport->active_connections, 0);
  _destroy_stack_init(transport);
  transport->conn_lock = platform_mutex_create();

  transport->host = get_memory(strlen(host) + 1);
  memcpy(transport->host, host, strlen(host) + 1);
  transport->port = port;

  if (api_key_hash != NULL) {
    transport->api_key_hash = get_memory(strlen(api_key_hash) + 1);
    memcpy(transport->api_key_hash, api_key_hash, strlen(api_key_hash) + 1);
  } else {
    transport->api_key_hash = NULL;
  }
  if (cert_path != NULL) {
    transport->cert_path = get_memory(strlen(cert_path) + 1);
    memcpy(transport->cert_path, cert_path, strlen(cert_path) + 1);
  } else {
    transport->cert_path = NULL;
  }
  if (key_path != NULL) {
    transport->key_path = get_memory(strlen(key_path) + 1);
    memcpy(transport->key_path, key_path, strlen(key_path) + 1);
  } else {
    transport->key_path = NULL;
  }

  transport->msquic = offs_msquic_open();
  if (transport->msquic == NULL) {
    fprintf(stderr, "wt_transport_create: MsQuic initialization failed\n");
    free(transport->cert_path);
    free(transport->key_path);
    free(transport->host);
    free(transport->api_key_hash);
    platform_mutex_destroy(transport->conn_lock);
    _destroy_stack_destroy(transport);
    pd_loop_destroy(transport->loop);
    actor_destroy(&transport->actor);
    free(transport);
    return NULL;
  }

  return transport;
}

void wt_transport_destroy(wt_transport_t* transport) {
  if (transport == NULL) {
    return;
  }
  if (atomic_load(&transport->running)) {
    wt_transport_stop(transport);
  }

  platform_mutex_lock(transport->conn_lock);
  for (int i = 0; i < transport->connections.length; i++) {
    wt_connection_t* conn = transport->connections.data[i];
    conn->is_closing = 1;
    conn->transport = NULL;
  }
  platform_mutex_unlock(transport->conn_lock);

  for (int i = 0; i < transport->connections.length; i++) {
    wt_connection_t* conn = transport->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  if (!atomic_load_explicit(&transport->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(transport->pool);
  }
  for (int i = transport->connections.length - 1; i >= 0; i--) {
    wt_connection_t* conn = transport->connections.data[i];
    message_queue_destroy(&conn->actor.queue);
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

  if (transport->listener != NULL) {
    transport->msquic->ListenerStop(transport->listener);
    transport->msquic->ListenerClose(transport->listener);
  }
  if (transport->configuration != NULL) {
    transport->msquic->ConfigurationClose(transport->configuration);
  }
  if (transport->registration != NULL) {
    transport->msquic->RegistrationClose(transport->registration);
  }
  offs_msquic_close();
  if (transport->cert_path != NULL) {
    free(transport->cert_path);
  }
  if (transport->key_path != NULL) {
    free(transport->key_path);
  }
  if (transport->host != NULL) {
    free(transport->host);
  }
  free(transport->api_key_hash);
  platform_mutex_destroy(transport->conn_lock);
  actor_destroy(&transport->actor);
  _destroy_stack_destroy(transport);
  pd_loop_destroy(transport->loop);
  free(transport);
}

/* MsQuic listener callback */
static QUIC_STATUS QUIC_API _wt_listener_callback(
    HQUIC listener, void* context, QUIC_LISTENER_EVENT* event) {
  wt_transport_t* transport = (wt_transport_t*)context;
  (void)listener;

  switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
      HQUIC connection = event->NEW_CONNECTION.Connection;
      transport->msquic->SetCallbackHandler(connection, _wt_connection_callback, transport);
      transport->msquic->ConnectionSetConfiguration(connection, transport->configuration);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

/* MsQuic connection callback */
static QUIC_STATUS QUIC_API _wt_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
  wt_transport_t* transport = (wt_transport_t*)context;
  (void)connection;

  switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
      if (transport->max_connections > 0 &&
          atomic_load(&transport->active_connections) >= transport->max_connections) {
        return QUIC_STATUS_CONNECTION_REFUSED;
      }
      /* Accept the connection — stream acceptance is handled separately */
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
      /* Connection fully closed — nothing to do here, connection cleanup
       * is handled by the connection actor system. */
      break;
    }
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
      /* Peer opened a bidirectional stream — this is a WebTransport request */
      HQUIC stream = event->PEER_STREAM_STARTED.Stream;
      wt_connection_t* wt_conn = wt_connection_create(transport, connection, stream);
      if (wt_conn == NULL) {
        transport->msquic->StreamClose(stream);
        return QUIC_STATUS_OUT_OF_MEMORY;
      }
      platform_mutex_lock(transport->conn_lock);
      vec_push(&transport->connections, wt_conn);
      platform_mutex_unlock(transport->conn_lock);
      atomic_fetch_add(&transport->active_connections, 1);

      /* Set stream callback to receive data */
      transport->msquic->SetCallbackHandler(stream, _wt_stream_callback, wt_conn);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

/* MsQuic stream callback */
static QUIC_STATUS QUIC_API _wt_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  wt_connection_t* conn = (wt_connection_t*)context;
  (void)stream;

  switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      /* Feed received data into stream framer via actor message */
      QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
      uint32_t buffer_count = event->RECEIVE.BufferCount;
      for (uint32_t i = 0; i < buffer_count; i++) {
        buffer_t* data = buffer_create_from_pointer_copy(
          buffers[i].Buffer, buffers[i].Length);
        message_t msg;
        msg.type = WT_CONNECTION_DATA;
        msg.payload = data;
        msg.payload_destroy = (void (*)(void*))buffer_destroy;
        actor_send(&conn->actor, &msg);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
      /* Free the send context */
      if (event->SEND_COMPLETE.ClientContext != NULL) {
        send_complete_context_t* send_ctx =
          (send_complete_context_t*)event->SEND_COMPLETE.ClientContext;
        if (send_ctx->frame != NULL) {
          free(send_ctx->frame);
        }
        free(send_ctx);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
      message_t msg;
      msg.type = WT_CONNECTION_HANGUP;
      msg.payload = NULL;
      msg.payload_destroy = NULL;
      actor_send(&conn->actor, &msg);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

static void* _server_thread(void* arg) {
  wt_transport_t* transport = (wt_transport_t*)arg;
  platform_thread_setup_stack();

  /* Open MsQuic registration */
  QUIC_REGISTRATION_CONFIG reg_config = {0};
  reg_config.AppName = "offs-wt";
  reg_config.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;
  QUIC_STATUS status = transport->msquic->RegistrationOpen(&reg_config, &transport->registration);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "wt_transport: RegistrationOpen failed: 0x%x\n", status);
    atomic_store(&transport->running, 0);
    return NULL;
  }

  /* Open configuration */
  QUIC_SETTINGS settings = {0};
  settings.PeerUnidiStreamCount = 0;
  settings.PeerBidiStreamCount = 1;
  settings.IsSet.PeerUnidiStreamCount = TRUE;
  settings.IsSet.PeerBidiStreamCount = TRUE;

  QUIC_BUFFER alpn = { sizeof("offs") - 1, (uint8_t*)"offs" };

  status = transport->msquic->ConfigurationOpen(
    transport->registration, &alpn, 1, &settings, sizeof(settings), NULL, &transport->configuration);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "wt_transport: ConfigurationOpen failed: 0x%x\n", status);
    transport->msquic->RegistrationClose(transport->registration);
    transport->registration = NULL;
    atomic_store(&transport->running, 0);
    return NULL;
  }

  /* Load credentials */
  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  if (transport->cert_path != NULL && transport->key_path != NULL) {
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &(QUIC_CERTIFICATE_FILE){
      .CertificateFile = transport->cert_path,
      .PrivateKeyFile = transport->key_path
    };
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NONE;
  } else {
    /* Self-signed / insecure mode for testing */
    cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }

  status = transport->msquic->ConfigurationLoadCredential(transport->configuration, &cred_config);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "wt_transport: ConfigurationLoadCredential failed: 0x%x\n", status);
    transport->msquic->ConfigurationClose(transport->configuration);
    transport->configuration = NULL;
    transport->msquic->RegistrationClose(transport->registration);
    transport->registration = NULL;
    atomic_store(&transport->running, 0);
    return NULL;
  }

  /* Open and start listener */
  status = transport->msquic->ListenerOpen(
    transport->registration, _wt_listener_callback, transport, &transport->listener);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "wt_transport: ListenerOpen failed: 0x%x\n", status);
    transport->msquic->ConfigurationClose(transport->configuration);
    transport->configuration = NULL;
    transport->msquic->RegistrationClose(transport->registration);
    transport->registration = NULL;
    atomic_store(&transport->running, 0);
    return NULL;
  }

  QUIC_ADDR addr = {0};
  QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_INET);
  QuicAddrSetPort(&addr, transport->port);

  status = transport->msquic->ListenerStart(transport->listener, &alpn, 1, &addr);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "wt_transport: ListenerStart failed: 0x%x\n", status);
    transport->msquic->ListenerClose(transport->listener);
    transport->listener = NULL;
    transport->msquic->ConfigurationClose(transport->configuration);
    transport->configuration = NULL;
    transport->msquic->RegistrationClose(transport->registration);
    transport->registration = NULL;
    atomic_store(&transport->running, 0);
    return NULL;
  }

  atomic_store(&transport->listening, 1);

  while (atomic_load(&transport->running)) {
    _destroy_stack_drain(transport);
    pd_loop_run_once(transport->loop, 100);
  }

  transport->msquic->ListenerStop(transport->listener);
  transport->msquic->ListenerClose(transport->listener);
  transport->listener = NULL;
  pd_loop_stop(transport->loop);

  return NULL;
}

void wt_transport_start(wt_transport_t* transport) {
  atomic_store(&transport->running, 1);
  transport->thread = platform_thread_create(_server_thread, transport);
}

void wt_transport_stop(wt_transport_t* transport) {
  atomic_store(&transport->running, 0);
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);
}

#else /* !HAS_MSQUIC */

/* Stubs when MsQuic is not available */
#include <stddef.h>
#include "wt_transport.h"

wt_transport_t* wt_transport_create(scheduler_pool_t* pool,
                                      block_cache_t* bc,
                                      ofd_cache_t* ofd_cache,
                                      tuple_cache_t* tc,
                                      const char* host,
                                      uint16_t port,
                                      const char* cert_path,
                                      const char* key_path,
                                      size_t max_connections,
                                      const char* api_key_hash) {
  (void)pool; (void)bc; (void)ofd_cache; (void)tc;
  (void)host; (void)port; (void)cert_path; (void)key_path; (void)max_connections; (void)api_key_hash;
  return NULL;
}

void wt_transport_destroy(wt_transport_t* transport) {
  (void)transport;
}

void wt_transport_start(wt_transport_t* transport) {
  (void)transport;
}

void wt_transport_stop(wt_transport_t* transport) {
  (void)transport;
}

#endif /* HAS_MSQUIC */