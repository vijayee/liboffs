//
// Lightweight HTTP/3 WebTransport-compatible QUIC endpoint.
//
// Uses MsQuic with ALPN "h3" and speaks length-prefixed CBOR frames over the
// first client-initiated bidirectional stream. This is a development/testing
// endpoint that matches the framing used by the browser JS client; it does not
// implement the full HTTP/3 control-stream SETTINGS/HEADERS exchange or QPACK.
//
#include "webtransport_h3.h"

#ifdef HAS_MSQUIC

#include "../../Util/allocator.h"
#include "../../Platform/platform.h"
#include "../../Util/log.h"
#include "../../Network/peer_verify.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include "../client_api_wire.h"
#include "../block_handlers.h"
#include "../health_handler.h"
#include "../../OFFStreams/off_url.h"
#include "../../OFFStreams/ori.h"
#include "../../OFFStreams/readable_off_stream.h"
#include "../../OFFStreams/readable_descriptor.h"
#include "../../OFFStreams/writeable_off_stream.h"
#include "../../OFFStreams/writeable_descriptor.h"
#include "../../OFFStreams/block_recipe.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../BlockCache/block_cache.h"
#include "../../Buffer/buffer.h"
#include "../../Actor/actor.h"
#include "../../RefCounter/refcounter.h"
#include "../../Streams/stream.h"
#include "../../Util/bcrypt.h"
#include "../../Util/vec.h"
#include "../../Network/msquic_singleton.h"
#include <poll-dancer/poll-dancer.h>
#include <cbor.h>
#include <cJSON.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WT_H3_READ_BUFFER_SIZE 65536
#define WT_H3_LENGTH_PREFIX_SIZE 4
#define WT_H3_MAX_MESSAGE_SIZE (16 * 1024 * 1024)

typedef struct {
  uint8_t* frame;
  QUIC_BUFFER buf;
} wt_h3_send_context_t;

static void* _wt_h3_server_thread(void* arg);

static QUIC_STATUS QUIC_API _wt_h3_listener_callback(
    HQUIC listener, void* context, QUIC_LISTENER_EVENT* event);
static QUIC_STATUS QUIC_API _wt_h3_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event);
static QUIC_STATUS QUIC_API _wt_h3_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event);

typedef struct webtransport_h3_conn_t webtransport_h3_conn_t;

static void _wt_h3_send_frame(webtransport_h3_conn_t* conn, cbor_item_t* frame);
static void _wt_h3_send_error(webtransport_h3_conn_t* conn, uint8_t status_code, const char* message);
static void _wt_h3_get_on_rs_data(void* ctx, void* data);
static void _wt_h3_get_on_rs_close(void* ctx, void* unused);
static void _wt_h3_get_on_rs_error(void* ctx, void* error);
static void _wt_h3_get_on_desc_close(void* ctx, void* unused);
static void _wt_h3_get_on_desc_error(void* ctx, void* error);
static void _wt_h3_get_on_tuple(void* ctx, void* data);
static void _wt_h3_dispatch_message(webtransport_h3_conn_t* conn, uint8_t type, cbor_item_t* frame);
static void _wt_h3_connection_dispatch(void* state, message_t* msg);
static void _wt_h3_block_send_frame(block_connection_t* conn, cbor_item_t* frame);
static void _wt_h3_block_send_error(block_connection_t* conn, uint8_t status, const char* msg);

typedef struct webtransport_h3_destroy_node_t {
  void* item;
  struct webtransport_h3_destroy_node_t* next;
} webtransport_h3_destroy_node_t;

typedef struct webtransport_h3_conn_t webtransport_h3_conn_t;

typedef struct webtransport_h3_t {
  actor_t actor;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  ATOMIC(uint8_t) running;
  ATOMIC(uint8_t) listening;
  platform_thread_t* thread;
  pd_loop_t* loop;
  platform_mutex_t* destroy_lock;
  webtransport_h3_destroy_node_t* destroy_head;

  const struct QUIC_API_TABLE* msquic;
  HQUIC registration;
  HQUIC configuration;
  HQUIC listener;
  char* host;
  uint16_t port;
  char* cert_path;
  char* key_path;
  void* peer_verify;
  bool allow_secure;
  void* win_cert_store;
  void* win_cert_context;
  ATOMIC(size_t) active_connections;

  platform_mutex_t* conn_lock;
  vec_t(webtransport_h3_conn_t*) connections;
  char* api_key_hash;
  health_context_t* health_ctx;
} webtransport_h3_t;

struct webtransport_h3_conn_t {
  refcounter_t refcounter;
  actor_t actor;
  webtransport_h3_t* transport;
  HQUIC connection;
  HQUIC stream;
  uint8_t is_closing;
  uint8_t is_authenticated;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  block_handler_ctx_t block_ctx;

  uint8_t* recv_buf;
  size_t recv_size;
  size_t recv_capacity;

