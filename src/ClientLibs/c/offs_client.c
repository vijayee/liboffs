//
// Created by victor on 5/20/26.
//
#ifdef _WIN32
/* Winsock2 must be included before windows.h (and before any header that
 * pulls windows in, such as OpenSSL or poll-dancer) to avoid the winsock1/
 * winsock2 conflict. Provides getaddrinfo/inet_ntop/setsockopt/SO_RCVTIMEO
 * used by offs_http_get. */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#endif

#include "offs_client.h"
#include "../../ClientAPI/client_api_wire.h"
#include "../../Network/stream_framer.h"
#include "../../Buffer/buffer.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include "../../ClientAPI/WS/ws_frame.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef _WIN32
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif
#include "../../Platform/platform.h"
#include "../../Platform/platform_local.h"
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <poll-dancer/poll-dancer.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#include "../../Network/msquic_singleton.h"
#endif

#define READ_BUFFER_SIZE 65536

/* OpenSSL is initialized once per process and never cleaned up — the 240-byte
 * CONF_modules_load allocation is a one-time global cost that persists until
 * process exit. Calling OPENSSL_cleanup() would break other OpenSSL users in
 * the same process (e.g. the WS transport server).
 *
 * OPENSSL_init_ssl is thread-safe and idempotent (internally refcounted since
 * OpenSSL 1.1.0), so it needs no pthread_once / INIT_ONCE guard — every call
 * after the first is a cheap no-op. */
static void _ssl_init(void) {
  OPENSSL_init_ssl(0, NULL);
}

typedef enum {
  OFFS_TRANSPORT_UNIX,
  OFFS_TRANSPORT_TCP,
  OFFS_TRANSPORT_WS,
  OFFS_TRANSPORT_WT,
} offs_transport_type_e;

offs_client_config_t offs_client_config_default(void) {
  offs_client_config_t config;
  config.connect_timeout_ms = 5000;
  config.ws_upgrade_timeout_ms = 5000;
  config.poll_timeout_ms = 10;  /* Short timeout for responsive recv */
  config.max_retries = 3;
  config.retry_base_delay_ms = 1000;
  return config;
}

static uint32_t _retry_delay_ms(const offs_client_config_t* config, uint32_t attempt) {
  uint32_t delay = config->retry_base_delay_ms * (1u << (attempt - 1));
  uint32_t jitter_range = delay / 4;
  if (jitter_range == 0) return delay;
  uint32_t rand_val;
  platform_random_bytes((uint8_t*)&rand_val, sizeof(rand_val));
  uint32_t offset = rand_val % (jitter_range * 2 + 1);
  return delay + offset - jitter_range;
}

struct offs_client_t {
  offs_transport_type_e transport_type;
  volatile uint8_t connected;
  offs_client_config_t config;
  union {
    struct {
      platform_socket_t* sock;
      uint8_t is_unix;
    } raw;
    struct {
      platform_socket_t* sock;
      SSL* ssl;
      SSL_CTX* ssl_ctx;
      uint8_t is_ssl;
      buffer_t* recv_buf;
    } ws;
    struct {
      void* registration;
      void* configuration;
      void* connection;
      void* stream;
      void* msquic;
      uint8_t stream_open;
      platform_mutex_t* recv_lock;
      platform_condvar_t* recv_cond;
      uint8_t* recv_buf;
      size_t recv_buf_size;
      size_t recv_buf_used;
      uint8_t* send_buf;
      size_t send_buf_len;
      uint8_t send_complete;
    } wt;
  } transport;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  platform_mutex_t* lock;
  platform_thread_t* recv_thread;
  volatile uint8_t running;
  pd_loop_t* loop;
  pd_watcher_t* watcher;
  uint8_t* read_buf;
  size_t read_buf_size;

  /* Auth */
  char* api_key;

  /* Callbacks */
  offs_put_response_cb_t put_cb;
  void* put_cb_ctx;
  offs_get_data_cb_t get_data_cb;
  void* get_data_cb_ctx;
  offs_get_end_cb_t get_end_cb;
  void* get_end_cb_ctx;
  offs_error_cb_t error_cb;
  void* error_cb_ctx;
  offs_block_put_cb_t block_put_cb;
  void* block_put_cb_ctx;
  offs_block_get_cb_t block_get_cb;
  void* block_get_cb_ctx;
  offs_block_delete_cb_t block_delete_cb;
  void* block_delete_cb_ctx;
  offs_health_cb_t health_cb;
  void* health_cb_ctx;
};

/* Forward declaration — needed for MsQuic callbacks that call _handle_frame */
static void _handle_frame(offs_client_t* client, uint8_t type, cbor_item_t* frame);

#ifdef HAS_MSQUIC
typedef struct {
  uint8_t* frame;
  QUIC_BUFFER buf;
} wt_send_context_t;

typedef struct {
  offs_client_t* client;
  HQUIC connection;
  uint8_t connected_event;
} wt_connect_context_t;
#endif

#ifdef HAS_MSQUIC
static QUIC_STATUS QUIC_API _wt_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  offs_client_t* client = (offs_client_t*)context;
  (void)stream;

  switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      /* MsQuic serializes stream callbacks, so framer access is safe without lock.
         Lock must NOT be held during _handle_frame — user callbacks may call
         offs_client_get/put which acquire client->lock in _send_frame. */
      const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
      uint32_t buffer_count = event->RECEIVE.BufferCount;
      for (uint32_t idx = 0; idx < buffer_count; idx++) {
        stream_framer_feed(client->framer, buffers[idx].Buffer, buffers[idx].Length);
        uint8_t* frame_data;
        size_t frame_len;
        while ((frame_data = stream_framer_next(client->framer, &frame_len)) != NULL) {
          struct cbor_load_result load_result;
          cbor_item_t* cbor_item = cbor_load(frame_data, frame_len, &load_result);
          free(frame_data);
          if (cbor_item != NULL && load_result.error.code == CBOR_ERR_NONE) {
            uint8_t type = client_api_wire_get_type(cbor_item);
            _handle_frame(client, type, cbor_item);
            cbor_decref(&cbor_item);
          } else if (cbor_item != NULL) {
            cbor_decref(&cbor_item);
          }
        }
      }
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
      if (event->SEND_COMPLETE.ClientContext != NULL) {
        wt_send_context_t* send_ctx = (wt_send_context_t*)event->SEND_COMPLETE.ClientContext;
        if (send_ctx->frame != NULL) {
          free(send_ctx->frame);
        }
        free(send_ctx);
      }
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API _wt_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
  wt_connect_context_t* connect_ctx = (wt_connect_context_t*)context;
  (void)connection;

  switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
      connect_ctx->connected_event = 1;
      break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
      break;
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}
#endif /* HAS_MSQUIC */

