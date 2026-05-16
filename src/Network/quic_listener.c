//
// Created by victor on 5/14/25.
//

#include "quic_listener.h"

// Destroy helper for QUIC data payloads (used in both HAS_MSQUIC and stub builds)
void quic_data_payload_destroy(quic_data_payload_t* payload) {
  if (payload == NULL) return;
  free(payload->data);
  free(payload);
}

#ifdef HAS_MSQUIC

#include "../Util/allocator.h"
#include "../Util/log.h"
#include <msquic.h>
#include <string.h>

// Destroy stack for deferred watcher cleanup (mirrors HTTP server pattern)
typedef struct quic_destroy_node_t {
  pd_watcher_t* watcher;
  struct quic_destroy_node_t* next;
} quic_destroy_node_t;

static void _destroy_stack_init(quic_listener_t* listener) {
  platform_lock_init(&listener->destroy_lock);
  listener->destroy_head = NULL;
}

static void _destroy_stack_push(quic_listener_t* listener, pd_watcher_t* watcher) {
  quic_destroy_node_t* node = get_clear_memory(sizeof(quic_destroy_node_t));
  node->watcher = watcher;
  platform_lock(&listener->destroy_lock);
  node->next = listener->destroy_head;
  listener->destroy_head = node;
  platform_unlock(&listener->destroy_lock);
  pd_loop_async_send(listener->loop, NULL);
}