  /* Buffered PUT state */
  writeable_off_stream_t* put_ws;
  writeable_descriptor_t* put_desc;
  new_blocks_recipe_t* put_recipe;
  char* put_content_type;
  char* put_file_name;
  size_t put_stream_length;
  char* put_server_address;
  buffer_t* put_file_hash;
  buffer_t* put_descriptor_hash;
};

/* Pipeline context for GET requests */
typedef struct {
  refcounter_t refcounter;
  webtransport_h3_conn_t* connection;
  readable_off_stream_t* rs;
  readable_descriptor_t* desc;
  ori_t* ori;
} wt_h3_get_pipeline_t;

static wt_h3_get_pipeline_t* _wt_h3_get_pipeline_create(webtransport_h3_conn_t* conn, ori_t* ori) {
  wt_h3_get_pipeline_t* pipeline = get_clear_memory(sizeof(wt_h3_get_pipeline_t));
  refcounter_init((refcounter_t*)pipeline);
  pipeline->connection = conn;
  pipeline->ori = ori;
  return pipeline;
}

/* Forward declarations */
static void _wt_h3_destroy_stack_init(webtransport_h3_t* transport);
static void _wt_h3_destroy_stack_push(webtransport_h3_t* transport, void* item);
static void _wt_h3_destroy_stack_drain(webtransport_h3_t* transport);
static void _wt_h3_destroy_stack_destroy(webtransport_h3_t* transport);
static void _wt_h3_connection_destroy(webtransport_h3_conn_t* conn);
static void _wt_h3_send_raw(webtransport_h3_conn_t* conn, const uint8_t* data, size_t length);

static void _wt_h3_destroy_stack_init(webtransport_h3_t* transport) {
  transport->destroy_lock = platform_mutex_create();
  transport->destroy_head = NULL;
}