static int _raw_send(offs_client_t* client, const uint8_t* data, size_t len) {
  ssize_t sent = platform_socket_send(client->transport.raw.sock, data, len);
  if (sent < 0) return -1;
  size_t total_sent = (size_t)sent;
  while (total_sent < len) {
    sent = platform_socket_send(client->transport.raw.sock, data + total_sent, len - total_sent);
    if (sent <= 0) return -1;
    total_sent += (size_t)sent;
  }
  return 0;
}

static int _ws_send(offs_client_t* client, const uint8_t* data, size_t len) {
  if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
    size_t total_written = 0;
    while (total_written < len) {
      int written = SSL_write(client->transport.ws.ssl, data + total_written, (int)(len - total_written));
      if (written <= 0) return -1;
      total_written += (size_t)written;
    }
    return 0;
  }
  return _raw_send(client, data, len);
}

static void _send_frame(offs_client_t* client, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);

  if (cbor_buf == NULL || cbor_len == 0) return;

  uint8_t* framed;
  size_t framed_len;

  if (client->transport_type == OFFS_TRANSPORT_WS) {
    framed = ws_frame_build_masked(WS_OPCODE_BINARY, cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return;

    platform_mutex_lock(client->lock);
    if (client->write_buffer != NULL && client->write_buffer->size > 0) {
      buffer_ensure_capacity(client->write_buffer, client->write_buffer->size + framed_len);
      memcpy(client->write_buffer->data + client->write_buffer->size, framed, framed_len);
      client->write_buffer->size += framed_len;
    } else {
      if (_ws_send(client, framed, framed_len) < 0) {
        client->connected = 0;
      }
    }
    platform_mutex_unlock(client->lock);
    free(framed);
  } else if (client->transport_type == OFFS_TRANSPORT_WT) {
    /* WT: length-prefix frame + MsQuic StreamSend */
    framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return;

#ifdef HAS_MSQUIC
    wt_send_context_t* send_ctx = get_clear_memory(sizeof(wt_send_context_t));
    if (send_ctx == NULL) {
      free(framed);
      return;
    }
    send_ctx->frame = framed;
    send_ctx->buf.Buffer = framed;
    send_ctx->buf.Length = (uint32_t)framed_len;

    platform_mutex_lock(client->lock);
    const struct QUIC_API_TABLE* msquic = (const struct QUIC_API_TABLE*)client->transport.wt.msquic;
    QUIC_STATUS status = msquic->StreamSend(
        (HQUIC)client->transport.wt.stream, &send_ctx->buf, 1, QUIC_SEND_FLAG_NONE, send_ctx);
    if (QUIC_FAILED(status)) {
      free(framed);
      free(send_ctx);
      client->connected = 0;
    }
    platform_mutex_unlock(client->lock);
    /* send_ctx and framed are freed in SEND_COMPLETE callback */
#else
    free(framed);
    client->connected = 0;
#endif
  } else {
    /* unix/tcp: length-prefix framing + send() */
    framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return;

    platform_mutex_lock(client->lock);
    if (client->write_buffer != NULL && client->write_buffer->size > 0) {
      buffer_ensure_capacity(client->write_buffer, client->write_buffer->size + framed_len);
      memcpy(client->write_buffer->data + client->write_buffer->size, framed, framed_len);
      client->write_buffer->size += framed_len;
    } else {
      ssize_t sent = platform_socket_send(client->transport.raw.sock, framed, framed_len);
      if (sent < 0) {
        client->connected = 0;
      } else if ((size_t)sent < framed_len) {
        size_t remaining = framed_len - (size_t)sent;
        client->write_buffer = buffer_create(remaining);
        memcpy(client->write_buffer->data, framed + sent, remaining);
        client->write_buffer->size = remaining;
      }
    }
    platform_mutex_unlock(client->lock);
    free(framed);
  }
}

static void _send_auth_request(offs_client_t* client) {
  if (client->api_key == NULL) return;

  client_api_auth_request_t auth;
  auth.api_key = (uint8_t*)client->api_key;
  auth.api_key_len = strlen(client->api_key);

  cbor_item_t* frame = client_api_auth_request_encode(&auth);
  _send_frame(client, frame);
}