static void _destroy_stack_drain(quic_listener_t* listener) {
  quic_destroy_node_t* node;
  platform_lock(&listener->destroy_lock);
  node = listener->destroy_head;
  listener->destroy_head = NULL;
  platform_unlock(&listener->destroy_lock);
  while (node != NULL) {
    quic_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(quic_listener_t* listener) {
  _destroy_stack_drain(listener);
  platform_lock_destroy(&listener->destroy_lock);
}

// msquic singleton — reference-counted process-wide QUIC API
static const struct QUIC_API_TABLE* g_msquic = NULL;
static uint32_t g_msquic_refcount = 0;
static PLATFORMLOCKTYPE(g_msquic_lock);
static bool g_msquic_lock_initialized = false;

static void ensure_msquic_lock_initialized(void) {
  if (!g_msquic_lock_initialized) {
    platform_lock_init(&g_msquic_lock);
    g_msquic_lock_initialized = true;
  }
}

static const struct QUIC_API_TABLE* msquic_open(void) {
  ensure_msquic_lock_initialized();
  platform_lock(&g_msquic_lock);
  if (g_msquic == NULL) {
    const struct QUIC_API_TABLE* table = NULL;
    QUIC_STATUS status;
    if (QUIC_FAILED(status = MsQuicOpen2(&table))) {
      log_error("MsQuicOpen2 failed: 0x%x", status);
      platform_unlock(&g_msquic_lock);
      return NULL;
    }
    g_msquic = table;
    g_msquic_refcount = 1;
    platform_unlock(&g_msquic_lock);
    return g_msquic;
  }
  g_msquic_refcount++;
  const struct QUIC_API_TABLE* result = g_msquic;
  platform_unlock(&g_msquic_lock);
  return result;
}

static void msquic_close(void) {
  ensure_msquic_lock_initialized();
  platform_lock(&g_msquic_lock);
  if (g_msquic == NULL || g_msquic_refcount == 0) {
    platform_unlock(&g_msquic_lock);
    return;
  }
  g_msquic_refcount--;
  if (g_msquic_refcount == 0) {
    MsQuicClose(g_msquic);
    g_msquic = NULL;
  }
  platform_unlock(&g_msquic_lock);
}

// QUIC stream callback — receives data and forwards to network actor
static QUIC_STATUS QUIC_API quic_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  quic_listener_t* listener = (quic_listener_t*)context;
  switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      // Forward received data to network protocol actor via actor_send
      for (uint32_t index = 0; index < event->RECEIVE.BufferCount; index++) {
        QUIC_BUFFER* buffer = &event->RECEIVE.Buffers[index];
        quic_data_payload_t* payload = get_clear_memory(sizeof(quic_data_payload_t));
        if (payload == NULL) continue;
        payload->length = buffer->Length;
        payload->data = get_clear_memory(buffer->Length);
        if (payload->data == NULL) {
          free(payload);
          continue;
        }
        memcpy(payload->data, buffer->Buffer, buffer->Length);

        message_t msg;
        msg.type = NETWORK_QUIC_DATA;
        msg.payload = payload;
        msg.payload_destroy = (void (*)(void*))quic_data_payload_destroy;
        actor_send(&listener->network->actor, &msg);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
      break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      break;
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// QUIC connection callback — handles new connections
static QUIC_STATUS QUIC_API quic_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
  quic_listener_t* listener = (quic_listener_t*)context;
  switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
      // Send NETWORK_QUIC_CONNECTED to protocol actor
      message_t msg;
      msg.type = NETWORK_QUIC_CONNECTED;
      msg.payload = connection;
      msg.payload_destroy = NULL;
      actor_send(&listener->network->actor, &msg);
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
      // Send NETWORK_QUIC_DISCONNECTED to protocol actor
      message_t msg;
      msg.type = NETWORK_QUIC_DISCONNECTED;
      msg.payload = connection;
      msg.payload_destroy = NULL;
      actor_send(&listener->network->actor, &msg);
      break;
    }
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
      // Accept the peer's stream, set our callback
      listener->msquic->SetCallbackHandler(
          event->PEER_STREAM_STARTED.Stream,
          (void*)quic_stream_callback,
          listener);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// QUIC listener callback — accepts new connections
static QUIC_STATUS QUIC_API quic_listener_callback(
    HQUIC listener_handle, void* context, QUIC_LISTENER_EVENT* event) {
  quic_listener_t* listener = (quic_listener_t*)context;
  switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
      HQUIC connection = event->NEW_CONNECTION.Connection;
      // Set connection callback and configuration
      listener->msquic->SetCallbackHandler(
          connection,
          (void*)quic_connection_callback,
          listener);
      listener->msquic->ConnectionSetConfiguration(
          connection,
          listener->configuration);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// I/O thread — runs poll-dancer event loop for timers
static void* _quic_listener_thread(void* arg) {
  quic_listener_t* listener = (quic_listener_t*)arg;
  platform_setup_thread_stack();
  while (ATOMIC_LOAD(&listener->running)) {
    _destroy_stack_drain(listener);
    pd_loop_run_once(listener->loop, 100);
  }
  return NULL;
}

quic_listener_t* quic_listener_create(network_t* network, scheduler_pool_t* pool) {
  quic_listener_t* listener = get_clear_memory(sizeof(quic_listener_t));
  listener->network = network;
  listener->pool = pool;
  listener->running = ATOMIC_VAR_INIT(0);

  actor_init(&listener->actor, listener, quic_listener_dispatch, pool);
  _destroy_stack_init(listener);

  listener->loop = pd_loop_create(NULL);
  if (listener->loop == NULL) {
    free(listener);
    return NULL;
  }

  listener->msquic = msquic_open();
  if (listener->msquic == NULL) {
    pd_loop_destroy(listener->loop);
    free(listener);
    return NULL;
  }

  return listener;
}

void quic_listener_destroy(quic_listener_t* listener) {
  if (listener == NULL) return;
  // Stop I/O thread if running
  if (ATOMIC_LOAD(&listener->running)) {
    quic_listener_stop(listener);
  }
  if (listener->listener != NULL) {
    listener->msquic->ListenerClose(listener->listener);
  }
  if (listener->configuration != NULL) {
    listener->msquic->ConfigurationClose(listener->configuration);
  }
  if (listener->registration != NULL) {
    listener->msquic->RegistrationClose(listener->registration);
  }
  msquic_close();
  if (listener->loop != NULL) {
    pd_loop_destroy(listener->loop);
  }
  _destroy_stack_destroy(listener);
  actor_destroy(&listener->actor);
  free(listener);
}

int quic_listener_start(quic_listener_t* listener, const char* host, uint16_t port) {
  if (listener == NULL) return -1;

  QUIC_STATUS status;

  // Open registration
  QUIC_REGISTRATION_CONFIG reg_config = {
    "liboffs",
    QUIC_EXECUTION_PROFILE_LOW_LATENCY
  };
  if (QUIC_FAILED(status = listener->msquic->RegistrationOpen(
          &reg_config, &listener->registration))) {
    log_error("RegistrationOpen failed: 0x%x", status);
    return -1;
  }

  // Open configuration
  QUIC_SETTINGS settings = {0};
  settings.PeerUnidiStreamCount = 1;
  settings.PeerBidiStreamCount = 1;
  settings.IsSet.PeerUnidiStreamCount = TRUE;
  settings.IsSet.PeerBidiStreamCount = TRUE;

  if (QUIC_FAILED(status = listener->msquic->ConfigurationOpen(
          listener->registration,
          &settings,
          sizeof(settings),
          NULL,
          0,
          &listener->configuration))) {
    log_error("ConfigurationOpen failed: 0x%x", status);
    listener->msquic->RegistrationClose(listener->registration);
    listener->registration = NULL;
    return -1;
  }

  // Load credentials
  authority_t* authority = listener->network->authority;
  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  if (authority != NULL && authority->node_cert_path != NULL && authority->node_key_path != NULL) {
    // Use X.509 certificate for TLS
    QUIC_CERTIFICATE_FILE cert_file = {
      .CertificateFile = authority->node_cert_path,
      .PrivateKeyFile = authority->node_key_path
    };
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    if (authority->ca_cert_path == NULL) {
      // No CA cert — skip peer verification
      cred_config.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }
  } else {
    // No certificate — use self-signed
    // No certificate configured — accept any peer (development mode)
    QUIC_CERTIFICATE_FILE self_signed = {0};
    cred_config.CertificateFile = &self_signed;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }

  if (QUIC_FAILED(status = listener->msquic->ConfigurationLoadCredential(
          listener->configuration,
          &cred_config))) {
    log_error("ConfigurationLoadCredential failed: 0x%x", status);
    listener->msquic->ConfigurationClose(listener->configuration);
    listener->configuration = NULL;
    listener->msquic->RegistrationClose(listener->registration);
    listener->registration = NULL;
    return -1;
  }

  // Open listener
  if (QUIC_FAILED(status = listener->msquic->ListenerOpen(
          listener->registration,
          quic_listener_callback,
          listener,
          &listener->listener))) {
    log_error("ListenerOpen failed: 0x%x", status);
    listener->msquic->ConfigurationClose(listener->configuration);
    listener->configuration = NULL;
    listener->msquic->RegistrationClose(listener->registration);
    listener->registration = NULL;
    return -1;
  }

  // Start listening
  QUIC_ADDR addr;
  QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
  if (host != NULL) {
    QuicAddrSetString(&addr, host);
  }
  QuicAddrSetPort(&addr, port);

  if (QUIC_FAILED(status = listener->msquic->ListenerStart(
          listener->listener,
          &addr))) {
    log_error("ListenerStart failed: 0x%x", status);
    listener->msquic->ListenerClose(listener->listener);
    listener->listener = NULL;
    listener->msquic->ConfigurationClose(listener->configuration);
    listener->configuration = NULL;
    listener->msquic->RegistrationClose(listener->registration);
    listener->registration = NULL;
    return -1;
  }

  // Start I/O thread
  ATOMIC_STORE(&listener->running, 1);
  pthread_create(&listener->thread, NULL, _quic_listener_thread, listener);

  return 0;
}

void quic_listener_stop(quic_listener_t* listener) {
  if (listener == NULL) return;
  ATOMIC_STORE(&listener->running, 0);
  pd_loop_async_send(listener->loop, NULL);
  pthread_join(listener->thread, NULL);
  if (listener->listener != NULL) {
    listener->msquic->ListenerStop(listener->listener);
  }
}

#else // !HAS_MSQUIC — stub implementations for build without msquic

#include <stdlib.h>

quic_listener_t* quic_listener_create(network_t* network, scheduler_pool_t* pool) {
  (void)network;
  (void)pool;
  return NULL;
}

void quic_listener_destroy(quic_listener_t* listener) {
  (void)listener;
}

int quic_listener_start(quic_listener_t* listener, const char* host, uint16_t port) {
  (void)listener;
  (void)host;
  (void)port;
  return -1;
}

void quic_listener_stop(quic_listener_t* listener) {
  (void)listener;
}

void quic_listener_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
}

#endif // HAS_MSQUIC