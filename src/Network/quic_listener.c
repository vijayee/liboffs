//
// Created by victor on 5/14/25.
//

#include "msquic_singleton.h"
#include "quic_listener.h"

// Destroy helpers for QUIC payloads (used in both HAS_MSQUIC and stub builds)
void quic_data_payload_destroy(quic_data_payload_t* payload) {
  if (payload == NULL) return;
  free(payload->data);
  free(payload);
}

void quic_connected_payload_destroy(quic_connected_payload_t* payload) {
  if (payload == NULL) return;
  // For DISCONNECTED payloads, ConnectionClose is called by the network
  // dispatch handler before this destroy runs. For CONNECTED payloads,
  // the connection and stream handles are owned by msquic.
  // peer_cert_der is stolen by pending_quic_add (set to NULL) when the
  // pending entry takes ownership; the destroy path frees any cert that
  // was never stolen (e.g. duplicate CONNECTED, or CONNECTED that failed
  // before pending_quic_add). See audit #8.
  free(payload->peer_cert_der);
  free(payload);
}

void quic_send_payload_destroy(quic_send_payload_t* payload) {
  if (payload == NULL) return;
  free(payload->data);
  free(payload);
}

#ifdef HAS_MSQUIC

#include "../Util/allocator.h"
#include "../Util/log.h"
#include "wire.h"
#include "stream_framer.h"
#include <msquic.h>
#include <string.h>
#include <cbor.h>
#include "peer_verify.h"

// Destroy stack for deferred watcher cleanup (mirrors HTTP server pattern)
typedef struct quic_destroy_node_t {
  pd_watcher_t* watcher;
  struct quic_destroy_node_t* next;
} quic_destroy_node_t;

static void _destroy_stack_init(quic_listener_t* listener) {
  listener->destroy_lock = platform_mutex_create();
  listener->destroy_head = NULL;
}

static void PLATFORM_UNUSED _destroy_stack_push(quic_listener_t* listener, pd_watcher_t* watcher) {
  quic_destroy_node_t* node = get_clear_memory(sizeof(quic_destroy_node_t));
  node->watcher = watcher;
  platform_mutex_lock(listener->destroy_lock);
  node->next = listener->destroy_head;
  listener->destroy_head = node;
  platform_mutex_unlock(listener->destroy_lock);
  pd_loop_async_send(listener->loop, NULL);
}

