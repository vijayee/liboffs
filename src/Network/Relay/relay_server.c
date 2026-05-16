//
// Created by victor on 5/16/26.
//

#include "relay_server.h"
#include "../msquic_singleton.h"
#include "../stream_framer.h"
#include "../wire.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"

#ifdef HAS_MSQUIC

#include <msquic.h>
#include <string.h>
#include <poll-dancer/poll-dancer.h>
#include <arpa/inet.h>

// --- Stream context: wraps server + client pointers for QUIC callbacks ---

typedef struct relay_stream_context_t {
  relay_server_t* server;
  relay_client_entry_t* client;
} relay_stream_context_t;

// --- Destroy stack for deferred pd_watcher cleanup ---

static void _destroy_stack_init(relay_server_t* server) {
  platform_lock_init(&server->destroy_lock);
  server->destroy_head = NULL;
}

static void _destroy_stack_push(relay_server_t* server, pd_watcher_t* watcher) {
  relay_destroy_node_t* node = get_clear_memory(sizeof(relay_destroy_node_t));
  if (node == NULL) {
    log_error("relay: failed to allocate destroy stack node");
    pd_watcher_destroy(watcher);
    return;
  }
  node->watcher = watcher;
  platform_lock(&server->destroy_lock);
  node->next = server->destroy_head;
  server->destroy_head = node;
  platform_unlock(&server->destroy_lock);
  pd_loop_async_send(server->loop, NULL);
}