static void _handle_frame(offs_client_t* client, uint8_t type, cbor_item_t* frame) {
  /* Snapshot callbacks under lock to avoid data race with public API setters */
  platform_mutex_lock(client->lock);
  offs_put_response_cb_t put_cb = client->put_cb;
  void* put_cb_ctx = client->put_cb_ctx;
  offs_get_data_cb_t get_data_cb = client->get_data_cb;
  void* get_data_cb_ctx = client->get_data_cb_ctx;
  offs_get_end_cb_t get_end_cb = client->get_end_cb;
  void* get_end_cb_ctx = client->get_end_cb_ctx;
  offs_error_cb_t error_cb = client->error_cb;
  void* error_cb_ctx = client->error_cb_ctx;
  offs_block_put_cb_t block_put_cb = client->block_put_cb;
  void* block_put_cb_ctx = client->block_put_cb_ctx;
  offs_block_get_cb_t block_get_cb = client->block_get_cb;
  void* block_get_cb_ctx = client->block_get_cb_ctx;
  offs_block_delete_cb_t block_delete_cb = client->block_delete_cb;
  void* block_delete_cb_ctx = client->block_delete_cb_ctx;
  offs_health_cb_t health_cb = client->health_cb;
  void* health_cb_ctx = client->health_cb_ctx;
  platform_mutex_unlock(client->lock);

  switch (type) {
    case CLIENT_API_PUT_RESPONSE: {
      client_api_put_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_put_response_decode(frame, &msg) == 0) {
        if (put_cb != NULL) {
          put_cb(put_cb_ctx, msg.ori_string);
        }
        client_api_put_response_destroy(&msg);
      }
      break;
    }
    case CLIENT_API_GET_RESPONSE_START: {
      break;
    }
    case CLIENT_API_GET_DATA: {
      client_api_get_data_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_get_data_decode(frame, &msg) == 0) {
        if (get_data_cb != NULL) {
          get_data_cb(get_data_cb_ctx, msg.data, msg.data_size);
        }
        free(msg.data);
      }
      break;
    }
    case CLIENT_API_GET_END: {
      if (get_end_cb != NULL) {
        get_end_cb(get_end_cb_ctx);
      }
      break;
    }
    case CLIENT_API_ERROR: {
      client_api_error_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_error_decode(frame, &msg) == 0) {
        if (error_cb != NULL) {
          error_cb(error_cb_ctx, msg.status_code, msg.message);
        }
        client_api_error_destroy(&msg);
      }
      break;
    }
    case CLIENT_API_BLOCK_PUT_RESPONSE: {
      client_api_block_put_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_block_put_response_decode(frame, &msg) == 0) {
        if (block_put_cb != NULL) {
          block_put_cb(block_put_cb_ctx, msg.status, msg.hash_data, msg.hash_len, msg.hash_is_text);
        }
        client_api_block_put_response_destroy(&msg);
      }
      break;
    }
    case CLIENT_API_BLOCK_GET_RESPONSE: {
      client_api_block_get_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_block_get_response_decode(frame, &msg) == 0) {
        if (block_get_cb != NULL) {
          block_get_cb(block_get_cb_ctx, msg.status, msg.data, msg.data_size);
        }
        client_api_block_get_response_destroy(&msg);
      }
      break;
    }
    case CLIENT_API_BLOCK_DELETE_RESPONSE: {
      client_api_block_delete_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_block_delete_response_decode(frame, &msg) == 0) {
        if (block_delete_cb != NULL) {
          block_delete_cb(block_delete_cb_ctx, msg.status);
        }
        client_api_block_delete_response_destroy(&msg);
      }
      break;
    }
    case CLIENT_API_HEALTH_RESPONSE: {
      client_api_health_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_health_response_decode(frame, &msg) == 0) {
        if (health_cb != NULL) {
          health_cb(health_cb_ctx, msg.json_data);
        }
        client_api_health_response_destroy(&msg);
      }
      break;
    }
    default:
      break;
  }
}

static void _client_raw_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data) {
  offs_client_t* client = (offs_client_t*)user_data;
  (void)loop;

  if (events & (PD_EVENT_ERROR | PD_EVENT_HANGUP)) {
    client->connected = 0;
    client->running = 0;
    return;
  }

  if (events & PD_EVENT_READ) {
    /* On Windows IOCP the bytes that triggered this completion sit in the
     * watcher's internal buffer; drain them with pd_watcher_drain_read.
     * On POSIX that returns 0, so fall back to a synchronous recv loop
     * (level-triggered epoll needs the loop to wake between recv() calls).
     * Feed all available bytes into the framer, then pull complete frames. */
    uint8_t buf[READ_BUFFER_SIZE];
    size_t total_read = 0;
    size_t n = pd_watcher_drain_read(watcher, buf, sizeof(buf));
    while (n > 0) {
      stream_framer_feed(client->framer, buf, n);
      total_read += n;
      if (n < sizeof(buf)) break;
      n = pd_watcher_drain_read(watcher, buf, sizeof(buf));
    }
    if (total_read == 0) {
      while (1) {
        ssize_t bytes_read = platform_socket_recv(client->transport.raw.sock, buf, sizeof(buf));
        if (bytes_read <= 0) {
          if (bytes_read == 0) {
            client->connected = 0;
            client->running = 0;
          }
          break;
        }
        stream_framer_feed(client->framer, buf, (size_t)bytes_read);
      }
    }

    uint8_t* frame_data;
    size_t frame_len;
    while ((frame_data = stream_framer_next(client->framer, &frame_len)) != NULL) {
      struct cbor_load_result load_result;
      cbor_item_t* cbor_item = cbor_load(frame_data, frame_len, &load_result);
      free(frame_data);
      if (cbor_item == NULL || load_result.error.code != CBOR_ERR_NONE) {
        if (cbor_item != NULL) cbor_decref(&cbor_item);
        continue;
      }
      uint8_t type = client_api_wire_get_type(cbor_item);
      _handle_frame(client, type, cbor_item);
      cbor_decref(&cbor_item);
    }
  }
}

static void _ws_append_recv_buf(offs_client_t* client, const uint8_t* data, size_t len) {
  if (len == 0) return;
  if (client->transport.ws.recv_buf == NULL) {
    client->transport.ws.recv_buf = buffer_create(len);
    memcpy(client->transport.ws.recv_buf->data, data, len);
    client->transport.ws.recv_buf->size = len;
  } else {
    buffer_ensure_capacity(client->transport.ws.recv_buf, client->transport.ws.recv_buf->size + len);
    memcpy(client->transport.ws.recv_buf->data + client->transport.ws.recv_buf->size, data, len);
    client->transport.ws.recv_buf->size += len;
  }
}