static void _destroy_stack_drain(quic_listener_t* listener) {
  quic_destroy_node_t* node;
  platform_mutex_lock(listener->destroy_lock);
  node = listener->destroy_head;
  listener->destroy_head = NULL;
  platform_mutex_unlock(listener->destroy_lock);
  while (node != NULL) {
    quic_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(quic_listener_t* listener) {
  _destroy_stack_drain(listener);
  platform_mutex_destroy(listener->destroy_lock);
}

// Connection tracking — maintains a list of active HQUIC connections so
// quic_listener_destroy can gracefully shut them all down before closing
// the registration.

static void _conn_track_init(quic_listener_t* listener) {
  listener->conn_lock = platform_mutex_create();
  listener->connections = NULL;
  listener->connection_count = 0;
  listener->connection_capacity = 0;
}

static void _conn_track_add(quic_listener_t* listener, HQUIC connection) {
  platform_mutex_lock(listener->conn_lock);
  /* Idempotent: CONNECTED can fire after quic_listener_connect already added
     the same HQUIC, and a duplicate would leave a stale slot after
     _conn_track_remove (which only removes the first match) -> the
     DISCONNECTED handler frees the HQUIC and the stale slot later gets
     ConnectionShutdown on a freed handle. Scan for an existing entry first.
     See docs/liboffs-audit-report.md #4. */
  for (size_t index = 0; index < listener->connection_count; index++) {
    if (listener->connections[index].connection == connection) {
      platform_mutex_unlock(listener->conn_lock);
      return;
    }
  }
  if (listener->connection_count >= listener->connection_capacity) {
    size_t new_cap = listener->connection_capacity == 0 ? 8 : listener->connection_capacity * 2;
    conn_track_entry_t* new_arr = get_clear_memory(new_cap * sizeof(conn_track_entry_t));
    if (new_arr == NULL) {
      platform_mutex_unlock(listener->conn_lock);
      return;
    }
    if (listener->connections != NULL) {
      memcpy(new_arr, listener->connections,
             listener->connection_count * sizeof(conn_track_entry_t));
      free(listener->connections);
    }
    listener->connections = new_arr;
    listener->connection_capacity = new_cap;
  }
  listener->connections[listener->connection_count].connection = connection;
  listener->connections[listener->connection_count].peer_cert_der = NULL;
  listener->connections[listener->connection_count].peer_cert_der_len = 0;
  listener->connection_count++;
  platform_mutex_unlock(listener->conn_lock);
}

// Stash the peer's leaf cert (DER) on the connection's tracking entry. The
// entry is created if it doesn't exist yet (PEER_CERTIFICATE_RECEIVED fires
// before CONNECTED on incoming connections). Takes ownership of cert_der
// (frees the previous cert if one was stashed). See audit #8.
static void _conn_track_set_cert(quic_listener_t* listener, HQUIC connection,
                                 uint8_t* cert_der, size_t cert_len) {
  platform_mutex_lock(listener->conn_lock);
  conn_track_entry_t* entry = NULL;
  for (size_t index = 0; index < listener->connection_count; index++) {
    if (listener->connections[index].connection == connection) {
      entry = &listener->connections[index];
      break;
    }
  }
  if (entry == NULL) {
    // Grow if needed and append a new entry.
    if (listener->connection_count >= listener->connection_capacity) {
      size_t new_cap = listener->connection_capacity == 0 ? 8 : listener->connection_capacity * 2;
      conn_track_entry_t* new_arr = get_clear_memory(new_cap * sizeof(conn_track_entry_t));
      if (new_arr == NULL) {
        platform_mutex_unlock(listener->conn_lock);
        free(cert_der);
        return;
      }
      if (listener->connections != NULL) {
        memcpy(new_arr, listener->connections,
               listener->connection_count * sizeof(conn_track_entry_t));
        free(listener->connections);
      }
      listener->connections = new_arr;
      listener->connection_capacity = new_cap;
    }
    entry = &listener->connections[listener->connection_count++];
    entry->connection = connection;
    entry->peer_cert_der = NULL;
    entry->peer_cert_der_len = 0;
  }
  free(entry->peer_cert_der);
  entry->peer_cert_der = cert_der;
  entry->peer_cert_der_len = cert_len;
  platform_mutex_unlock(listener->conn_lock);
}

// Steal the stashed peer cert (DER) from the connection's tracking entry,
// leaving the entry's cert NULL. Returns the cert in *out_cert / *out_len,
// or NULL if no cert was stashed (allow_insecure mode). See audit #8.
static void _conn_track_steal_cert(quic_listener_t* listener, HQUIC connection,
                                   uint8_t** out_cert, size_t* out_len) {
  *out_cert = NULL;
  *out_len = 0;
  platform_mutex_lock(listener->conn_lock);
  for (size_t index = 0; index < listener->connection_count; index++) {
    if (listener->connections[index].connection == connection) {
      *out_cert = listener->connections[index].peer_cert_der;
      *out_len = listener->connections[index].peer_cert_der_len;
      listener->connections[index].peer_cert_der = NULL;
      listener->connections[index].peer_cert_der_len = 0;
      break;
    }
  }
  platform_mutex_unlock(listener->conn_lock);
}

static void _conn_track_remove(quic_listener_t* listener, HQUIC connection) {
  platform_mutex_lock(listener->conn_lock);
  for (size_t index = 0; index < listener->connection_count; index++) {
    if (listener->connections[index].connection == connection) {
      // Free any cert still stashed on the entry (e.g. connection torn down
      // before CONNECTED stole it, or duplicate shutdown path).
      free(listener->connections[index].peer_cert_der);
      listener->connections[index] = listener->connections[listener->connection_count - 1];
      listener->connection_count--;
      break;
    }
  }
  platform_mutex_unlock(listener->conn_lock);
}

static void _conn_track_shutdown_all(quic_listener_t* listener) {
  // Snapshot the connection list under the lock, then release it before
  // calling ConnectionShutdown. This avoids holding conn_lock during msquic
  // API calls which could deadlock if SHUTDOWN_COMPLETE fires synchronously
  // and tries to acquire conn_lock in _conn_track_remove.
  platform_mutex_lock(listener->conn_lock);
  size_t count = listener->connection_count;
  HQUIC* snapshot = NULL;
  if (count > 0) {
    snapshot = get_clear_memory(count * sizeof(HQUIC));
    if (snapshot != NULL) {
      for (size_t index = 0; index < count; index++) {
        snapshot[index] = listener->connections[index].connection;
      }
    }
  }
  platform_mutex_unlock(listener->conn_lock);

  if (snapshot != NULL) {
    for (size_t index = 0; index < count; index++) {
      listener->msquic->ConnectionShutdown(
          snapshot[index],
          QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
          0);
    }
    free(snapshot);
  }
}

static void _conn_track_destroy(quic_listener_t* listener) {
  // Connections are closed by their SHUTDOWN_COMPLETE callbacks,
  // which fire after _conn_track_shutdown_all initiates shutdown.
  // Free any certs still stashed on entries, then the tracking array.
  for (size_t index = 0; index < listener->connection_count; index++) {
    free(listener->connections[index].peer_cert_der);
  }
  platform_mutex_destroy(listener->conn_lock);
  free(listener->connections);
  listener->connections = NULL;
  listener->connection_count = 0;
  listener->connection_capacity = 0;
}

#ifndef NDEBUG
// Test-only accessors for _conn_track_* — exposed so unit tests can drive
// the connection tracking array without spinning up a full msquic
// registration. See test_quic_integration.cpp ConnTrackAddIsIdempotent.
void quic_listener__conn_track_init_for_test(quic_listener_t* listener) {
  _conn_track_init(listener);
}

void quic_listener__conn_track_destroy_for_test(quic_listener_t* listener) {
  _conn_track_destroy(listener);
}

void quic_listener__conn_track_add_for_test(quic_listener_t* listener,
                                              HQUIC connection) {
  _conn_track_add(listener, connection);
}

size_t quic_listener__conn_track_count_for_test(quic_listener_t* listener) {
  platform_mutex_lock(listener->conn_lock);
  size_t count = listener->connection_count;
  platform_mutex_unlock(listener->conn_lock);
  return count;
}
#endif // NDEBUG

// Per-stream context carrying the parent connection handle and a framer
// for deframing received data into complete messages.
typedef struct quic_stream_context_t {
  quic_listener_t* listener;
  HQUIC connection;
  HQUIC persistent_stream;  // non-NULL if this is the persistent bidi stream
  QUIC_ADDR peer_addr;
  stream_framer_t* framer;
} quic_stream_context_t;

// Send-complete context — frees frame buffer and QUIC_BUFFER after msquic completes the send.
// The QUIC_BUFFER must be heap-allocated because msquic stores a pointer to it
// and reads from it asynchronously in the worker thread.
typedef struct {
  uint8_t* frame;
  QUIC_BUFFER buf;
} send_complete_context_t;

// Process a single deframed message — decode CBOR and dispatch to network actor
static void _quic_process_deframed(quic_stream_context_t* stream_ctx,
                                    uint8_t* msg_data, size_t msg_len) {
  quic_data_payload_t* payload = get_clear_memory(sizeof(quic_data_payload_t));
  if (payload == NULL) return;
  payload->length = msg_len;
  payload->data = msg_data;
  memcpy(&payload->peer_addr, &stream_ctx->peer_addr, sizeof(QUIC_ADDR));
  payload->quic_connection = stream_ctx->connection;

  message_t msg;
  msg.type = NETWORK_QUIC_DATA;
  msg.payload = payload;
  msg.payload_destroy = (void (*)(void*))quic_data_payload_destroy;
  actor_send(&stream_ctx->listener->network->actor, &msg);
}

// QUIC stream callback — receives framed data, deframes, and forwards to network actor
static QUIC_STATUS QUIC_API quic_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  (void)stream;
  quic_stream_context_t* stream_ctx = (quic_stream_context_t*)context;
  switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      if (stream_ctx->framer != NULL) {
        // Feed received bytes into the framer and extract complete frames
        for (uint32_t index = 0; index < event->RECEIVE.BufferCount; index++) {
          const QUIC_BUFFER* buffer = &event->RECEIVE.Buffers[index];
          stream_framer_feed(stream_ctx->framer, buffer->Buffer, buffer->Length);
        }
        size_t msg_len = 0;
        uint8_t* msg_data = NULL;
        while ((msg_data = stream_framer_next(stream_ctx->framer, &msg_len)) != NULL) {
          _quic_process_deframed(stream_ctx, msg_data, msg_len);
        }
      } else {
        // No framer (shouldn't happen) — forward raw data as fallback
        for (uint32_t index = 0; index < event->RECEIVE.BufferCount; index++) {
          const QUIC_BUFFER* buffer = &event->RECEIVE.Buffers[index];
          uint8_t* copy = get_clear_memory(buffer->Length);
          if (copy == NULL) continue;
          memcpy(copy, buffer->Buffer, buffer->Length);
          _quic_process_deframed(stream_ctx, copy, buffer->Length);
        }
      }
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
      send_complete_context_t* send_ctx =
          (send_complete_context_t*)event->SEND_COMPLETE.ClientContext;
      if (send_ctx != NULL) {
        free(send_ctx->frame);
        free(send_ctx);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      if (stream_ctx->framer != NULL) {
        stream_framer_destroy(stream_ctx->framer);
        stream_ctx->framer = NULL;
      }
      free(stream_ctx);
      break;
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

// Build and send a salutation frame on an already-opened stream
static void _send_salutation_on_stream(quic_listener_t* listener, HQUIC stream) {
  authority_t* authority = listener->network->authority;
  if (authority == NULL || authority->public_key == NULL) return;

  wire_salutation_t salut;
  memset(&salut, 0, sizeof(salut));
  memcpy(&salut.sender_id, &authority->local_id, sizeof(node_id_t));
  salut.public_key = authority->public_key;
  salut.public_key_len = authority->public_key_len;

  cbor_item_t* cbor = wire_salutation_encode(&salut);
  if (cbor == NULL) return;

  unsigned char* cbor_data = NULL;
  size_t cbor_len = 0;
  size_t serialized = cbor_serialize_alloc(cbor, &cbor_data, &cbor_len);
  cbor_decref(&cbor);
  if (cbor_data == NULL || serialized == 0) return;

  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(cbor_data, cbor_len, &frame_len);
  free(cbor_data);
  if (frame == NULL) return;

  send_complete_context_t* send_ctx =
      get_clear_memory(sizeof(send_complete_context_t));
  if (send_ctx == NULL) {
    free(frame);
    return;
  }
  send_ctx->frame = frame;
  send_ctx->buf.Buffer = frame;
  send_ctx->buf.Length = (uint32_t)frame_len;
  QUIC_STATUS status = listener->msquic->StreamSend(
      stream, &send_ctx->buf, 1, QUIC_SEND_FLAG_NONE, send_ctx);
  if (QUIC_FAILED(status)) {
    log_error("quic_listener: salutation StreamSend failed: 0x%x", status);
    free(frame);
    free(send_ctx);
  }
}

// Open a persistent bidirectional stream for ongoing data exchange.
// The salutation is sent via a deferred actor message to avoid calling
// StreamSend in the same callback as StreamStart (which crashes msquic).
static HQUIC _open_persistent_stream(quic_listener_t* listener, HQUIC connection,
                                      const QUIC_ADDR* peer_addr) {
  // Allocate context before StreamOpen so we can pass it directly as the callback context
  quic_stream_context_t* stream_ctx = get_clear_memory(sizeof(quic_stream_context_t));
  if (stream_ctx == NULL) return NULL;
  stream_ctx->listener = listener;
  stream_ctx->connection = connection;
  memcpy(&stream_ctx->peer_addr, peer_addr, sizeof(QUIC_ADDR));
  stream_ctx->framer = stream_framer_create();

  HQUIC stream = NULL;
  QUIC_STATUS status = listener->msquic->StreamOpen(
      connection, QUIC_STREAM_OPEN_FLAG_NONE,
      quic_stream_callback, stream_ctx, &stream);
  if (QUIC_FAILED(status) || stream == NULL) {
    log_error("quic_listener: persistent StreamOpen failed: 0x%x", status);
    stream_framer_destroy(stream_ctx->framer);
    free(stream_ctx);
    return NULL;
  }

  stream_ctx->persistent_stream = stream;

  status = listener->msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
  if (QUIC_FAILED(status)) {
    log_error("quic_listener: persistent StreamStart failed: 0x%x", status);
    // Destroy the framer now since SHUTDOWN_COMPLETE won't access it after
    // we clear the pointer. StreamClose triggers SHUTDOWN_COMPLETE which
    // frees stream_ctx — we must not free it here or we'd double-free.
    stream_framer_destroy(stream_ctx->framer);
    stream_ctx->framer = NULL;
    listener->msquic->StreamClose(stream);
    return NULL;
  }

  // Defer salutation send to next actor dispatch — msquic needs time
  // to fully initialize the stream's internal send buffer before
  // StreamSend can be called safely.
  quic_salutation_payload_t* salut_payload =
      get_clear_memory(sizeof(quic_salutation_payload_t));
  if (salut_payload != NULL) {
    salut_payload->stream = stream;
    message_t salut_msg;
    memset(&salut_msg, 0, sizeof(salut_msg));
    salut_msg.type = QUIC_LISTENER_SEND_SALUTATION;
    salut_msg.payload = salut_payload;
    salut_msg.payload_destroy = free;
    actor_send(&listener->actor, &salut_msg);
  }

  return stream;
}

// QUIC connection callback — handles new connections
static QUIC_STATUS QUIC_API quic_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
  quic_listener_t* listener = (quic_listener_t*)context;
  switch (event->Type) {
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
      // Fired during the TLS handshake (before CONNECTED) when
      // QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED is set. With
      // QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES, the Certificate is a
      // QUIC_BUFFER* whose Buffer points to the DER-encoded leaf cert
      // (i2d_X509). Stash a copy on the connection's tracking entry so the
      // CONNECTED handler can pass it to the salutation handler for the
      // audit #8 public_key pin. In allow_insecure mode (no cert indication
      // flag), this event doesn't fire and the pin is a no-op.
      QUIC_BUFFER* cert_buf = (QUIC_BUFFER*)event->PEER_CERTIFICATE_RECEIVED.Certificate;
      if (cert_buf != NULL && cert_buf->Buffer != NULL && cert_buf->Length > 0) {
        uint8_t* cert_copy = get_clear_memory(cert_buf->Length);
        if (cert_copy != NULL) {
          memcpy(cert_copy, cert_buf->Buffer, cert_buf->Length);
          _conn_track_set_cert(listener, connection, cert_copy, cert_buf->Length);
        } else {
          // Alloc failure: the pin will be a no-op for this connection (the
          // cert isn't stashed). CA validation still happened; this only
          // drops the defense-in-depth pubkey pin. Log so it's observable.
          log_warn("quic_listener: failed to copy peer cert; salutation pin "
                   "will be skipped for this connection (audit #8)");
        }
      }
      // Do not reject the handshake here — MsQuic's TLS layer validates the
      // cert against the CA (SET_CA_CERTIFICATE_FILE). The salutation handler
      // does the public_key pin after the BLAKE3 self-consistency check.
      break;
    }
    case QUIC_CONNECTION_EVENT_CONNECTED: {
      log_info("quic_listener: connection CONNECTED");
      _conn_track_add(listener, connection);

      // Extract peer address from the connection
      QUIC_ADDR peer_addr;
      uint32_t peer_addr_len = sizeof(peer_addr);
      listener->msquic->GetParam(
          connection,
          QUIC_PARAM_CONN_REMOTE_ADDRESS,
          &peer_addr_len,
          &peer_addr);

      // Steal the peer's leaf cert (DER) stashed during
      // PEER_CERTIFICATE_RECEIVED. NULL in allow_insecure mode (no cert
      // indication flag) — the salutation pin is then a no-op. See audit #8.
      uint8_t* peer_cert_der = NULL;
      size_t peer_cert_der_len = 0;
      _conn_track_steal_cert(listener, connection, &peer_cert_der, &peer_cert_der_len);

      // Open a persistent bidirectional stream — salutation is sent as
      // the first framed message on this same stream
      HQUIC persistent_stream = _open_persistent_stream(listener, connection, &peer_addr);
      if (persistent_stream == NULL) {
        log_error("quic_listener: failed to open persistent stream on CONNECTED, "
                  "shutting down connection %p", (void*)connection);
        free(peer_cert_der);
        listener->msquic->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        break;
      }

      // Build payload with connection handle, persistent stream handle, and peer address
      quic_connected_payload_t* payload = get_clear_memory(sizeof(quic_connected_payload_t));
      if (payload != NULL) {
        payload->connection = connection;
        payload->stream = persistent_stream;
        memcpy(&payload->peer_addr, &peer_addr, sizeof(peer_addr));
        payload->peer_cert_der = peer_cert_der;
        payload->peer_cert_der_len = peer_cert_der_len;
        peer_cert_der = NULL;  // ownership transferred to payload
      } else {
        free(peer_cert_der);
      }

      message_t msg;
      msg.type = NETWORK_QUIC_CONNECTED;
      msg.payload = payload;
      msg.payload_destroy = (void (*)(void*))quic_connected_payload_destroy;
      actor_send(&listener->network->actor, &msg);
      break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
      log_info("quic_listener: connection SHUTDOWN_COMPLETE");
      _conn_track_remove(listener, connection);

      // Pass the connection handle to the dispatch handler so it can look up
      // the peer before closing. ConnectionClose is called from the dispatch
      // handler after cleanup is complete.
      quic_connected_payload_t* payload = get_clear_memory(sizeof(quic_connected_payload_t));
      if (payload != NULL) {
        payload->connection = connection;
        payload->stream = NULL;
        memset(&payload->peer_addr, 0, sizeof(payload->peer_addr));
      }

      message_t msg;
      msg.type = NETWORK_QUIC_DISCONNECTED;
      msg.payload = payload;
      msg.payload_destroy = (void (*)(void*))quic_connected_payload_destroy;
      actor_send(&listener->network->actor, &msg);
      break;
    }
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
      // Allocate per-stream context with connection handle and peer address
      quic_stream_context_t* stream_ctx = get_clear_memory(sizeof(quic_stream_context_t));
      if (stream_ctx == NULL) {
        log_error("quic_listener: failed to allocate stream context, rejecting stream");
        return QUIC_STATUS_ABORTED;
      }
      stream_ctx->listener = listener;
      stream_ctx->connection = connection;
      // Retrieve peer address from the connection for the stream context
      uint32_t addr_len = sizeof(stream_ctx->peer_addr);
      listener->msquic->GetParam(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS,
                                  &addr_len, &stream_ctx->peer_addr);
      // Each peer-started stream gets its own framer for deframing
      stream_ctx->framer = stream_framer_create();
      // Accept the peer's stream, set our callback with stream context
      listener->msquic->SetCallbackHandler(
          event->PEER_STREAM_STARTED.Stream,
          (void*)quic_stream_callback,
          stream_ctx);
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
  (void)listener_handle;
  quic_listener_t* listener = (quic_listener_t*)context;
  switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
      HQUIC connection = event->NEW_CONNECTION.Connection;
      log_info("quic_listener: NEW_CONNECTION received");
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
  platform_thread_setup_stack();
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
  _conn_track_init(listener);

  listener->loop = pd_loop_create(NULL);
  if (listener->loop == NULL) {
    free(listener);
    return NULL;
  }

  listener->msquic = offs_msquic_open();
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
  // Shut down all active connections gracefully so their SHUTDOWN_COMPLETE
  // callbacks fire and clean up. This must happen before RegistrationClose
  // or msquic worker threads could access freed memory.
  if (listener->peer_verify != NULL) {
    peer_verify_ctx_destroy((peer_verify_ctx_t*)listener->peer_verify);
    listener->peer_verify = NULL;
  }
  _conn_track_shutdown_all(listener);
  if (listener->listener != NULL) {
    listener->msquic->ListenerClose(listener->listener);
  }
  if (listener->configuration != NULL) {
    listener->msquic->ConfigurationClose(listener->configuration);
  }
  if (listener->registration != NULL) {
    listener->msquic->RegistrationClose(listener->registration);
  }
  offs_msquic_close();
  if (listener->loop != NULL) {
    pd_loop_destroy(listener->loop);
  }
  _destroy_stack_destroy(listener);
  _conn_track_destroy(listener);
  actor_destroy(&listener->actor);
  free(listener);
}

int quic_listener_start(quic_listener_t* listener, const char* host, uint16_t port) {
  if (listener == NULL) return -1;

  listener->listen_port = port;

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

  // ALPN — negotiate the "offs" protocol
  QUIC_BUFFER alpn = { sizeof("offs") - 1, (uint8_t*)"offs" };

  // Open configuration
  QUIC_SETTINGS settings = {0};
  settings.PeerUnidiStreamCount = 1;
  settings.PeerBidiStreamCount = 1;
  settings.IsSet.PeerUnidiStreamCount = TRUE;
  settings.IsSet.PeerBidiStreamCount = TRUE;

  if (QUIC_FAILED(status = listener->msquic->ConfigurationOpen(
          listener->registration,
          &alpn,
          1,
          &settings,
          sizeof(settings),
          NULL,
          &listener->configuration))) {
    log_error("ConfigurationOpen failed: 0x%x", status);
    listener->msquic->RegistrationClose(listener->registration);
    listener->registration = NULL;
    return -1;
  }

  // Load credentials
  authority_t* authority = listener->network->authority;
  // Create peer verifier if CA cert is loaded
  if (authority != NULL && authority->ca_cert_data != NULL && authority->ca_cert_len > 0) {
    listener->peer_verify = peer_verify_ctx_create(authority->ca_cert_data, authority->ca_cert_len);
  }
  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  QUIC_CERTIFICATE_FILE cert_file = {0};
  if (authority != NULL && authority->node_cert_path != NULL && authority->node_key_path != NULL) {
    cert_file.CertificateFile = authority->node_cert_path;
    cert_file.PrivateKeyFile = authority->node_key_path;
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    if (listener->peer_verify != NULL) {
      cred_config.Flags = QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE |
                          QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
                          QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
      cred_config.CaCertificateFile = peer_verify_ctx_path(
          (peer_verify_ctx_t*)listener->peer_verify);
    } else if (authority != NULL && authority->allow_insecure) {
      log_warn("quic_listener: no CA configured and allow_insecure is set — "
               "TLS will not authenticate peer certs (MITM possible). "
               "Configure a CA for production. See audit #11.");
      cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    } else {
      log_error("quic_listener: no CA configured and allow_insecure is not set "
                "— refusing to start. Configure a CA, or set allow_insecure=1 "
                "for trusted-LAN use. See audit #11.");
      listener->msquic->ConfigurationClose(listener->configuration);
      listener->configuration = NULL;
      listener->msquic->RegistrationClose(listener->registration);
      listener->registration = NULL;
      return -1;
    }
  } else {
    cred_config.CertificateFile = &cert_file;
    if (listener->peer_verify != NULL) {
      cred_config.Flags = QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE |
                          QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
                          QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
      cred_config.CaCertificateFile = peer_verify_ctx_path(
          (peer_verify_ctx_t*)listener->peer_verify);
    } else if (authority != NULL && authority->allow_insecure) {
      log_warn("quic_listener: no CA configured and allow_insecure is set — "
               "TLS will not authenticate peer certs (MITM possible). "
               "Configure a CA for production. See audit #11.");
      cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    } else {
      log_error("quic_listener: no CA configured and allow_insecure is not set "
                "— refusing to start. Configure a CA, or set allow_insecure=1 "
                "for trusted-LAN use. See audit #11.");
      listener->msquic->ConfigurationClose(listener->configuration);
      listener->configuration = NULL;
      listener->msquic->RegistrationClose(listener->registration);
      listener->registration = NULL;
      return -1;
    }
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
    QuicAddrFromString(host, port, &addr);
  } else {
    QuicAddrSetPort(&addr, port);
  }

  if (QUIC_FAILED(status = listener->msquic->ListenerStart(
          listener->listener,
          &alpn,
          1,
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
  listener->thread = platform_thread_create(_quic_listener_thread, listener);
  if (listener->thread == NULL) {
    ATOMIC_STORE(&listener->running, 0);
    log_error("quic_listener: platform_thread_create failed");
    return -1;
  }

  log_info("quic_listener: listening on %s:%u", host ? host : "0.0.0.0", port);
  return 0;
}

void quic_listener_stop(quic_listener_t* listener) {
  if (listener == NULL) return;
  ATOMIC_STORE(&listener->running, 0);
  pd_loop_async_send(listener->loop, NULL);
  platform_thread_join(listener->thread);
  if (listener->listener != NULL) {
    listener->msquic->ListenerStop(listener->listener);
  }
}

int quic_listener_connect(quic_listener_t* listener, const char* host, uint16_t port) {
  if (listener == NULL || listener->msquic == NULL) return -1;
  if (listener->registration == NULL || listener->configuration == NULL) return -1;

  QUIC_STATUS status;

  // Open a client connection — same callback handles CONNECTED/SHUTDOWN/PEER_STREAM_STARTED
  HQUIC connection = NULL;
  status = listener->msquic->ConnectionOpen(
      listener->registration,
      quic_connection_callback,
      listener,
      &connection);
  if (QUIC_FAILED(status)) {
    log_error("quic_listener: ConnectionOpen failed: 0x%x", status);
    return -1;
  }

  // Set the configuration
  listener->msquic->ConnectionSetConfiguration(connection, listener->configuration);

  // Start the connection to the remote peer
  status = listener->msquic->ConnectionStart(
      connection,
      listener->configuration,
      QUIC_ADDRESS_FAMILY_INET,
      host != NULL ? host : "127.0.0.1",
      port);
  if (QUIC_FAILED(status)) {
    log_error("quic_listener: ConnectionStart failed: 0x%x", status);
    listener->msquic->ConnectionClose(connection);
    return -1;
  }

  // Track outgoing connection for graceful shutdown
  _conn_track_add(listener, connection);

  return 0;
}

void quic_listener_dispatch(void* state, message_t* msg) {
  quic_listener_t* listener = (quic_listener_t*)state;
  if (msg == NULL) return;

  switch (msg->type) {
    case QUIC_LISTENER_SEND_SALUTATION: {
      quic_salutation_payload_t* payload = (quic_salutation_payload_t*)msg->payload;
      if (payload == NULL || payload->stream == NULL) break;
      _send_salutation_on_stream(listener, (HQUIC)payload->stream);
      break;
    }
    default:
      break;
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

int quic_listener_connect(quic_listener_t* listener, const char* host, uint16_t port) {
  (void)listener;
  (void)host;
  (void)port;
  return -1;
}

void quic_listener_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
}

#endif // HAS_MSQUIC