static void _destroy_stack_drain(relay_server_t* server) {
  relay_destroy_node_t* node;
  platform_lock(&server->destroy_lock);
  node = server->destroy_head;
  server->destroy_head = NULL;
  platform_unlock(&server->destroy_lock);
  while (node != NULL) {
    relay_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(relay_server_t* server) {
  _destroy_stack_drain(server);
  platform_lock_destroy(&server->destroy_lock);
}

// --- Client management ---

static relay_client_entry_t* _relay_find_client_by_endpoint(
    relay_server_t* server, uint32_t endpoint_id) {
  for (size_t index = 0; index < RELAY_MAX_CLIENTS; index++) {
    if (server->clients[index].active &&
        server->clients[index].endpoint_id == endpoint_id) {
      return &server->clients[index];
    }
  }
  return NULL;
}

static void _relay_remove_client(relay_server_t* server, relay_client_entry_t* client) {
  if (client == NULL) return;
  client->active = 0;
  if (client->framer != NULL) {
    stream_framer_destroy(client->framer);
    client->framer = NULL;
  }
  client->connection = NULL;
  client->stream = NULL;
  client->endpoint_id = 0;
}

// --- Send helper: allocates QUIC_BUFFER + framed data, sends on stream ---

typedef struct relay_send_context_t {
  QUIC_BUFFER buffer;
  uint8_t* framed_data;
} relay_send_context_t;

static void _relay_send_complete(void* client_context) {
  relay_send_context_t* ctx = (relay_send_context_t*)client_context;
  if (ctx != NULL) {
    free(ctx->framed_data);
    free(ctx);
  }
}

static QUIC_STATUS _relay_send_on_stream(
    relay_server_t* server, HQUIC stream, const uint8_t* data, size_t data_len) {
  size_t framed_len = 0;
  uint8_t* framed = stream_frame_encode(data, data_len, &framed_len);
  if (framed == NULL) {
    log_error("relay: failed to frame message for stream send");
    return QUIC_STATUS_OUT_OF_MEMORY;
  }

  relay_send_context_t* send_ctx = get_clear_memory(sizeof(relay_send_context_t));
  if (send_ctx == NULL) {
    free(framed);
    return QUIC_STATUS_OUT_OF_MEMORY;
  }
  send_ctx->framed_data = framed;
  send_ctx->buffer.Buffer = framed;
  send_ctx->buffer.Length = (uint32_t)framed_len;

  QUIC_STATUS status = server->msquic->StreamSend(
      stream, &send_ctx->buffer, 1, QUIC_SEND_FLAG_NONE, send_ctx);
  if (QUIC_FAILED(status)) {
    log_error("relay: StreamSend failed: 0x%x", status);
    free(framed);
    free(send_ctx);
  }
  return status;
}

// --- Handle ADDR_REQUEST from a client ---

static void _relay_handle_addr_request(
    relay_server_t* server, relay_client_entry_t* client,
    HQUIC connection, uint64_t message_id) {
  QUIC_ADDR remote_addr;
  uint32_t remote_addr_len = sizeof(remote_addr);
  QUIC_STATUS status = server->msquic->GetParam(
      connection, QUIC_PARAM_CONN_REMOTE_ADDRESS,
      &remote_addr_len, &remote_addr);
  if (QUIC_FAILED(status)) {
    log_error("relay: GetParam(CONN_REMOTE_ADDRESS) failed: 0x%x", status);
    return;
  }

  uint32_t reflexive_addr = 0;
  uint16_t reflexive_port = 0;
  QUIC_ADDRESS_FAMILY family = QuicAddrGetFamily(&remote_addr);
  if (family == QUIC_ADDRESS_FAMILY_INET) {
    reflexive_addr = ntohl(remote_addr.Ipv4.sin_addr.s_addr);
    reflexive_port = ntohs(remote_addr.Ipv4.sin_port);
  } else if (family == QUIC_ADDRESS_FAMILY_INET6) {
    reflexive_addr = ntohl((uint32_t)remote_addr.Ipv6.sin6_addr.s6_addr[15] |
                           ((uint32_t)remote_addr.Ipv6.sin6_addr.s6_addr[14] << 8) |
                           ((uint32_t)remote_addr.Ipv6.sin6_addr.s6_addr[13] << 16) |
                           ((uint32_t)remote_addr.Ipv6.sin6_addr.s6_addr[12] << 24));
    reflexive_port = ntohs(remote_addr.Ipv6.sin6_port);
  }

  wire_addr_response_t response;
  memset(&response, 0, sizeof(response));
  response.message_id = message_id;
  response.endpoint_id = client->endpoint_id;
  response.reflexive_addr = reflexive_addr;
  response.reflexive_port = reflexive_port;

  cbor_item_t* cbor = wire_addr_response_encode(&response);
  if (cbor == NULL) {
    log_error("relay: failed to encode ADDR_RESPONSE CBOR");
    return;
  }

  size_t cbor_len = 0;
  unsigned char* cbor_data = NULL;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_len);
  cbor_decref(&cbor);

  if (cbor_data == NULL) {
    log_error("relay: failed to serialize ADDR_RESPONSE CBOR");
    return;
  }

  QUIC_STATUS send_status = _relay_send_on_stream(
      server, (HQUIC)client->stream, cbor_data, cbor_len);
  if (QUIC_FAILED(send_status)) {
    log_error("relay: failed to send ADDR_RESPONSE on stream");
  }
  free(cbor_data);
}

// --- Handle RELAY_SEND from a client ---

static void _relay_handle_relay_send(
    relay_server_t* server, relay_client_entry_t* src_client,
    wire_relay_send_t* relay_send) {
  platform_lock(&server->clients_lock);
  relay_client_entry_t* dest_client = _relay_find_client_by_endpoint(
      server, relay_send->dest_endpoint_id);
  if (dest_client == NULL) {
    platform_unlock(&server->clients_lock);
    log_error("relay: dest endpoint %u not found", relay_send->dest_endpoint_id);
    return;
  }

  wire_relay_received_t received;
  memset(&received, 0, sizeof(received));
  received.src_endpoint_id = relay_send->src_endpoint_id;
  received.payload = relay_send->payload;
  received.payload_len = relay_send->payload_len;

  cbor_item_t* cbor = wire_relay_received_encode(&received);
  if (cbor == NULL) {
    platform_unlock(&server->clients_lock);
    log_error("relay: failed to encode RELAY_RECEIVED CBOR");
    return;
  }

  size_t cbor_len = 0;
  unsigned char* cbor_data = NULL;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_len);
  cbor_decref(&cbor);

  if (cbor_data == NULL) {
    platform_unlock(&server->clients_lock);
    log_error("relay: failed to serialize RELAY_RECEIVED CBOR");
    return;
  }

  QUIC_STATUS status = _relay_send_on_stream(
      server, (HQUIC)dest_client->stream, cbor_data, cbor_len);
  if (QUIC_FAILED(status)) {
    log_error("relay: failed to send RELAY_RECEIVED to endpoint %u",
              dest_client->endpoint_id);
  }
  platform_unlock(&server->clients_lock);
  free(cbor_data);
}

// --- Process a complete framed message from a client ---

