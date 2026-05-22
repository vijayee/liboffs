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
#include <unistd.h>

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
  client->destroy_lock = platform_mutex_create();
  client->destroy_head = NULL;
}

static void PLATFORM_UNUSED _destroy_stack_push(relay_client_t* client, pd_watcher_t* watcher) {
  relay_client_destroy_node_t* node = get_clear_memory(sizeof(relay_client_destroy_node_t));
  if (node == NULL) {
    log_error("relay_client: failed to allocate destroy stack node");
    pd_watcher_destroy(watcher);
    return;
  }
  node->watcher = watcher;
  platform_mutex_lock(client->destroy_lock);
  node->next = client->destroy_head;
  client->destroy_head = node;
  platform_mutex_unlock(client->destroy_lock);
  pd_loop_async_send(client->loop, NULL);
}

static void _destroy_stack_drain(relay_client_t* client) {
  relay_client_destroy_node_t* node;
  platform_mutex_lock(client->destroy_lock);
  node = client->destroy_head;
  client->destroy_head = NULL;
  platform_mutex_unlock(client->destroy_lock);
  while (node != NULL) {
    relay_client_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(relay_client_t* client) {
  _destroy_stack_drain(client);
  platform_mutex_destroy(client->destroy_lock);
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
  (void)stream;
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
      client->retry_count = 0;  // reset retry counter on successful connection

      // Open a bidirectional stream for relay communication
      HQUIC stream = NULL;
      QUIC_STATUS status = client->msquic->StreamOpen(
          connection, QUIC_STREAM_OPEN_FLAG_NONE,
          _relay_client_stream_callback, client, &stream);
      if (QUIC_FAILED(status)) {
        log_error("relay_client: StreamOpen failed: 0x%x", status);
        client->msquic->ConnectionClose(connection);
        client->connection = NULL;
        return QUIC_STATUS_SUCCESS;
      }

      client->stream = stream;

      status = client->msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
      if (QUIC_FAILED(status)) {
        log_error("relay_client: StreamStart failed: 0x%x", status);
        client->msquic->StreamClose(stream);
        client->stream = NULL;
        client->msquic->ConnectionClose(connection);
        client->connection = NULL;
        return QUIC_STATUS_SUCCESS;
      }

      ATOMIC_STORE(&client->connected, 1);

      // Send ADDR_REQUEST to learn our endpoint ID from the relay server.
      wire_addr_request_t* addr_req = get_clear_memory(sizeof(wire_addr_request_t));
      if (addr_req != NULL) {
        message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = RELAY_CLIENT_ADDR_REQUEST;
        msg.payload = addr_req;
        msg.payload_destroy = free;
        actor_send(&client->actor, &msg);
      }
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: {
      QUIC_STATUS shutdown_status = event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
      log_error("relay_client: connection shutdown by transport, "
               "status=0x%x, error_code=0x%lx",
               shutdown_status,
               (unsigned long)event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
      // Schedule a retry if the error is UNREACHABLE and we haven't exhausted retries.
      // MsQuic can return UNREACHABLE transiently on localhost when multiple QUIC
      // connections start concurrently.
      if (shutdown_status == QUIC_STATUS_UNREACHABLE &&
          !client->shutdown_pending &&
          client->retry_count < RELAY_CLIENT_MAX_RETRIES) {
        client->retry_count++;
        log_info("relay_client: scheduling retry %u/%u",
                 client->retry_count, RELAY_CLIENT_MAX_RETRIES);
      }
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER: {
      log_error("relay_client: connection shutdown by peer, "
               "error_code=0x%lx",
               (unsigned long)event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
      log_info("relay_client: connection shutdown complete, "
               "handshake_completed=%u, peer_ack=%u, app_close_in_progress=%u",
               event->SHUTDOWN_COMPLETE.HandshakeCompleted,
               event->SHUTDOWN_COMPLETE.PeerAcknowledgedShutdown,
               event->SHUTDOWN_COMPLETE.AppCloseInProgress);
      ATOMIC_STORE(&client->connected, 0);

      // If a retry was scheduled (retry_count was incremented by
      // SHUTDOWN_INITIATED_BY_TRANSPORT for UNREACHABLE), attempt reconnect.
      // We defer the retry to the I/O thread via actor_send to avoid blocking
      // the MsQuic worker thread and to prevent thread leaks from spawning
      // a new I/O thread while the old one is still running.
      if (client->retry_count > 0 && client->retry_count <= RELAY_CLIENT_MAX_RETRIES &&
          !client->shutdown_pending && ATOMIC_LOAD(&client->running)) {

        // Clean up the current QUIC connection resources before retrying.
        if (client->stream != NULL) {
          client->msquic->StreamClose(client->stream);
          client->stream = NULL;
        }
        client->msquic->ConnectionClose(client->connection);
        client->connection = NULL;
        if (client->configuration != NULL) {
          client->msquic->ConfigurationClose(client->configuration);
          client->configuration = NULL;
        }
        if (client->owns_registration && client->registration != NULL) {
          client->msquic->RegistrationClose(client->registration);
          client->registration = NULL;
        }
        if (client->framer != NULL) {
          stream_framer_destroy(client->framer);
          client->framer = NULL;
        }

        // Defer reconnect to the I/O thread — it will apply exponential
        // backoff and call relay_client_connect from a safe context.
        unsigned long delay_ms = RELAY_CLIENT_RETRY_DELAY_MS * (1 << (client->retry_count - 1));
        log_info("relay_client: scheduling retry (attempt %u/%u) to %s:%u in %lums",
                 client->retry_count, RELAY_CLIENT_MAX_RETRIES,
                 client->relay_host ? client->relay_host : "127.0.0.1",
                 client->relay_port, delay_ms);

        relay_retry_payload_t* retry_payload = get_clear_memory(sizeof(relay_retry_payload_t));
        if (retry_payload != NULL) {
          retry_payload->delay_ms = delay_ms;
          message_t msg;
          memset(&msg, 0, sizeof(msg));
          msg.type = RELAY_CLIENT_RETRY;
          msg.payload = retry_payload;
          msg.payload_destroy = free;
          actor_send(&client->actor, &msg);
        }
      }
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
  platform_thread_setup_stack();
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

  // Mark as shutting down so retry logic doesn't fire during cleanup
  client->shutdown_pending = 1;

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
  if (client->registration != NULL && client->msquic != NULL && client->owns_registration) {
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
  if (client->relay_host != NULL) { free(client->relay_host); client->relay_host = NULL; }
  _destroy_stack_destroy(client);
  actor_destroy(&client->actor);
  free(client);
}

int relay_client_connect(relay_client_t* client, const char* host, uint16_t port,
#ifdef HAS_MSQUIC
                          HQUIC shared_registration
#else
                          void* shared_registration
#endif
                          ) {
  if (client == NULL) return -1;

  // Store connection parameters for retry support
  if (host != NULL && host != client->relay_host) {
    // New host provided (not from a retry) — update the stored copy
    if (client->relay_host != NULL) {
      free(client->relay_host);
    }
    client->relay_host = strdup(host);
    if (client->relay_host == NULL) {
      log_error("relay_client: failed to allocate relay_host");
      return -1;
    }
  }
  client->relay_port = port;
  client->shared_registration = shared_registration;
  client->shutdown_pending = 0;

  QUIC_STATUS status;

  // Use shared registration if provided (avoids UDP socket conflicts when
  // a quic_listener is also active in the same process), otherwise create our own
  if (shared_registration != NULL) {
    client->registration = shared_registration;
    client->owns_registration = 0;
  } else {
    QUIC_REGISTRATION_CONFIG reg_config = {
      "offs_relay",
      QUIC_EXECUTION_PROFILE_LOW_LATENCY
    };
    if (QUIC_FAILED(status = client->msquic->RegistrationOpen(
            &reg_config, &client->registration))) {
      log_error("relay_client: RegistrationOpen failed: 0x%x", status);
      return -1;
    }
    client->owns_registration = 1;
  }

  QUIC_BUFFER alpn = { sizeof("offs_relay") - 1, (uint8_t*)"offs_relay" };

  // Open configuration (each connection type needs its own configuration for its ALPN)
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
    client->msquic->ConfigurationClose(client->configuration);
    client->configuration = NULL;
    if (client->owns_registration) {
      client->msquic->RegistrationClose(client->registration);
    }
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
    if (client->owns_registration) {
      client->msquic->RegistrationClose(client->registration);
    }
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
    if (client->owns_registration) {
      client->msquic->RegistrationClose(client->registration);
    }
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
    if (client->owns_registration) {
      client->msquic->RegistrationClose(client->registration);
    }
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
    if (client->owns_registration) {
      client->msquic->RegistrationClose(client->registration);
    }
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
  client->thread = platform_thread_create(_relay_client_thread, client);
  if (client->thread == NULL) {
    ATOMIC_STORE(&client->running, 0);
    log_error("relay_client: platform_thread_create failed");
    return -1;
  }

  log_info("relay_client: connecting to %s:%u", host != NULL ? host : "127.0.0.1", port);
  return 0;
}

void relay_client_disconnect(relay_client_t* client) {
  if (client == NULL) return;
  client->shutdown_pending = 1;  // prevent retry during intentional disconnect
  ATOMIC_STORE(&client->running, 0);
  pd_loop_async_send(client->loop, NULL);
  platform_thread_join(client->thread);

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

  switch (msg->type) {
    case RELAY_CLIENT_RETRY: {
      // Retry the relay connection after exponential backoff delay.
      // This is dispatched from the SHUTDOWN_COMPLETE callback via actor_send,
      // so it runs on the I/O thread where pd_loop is safe to use.
      relay_retry_payload_t* retry = (relay_retry_payload_t*)msg->payload;
      if (retry == NULL) break;
      unsigned long delay_ms = retry->delay_ms;
      log_info("relay_client: retrying connection to %s:%u (delayed %lums)",
               client->relay_host ? client->relay_host : "127.0.0.1",
               client->relay_port, delay_ms);
      platform_sleep_ms(delay_ms);
      relay_client_connect(client, client->relay_host, client->relay_port,
                           client->shared_registration);
      break;
    }
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

int relay_client_connect(relay_client_t* client, const char* host, uint16_t port,
                          void* shared_registration) {
  (void)client;
  (void)host;
  (void)port;
  (void)shared_registration;
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