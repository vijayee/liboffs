//
// Created by victor on 5/16/26.
//

#include "relay_client.h"
#include "msquic_singleton.h"
#include "wire.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <cbor.h>

#ifdef HAS_MSQUIC

#include <msquic.h>
#include <string.h>
#include <poll-dancer/poll-dancer.h>
#include <arpa/inet.h>

// --- Send context: wraps framed data for QUIC StreamSend completion ---

typedef struct relay_send_context_t {
  QUIC_BUFFER buffer;
  uint8_t* framed_data;
} relay_send_context_t;

static void _relay_send_complete(void* context) {
  relay_send_context_t* send_context = (relay_send_context_t*)context;
  if (send_context != NULL) {
    free(send_context->framed_data);
    free(send_context);
  }
}

// --- Destroy stack for deferred pd_watcher cleanup ---

static void _destroy_stack_init(relay_client_t* client) {
  platform_lock_init(&client->destroy_lock);
  client->destroy_head = NULL;
}

static void _destroy_stack_push(relay_client_t* client, pd_watcher_t* watcher) {
  relay_client_destroy_node_t* node = get_clear_memory(sizeof(relay_client_destroy_node_t));
  if (node == NULL) {
    log_error("relay_client: failed to allocate destroy stack node");
    pd_watcher_destroy(watcher);
    return;
  }
  node->watcher = watcher;
  platform_lock(&client->destroy_lock);
  node->next = client->destroy_head;
  client->destroy_head = node;
  platform_unlock(&client->destroy_lock);
  pd_loop_async_send(client->loop, NULL);
}