static void _relay_process_message(
    relay_server_t* server, relay_client_entry_t* client,
    HQUIC connection, uint8_t* msg_data, size_t msg_len) {
  struct cbor_load_result load_result;
  cbor_item_t* cbor = cbor_load(msg_data, msg_len, &load_result);
  if (cbor == NULL) {
    log_error("relay: failed to parse CBOR message from endpoint %u",
              client->endpoint_id);
    return;
  }

  uint8_t msg_type = wire_get_type(cbor);
  switch (msg_type) {
    case WIRE_ADDR_REQUEST: {
      wire_addr_request_t request;
      memset(&request, 0, sizeof(request));
      if (wire_addr_request_decode(cbor, &request) == 0) {
        _relay_handle_addr_request(server, client, connection, request.message_id);
      } else {
        log_error("relay: failed to decode ADDR_REQUEST from endpoint %u",
                  client->endpoint_id);
      }
      break;
    }
    case WIRE_RELAY_SEND: {
      wire_relay_send_t relay_send;
      memset(&relay_send, 0, sizeof(relay_send));
      if (wire_relay_send_decode(cbor, &relay_send) == 0) {
        relay_send.src_endpoint_id = client->endpoint_id;
        _relay_handle_relay_send(server, client, &relay_send);
        free(relay_send.payload);
      } else {
        log_error("relay: failed to decode RELAY_SEND from endpoint %u",
                  client->endpoint_id);
      }
      break;
    }
    default:
      log_error("relay: unknown message type %u from endpoint %u",
                msg_type, client->endpoint_id);
      break;
  }
  cbor_decref(&cbor);
}

// --- QUIC stream callback ---