static void _client_ws_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                      pd_event_t events, void* user_data) {
  offs_client_t* client = (offs_client_t*)user_data;
  (void)loop;

  if (events & (PD_EVENT_ERROR | PD_EVENT_HANGUP)) {
    client->connected = 0;
    client->running = 0;
    return;
  }

  if (events & PD_EVENT_READ) {
    uint8_t buf[READ_BUFFER_SIZE];

    /* SSL owns the encrypted byte stream and reads it itself via the socket
     * BIO, so keep SSL_read for wss://. For plain ws:// the Windows IOCP
     * completion buffers the bytes in the watcher — drain them with
     * pd_watcher_drain_read (POSIX returns 0, falling back to recv). */
    if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
      ssize_t bytes_read = SSL_read(client->transport.ws.ssl, buf, sizeof(buf));
      if (bytes_read <= 0) {
        client->connected = 0;
        client->running = 0;
        return;
      }
      _ws_append_recv_buf(client, buf, (size_t)bytes_read);
    } else {
      size_t n = pd_watcher_drain_read(watcher, buf, sizeof(buf));
      if (n == 0) {
        ssize_t bytes_read = platform_socket_recv(client->transport.ws.sock, buf, sizeof(buf));
        if (bytes_read <= 0) {
          client->connected = 0;
          client->running = 0;
          return;
        }
        _ws_append_recv_buf(client, buf, (size_t)bytes_read);
      } else {
        while (n > 0) {
          _ws_append_recv_buf(client, buf, n);
          if (n < sizeof(buf)) break;
          n = pd_watcher_drain_read(watcher, buf, sizeof(buf));
        }
      }
    }

    /* Parse all complete WebSocket frames */
    while (1) {
      ws_frame_t parsed;
      size_t needed;
      ssize_t consumed = ws_frame_parse(
        client->transport.ws.recv_buf->data,
        client->transport.ws.recv_buf->size,
        &parsed, &needed);
      if (consumed == 0) break; /* incomplete frame */
      if (consumed < 0) {
        client->connected = 0;
        client->running = 0;
        return;
      }
      /* Consume parsed bytes from recv buffer */
      size_t remaining = client->transport.ws.recv_buf->size - (size_t)consumed;
      if (remaining > 0) {
        memmove(client->transport.ws.recv_buf->data,
                client->transport.ws.recv_buf->data + consumed,
                remaining);
      }
      client->transport.ws.recv_buf->size = remaining;

      if (parsed.opcode == WS_OPCODE_BINARY && parsed.payload != NULL && parsed.payload_len > 0) {
        struct cbor_load_result load_result;
        cbor_item_t* item = cbor_load(parsed.payload, parsed.payload_len, &load_result);
        if (item != NULL && load_result.error.code == CBOR_ERR_NONE) {
          uint8_t type = client_api_wire_get_type(item);
          _handle_frame(client, type, item);
          cbor_decref(&item);
        } else if (item != NULL) {
          cbor_decref(&item);
        }
      } else if (parsed.opcode == WS_OPCODE_CLOSE) {
        client->connected = 0;
        client->running = 0;
        ws_frame_destroy(&parsed);
        return;
      } else if (parsed.opcode == WS_OPCODE_PING) {
        /* RFC 6455: respond to PING with PONG — must hold lock to serialize with _send_frame */
        size_t pong_len;
        uint8_t* pong = ws_frame_build(WS_OPCODE_PONG, parsed.payload, parsed.payload_len, &pong_len);
        if (pong != NULL) {
          platform_mutex_lock(client->lock);
          if (_ws_send(client, pong, pong_len) < 0) {
            client->connected = 0;
          }
          platform_mutex_unlock(client->lock);
          free(pong);
        }
      }
      ws_frame_destroy(&parsed);
    }
  }
}

static void* _recv_thread(void* arg) {
  offs_client_t* client = (offs_client_t*)arg;
  platform_socket_t* sock;
  pd_callback_t callback;

  if (client->transport_type == OFFS_TRANSPORT_WS) {
    sock = client->transport.ws.sock;
    callback = _client_ws_read_callback;
  } else {
    sock = client->transport.raw.sock;
    callback = _client_raw_read_callback;
  }

  client->loop = pd_loop_create(NULL);
  if (client->loop == NULL) return NULL;

  client->watcher = pd_watcher_create(client->loop, platform_socket_fd(sock),
      PD_EVENT_READ | PD_EVENT_ERROR | PD_EVENT_HANGUP,
      callback, client);
  if (client->watcher == NULL) {
    pd_loop_destroy(client->loop);
    client->loop = NULL;
    return NULL;
  }

  pd_watcher_start(client->watcher);

  while (client->running) {
    pd_loop_run_once(client->loop, (int)client->config.poll_timeout_ms);
  }

  pd_watcher_destroy(client->watcher);
  client->watcher = NULL;
  pd_loop_destroy(client->loop);
  client->loop = NULL;

  return NULL;
}

static platform_socket_t* _connect_unix(const char* path) {
  return platform_local_connect(path);
}

static platform_socket_t* _connect_tcp(const char* host, uint16_t port) {
  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (sock == NULL) {
    log_error("_connect_tcp: socket creation failed for %s:%u", host, port);
    return NULL;
  }

  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET;
  addr.inet.port = port;
  if (platform_address_parse(&addr, host, port) != 0) {
    log_error("_connect_tcp: address parse failed for %s:%u", host, port);
    platform_socket_destroy(sock);
    return NULL;
  }

  if (platform_socket_connect(sock, &addr) < 0) {
    log_error("_connect_tcp: connect to %s:%u failed", host, port);
    platform_socket_destroy(sock);
    return NULL;
  }
  return sock;
}

static char* _ws_compute_accept_key(const char* client_key) {
  const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  size_t combined_len = strlen(client_key) + strlen(magic);
  uint8_t* combined = get_memory(combined_len + 1);
  memcpy(combined, client_key, strlen(client_key));
  memcpy(combined + strlen(client_key), magic, strlen(magic));
  combined[combined_len] = '\0';

  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(combined, combined_len, hash);
  free(combined);

  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* bio = BIO_new(BIO_s_mem());
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(bio, hash, SHA_DIGEST_LENGTH);
  BIO_flush(bio);

  char* result;
  long len = BIO_get_mem_data(bio, &result);
  char* accept_key = get_memory(len + 1);
  memcpy(accept_key, result, len);
  accept_key[len] = '\0';

  BIO_free_all(bio);
  return accept_key;
}