static void _wt_h3_destroy_stack_push(webtransport_h3_t* transport, void* item) {
  webtransport_h3_destroy_node_t* node = get_clear_memory(sizeof(webtransport_h3_destroy_node_t));
  node->item = item;
  platform_mutex_lock(transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_mutex_unlock(transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _wt_h3_destroy_stack_drain(webtransport_h3_t* transport) {
  webtransport_h3_destroy_node_t* node;
  platform_mutex_lock(transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_mutex_unlock(transport->destroy_lock);
  while (node != NULL) {
    webtransport_h3_destroy_node_t* next = node->next;
    HQUIC handle = (HQUIC)node->item;
    if (handle != NULL) {
      transport->msquic->StreamClose(handle);
    }
    free(node);
    node = next;
  }
}

static void _wt_h3_destroy_stack_destroy(webtransport_h3_t* transport) {
  _wt_h3_destroy_stack_drain(transport);
  platform_mutex_destroy(transport->destroy_lock);
}

void _wt_h3_server_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
  /* Reserved for future control messages. */
}

webtransport_h3_t* webtransport_h3_create(scheduler_pool_t* pool,
                                           block_cache_t* bc,
                                           ofd_cache_t* ofd_cache,
                                           tuple_cache_t* tc,
                                           const char* host,
                                           uint16_t port,
                                           const char* cert_path,
                                           const char* key_path,
                                           const char* ca_path,
                                           bool allow_secure,
                                           const char* api_key_hash,
                                           health_context_t* health_ctx) {
  webtransport_h3_t* transport = get_clear_memory(sizeof(webtransport_h3_t));
  transport->pool = pool;
  transport->bc = bc;
  transport->ofd_cache = ofd_cache;
  transport->tc = tc;
  actor_init(&transport->actor, transport, _wt_h3_server_dispatch, transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  atomic_store(&transport->listening, 0);
  transport->listener = NULL;
  transport->registration = NULL;
  transport->configuration = NULL;
  atomic_store(&transport->active_connections, 0);
  _wt_h3_destroy_stack_init(transport);
  transport->conn_lock = platform_mutex_create();

  transport->host = get_memory(strlen(host) + 1);
  memcpy(transport->host, host, strlen(host) + 1);
  transport->port = port;

  if (api_key_hash != NULL) {
    transport->api_key_hash = get_memory(strlen(api_key_hash) + 1);
    memcpy(transport->api_key_hash, api_key_hash, strlen(api_key_hash) + 1);
  }
  transport->health_ctx = health_ctx;

  if (cert_path != NULL) {
    transport->cert_path = get_memory(strlen(cert_path) + 1);
    memcpy(transport->cert_path, cert_path, strlen(cert_path) + 1);
  }
  if (key_path != NULL) {
    transport->key_path = get_memory(strlen(key_path) + 1);
    memcpy(transport->key_path, key_path, strlen(key_path) + 1);
  }

  transport->peer_verify = NULL;
  transport->allow_secure = allow_secure;
  if (ca_path != NULL) {
    transport->peer_verify = peer_verify_ctx_create_from_pem_file(ca_path);
    if (transport->peer_verify == NULL) {
      fprintf(stderr, "webtransport_h3_create: failed to load CA from %s\n", ca_path);
      free(transport->cert_path);
      free(transport->key_path);
      free(transport->host);
      free(transport->api_key_hash);
      platform_mutex_destroy(transport->conn_lock);
      _wt_h3_destroy_stack_destroy(transport);
      pd_loop_destroy(transport->loop);
      actor_destroy(&transport->actor);
      free(transport);
      return NULL;
    }
  }

  transport->win_cert_store = NULL;
  transport->win_cert_context = NULL;

  transport->msquic = offs_msquic_open();
  if (transport->msquic == NULL) {
    fprintf(stderr, "webtransport_h3_create: MsQuic initialization failed\n");
    free(transport->cert_path);
    free(transport->key_path);
    free(transport->host);
    free(transport->api_key_hash);
    platform_mutex_destroy(transport->conn_lock);
    _wt_h3_destroy_stack_destroy(transport);
    pd_loop_destroy(transport->loop);
    actor_destroy(&transport->actor);
    free(transport);
    return NULL;
  }

  return transport;
}

void webtransport_h3_destroy(webtransport_h3_t* transport) {
  if (transport == NULL) {
    return;
  }
  if (atomic_load(&transport->running)) {
    webtransport_h3_stop(transport);
  }

  platform_mutex_lock(transport->conn_lock);
  for (int i = 0; i < transport->connections.length; i++) {
    webtransport_h3_conn_t* conn = transport->connections.data[i];
    conn->is_closing = 1;
    conn->transport = NULL;
  }
  platform_mutex_unlock(transport->conn_lock);

  for (int i = 0; i < transport->connections.length; i++) {
    webtransport_h3_conn_t* conn = transport->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  if (!atomic_load_explicit(&transport->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(transport->pool);
  }

  platform_mutex_lock(transport->conn_lock);
  for (int i = transport->connections.length - 1; i >= 0; i--) {
    webtransport_h3_conn_t* conn = transport->connections.data[i];
    actor_detach_pool(&conn->actor);
    message_queue_destroy(&conn->actor.queue);
    if (conn->stream != NULL) {
      transport->msquic->StreamClose(conn->stream);
    }
    if (conn->connection != NULL) {
      transport->msquic->ConnectionClose(conn->connection);
    }
    if (conn->recv_buf != NULL) {
      free(conn->recv_buf);
    }
    free(conn);
  }
  vec_deinit(&transport->connections);
  platform_mutex_unlock(transport->conn_lock);

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

  free(transport->cert_path);
  free(transport->key_path);
  if (transport->peer_verify != NULL) {
    peer_verify_ctx_destroy((peer_verify_ctx_t*)transport->peer_verify);
    transport->peer_verify = NULL;
  }
  free(transport->host);
  free(transport->api_key_hash);
  platform_mutex_destroy(transport->conn_lock);
  actor_destroy(&transport->actor);
  _wt_h3_destroy_stack_destroy(transport);
  pd_loop_destroy(transport->loop);
  free(transport);
}

static QUIC_STATUS QUIC_API _wt_h3_listener_callback(
    HQUIC listener, void* context, QUIC_LISTENER_EVENT* event) {
  webtransport_h3_t* transport = (webtransport_h3_t*)context;
  (void)listener;

  switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
      HQUIC connection = event->NEW_CONNECTION.Connection;
      transport->msquic->SetCallbackHandler(connection, _wt_h3_connection_callback, transport);
      transport->msquic->ConnectionSetConfiguration(connection, transport->configuration);
      atomic_fetch_add(&transport->active_connections, 1);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API _wt_h3_connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
  webtransport_h3_t* transport = (webtransport_h3_t*)context;
  (void)connection;

  switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
      break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
      atomic_fetch_sub(&transport->active_connections, 1);
      break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
      HQUIC stream = event->PEER_STREAM_STARTED.Stream;
      webtransport_h3_conn_t* conn = get_clear_memory(sizeof(webtransport_h3_conn_t));
      refcounter_init((refcounter_t*)conn);
      conn->transport = transport;
      conn->connection = connection;
      conn->stream = stream;
      conn->pool = transport->pool;
      conn->bc = transport->bc;
      conn->ofd_cache = transport->ofd_cache;
      conn->tc = transport->tc;
      conn->block_ctx.conn = conn;
      conn->block_ctx.bc = transport->bc;
      conn->block_ctx.actor = &conn->actor;
      conn->block_ctx.is_authenticated = 0;
      conn->block_ctx.send_frame = _wt_h3_block_send_frame;
      conn->block_ctx.send_error = _wt_h3_block_send_error;
      actor_init(&conn->actor, conn, _wt_h3_connection_dispatch, transport->pool);
      platform_mutex_lock(transport->conn_lock);
      vec_push(&transport->connections, conn);
      platform_mutex_unlock(transport->conn_lock);
      transport->msquic->SetCallbackHandler(stream, _wt_h3_stream_callback, conn);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

static void _wt_h3_connection_dispatch(void* state, message_t* msg) {
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)state;
  if (conn->is_closing) {
    return;
  }
  switch (msg->type) {
    case CACHE_PUT_RESULT:
    case CACHE_GET_RESULT:
    case CACHE_REMOVE_RESULT:
      block_handle_cache_result(&conn->block_ctx, msg);
      break;
    default:
      break;
  }
}

static void _wt_h3_send_raw(webtransport_h3_conn_t* conn, const uint8_t* data, size_t length) {
  if (conn->transport == NULL || conn->stream == NULL) {
    return;
  }
  wt_h3_send_context_t* send_ctx = get_clear_memory(sizeof(wt_h3_send_context_t));
  send_ctx->frame = get_memory(length);
  memcpy(send_ctx->frame, data, length);
  send_ctx->buf.Buffer = send_ctx->frame;
  send_ctx->buf.Length = (uint32_t)length;
  QUIC_STATUS status = conn->transport->msquic->StreamSend(
      conn->stream, &send_ctx->buf, 1, QUIC_SEND_FLAG_NONE, send_ctx);
  if (QUIC_FAILED(status)) {
    free(send_ctx->frame);
    free(send_ctx);
  }
}

static void _wt_h3_send_frame(webtransport_h3_conn_t* conn, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);
  if (cbor_buf == NULL || cbor_len == 0) {
    return;
  }
  if (cbor_len > WT_H3_MAX_MESSAGE_SIZE) {
    free(cbor_buf);
    return;
  }

  uint8_t* prefixed = get_memory(WT_H3_LENGTH_PREFIX_SIZE + cbor_len);
  prefixed[0] = (uint8_t)((cbor_len >> 24) & 0xFF);
  prefixed[1] = (uint8_t)((cbor_len >> 16) & 0xFF);
  prefixed[2] = (uint8_t)((cbor_len >> 8) & 0xFF);
  prefixed[3] = (uint8_t)(cbor_len & 0xFF);
  memcpy(prefixed + WT_H3_LENGTH_PREFIX_SIZE, cbor_buf, cbor_len);
  free(cbor_buf);

  _wt_h3_send_raw(conn, prefixed, WT_H3_LENGTH_PREFIX_SIZE + cbor_len);
  free(prefixed);
}

static void _wt_h3_send_error(webtransport_h3_conn_t* conn, uint8_t status_code, const char* message) {
  client_api_error_t error_msg;
  error_msg.status_code = status_code;
  error_msg.message = (char*)message;
  cbor_item_t* frame = client_api_error_encode(&error_msg);
  _wt_h3_send_frame(conn, frame);
}

static void _wt_h3_block_send_frame(block_connection_t* conn, cbor_item_t* frame) {
  _wt_h3_send_frame((webtransport_h3_conn_t*)conn, frame);
}

static void _wt_h3_block_send_error(block_connection_t* conn, uint8_t status, const char* msg) {
  _wt_h3_send_error((webtransport_h3_conn_t*)conn, status, msg);
}

static void _wt_h3_feed_message(webtransport_h3_conn_t* conn, const uint8_t* data, size_t length) {
  struct cbor_load_result load_result;
  cbor_item_t* frame = cbor_load(data, length, &load_result);
  if (frame == NULL || load_result.error.code != CBOR_ERR_NONE) {
    if (frame != NULL) {
      cbor_decref(&frame);
    }
    _wt_h3_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid CBOR frame");
    return;
  }
  uint8_t type = client_api_wire_get_type(frame);
  _wt_h3_dispatch_message(conn, type, frame);
  cbor_decref(&frame);
}

static void _wt_h3_append_recv(webtransport_h3_conn_t* conn, const uint8_t* data, size_t length) {
  if (conn->recv_size + length > conn->recv_capacity) {
    size_t new_capacity = conn->recv_capacity ? conn->recv_capacity * 2 : WT_H3_READ_BUFFER_SIZE;
    while (new_capacity < conn->recv_size + length) {
      new_capacity *= 2;
    }
    conn->recv_buf = realloc(conn->recv_buf, new_capacity);
    conn->recv_capacity = new_capacity;
  }
  memcpy(conn->recv_buf + conn->recv_size, data, length);
  conn->recv_size += length;

  while (conn->recv_size >= WT_H3_LENGTH_PREFIX_SIZE) {
    size_t msg_len = ((size_t)conn->recv_buf[0] << 24) |
                     ((size_t)conn->recv_buf[1] << 16) |
                     ((size_t)conn->recv_buf[2] << 8) |
                     (size_t)conn->recv_buf[3];
    if (msg_len > WT_H3_MAX_MESSAGE_SIZE) {
      _wt_h3_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Message too large");
      conn->recv_size = 0;
      return;
    }
    if (conn->recv_size < WT_H3_LENGTH_PREFIX_SIZE + msg_len) {
      break;
    }
    _wt_h3_feed_message(conn, conn->recv_buf + WT_H3_LENGTH_PREFIX_SIZE, msg_len);
    size_t consumed = WT_H3_LENGTH_PREFIX_SIZE + msg_len;
    memmove(conn->recv_buf, conn->recv_buf + consumed, conn->recv_size - consumed);
    conn->recv_size -= consumed;
  }
}

static QUIC_STATUS QUIC_API _wt_h3_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)context;
  (void)stream;

  switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
      uint32_t buffer_count = event->RECEIVE.BufferCount;
      for (uint32_t i = 0; i < buffer_count; i++) {
        _wt_h3_append_recv(conn, buffers[i].Buffer, buffers[i].Length);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
      if (event->SEND_COMPLETE.ClientContext != NULL) {
        wt_h3_send_context_t* send_ctx = (wt_h3_send_context_t*)event->SEND_COMPLETE.ClientContext;
        if (send_ctx->frame != NULL) {
          free(send_ctx->frame);
        }
        free(send_ctx);
      }
      break;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
      if (conn->transport != NULL) {
        _wt_h3_destroy_stack_push(conn->transport, stream);
      }
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

/* PUT pipeline callbacks */

static void _wt_h3_put_on_descriptor_data(void* ctx, void* data) {
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (conn->put_descriptor_hash != NULL) {
    DESTROY(conn->put_descriptor_hash, buffer);
  }
  conn->put_descriptor_hash = REFERENCE(payload, buffer_t);
}

static void _wt_h3_put_on_stream_data(void* ctx, void* data) {
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)ctx;
  buffer_t* buf = (buffer_t*)data;
  if (buf->size == 32 && conn->put_file_hash == NULL) {
    conn->put_file_hash = REFERENCE(buf, buffer_t);
  } else {
    tuple_t* tuple = REFERENCE(buf, tuple_t);
    writeable_descriptor_write(conn->put_desc, tuple);
    DESTROY(tuple, tuple);
  }
}

static void _wt_h3_put_on_stream_error(void* ctx, void* error) {
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)ctx;
  const char* message = "PUT stream error";
  if (error != NULL) {
    async_error_t* async_error = (async_error_t*)error;
    if (async_error->message != NULL) {
      message = async_error->message;
    }
  }
  _wt_h3_send_error(conn, CLIENT_API_STATUS_INTERNAL_ERROR, message);
}

static void _wt_h3_put_on_descriptor_error(void* ctx, void* error) {
  (void)error;
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)ctx;
  _wt_h3_send_error(conn, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT descriptor error");
}

static void _wt_h3_put_on_stream_close(void* ctx, void* unused) {
  (void)unused;
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)ctx;
  writeable_descriptor_close(conn->put_desc);
}

static void _wt_h3_put_on_descriptor_close(void* ctx, void* unused) {
  (void)unused;
  webtransport_h3_conn_t* conn = (webtransport_h3_conn_t*)ctx;

  off_url_t* url = off_url_create();
  free(url->content_type);
  url->content_type = strdup(conn->put_content_type);
  url->file_name = strdup(conn->put_file_name);
  url->stream_length = conn->put_stream_length;
  if (conn->put_server_address != NULL) {
    free(url->server_address);
    url->server_address = strdup(conn->put_server_address);
  }
  url->file_hash = buffer_copy(conn->put_file_hash);
  url->descriptor_hash = buffer_copy(conn->put_descriptor_hash);

  char* ori_string = off_url_to_string(url);
  client_api_put_response_t response;
  response.ori_string = ori_string;
  cbor_item_t* frame = client_api_put_response_encode(&response);
  _wt_h3_send_frame(conn, frame);
  free(ori_string);
  off_url_destroy(url);

  refcounter_dereference((refcounter_t*)conn->put_recipe);
  scheduler_pool_defer_cleanup(((stream_t*)conn->put_ws)->pool, conn->put_recipe,
                               (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)conn->put_desc);
  stream_deferred_deref((stream_t*)conn->put_ws);

  if (conn->put_content_type != NULL) free(conn->put_content_type);
  if (conn->put_file_name != NULL) free(conn->put_file_name);
  if (conn->put_server_address != NULL) free(conn->put_server_address);
  if (conn->put_file_hash != NULL) DESTROY(conn->put_file_hash, buffer);
  if (conn->put_descriptor_hash != NULL) DESTROY(conn->put_descriptor_hash, buffer);
  conn->put_ws = NULL;
  conn->put_desc = NULL;
  conn->put_recipe = NULL;
  conn->put_content_type = NULL;
  conn->put_file_name = NULL;
  conn->put_server_address = NULL;
  conn->put_file_hash = NULL;
  conn->put_descriptor_hash = NULL;
}

static void _wt_h3_handle_put(webtransport_h3_conn_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _wt_h3_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_put_request_decode(frame, &msg) != 0) {
    _wt_h3_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid PUT request");
    return;
  }
  if (msg.content_type == NULL || msg.file_name == NULL) {
    client_api_put_request_destroy(&msg);
    _wt_h3_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Missing content_type or file_name");
    return;
  }

  size_t tuple_size = msg.has_tuple_size ? msg.tuple_size : 3;
  size_t required = writeable_off_stream_estimate_required_bytes(
      msg.stream_length, tuple_size, 32);
  if (block_cache_can_fit(conn->bc, required) != CACHE_FIT_OK) {
    _wt_h3_send_error(conn, CLIENT_API_STATUS_INTERNAL_ERROR, "cache full");
    client_api_put_request_destroy(&msg);
    return;
  }

  conn->put_content_type = msg.content_type;
  conn->put_file_name = msg.file_name;
  conn->put_stream_length = msg.stream_length;
  conn->put_server_address = msg.server_address;
  msg.content_type = NULL;
  msg.file_name = NULL;
  msg.server_address = NULL;

  block_size_e block_type = standard;
  size_t descriptor_pad = 32;
  conn->put_recipe = new_blocks_recipe_create(conn->pool, conn->bc, block_type);
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  vec_push(&recipes, (block_recipe_t*)conn->put_recipe);

  conn->put_ws = writeable_off_stream_create(
      conn->pool, conn->bc, conn->tc, block_type, tuple_size, descriptor_pad, recipes, NULL);
  conn->put_desc = writeable_descriptor_create(
      conn->pool, conn->bc, block_type, descriptor_pad, tuple_size, msg.stream_length, NULL);

  stream_subscribe((stream_t*)conn->put_ws, close_event, conn, _wt_h3_put_on_stream_close, NULL);
  stream_subscribe((stream_t*)conn->put_ws, data_event, conn, _wt_h3_put_on_stream_data, NULL);
  stream_subscribe((stream_t*)conn->put_ws, error_event, conn, _wt_h3_put_on_stream_error, NULL);
  stream_subscribe((stream_t*)conn->put_desc, close_event, conn, _wt_h3_put_on_descriptor_close, NULL);
  stream_subscribe((stream_t*)conn->put_desc, data_event, conn, _wt_h3_put_on_descriptor_data, NULL);
  stream_subscribe((stream_t*)conn->put_desc, error_event, conn, _wt_h3_put_on_descriptor_error, NULL);

  if (msg.data != NULL && msg.data_size > 0) {
    buffer_t* data = buffer_create_from_pointer_copy(msg.data, msg.data_size);
    writeable_off_stream_write(conn->put_ws, data);
    DESTROY(data, buffer);
    writeable_off_stream_finalize(conn->put_ws);
  }

  free(msg.data);
  msg.data = NULL;
  msg.data_size = 0;
  client_api_put_request_destroy(&msg);
}

static void _wt_h3_get_on_tuple(void* ctx, void* data) {
  wt_h3_get_pipeline_t* pipeline = (wt_h3_get_pipeline_t*)ctx;
  tuple_t* tuple = (tuple_t*)data;
  readable_off_stream_write(pipeline->rs, tuple);
}

static void _wt_h3_get_on_rs_data(void* ctx, void* data) {
  wt_h3_get_pipeline_t* pipeline = (wt_h3_get_pipeline_t*)ctx;
  buffer_t* buf = (buffer_t*)data;
  client_api_get_data_t get_data;
  get_data.data = buf->data;
  get_data.data_size = buf->size;
  _wt_h3_send_frame(pipeline->connection, client_api_get_data_encode(&get_data));
}

static void _wt_h3_get_on_rs_close(void* ctx, void* unused) {
  (void)unused;
  wt_h3_get_pipeline_t* pipeline = (wt_h3_get_pipeline_t*)ctx;
  _wt_h3_send_frame(pipeline->connection, client_api_get_end_encode());
  stream_deferred_deref((stream_t*)pipeline->rs);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _wt_h3_get_on_rs_error(void* ctx, void* error) {
  (void)error;
  wt_h3_get_pipeline_t* pipeline = (wt_h3_get_pipeline_t*)ctx;
  _wt_h3_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "Stream error");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
}

static void _wt_h3_get_on_desc_close(void* ctx, void* unused) {
  (void)unused;
  wt_h3_get_pipeline_t* pipeline = (wt_h3_get_pipeline_t*)ctx;
  stream_deferred_deref((stream_t*)pipeline->desc);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _wt_h3_get_on_desc_error(void* ctx, void* error) {
  (void)error;
  wt_h3_get_pipeline_t* pipeline = (wt_h3_get_pipeline_t*)ctx;
  _wt_h3_send_error(pipeline->connection, CLIENT_API_STATUS_NOT_FOUND, "Not found");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
  stream_deactivate((stream_t*)pipeline->desc, NULL);
}

static void _wt_h3_handle_get(webtransport_h3_conn_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _wt_h3_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_get_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_get_request_decode(frame, &msg) != 0) {
    _wt_h3_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid GET request");
    return;
  }

  off_url_t* url = off_url_parse(msg.ori_string);
  client_api_get_request_destroy(&msg);
  if (url == NULL) {
    _wt_h3_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid OFF URL");
    return;
  }

  ori_t* ori = ori_create(url->stream_length);
  ori->block_type = standard;
  ori->tuple_size = 3;
  if (url->descriptor_hash != NULL) {
    ori->descriptor_hash = REFERENCE(url->descriptor_hash, buffer_t);
  }
  if (url->file_hash != NULL) {
    ori->file_hash = REFERENCE(url->file_hash, buffer_t);
  }
  if (url->file_name != NULL) {
    ori->file_name = get_memory(strlen(url->file_name) + 1);
    memcpy(ori->file_name, url->file_name, strlen(url->file_name) + 1);
  }
  off_url_destroy(url);

  client_api_get_response_start_t response_start;
  response_start.content_type = "application/octet-stream";
  response_start.content_length = ori->final_byte;
  response_start.has_range = 0;
  response_start.range_start = 0;
  response_start.range_end = 0;
  _wt_h3_send_frame(conn, client_api_get_response_start_encode(&response_start));

  wt_h3_get_pipeline_t* pipeline = _wt_h3_get_pipeline_create(conn, ori);
  REFERENCE(pipeline, wt_h3_get_pipeline_t);

  size_t descriptor_pad = 32;
  readable_off_stream_t* rs = readable_off_stream_create(
      conn->pool, conn->bc, conn->tc, REFERENCE(ori, ori_t), descriptor_pad, NULL);
  readable_descriptor_t* desc = readable_descriptor_create(
      conn->pool, conn->bc, REFERENCE(ori, ori_t), descriptor_pad, NULL);

  pipeline->rs = rs;
  pipeline->desc = desc;

  stream_subscribe((stream_t*)rs, data_event, pipeline, _wt_h3_get_on_rs_data, NULL);
  stream_subscribe((stream_t*)rs, close_event, pipeline, _wt_h3_get_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, error_event, pipeline, _wt_h3_get_on_rs_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _wt_h3_get_on_desc_close, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _wt_h3_get_on_desc_error, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _wt_h3_get_on_tuple, NULL);

  readable_descriptor_push(desc);
}

static void _wt_h3_dispatch_message(webtransport_h3_conn_t* conn, uint8_t type, cbor_item_t* frame) {
  switch (type) {
    case CLIENT_API_AUTH_REQUEST:
      if (conn->transport == NULL || conn->transport->api_key_hash == NULL) {
        conn->is_authenticated = 1;
        conn->block_ctx.is_authenticated = 1;
      } else {
        client_api_auth_request_t auth;
        if (client_api_auth_request_decode(frame, &auth) != 0) {
          _wt_h3_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Invalid auth request");
          return;
        }
        char* key = get_memory(auth.api_key_len + 1);
        memcpy(key, auth.api_key, auth.api_key_len);
        key[auth.api_key_len] = '\0';
        if (bcrypt_check(key, conn->transport->api_key_hash) == 0) {
          conn->is_authenticated = 1;
          conn->block_ctx.is_authenticated = 1;
        } else {
          _wt_h3_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication failed");
        }
        free(key);
        client_api_auth_request_destroy(&auth);
      }
      break;
    case CLIENT_API_PUT_REQUEST:
      _wt_h3_handle_put(conn, frame);
      break;
    case CLIENT_API_GET_REQUEST:
      _wt_h3_handle_get(conn, frame);
      break;
    case CLIENT_API_BLOCK_PUT_REQUEST:
      block_handle_put_request(&conn->block_ctx, frame);
      break;
    case CLIENT_API_BLOCK_GET_REQUEST:
      block_handle_get_request(&conn->block_ctx, frame);
      break;
    case CLIENT_API_BLOCK_DELETE_REQUEST:
      block_handle_delete_request(&conn->block_ctx, frame);
      break;
    case CLIENT_API_HEALTH_REQUEST: {
      health_data_t data = health_data_collect(conn->transport->health_ctx);
      cJSON* json_obj = health_data_to_json(&data);
      char* json_str = cJSON_Print(json_obj);
      cJSON_Delete(json_obj);
      if (json_str == NULL) {
        _wt_h3_send_error(conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Health data serialization failed");
        break;
      }
      client_api_health_response_t resp;
      resp.json_data = json_str;
      _wt_h3_send_frame(conn, client_api_health_response_encode(&resp));
      free(json_str);
      break;
    }
    default:
      _wt_h3_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Unknown message type");
      break;
  }
}

static void* _wt_h3_server_thread(void* arg) {
  webtransport_h3_t* transport = (webtransport_h3_t*)arg;
  platform_thread_setup_stack();

  QUIC_REGISTRATION_CONFIG reg_config = {0};
  reg_config.AppName = "offs-wt-h3";
  reg_config.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;
  QUIC_STATUS status = transport->msquic->RegistrationOpen(&reg_config, &transport->registration);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "webtransport_h3: RegistrationOpen failed: 0x%x\n", status);
    atomic_store(&transport->running, 0);
    return NULL;
  }

  QUIC_SETTINGS settings = {0};
  settings.PeerUnidiStreamCount = 0;
  settings.PeerBidiStreamCount = 1;
  settings.IsSet.PeerUnidiStreamCount = TRUE;
  settings.IsSet.PeerBidiStreamCount = TRUE;
  settings.IdleTimeoutMs = 1800000;
  settings.IsSet.IdleTimeoutMs = TRUE;

  QUIC_BUFFER alpn = { sizeof("h3") - 1, (uint8_t*)"h3" };

  status = transport->msquic->ConfigurationOpen(
      transport->registration, &alpn, 1, &settings, sizeof(settings), NULL, &transport->configuration);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "webtransport_h3: ConfigurationOpen failed: 0x%x\n", status);
    transport->msquic->RegistrationClose(transport->registration);
    transport->registration = NULL;
    atomic_store(&transport->running, 0);
    return NULL;
  }

  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  if (transport->cert_path != NULL && transport->key_path != NULL) {
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &(QUIC_CERTIFICATE_FILE){
      .CertificateFile = transport->cert_path,
      .PrivateKeyFile = transport->key_path
    };
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    if (transport->peer_verify != NULL) {
      cred_config.Flags |= QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;
      cred_config.CaCertificateFile = peer_verify_ctx_path((peer_verify_ctx_t*)transport->peer_verify);
    } else if (transport->allow_secure) {
      log_error("webtransport_h3: allow_secure=true but no CA configured — refusing to start");
      transport->msquic->ConfigurationClose(transport->configuration);
      transport->configuration = NULL;
      transport->msquic->RegistrationClose(transport->registration);
      transport->registration = NULL;
      atomic_store(&transport->running, 0);
      return NULL;
    } else {
      log_info("webtransport_h3: running without CA-based client cert validation");
      cred_config.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }
  } else {
    cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
    if (transport->peer_verify != NULL) {
      cred_config.Flags = QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;
      cred_config.CaCertificateFile = peer_verify_ctx_path((peer_verify_ctx_t*)transport->peer_verify);
    } else if (transport->allow_secure) {
      log_error("webtransport_h3: allow_secure=true but no CA configured — refusing to start");
      transport->msquic->ConfigurationClose(transport->configuration);
      transport->configuration = NULL;
      transport->msquic->RegistrationClose(transport->registration);
      transport->registration = NULL;
      atomic_store(&transport->running, 0);
      return NULL;
    } else {
      log_info("webtransport_h3: running without CA-based client cert validation");
      cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }
  }

  status = transport->msquic->ConfigurationLoadCredential(transport->configuration, &cred_config);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "webtransport_h3: ConfigurationLoadCredential failed: 0x%x\n", status);
    transport->msquic->ConfigurationClose(transport->configuration);
    transport->configuration = NULL;
    transport->msquic->RegistrationClose(transport->registration);
    transport->registration = NULL;
    atomic_store(&transport->running, 0);
    return NULL;
  }

  status = transport->msquic->ListenerOpen(
      transport->registration, _wt_h3_listener_callback, transport, &transport->listener);
  if (QUIC_FAILED(status)) {
    fprintf(stderr, "webtransport_h3: ListenerOpen failed: 0x%x\n", status);
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
    fprintf(stderr, "webtransport_h3: ListenerStart failed: 0x%x\n", status);
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
    _wt_h3_destroy_stack_drain(transport);
    pd_loop_run_once(transport->loop, 100);
  }

  transport->msquic->ListenerStop(transport->listener);
  transport->msquic->ListenerClose(transport->listener);
  transport->listener = NULL;
  pd_loop_stop(transport->loop);

  return NULL;
}

void webtransport_h3_start(webtransport_h3_t* transport) {
  atomic_store(&transport->running, 1);
  transport->thread = platform_thread_create(_wt_h3_server_thread, transport);
}

void webtransport_h3_stop(webtransport_h3_t* transport) {
  if (transport == NULL) {
    return;
  }
  atomic_store(&transport->running, 0);
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);

  platform_mutex_lock(transport->conn_lock);
  for (int index = 0; index < transport->connections.length; index++) {
    webtransport_h3_conn_t* conn = transport->connections.data[index];
    if (conn != NULL && conn->connection != NULL) {
      transport->msquic->ConnectionShutdown(conn->connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
  }
  size_t remaining = atomic_load(&transport->active_connections);
  platform_mutex_unlock(transport->conn_lock);

  for (int wait_ms = 0; wait_ms < 5000 && remaining > 0; wait_ms += 10) {
    platform_sleep_ms(10);
    remaining = atomic_load(&transport->active_connections);
  }
  if (remaining > 0) {
    log_error("webtransport_h3: shutdown timed out with %zu connections active", remaining);
  }
}

#endif /* HAS_MSQUIC */