static QUIC_STATUS QUIC_API _relay_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  relay_stream_context_t* ctx = (relay_stream_context_t*)context;
  relay_server_t* server = ctx->server;
  relay_client_entry_t* client = ctx->client;

  switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      for (uint32_t index = 0; index < event->RECEIVE.BufferCount; index++) {
        const QUIC_BUFFER* buffer = &event->RECEIVE.Buffers[index];
        if (client->framer != NULL) {
          stream_framer_feed(client->framer, buffer->Buffer, buffer->Length);
        }
      }

      size_t msg_len = 0;
      uint8_t* msg_data = NULL;
      while ((msg_data = stream_framer_next(client->framer, &msg_len)) != NULL) {
        _relay_process_message(server, client, (HQUIC)client->connection, msg_data, msg_len);
        free(msg_data);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
      relay_send_context_t* send_ctx = (relay_send_context_t*)event->SEND_COMPLETE.ClientContext;
      _relay_send_complete(send_ctx);
      break;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
      free(ctx);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// --- QUIC connection callback ---

static QUIC_STATUS QUIC_API _relay_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
  relay_server_t* server = (relay_server_t*)context;

  switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
      platform_lock(&server->clients_lock);
      if (server->num_clients >= RELAY_MAX_CLIENTS) {
        platform_unlock(&server->clients_lock);
        log_error("relay: max clients reached, rejecting connection");
        server->msquic->ConnectionClose(connection);
        return QUIC_STATUS_SUCCESS;
      }

      // Find a free slot in the clients array
      size_t slot = server->num_clients;
      for (size_t index = 0; index < RELAY_MAX_CLIENTS; index++) {
        if (!server->clients[index].active) {
          slot = index;
          break;
        }
      }

      uint32_t endpoint_id = server->next_endpoint_id++;

      relay_stream_context_t* stream_ctx = get_clear_memory(sizeof(relay_stream_context_t));
      if (stream_ctx == NULL) {
        platform_unlock(&server->clients_lock);
        log_error("relay: failed to allocate stream context");
        server->msquic->ConnectionClose(connection);
        return QUIC_STATUS_SUCCESS;
      }

      relay_client_entry_t* client = &server->clients[slot];
      client->endpoint_id = endpoint_id;
      client->connection = (void*)connection;
      client->active = 1;
      client->framer = stream_framer_create();
      if (client->framer == NULL) {
        platform_unlock(&server->clients_lock);
        log_error("relay: failed to create stream framer");
        free(stream_ctx);
        client->active = 0;
        client->connection = NULL;
        server->msquic->ConnectionClose(connection);
        return QUIC_STATUS_SUCCESS;
      }

      stream_ctx->server = server;
      stream_ctx->client = client;

      HQUIC stream = NULL;
      QUIC_STATUS status = server->msquic->StreamOpen(
          connection, QUIC_STREAM_OPEN_FLAG_NONE,
          _relay_stream_callback, stream_ctx, &stream);
      if (QUIC_FAILED(status)) {
        log_error("relay: StreamOpen failed: 0x%x", status);
        stream_framer_destroy(client->framer);
        client->framer = NULL;
        client->active = 0;
        client->connection = NULL;
        platform_unlock(&server->clients_lock);
        free(stream_ctx);
        server->msquic->ConnectionClose(connection);
        return QUIC_STATUS_SUCCESS;
      }

      client->stream = (void*)stream;

      status = server->msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
      if (QUIC_FAILED(status)) {
        log_error("relay: StreamStart failed: 0x%x", status);
        server->msquic->StreamClose(stream);
        stream_framer_destroy(client->framer);
        client->framer = NULL;
        client->active = 0;
        client->connection = NULL;
        client->stream = NULL;
        platform_unlock(&server->clients_lock);
        free(stream_ctx);
        server->msquic->ConnectionClose(connection);
        return QUIC_STATUS_SUCCESS;
      }

      server->num_clients++;
      log_info("relay: client connected with endpoint_id=%u", endpoint_id);
      platform_unlock(&server->clients_lock);
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
      platform_lock(&server->clients_lock);
      for (size_t index = 0; index < RELAY_MAX_CLIENTS; index++) {
        if (server->clients[index].active &&
            server->clients[index].connection == (void*)connection) {
          uint32_t endpoint_id = server->clients[index].endpoint_id;
          if (server->clients[index].stream != NULL) {
            server->msquic->StreamClose((HQUIC)server->clients[index].stream);
          }
          _relay_remove_client(server, &server->clients[index]);
          if (server->num_clients > 0) {
            server->num_clients--;
          }
          log_info("relay: client endpoint_id=%u disconnected", endpoint_id);
          break;
        }
      }
      platform_unlock(&server->clients_lock);
      server->msquic->ConnectionClose(connection);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// --- QUIC listener callback ---

static QUIC_STATUS QUIC_API _relay_listener_callback(
    HQUIC listener_handle, void* context, QUIC_LISTENER_EVENT* event) {
  relay_server_t* server = (relay_server_t*)context;

  switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
      HQUIC connection = event->NEW_CONNECTION.Connection;
      server->msquic->SetCallbackHandler(
          connection,
          (void*)_relay_connection_callback,
          server);
      server->msquic->ConnectionSetConfiguration(
          connection,
          server->configuration);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// --- I/O thread ---

static void* _relay_server_thread(void* arg) {
  relay_server_t* server = (relay_server_t*)arg;
  platform_setup_thread_stack();
  while (ATOMIC_LOAD(&server->running)) {
    _destroy_stack_drain(server);
    pd_loop_run_once(server->loop, 100);
  }
  return NULL;
}

// --- Public API ---

relay_server_t* relay_server_create(scheduler_pool_t* pool) {
  relay_server_t* server = get_clear_memory(sizeof(relay_server_t));
  if (server == NULL) return NULL;

  server->pool = pool;
  server->running = ATOMIC_VAR_INIT(0);
  server->next_endpoint_id = 1;

  actor_init(&server->actor, server, relay_server_dispatch, pool);

  platform_lock_init(&server->clients_lock);
  _destroy_stack_init(server);

  server->loop = pd_loop_create(NULL);
  if (server->loop == NULL) {
    platform_lock_destroy(&server->clients_lock);
    _destroy_stack_destroy(server);
    actor_destroy(&server->actor);
    free(server);
    return NULL;
  }

  server->msquic = offs_msquic_open();
  if (server->msquic == NULL) {
    pd_loop_destroy(server->loop);
    platform_lock_destroy(&server->clients_lock);
    _destroy_stack_destroy(server);
    actor_destroy(&server->actor);
    free(server);
    return NULL;
  }

  return server;
}

void relay_server_destroy(relay_server_t* server) {
  if (server == NULL) return;

  if (ATOMIC_LOAD(&server->running)) {
    relay_server_stop(server);
  }

  for (size_t index = 0; index < RELAY_MAX_CLIENTS; index++) {
    if (server->clients[index].active) {
      if (server->clients[index].stream != NULL && server->msquic != NULL) {
        server->msquic->StreamClose((HQUIC)server->clients[index].stream);
      }
      _relay_remove_client(server, &server->clients[index]);
    }
  }

  if (server->listener != NULL) {
    server->msquic->ListenerClose(server->listener);
  }
  if (server->configuration != NULL) {
    server->msquic->ConfigurationClose(server->configuration);
  }
  if (server->registration != NULL) {
    server->msquic->RegistrationClose(server->registration);
  }
  offs_msquic_close();
  if (server->loop != NULL) {
    pd_loop_destroy(server->loop);
  }
  if (server->cert_path != NULL) { free(server->cert_path); server->cert_path = NULL; }
  if (server->key_path != NULL) { free(server->key_path); server->key_path = NULL; }
  _destroy_stack_destroy(server);
  platform_lock_destroy(&server->clients_lock);
  actor_destroy(&server->actor);
  free(server);
}

int relay_server_start(relay_server_t* server, const char* host, uint16_t port) {
  if (server == NULL) return -1;

  QUIC_STATUS status;

  QUIC_REGISTRATION_CONFIG reg_config = {
    "offs_relay",
    QUIC_EXECUTION_PROFILE_LOW_LATENCY
  };
  if (QUIC_FAILED(status = server->msquic->RegistrationOpen(
          &reg_config, &server->registration))) {
    log_error("relay: RegistrationOpen failed: 0x%x", status);
    return -1;
  }

  QUIC_BUFFER alpn = { sizeof("offs_relay") - 1, (uint8_t*)"offs_relay" };

  QUIC_SETTINGS settings = {0};
  settings.PeerBidiStreamCount = 1;
  settings.IsSet.PeerBidiStreamCount = TRUE;

  if (QUIC_FAILED(status = server->msquic->ConfigurationOpen(
          server->registration,
          &alpn,
          1,
          &settings,
          sizeof(settings),
          NULL,
          &server->configuration))) {
    log_error("relay: ConfigurationOpen failed: 0x%x", status);
    server->msquic->RegistrationClose(server->registration);
    server->registration = NULL;
    return -1;
  }

  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  QUIC_CERTIFICATE_FILE cert_file = {0};
  if (server->cert_path && server->key_path) {
    cert_file.CertificateFile = server->cert_path;
    cert_file.PrivateKeyFile = server->key_path;
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  } else {
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }

  if (QUIC_FAILED(status = server->msquic->ConfigurationLoadCredential(
          server->configuration,
          &cred_config))) {
    log_error("relay: ConfigurationLoadCredential failed: 0x%x", status);
    server->msquic->ConfigurationClose(server->configuration);
    server->configuration = NULL;
    server->msquic->RegistrationClose(server->registration);
    server->registration = NULL;
    return -1;
  }

  if (QUIC_FAILED(status = server->msquic->ListenerOpen(
          server->registration,
          _relay_listener_callback,
          server,
          &server->listener))) {
    log_error("relay: ListenerOpen failed: 0x%x", status);
    server->msquic->ConfigurationClose(server->configuration);
    server->configuration = NULL;
    server->msquic->RegistrationClose(server->registration);
    server->registration = NULL;
    return -1;
  }

  QUIC_ADDR addr;
  QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
  if (host != NULL) {
    QuicAddrFromString(host, port, &addr);
  } else {
    QuicAddrSetPort(&addr, port);
  }

  if (QUIC_FAILED(status = server->msquic->ListenerStart(
          server->listener,
          &alpn,
          1,
          &addr))) {
    log_error("relay: ListenerStart failed: 0x%x", status);
    server->msquic->ListenerClose(server->listener);
    server->listener = NULL;
    server->msquic->ConfigurationClose(server->configuration);
    server->configuration = NULL;
    server->msquic->RegistrationClose(server->registration);
    server->registration = NULL;
    return -1;
  }

  server->listen_port = port;

  ATOMIC_STORE(&server->running, 1);
  if (pthread_create(&server->thread, NULL, _relay_server_thread, server) != 0) {
    ATOMIC_STORE(&server->running, 0);
    log_error("relay: pthread_create failed");
    return -1;
  }

  log_info("relay: listening on port %u", port);
  return 0;
}

void relay_server_stop(relay_server_t* server) {
  if (server == NULL) return;
  ATOMIC_STORE(&server->running, 0);
  pd_loop_async_send(server->loop, NULL);
  pthread_join(server->thread, NULL);
  if (server->listener != NULL) {
    server->msquic->ListenerStop(server->listener);
  }
}

void relay_server_dispatch(void* state, message_t* msg) {
  relay_server_t* server = (relay_server_t*)state;
  if (msg == NULL) return;
  (void)server;
}

#else // !HAS_MSQUIC — stub implementations

#include <stdlib.h>

relay_server_t* relay_server_create(scheduler_pool_t* pool) {
  (void)pool;
  return NULL;
}

void relay_server_destroy(relay_server_t* server) {
  (void)server;
}

int relay_server_start(relay_server_t* server, const char* host, uint16_t port) {
  (void)server;
  (void)host;
  (void)port;
  return -1;
}

void relay_server_stop(relay_server_t* server) {
  (void)server;
}

void relay_server_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
}

#endif // HAS_MSQUIC