static int _ws_upgrade(offs_client_t* client, const char* ws_host) {
  /* Generate a 16-byte random key and base64 encode */
  uint8_t raw_key[16];
  if (platform_random_bytes(raw_key, sizeof(raw_key)) != 0) return -1;

  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* bio = BIO_new(BIO_s_mem());
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(bio, raw_key, 16);
  BIO_flush(bio);

  char* key_b64;
  long key_len = BIO_get_mem_data(bio, &key_b64);
  char* client_key = get_memory(key_len + 1);
  memcpy(client_key, key_b64, key_len);
  client_key[key_len] = '\0';
  BIO_free_all(bio);

  /* Compute expected accept key for response validation */
  char* expected_accept = _ws_compute_accept_key(client_key);

  /* Build and send upgrade request */
  char request[1024];
  snprintf(request, sizeof(request),
    "GET /offs HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n",
    ws_host, client_key);

  free(client_key);

  if (_ws_send(client, (const uint8_t*)request, strlen(request)) < 0) {
    free(expected_accept);
    return -1;
  }

  /* Read response — loop until we have the complete HTTP headers (\r\n\r\n)
   * or the upgrade timeout expires. */
  char response[4096];
  size_t total = 0;
  uint64_t deadline_ns = platform_monotonic_ns() +
      (uint64_t)client->config.ws_upgrade_timeout_ms * 1000000ULL;
  while (platform_monotonic_ns() < deadline_ns) {
    ssize_t received;
    if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
      received = SSL_read(client->transport.ws.ssl, response + total, sizeof(response) - 1 - total);
    } else {
      received = platform_socket_recv(client->transport.ws.sock, response + total, sizeof(response) - 1 - total);
    }
    if (received <= 0) {
      free(expected_accept);
      return -1;
    }
    total += (size_t)received;
    response[total] = '\0';
    if (strstr(response, "\r\n\r\n") != NULL) break;
  }
  if (total == 0 || strstr(response, "\r\n\r\n") == NULL) {
    free(expected_accept);
    return -1;
  }

  /* Check for 101 Switching Protocols on the status line.
   * Parse "HTTP/x.y NNN" — the status code follows the first space. */
  {
    const char* first_space = strchr(response, ' ');
    if (first_space == NULL || first_space[1] != '1' ||
        first_space[2] != '0' || first_space[3] != '1' ||
        first_space[4] != ' ') {
      free(expected_accept);
      return -1;
    }
  }

  /* Validate Sec-WebSocket-Accept header */
  const char* accept_header = strstr(response, "Sec-WebSocket-Accept:");
  if (accept_header == NULL) {
    accept_header = strstr(response, "sec-websocket-accept:");
  }
  if (accept_header != NULL && expected_accept != NULL) {
    /* Skip past the header name (case-insensitive match) */
    const char* colon = strchr(accept_header, ':');
    if (colon != NULL) {
      accept_header = colon + 1;
    }
    while (*accept_header == ' ') accept_header++;
    char* crlf = strstr(accept_header, "\r\n");
    if (crlf != NULL) {
      size_t accept_len = (size_t)(crlf - accept_header);
      if (accept_len != strlen(expected_accept) ||
          memcmp(accept_header, expected_accept, accept_len) != 0) {
        free(expected_accept);
        return -1;
      }
    }
  }

  free(expected_accept);
  return 0;
}