static void _destroy_stack_drain(relay_client_t* client) {
  relay_client_destroy_node_t* node;
  platform_lock(&client->destroy_lock);
  node = client->destroy_head;
  client->destroy_head = NULL;
  platform_unlock(&client->destroy_lock);
  while (node != NULL) {
    relay_client_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(relay_client_t* client) {
  _destroy_stack_drain(client);
  platform_lock_destroy(&client->destroy_lock);
}

// --- Send helper: frame CBOR data and StreamSend ---

static QUIC_STATUS _relay_client_send_on_stream(
    relay_client_t* client, const uint8_t* data, size_t data_len) {
  size_t framed_len = 0;
  uint8_t* framed = stream_frame_encode(data, data_len, &framed_len);
  if (framed == NULL) {
    log_error("relay_client: failed to frame message for stream send");
    return QUIC_STATUS_OUT_OF_MEMORY;
  }

  relay_send_context_t* send_context = get_clear_memory(sizeof(relay_send_context_t));
  if (send_context == NULL) {
    free(framed);
    return QUIC_STATUS_OUT_OF_MEMORY;
  }
  send_context->framed_data = framed;
  send_context->buffer.Buffer = framed;
  send_context->buffer.Length = (uint32_t)framed_len;

  QUIC_STATUS status = client->msquic->StreamSend(
      client->stream, &send_context->buffer, 1, QUIC_SEND_FLAG_NONE, send_context);
  if (QUIC_FAILED(status)) {
    log_error("relay_client: StreamSend failed: 0x%x", status);
    free(framed);
    free(send_context);
  }
  return status;
}

// --- Process a complete framed message from the relay server ---

static void _relay_client_process_message(
    relay_client_t* client, uint8_t* msg_data, size_t msg_len) {
  struct cbor_load_result load_result;
  cbor_item_t* cbor = cbor_load(msg_data, msg_len, &load_result);
  if (cbor == NULL) {
    log_error("relay_client: failed to parse CBOR message");
    return;
  }

  uint8_t msg_type = wire_get_type(cbor);
  switch (msg_type) {
    case WIRE_ADDR_RESPONSE: {
      wire_addr_response_t response;
      memset(&response, 0, sizeof(response));
      if (wire_addr_response_decode(cbor, &response) == 0) {
        client->local_endpoint_id = response.endpoint_id;
        log_info("relay_client: received ADDR_RESPONSE, endpoint_id=%u, addr=%u:%u",
                 response.endpoint_id, response.reflexive_addr, response.reflexive_port);
      } else {
        log_error("relay_client: failed to decode ADDR_RESPONSE");
      }
      break;
    }
    case WIRE_RELAY_RECEIVED: {
      wire_relay_received_t received;
      memset(&received, 0, sizeof(received));
      if (wire_relay_received_decode(cbor, &received) == 0) {
        // Create payload and actor_send NETWORK_RELAY_RECEIVED to network
        wire_relay_received_t* payload = get_clear_memory(sizeof(wire_relay_received_t));
        if (payload != NULL) {
          payload->src_endpoint_id = received.src_endpoint_id;
          payload->payload = received.payload;
          payload->payload_len = received.payload_len;
          // Transfer ownership of payload data — do not free received.payload
          message_t msg;
          memset(&msg, 0, sizeof(msg));
          msg.type = NETWORK_RELAY_RECEIVED;
          msg.payload = payload;
          msg.payload_destroy = (void (*)(void*))wire_relay_received_destroy;
          actor_send(&client->network->actor, &msg);
        } else {
          free(received.payload);
          log_error("relay_client: failed to allocate RELAY_RECEIVED payload");
        }
      } else {
        log_error("relay_client: failed to decode RELAY_RECEIVED");
      }
      break;
    }
    default:
      log_error("relay_client: unknown message type %u from relay server", msg_type);
      break;
  }
  cbor_decref(&cbor);
}

// --- QUIC stream callback ---

static QUIC_STATUS QUIC_API _relay_client_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  relay_client_t* client = (relay_client_t*)context;

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
        _relay_client_process_message(client, msg_data, msg_len);
        free(msg_data);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
      relay_send_context_t* send_context =
          (relay_send_context_t*)event->SEND_COMPLETE.ClientContext;
      _relay_send_complete(send_context);
      break;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
      log_info("relay_client: stream shutdown complete");
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// --- QUIC connection callback ---

static QUIC_STATUS QUIC_API _relay_client_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
  relay_client_t* client = (relay_client_t*)context;

  switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
      log_info("relay_client: connected to relay server");

      // Open a bidirectional stream for relay communication
      HQUIC stream = NULL;
      QUIC_STATUS status = client->msquic->StreamOpen(
          connection, QUIC_STREAM_OPEN_FLAG_NONE,
          _relay_client_stream_callback, client, &stream);
      if (QUIC_FAILED(status)) {
        log_error("relay_client: StreamOpen failed: 0x%x", status);
        client->msquic->ConnectionClose(connection);
        return QUIC_STATUS_SUCCESS;
      }

      client->stream = stream;

      status = client->msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
      if (QUIC_FAILED(status)) {
        log_error("relay_client: StreamStart failed: 0x%x", status);
        client->msquic->StreamClose(stream);
        client->stream = NULL;
        client->msquic->ConnectionClose(connection);
        return QUIC_STATUS_SUCCESS;
      }

      ATOMIC_STORE(&client->connected, 1);
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
      log_info("relay_client: connection shutdown complete");
      ATOMIC_STORE(&client->connected, 0);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// --- I/O thread ---

static void* _relay_client_thread(void* arg) {
  relay_client_t* client = (relay_client_t*)arg;
  platform_setup_thread_stack();
  while (ATOMIC_LOAD(&client->running)) {
    _destroy_stack_drain(client);
    pd_loop_run_once(client->loop, 100);
  }
  return NULL;
}

// --- Public API ---

relay_client_t* relay_client_create(network_t* network, scheduler_pool_t* pool) {
  relay_client_t* client = get_clear_memory(sizeof(relay_client_t));
  if (client == NULL) return NULL;

  client->network = network;
  client->pool = pool;
  client->running = ATOMIC_VAR_INIT(0);

  actor_init(&client->actor, client, relay_client_dispatch, pool);

  _destroy_stack_init(client);

  client->loop = pd_loop_create(NULL);
  if (client->loop == NULL) {
    _destroy_stack_destroy(client);
    actor_destroy(&client->actor);
    free(client);
    return NULL;
  }

  client->msquic = offs_msquic_open();
  if (client->msquic == NULL) {
    pd_loop_destroy(client->loop);
    _destroy_stack_destroy(client);
    actor_destroy(&client->actor);
    free(client);
    return NULL;
  }

  return client;
}

void relay_client_destroy(relay_client_t* client) {
  if (client == NULL) return;

  if (ATOMIC_LOAD(&client->running)) {
    relay_client_disconnect(client);
  }

  if (client->stream != NULL && client->msquic != NULL) {
    client->msquic->StreamClose(client->stream);
    client->stream = NULL;
  }
  if (client->connection != NULL && client->msquic != NULL) {
    client->msquic->ConnectionClose(client->connection);
    client->connection = NULL;
  }
  if (client->configuration != NULL && client->msquic != NULL) {
    client->msquic->ConfigurationClose(client->configuration);
    client->configuration = NULL;
  }
  if (client->registration != NULL && client->msquic != NULL) {
    client->msquic->RegistrationClose(client->registration);
    client->registration = NULL;
  }

  offs_msquic_close();

  if (client->framer != NULL) {
    stream_framer_destroy(client->framer);
    client->framer = NULL;
  }
  if (client->loop != NULL) {
    pd_loop_destroy(client->loop);
  }
  if (client->cert_path != NULL) { free(client->cert_path); client->cert_path = NULL; }
  if (client->key_path != NULL) { free(client->key_path); client->key_path = NULL; }
  _destroy_stack_destroy(client);
  actor_destroy(&client->actor);
  free(client);
}

int relay_client_connect(relay_client_t* client, const char* host, uint16_t port) {
  if (client == NULL) return -1;

  QUIC_STATUS status;

  // Open registration with "offs_relay" ALPN
  QUIC_REGISTRATION_CONFIG reg_config = {
    "offs_relay",
    QUIC_EXECUTION_PROFILE_LOW_LATENCY
  };
  if (QUIC_FAILED(status = client->msquic->RegistrationOpen(
          &reg_config, &client->registration))) {
    log_error("relay_client: RegistrationOpen failed: 0x%x", status);
    return -1;
  }

  QUIC_BUFFER alpn = { sizeof("offs_relay") - 1, (uint8_t*)"offs_relay" };

  // Open configuration
  QUIC_SETTINGS settings = {0};
  settings.PeerBidiStreamCount = 1;
  settings.IsSet.PeerBidiStreamCount = TRUE;

  if (QUIC_FAILED(status = client->msquic->ConfigurationOpen(
          client->registration,
          &alpn,
          1,
          &settings,
          sizeof(settings),
          NULL,
          &client->configuration))) {
    log_error("relay_client: ConfigurationOpen failed: 0x%x", status);
    client->msquic->RegistrationClose(client->registration);
    client->registration = NULL;
    return -1;
  }

  // Load credentials — use cert/key if configured, otherwise self-signed/no-cert
  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  QUIC_CERTIFICATE_FILE cert_file = {0};
  if (client->cert_path && client->key_path) {
    cert_file.CertificateFile = client->cert_path;
    cert_file.PrivateKeyFile = client->key_path;
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  } else {
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }

  if (QUIC_FAILED(status = client->msquic->ConfigurationLoadCredential(
          client->configuration,
          &cred_config))) {
    log_error("relay_client: ConfigurationLoadCredential failed: 0x%x", status);
    client->msquic->ConfigurationClose(client->configuration);
    client->configuration = NULL;
    client->msquic->RegistrationClose(client->registration);
    client->registration = NULL;
    return -1;
  }

  // Start connection
  if (QUIC_FAILED(status = client->msquic->ConnectionOpen(
          client->registration,
          _relay_client_connection_callback,
          client,
          &client->connection))) {
    log_error("relay_client: ConnectionOpen failed: 0x%x", status);
    client->msquic->ConfigurationClose(client->configuration);
    client->configuration = NULL;
    client->msquic->RegistrationClose(client->registration);
    client->registration = NULL;
    return -1;
  }

  // Set configuration on connection
  client->msquic->ConnectionSetConfiguration(client->connection, client->configuration);

  // Create framer for stream data — must be ready before ConnectionStart
  // since data can arrive asynchronously once connected
  client->framer = stream_framer_create();
  if (client->framer == NULL) {
    log_error("relay_client: failed to create stream framer");
    client->msquic->ConnectionClose(client->connection);
    client->connection = NULL;
    client->msquic->ConfigurationClose(client->configuration);
    client->configuration = NULL;
    client->msquic->RegistrationClose(client->registration);
    client->registration = NULL;
    return -1;
  }

  // Start connection to relay server
  QUIC_ADDR server_addr;
  QuicAddrSetFamily(&server_addr, QUIC_ADDRESS_FAMILY_INET);
  if (host != NULL) {
    QuicAddrFromString(host, port, &server_addr);
  } else {
    QuicAddrSetPort(&server_addr, port);
  }

  if (QUIC_FAILED(status = client->msquic->ConnectionStart(
          client->connection,
          client->configuration,
          QUIC_ADDRESS_FAMILY_INET,
          host != NULL ? host : "127.0.0.1",
          port))) {
    log_error("relay_client: ConnectionStart failed: 0x%x", status);
    client->msquic->ConnectionClose(client->connection);
    client->connection = NULL;
    client->msquic->ConfigurationClose(client->configuration);
    client->configuration = NULL;
    client->msquic->RegistrationClose(client->registration);
    client->registration = NULL;
    return -1;
  }

  // Store relay address
  memset(&client->relay_addr, 0, sizeof(client->relay_addr));
  if (host != NULL) {
    struct sockaddr_in* addr_in = (struct sockaddr_in*)&client->relay_addr;
    addr_in->sin_family = AF_INET;
    inet_pton(AF_INET, host, &addr_in->sin_addr);
    addr_in->sin_port = htons(port);
  }

  // Start I/O thread
  ATOMIC_STORE(&client->running, 1);
  pthread_create(&client->thread, NULL, _relay_client_thread, client);

  log_info("relay_client: connecting to %s:%u", host != NULL ? host : "127.0.0.1", port);
  return 0;
}

void relay_client_disconnect(relay_client_t* client) {
  if (client == NULL) return;
  ATOMIC_STORE(&client->running, 0);
  pd_loop_async_send(client->loop, NULL);
  pthread_join(client->thread, NULL);

  if (client->stream != NULL && client->msquic != NULL) {
    client->msquic->StreamClose(client->stream);
    client->stream = NULL;
  }
  if (client->connection != NULL && client->msquic != NULL) {
    client->msquic->ConnectionClose(client->connection);
    client->connection = NULL;
  }
  ATOMIC_STORE(&client->connected, 0);
}

void relay_client_dispatch(void* state, message_t* msg) {
  relay_client_t* client = (relay_client_t*)state;
  if (msg == NULL) return;
  if (!ATOMIC_LOAD(&client->running) || !ATOMIC_LOAD(&client->connected)) {
    log_error("relay_client: dispatch called while not connected");
    if (msg->payload_destroy != NULL && msg->payload != NULL) {
      msg->payload_destroy(msg->payload);
    }
    return;
  }

  switch (msg->type) {
    case RELAY_CLIENT_SEND: {
      wire_relay_send_t* relay_send = (wire_relay_send_t*)msg->payload;
      if (relay_send == NULL) break;

      cbor_item_t* cbor = wire_relay_send_encode(relay_send);
      if (cbor == NULL) {
        log_error("relay_client: failed to encode RELAY_SEND CBOR");
        break;
      }

      size_t cbor_len = 0;
      unsigned char* cbor_data = NULL;
      cbor_serialize_alloc(cbor, &cbor_data, &cbor_len);
      cbor_decref(&cbor);

      if (cbor_data == NULL) {
        log_error("relay_client: failed to serialize RELAY_SEND CBOR");
        break;
      }

      QUIC_STATUS send_status = _relay_client_send_on_stream(client, cbor_data, cbor_len);
      if (QUIC_FAILED(send_status)) {
        log_error("relay_client: failed to send RELAY_SEND on stream");
      }
      free(cbor_data);
      break;
    }
    case RELAY_CLIENT_ADDR_REQUEST: {
      wire_addr_request_t* addr_request = (wire_addr_request_t*)msg->payload;
      if (addr_request == NULL) break;

      cbor_item_t* cbor = wire_addr_request_encode(addr_request);
      if (cbor == NULL) {
        log_error("relay_client: failed to encode ADDR_REQUEST CBOR");
        break;
      }

      size_t cbor_len = 0;
      unsigned char* cbor_data = NULL;
      cbor_serialize_alloc(cbor, &cbor_data, &cbor_len);
      cbor_decref(&cbor);

      if (cbor_data == NULL) {
        log_error("relay_client: failed to serialize ADDR_REQUEST CBOR");
        break;
      }

      QUIC_STATUS send_status = _relay_client_send_on_stream(client, cbor_data, cbor_len);
      if (QUIC_FAILED(send_status)) {
        log_error("relay_client: failed to send ADDR_REQUEST on stream");
      }
      free(cbor_data);
      break;
    }
    default:
      break;
  }

  if (msg->payload_destroy != NULL && msg->payload != NULL) {
    msg->payload_destroy(msg->payload);
  }
}

#else // !HAS_MSQUIC — stub implementations

#include <stdlib.h>

relay_client_t* relay_client_create(network_t* network, scheduler_pool_t* pool) {
  (void)network;
  (void)pool;
  return NULL;
}

void relay_client_destroy(relay_client_t* client) {
  (void)client;
}

int relay_client_connect(relay_client_t* client, const char* host, uint16_t port) {
  (void)client;
  (void)host;
  (void)port;
  return -1;
}

void relay_client_disconnect(relay_client_t* client) {
  (void)client;
}

void relay_client_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
}

#endif // HAS_MSQUIC