static offs_client_t* _connect_attempt(const char* transport_url, const char* api_key,
                                         const offs_client_config_t* config) {
  if (transport_url == NULL) return NULL;

  offs_client_t* client = get_clear_memory(sizeof(offs_client_t));
  client->config = (config != NULL) ? *config : offs_client_config_default();
  client->transport_type = OFFS_TRANSPORT_UNIX;
  client->connected = 0;
  client->running = 0;
  client->framer = stream_framer_create();
  client->write_buffer = NULL;
  client->lock = platform_mutex_create();
  client->put_cb = NULL;
  client->put_cb_ctx = NULL;
  client->get_data_cb = NULL;
  client->get_data_cb_ctx = NULL;
  client->get_end_cb = NULL;
  client->get_end_cb_ctx = NULL;
  client->error_cb = NULL;
  client->error_cb_ctx = NULL;
  client->block_put_cb = NULL;
  client->block_put_cb_ctx = NULL;
  client->block_get_cb = NULL;
  client->block_get_cb_ctx = NULL;
  client->block_delete_cb = NULL;
  client->block_delete_cb_ctx = NULL;
  client->health_cb = NULL;
  client->health_cb_ctx = NULL;

  if (api_key != NULL) {
    size_t key_len = strlen(api_key);
    client->api_key = get_memory(key_len + 1);
    memcpy(client->api_key, api_key, key_len + 1);
  }

  if (strncmp(transport_url, "unix://", 7) == 0) {
    const char* path = transport_url + 7;
    client->transport.raw.sock = _connect_unix(path);
    client->transport.raw.is_unix = 1;
    client->transport_type = OFFS_TRANSPORT_UNIX;
  } else if (strncmp(transport_url, "tcp://", 6) == 0) {
    const char* addr = transport_url + 6;
    char* host = get_memory(strlen(addr) + 1);
    strcpy(host, addr);
    char* colon = strrchr(host, ':');
    if (colon == NULL) {
      free(host);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }
    *colon = '\0';
    uint16_t port = (uint16_t)atoi(colon + 1);
    client->transport.raw.sock = _connect_tcp(host, port);
    client->transport.raw.is_unix = 0;
    client->transport_type = OFFS_TRANSPORT_TCP;
    free(host);
  } else if (strncmp(transport_url, "ws://", 5) == 0 || strncmp(transport_url, "wss://", 6) == 0) {
    uint8_t is_ssl = (transport_url[4] == 's');
    const char* addr_start = is_ssl ? transport_url + 6 : transport_url + 5;
    char* addr_copy = get_memory(strlen(addr_start) + 1);
    strcpy(addr_copy, addr_start);
    /* Extract path (everything after first /) */
    char* path_start = strchr(addr_copy, '/');
    if (path_start != NULL) {
      *path_start = '\0';
    }
    /* Extract host and port */
    char* colon = strrchr(addr_copy, ':');
    uint16_t port;
    char* ws_host;
    if (colon != NULL) {
      *colon = '\0';
      port = (uint16_t)atoi(colon + 1);
      ws_host = addr_copy;
    } else {
      port = is_ssl ? 443 : 80;
      ws_host = addr_copy;
    }

    client->transport.ws.sock = _connect_tcp(ws_host, port);
    client->transport.ws.ssl = NULL;
    client->transport.ws.is_ssl = is_ssl;
    client->transport.ws.recv_buf = NULL;

    if (client->transport.ws.sock == NULL) {
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    _ssl_init(); /* Needed for SHA1 in WS upgrade handshake (and TLS if wss://) */

    if (is_ssl) {
      SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_client_method());
      if (ssl_ctx == NULL) {

        free(addr_copy);
        platform_socket_destroy(client->transport.ws.sock);
        stream_framer_destroy(client->framer);
        platform_mutex_destroy(client->lock);
        free(client->api_key);
        free(client);
        return NULL;
      }
      SSL* ssl = SSL_new(ssl_ctx);
      SSL_set_fd(ssl, platform_socket_fd(client->transport.ws.sock));
      SSL_set_tlsext_host_name(ssl, ws_host);
      if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);

        free(addr_copy);
        platform_socket_destroy(client->transport.ws.sock);
        stream_framer_destroy(client->framer);
        platform_mutex_destroy(client->lock);
        free(client->api_key);
        free(client);
        return NULL;
      }
      client->transport.ws.ssl = ssl;
      client->transport.ws.ssl_ctx = ssl_ctx;
    }

    /* addr_copy still alive here — ws_host points into it */
    if (_ws_upgrade(client, ws_host) != 0) {
      if (client->transport.ws.ssl != NULL) {
        SSL_free(client->transport.ws.ssl);
      }
      if (client->transport.ws.ssl_ctx != NULL) {
        SSL_CTX_free(client->transport.ws.ssl_ctx);
      }
      free(addr_copy);
      platform_socket_destroy(client->transport.ws.sock);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    free(addr_copy);

    client->transport_type = OFFS_TRANSPORT_WS;
    client->connected = 1;
    client->running = 1;
    _send_auth_request(client);
    /* WS doesn't use stream_framer — it uses ws_frame_parse instead */
    stream_framer_destroy(client->framer);
    client->framer = NULL;
    client->recv_thread = platform_thread_create(_recv_thread, client);
    return client;
  } else if (strncmp(transport_url, "wt://", 5) == 0 || strncmp(transport_url, "wts://", 6) == 0) {
#ifdef HAS_MSQUIC
    uint8_t is_secure = (transport_url[4] == 's');
    const char* addr_start = is_secure ? transport_url + 6 : transport_url + 5;
    char* addr_copy = get_memory(strlen(addr_start) + 1);
    strcpy(addr_copy, addr_start);

    /* Extract host and port */
    char* colon = strrchr(addr_copy, ':');
    uint16_t port;
    const char* wt_host;
    if (colon != NULL) {
      *colon = '\0';
      port = (uint16_t)atoi(colon + 1);
      wt_host = addr_copy;
    } else {
      port = 443;
      wt_host = addr_copy;
    }

    const struct QUIC_API_TABLE* msquic = offs_msquic_open();
    if (msquic == NULL) {
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    /* Registration */
    QUIC_REGISTRATION_CONFIG reg_config = { "offs-client", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    HQUIC registration = NULL;
    if (QUIC_FAILED(msquic->RegistrationOpen(&reg_config, &registration))) {
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    /* Configuration */
    HQUIC configuration = NULL;
    QUIC_SETTINGS settings = {0};
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    /* Idle timeout must be longer than the largest expected server-side
       processing time for a streaming PUT. MsQuic default is 30s, which
       is too short for multi-GB uploads because the server is silent
       while receiving. */
    settings.IdleTimeoutMs = 1800000;  /* 30 minutes */
    settings.IsSet.IdleTimeoutMs = TRUE;

    QUIC_BUFFER alpn = { sizeof("offs") - 1, (uint8_t*)"offs" };

    QUIC_CREDENTIAL_CONFIG cred_config = {0};
    cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (!is_secure) {
      cred_config.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    if (QUIC_FAILED(msquic->ConfigurationOpen(registration, &alpn, 1, &settings, sizeof(settings), NULL, &configuration))) {
      msquic->RegistrationClose(registration);
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }
    if (QUIC_FAILED(msquic->ConfigurationLoadCredential(configuration, &cred_config))) {
      msquic->ConfigurationClose(configuration);
      msquic->RegistrationClose(registration);
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    /* Open connection */
    HQUIC connection = NULL;
    wt_connect_context_t connect_ctx = { client, NULL, 0 };
    if (QUIC_FAILED(msquic->ConnectionOpen(registration, _wt_connection_callback, &connect_ctx, &connection))) {
      msquic->ConfigurationClose(configuration);
      msquic->RegistrationClose(registration);
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    /* Start connection */
    if (QUIC_FAILED(msquic->ConnectionStart(connection, configuration, QUIC_ADDRESS_FAMILY_INET, wt_host, port))) {
      msquic->ConnectionClose(connection);
      msquic->ConfigurationClose(configuration);
      msquic->RegistrationClose(registration);
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    /* Wait for connection (poll with configurable timeout) */
    {
      uint64_t connect_deadline_ns = platform_monotonic_ns() +
          (uint64_t)client->config.connect_timeout_ms * 1000000ULL;
      while (!connect_ctx.connected_event && platform_monotonic_ns() < connect_deadline_ns) {
        platform_sleep_ms(10);
      }
    }
    if (!connect_ctx.connected_event) {
      msquic->ConnectionClose(connection);
      msquic->ConfigurationClose(configuration);
      msquic->RegistrationClose(registration);
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    /* Open bidirectional stream */
    HQUIC stream = NULL;
    if (QUIC_FAILED(msquic->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE, _wt_stream_callback, client, &stream))) {
      msquic->ConnectionClose(connection);
      msquic->ConfigurationClose(configuration);
      msquic->RegistrationClose(registration);
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    /* Start the stream */
    if (QUIC_FAILED(msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
      msquic->StreamClose(stream);
      msquic->ConnectionClose(connection);
      msquic->ConfigurationClose(configuration);
      msquic->RegistrationClose(registration);
      offs_msquic_close();
      free(addr_copy);
      stream_framer_destroy(client->framer);
      platform_mutex_destroy(client->lock);
      free(client->api_key);
      free(client);
      return NULL;
    }

    client->transport_type = OFFS_TRANSPORT_WT;
    client->transport.wt.registration = registration;
    client->transport.wt.configuration = configuration;
    client->transport.wt.connection = connection;
    client->transport.wt.stream = stream;
    client->transport.wt.msquic = (void*)msquic;
    client->transport.wt.stream_open = 1;
    /* recv_lock, recv_cond, recv_buf, etc. are zeroed by get_clear_memory */
    client->connected = 1;
    client->running = 1;
    _send_auth_request(client);
    free(addr_copy);
    /* WT doesn't use a recv thread — data arrives via MsQuic stream callback */
    return client;
#else
    /* WT not supported — MsQuic not available */
    stream_framer_destroy(client->framer);
    platform_mutex_destroy(client->lock);
    free(client->api_key);
    free(client);
    return NULL;
#endif
  } else {
    stream_framer_destroy(client->framer);
    platform_mutex_destroy(client->lock);
    free(client->api_key);
    free(client);
    return NULL;
  }

  if (client->transport.raw.sock == NULL) {
    stream_framer_destroy(client->framer);
    platform_mutex_destroy(client->lock);
    free(client->api_key);
    free(client);
    return NULL;
  }

  client->connected = 1;
  client->running = 1;
  _send_auth_request(client);
  client->recv_thread = platform_thread_create(_recv_thread, client);

  return client;
}

offs_client_t* offs_client_connect_ex(const char* transport_url, const char* api_key,
                                       const offs_client_config_t* config) {
  offs_client_config_t effective_config = (config != NULL) ? *config : offs_client_config_default();
  uint32_t max_retries = effective_config.max_retries;

  for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
    offs_client_t* client = _connect_attempt(transport_url, api_key, &effective_config);
    if (client != NULL) return client;

    if (attempt < max_retries) {
      uint32_t delay_ms = _retry_delay_ms(&effective_config, attempt + 1);
      platform_sleep_ms(delay_ms);
    }
  }

  return NULL;
}

offs_client_t* offs_client_connect(const char* transport_url, const char* api_key) {
  return offs_client_connect_ex(transport_url, api_key, NULL);
}

void offs_client_disconnect(offs_client_t* client) {
  if (client == NULL) return;

  client->running = 0;
  client->connected = 0;

  /* Stop the poll-dancer loop to unblock the recv thread */
  if (client->loop != NULL) {
    pd_loop_stop(client->loop);
  }

  switch (client->transport_type) {
    case OFFS_TRANSPORT_UNIX:
    case OFFS_TRANSPORT_TCP:
      if (client->transport.raw.sock != NULL) {
        platform_socket_shutdown(client->transport.raw.sock, PLATFORM_SHUT_RDWR);
        platform_socket_destroy(client->transport.raw.sock);
        client->transport.raw.sock = NULL;
      }
      break;
    case OFFS_TRANSPORT_WS:
      if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
        SSL_shutdown(client->transport.ws.ssl);
        SSL_free(client->transport.ws.ssl);
      }
      if (client->transport.ws.ssl_ctx != NULL) {
        SSL_CTX_free(client->transport.ws.ssl_ctx);
      }
      if (client->transport.ws.sock != NULL) {
        platform_socket_shutdown(client->transport.ws.sock, PLATFORM_SHUT_RDWR);
        platform_socket_destroy(client->transport.ws.sock);
        client->transport.ws.sock = NULL;
      }
      if (client->transport.ws.recv_buf != NULL) {
        DESTROY(client->transport.ws.recv_buf, buffer);
      }
      break;
    case OFFS_TRANSPORT_WT:
#ifdef HAS_MSQUIC
      {
        const struct QUIC_API_TABLE* msquic = (const struct QUIC_API_TABLE*)client->transport.wt.msquic;
        if (client->transport.wt.stream != NULL) {
          msquic->StreamClose((HQUIC)client->transport.wt.stream);
        }
        if (client->transport.wt.connection != NULL) {
          msquic->ConnectionClose((HQUIC)client->transport.wt.connection);
        }
        if (client->transport.wt.configuration != NULL) {
          msquic->ConfigurationClose((HQUIC)client->transport.wt.configuration);
        }
        if (client->transport.wt.registration != NULL) {
          msquic->RegistrationClose((HQUIC)client->transport.wt.registration);
        }
        offs_msquic_close();
      }
#else
      /* WT not available — nothing to clean up */
#endif
      break;
  }

  if (client->recv_thread != NULL) {
    platform_thread_join(client->recv_thread);
  }

  if (client->framer != NULL) {
    stream_framer_destroy(client->framer);
  }
  if (client->write_buffer != NULL) {
    DESTROY(client->write_buffer, buffer);
  }
  platform_mutex_destroy(client->lock);
  free(client->api_key);
  free(client);
}

int offs_client_put(offs_client_t* client,
                    const char* content_type,
                    const char* file_name,
                    size_t stream_length,
                    const uint8_t* data,
                    size_t data_len,
                    offs_put_response_cb_t callback,
                    void* ctx) {
  offs_put_options_t options;
  memset(&options, 0, sizeof(options));
  options.content_type = content_type;
  options.file_name = file_name;
  options.stream_length = stream_length;
  return offs_client_put_ex(client, &options, data, data_len, callback, ctx);
}

static void _fill_put_request(client_api_put_request_t* msg, const offs_put_options_t* options,
                               const uint8_t* data, size_t data_len) {
  memset(msg, 0, sizeof(*msg));
  msg->content_type = (char*)options->content_type;
  msg->file_name = (char*)options->file_name;
  msg->stream_length = options->stream_length;
  msg->server_address = (char*)options->server_address;
  msg->data = (uint8_t*)data;
  msg->data_size = data_len;
  msg->recycler_urls = (char**)options->recycler_urls;
  msg->recycler_count = options->recycler_count;
  msg->temporary = options->temporary;
}

int offs_client_put_ex(offs_client_t* client,
                       const offs_put_options_t* options,
                       const uint8_t* data,
                       size_t data_len,
                       offs_put_response_cb_t callback,
                       void* ctx) {
  if (client == NULL || !client->connected || options == NULL) return -1;

  platform_mutex_lock(client->lock);
  client->put_cb = callback;
  client->put_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_put_request_t msg;
  _fill_put_request(&msg, options, data, data_len);

  cbor_item_t* frame = client_api_put_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_put_stream_start_ex(offs_client_t* client,
                                     const offs_put_options_t* options) {
  if (client == NULL || !client->connected || options == NULL) return -1;

  client_api_put_request_t msg;
  _fill_put_request(&msg, options, NULL, 0);

  cbor_item_t* frame = client_api_put_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_put_stream_start(offs_client_t* client,
                                  const char* content_type,
                                  const char* file_name,
                                  size_t stream_length) {
  offs_put_options_t options;
  memset(&options, 0, sizeof(options));
  options.content_type = content_type;
  options.file_name = file_name;
  options.stream_length = stream_length;
  return offs_client_put_stream_start_ex(client, &options);
}

int offs_client_put_stream_data(offs_client_t* client,
                                 const uint8_t* data,
                                 size_t len) {
  if (client == NULL || !client->connected) return -1;

  client_api_put_data_t msg;
  msg.data = (uint8_t*)data;
  msg.data_size = len;

  cbor_item_t* frame = client_api_put_data_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_put_stream_end(offs_client_t* client,
                                offs_put_response_cb_t callback,
                                void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->put_cb = callback;
  client->put_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  cbor_item_t* frame = client_api_put_end_encode();
  _send_frame(client, frame);

  return 0;
}

int offs_client_get(offs_client_t* client,
                     const char* ori_string,
                     offs_get_data_cb_t data_cb,
                     offs_get_end_cb_t end_cb,
                     offs_error_cb_t error_cb,
                     void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->get_data_cb = data_cb;
  client->get_data_cb_ctx = ctx;
  client->get_end_cb = end_cb;
  client->get_end_cb_ctx = ctx;
  client->error_cb = error_cb;
  client->error_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_get_request_t msg;
  msg.ori_string = (char*)ori_string;
  msg.has_range = 0;
  msg.range_start = 0;
  msg.range_end = 0;

  cbor_item_t* frame = client_api_get_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_block_put(offs_client_t* client,
    const uint8_t* data, size_t data_len, uint8_t encoding,
    offs_block_put_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->block_put_cb = callback;
  client->block_put_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_block_put_request_t msg;
  msg.data = (uint8_t*)data;
  msg.data_size = data_len;
  msg.encoding = encoding;

  cbor_item_t* frame = client_api_block_put_request_encode(&msg);
  _send_frame(client, frame);
  return 0;
}

int offs_client_block_get(offs_client_t* client,
    const uint8_t* hash_data, size_t hash_len,
    offs_block_get_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->block_get_cb = callback;
  client->block_get_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_block_get_request_t msg;
  msg.hash_data = (uint8_t*)hash_data;
  msg.hash_len = hash_len;

  cbor_item_t* frame = client_api_block_get_request_encode(&msg);
  _send_frame(client, frame);
  return 0;
}

int offs_client_block_delete(offs_client_t* client,
    const uint8_t* hash_data, size_t hash_len,
    offs_block_delete_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->block_delete_cb = callback;
  client->block_delete_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_block_delete_request_t msg;
  msg.hash_data = (uint8_t*)hash_data;
  msg.hash_len = hash_len;

  cbor_item_t* frame = client_api_block_delete_request_encode(&msg);
  _send_frame(client, frame);
  return 0;
}

int offs_client_health(offs_client_t* client,
    offs_health_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->health_cb = callback;
  client->health_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  cbor_item_t* frame = client_api_health_request_encode();
  _send_frame(client, frame);
  return 0;
}

buffer_t* offs_http_get(const char* url) {
  if (!url) return NULL;

  /* Parse URL: http://host[:port]/path */
  const char* prefix = "http://";
  if (strncmp(url, prefix, 7) != 0) return NULL;
  const char* rest = url + 7;

  /* Extract host */
  const char* host_start = rest;
  const char* host_end = strchr(host_start, ':');
  const char* port_end = strchr(host_start, '/');
  const char* path_start = NULL;

  char host[256];
  int port = 80;

  if (host_end && (!port_end || host_end < port_end)) {
    /* Has port */
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len >= sizeof(host)) return NULL;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    port = (int)strtol(host_end + 1, NULL, 10);
    path_start = strchr(host_end, '/');
  } else if (port_end) {
    /* No port, has path */
    size_t host_len = (size_t)(port_end - host_start);
    if (host_len >= sizeof(host)) return NULL;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    path_start = port_end;
  } else {
    /* No port, no path */
    size_t host_len = strlen(host_start);
    if (host_len >= sizeof(host)) return NULL;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    path_start = "/";
  }

  if (port <= 0 || port > 65535) return NULL;
  if (!path_start) path_start = "/";

  /* Resolve hostname to IP address */
  char ip_str[INET_ADDRSTRLEN];
  {
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return NULL;
    struct sockaddr_in* sin = (struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
    freeaddrinfo(res);
  }

  /* Create socket and resolve address using platform abstractions */
  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (sock == NULL) return NULL;

  platform_address_t addr;
  if (platform_address_parse(&addr, ip_str, (uint16_t)port) != 0) {
    platform_socket_destroy(sock);
    return NULL;
  }

  /* Set send/recv timeouts on the underlying fd. Windows interprets
   * SO_RCVTIMEO/SO_SNDTIMEO as a DWORD millisecond count; POSIX uses
   * struct timeval. */
  {
    int fd = platform_socket_fd(sock);
#ifdef _WIN32
    DWORD timeout_ms = 10000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
  }

  if (platform_socket_connect(sock, &addr) < 0) {
    log_error("offs_http_get: connect to %s:%d failed", ip_str, port);
    platform_socket_destroy(sock);
    return NULL;
  }

  /* Build and send HTTP GET request */
  {
    char request[4096];
    int req_len = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
      path_start, host);
    if (req_len < 0 || req_len >= (int)sizeof(request)) {
      platform_socket_destroy(sock);
      return NULL;
    }

    if (platform_socket_send(sock, request, (size_t)req_len) < 0) {
      log_error("offs_http_get: send request to %s:%d failed", ip_str, port);
      platform_socket_destroy(sock);
      return NULL;
    }
  }

  /* Read response */
  char resp_buf[65536];
  size_t total = 0;
  ssize_t n;
  while ((n = platform_socket_recv(sock, resp_buf + total,
      sizeof(resp_buf) - total - 1)) > 0) {
    total += (size_t)n;
    if (total >= sizeof(resp_buf) - 1) break;
  }
  platform_socket_destroy(sock);

  if (total == 0) return NULL;
  resp_buf[total] = '\0';

  /* Find body (after \r\n\r\n) */
  char* body = strstr(resp_buf, "\r\n\r\n");
  if (!body) return NULL;
  body += 4;

  size_t body_len = (size_t)(resp_buf + total - body);

  /* Parse Content-Length if present (case-insensitive, RFC 7230) */
  {
    char* cl_header = NULL;
    for (char* scan = resp_buf; *scan; scan++) {
      if (strncasecmp(scan, "Content-Length:", 15) == 0) {
        cl_header = scan;
        break;
      }
    }
    if (cl_header) {
      cl_header += 15;
      while (*cl_header == ' ') cl_header++;
      size_t cl = (size_t)strtol(cl_header, NULL, 10);
      if (cl < body_len) body_len = cl;
    }
  }

  if (body_len == 0) return NULL;

  buffer_t* result = buffer_create(body_len);
  if (!result) return NULL;
  memcpy(result->data, body, body_len);
  return result;
}
