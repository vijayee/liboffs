//
// Created by victor on 5/20/26.
//
#include "ws_connection.h"
#include "ws_transport.h"
#include "../client_api_wire.h"
#include "../../OFFStreams/off_url.h"
#include "../../OFFStreams/ori.h"
#include "../../OFFStreams/readable_off_stream.h"
#include "../../OFFStreams/readable_descriptor.h"
#include "../../OFFStreams/writeable_off_stream.h"
#include "../../OFFStreams/writeable_descriptor.h"
#include "../../OFFStreams/block_recipe.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/ofd.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../OFFStreams/tuple.h"
#include "../../BlockCache/block_cache.h"
#include "../../Buffer/buffer.h"
#include "../../Actor/actor.h"
#include "../../Actor/message.h"
#include "../../Scheduler/scheduler.h"
#include "../../RefCounter/refcounter.h"
#include "../../Streams/stream.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include <cbor.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include "../../Util/bcrypt.h"

#define READ_BUFFER_SIZE 65536
#define WS_UPGRADE_BUFFER_INITIAL 4096

/* RFC 6455 magic GUID for WebSocket accept key computation */
static const char ws_magic_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* Pipeline context for GET requests */
typedef struct {
  refcounter_t refcounter;
  readable_descriptor_t* desc;
  readable_off_stream_t* rs;
  tuple_cache_t* tc;
  ws_connection_t* connection;
  ori_t* ori;
} ws_get_pipeline_t;

/* Pipeline context for PUT requests */
typedef struct {
  refcounter_t refcounter;
  ws_connection_t* connection;
  writeable_off_stream_t* ws;
  writeable_descriptor_t* desc;
  new_blocks_recipe_t* recipe;
  char* content_type;
  char* file_name;
  size_t stream_length;
  char* server_address;
  buffer_t* file_hash;
  buffer_t* descriptor_hash;
  size_t file_hash_offset;
} ws_put_pipeline_t;

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data);
static void _ws_dispatch_frame(ws_connection_t* conn, uint8_t type, cbor_item_t* frame);

/* --- Base64 encode helper for WebSocket accept key --- */
static size_t _base64_encode(const uint8_t* input, size_t input_len, char* output) {
  static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0;
  size_t out = 0;
  while (i < input_len) {
    uint32_t octet_a = i < input_len ? input[i++] : 0;
    uint32_t octet_b = i < input_len ? input[i++] : 0;
    uint32_t octet_c = i < input_len ? input[i++] : 0;
    uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
    output[out++] = b64_table[(triple >> 18) & 0x3F];
    output[out++] = b64_table[(triple >> 12) & 0x3F];
    output[out++] = b64_table[(triple >> 6) & 0x3F];
    output[out++] = b64_table[triple & 0x3F];
  }
  /* Add padding */
  size_t mod = input_len % 3;
  if (mod == 1) {
    output[out - 1] = '=';
    output[out - 2] = '=';
  } else if (mod == 2) {
    output[out - 1] = '=';
  }
  output[out] = '\0';
  return out;
}

/* --- HTTP upgrade parsing helpers --- */

/* Find the end of HTTP headers (\r\n\r\n). Returns offset of end, or -1 if not found. */
static int _find_headers_end(const uint8_t* data, size_t len) {
  for (size_t i = 0; i + 3 < len; i++) {
    if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
      return (int)(i + 4);
    }
  }
  return -1;
}

/* Case-insensitive substring search in HTTP header block */
static int _header_contains(const uint8_t* data, size_t len, const char* header_name, const char* value) {
  /* Build search string like "Upgrade: websocket" or "Sec-WebSocket-Key: " */
  char search[256];
  snprintf(search, sizeof(search), "\r\n%s: ", header_name);
  size_t search_len = strlen(search);

  for (size_t i = 0; i + search_len <= len; i++) {
    if (strncasecmp((const char*)(data + i), search, search_len) == 0) {
      size_t val_start = i + search_len;
      size_t val_end = val_start;
      while (val_end < len && data[val_end] != '\r' && data[val_end] != '\n') {
        val_end++;
      }
      /* Check if the header value contains the expected substring */
      size_t val_len = val_end - val_start;
      size_t expected_len = strlen(value);
      if (val_len >= expected_len) {
        for (size_t j = 0; j + expected_len <= val_len; j++) {
          if (strncasecmp((const char*)(data + val_start + j), value, expected_len) == 0) {
            return 1;
          }
        }
      }
    }
  }
  return 0;
}

/* Extract the value of a header from the HTTP header block.
 * Returns a heap-allocated string that the caller must free, or NULL. */
static char* _header_get_value(const uint8_t* data, size_t len, const char* header_name) {
  char search[256];
  snprintf(search, sizeof(search), "\r\n%s: ", header_name);
  size_t search_len = strlen(search);

  for (size_t i = 0; i + search_len <= len; i++) {
    if (strncasecmp((const char*)(data + i), search, search_len) == 0) {
      size_t val_start = i + search_len;
      size_t val_end = val_start;
      while (val_end < len && data[val_end] != '\r' && data[val_end] != '\n') {
        val_end++;
      }
      size_t val_len = val_end - val_start;
      /* Trim leading/trailing whitespace */
      while (val_len > 0 && (data[val_start] == ' ' || data[val_start] == '\t')) {
        val_start++;
        val_len--;
      }
      while (val_len > 0 && (data[val_start + val_len - 1] == ' ' || data[val_start + val_len - 1] == '\t')) {
        val_len--;
      }
      char* result = get_memory(val_len + 1);
      memcpy(result, data + val_start, val_len);
      result[val_len] = '\0';
      return result;
    }
  }
  return NULL;
}

/* Compute Sec-WebSocket-Accept from Sec-WebSocket-Key per RFC 6455 section 4.2.2 */
static char* _compute_ws_accept_key(const char* client_key) {
  size_t key_len = strlen(client_key);
  size_t guid_len = strlen(ws_magic_guid);
  size_t combined_len = key_len + guid_len;
  uint8_t* combined = get_memory(combined_len);
  memcpy(combined, client_key, key_len);
  memcpy(combined + key_len, ws_magic_guid, guid_len);

  unsigned char sha1_hash[SHA_DIGEST_LENGTH];
  SHA1(combined, combined_len, sha1_hash);
  free(combined);

  char* accept_key = get_memory(64);
  _base64_encode(sha1_hash, SHA_DIGEST_LENGTH, accept_key);
  return accept_key;
}

/* --- Helper: send raw bytes to connection (handles SSL and buffering) --- */

static void _ws_connection_send_raw(ws_connection_t* conn, const uint8_t* data, size_t length) {
  if (conn == NULL || conn->sock == NULL) {
    return;
  }
  ws_connection_write(conn, data, length);
}

/* --- Helper: serialize a CBOR item and send as WebSocket binary frame --- */

static void _ws_connection_send_frame(ws_connection_t* conn, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);

  if (cbor_buf == NULL || cbor_len == 0) return;

  /* Wrap CBOR data in a WebSocket binary frame */
  size_t ws_frame_len;
  uint8_t* ws_frame = ws_frame_build(WS_OPCODE_BINARY, cbor_buf, cbor_len, &ws_frame_len);
  free(cbor_buf);

  if (ws_frame != NULL) {
    _ws_connection_send_raw(conn, ws_frame, ws_frame_len);
    free(ws_frame);
  }
}

/* Helper: send an error frame */
static void _ws_connection_send_error(ws_connection_t* conn, uint8_t status_code, const char* message) {
  client_api_error_t error_msg;
  error_msg.status_code = status_code;
  error_msg.message = (char*)message;
  cbor_item_t* frame = client_api_error_encode(&error_msg);
  _ws_connection_send_frame(conn, frame);
}

/* --- Watcher helpers (follow tcp_connection pattern) --- */

static void _connection_update_watcher(ws_connection_t* connection, pd_event_t events) {
  if (connection->transport == NULL) return;
  pd_watcher_t* watcher = ATOMIC_LOAD(&connection->watcher);
  if (watcher == NULL) return;
  ws_watcher_update_payload_t* payload = get_clear_memory(sizeof(ws_watcher_update_payload_t));
  payload->watcher = watcher;
  payload->events = events;
  message_t msg;
  msg.type = WS_SERVER_UPDATE_WATCHER;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&connection->actor, &msg);
}

static void _connection_stop_watcher(ws_connection_t* connection) {
  pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
  if (watcher == NULL) return;
  if (connection->transport != NULL) {
    ws_watcher_update_payload_t* payload = get_clear_memory(sizeof(ws_watcher_update_payload_t));
    payload->watcher = watcher;
    payload->events = 0;
    message_t msg;
    msg.type = WS_SERVER_STOP_WATCHER;
    msg.payload = payload;
    msg.payload_destroy = free;
    actor_send(&connection->transport->actor, &msg);
  } else {
    pd_watcher_stop(watcher);
    pd_watcher_destroy(watcher);
  }
}

static void _connection_close_fd(ws_connection_t* connection) {
  if (connection->ssl != NULL) {
    SSL_shutdown(connection->ssl);
    SSL_free(connection->ssl);
    connection->ssl = NULL;
  }
  if (connection->sock != NULL) {
    platform_socket_destroy(connection->sock);
    connection->sock = NULL;
  }
  connection->is_closing = 1;
}

/* --- HTTP Upgrade handshake handler --- */

static void _ws_handle_upgrade(ws_connection_t* conn, const uint8_t* data, size_t len) {
  /* Validate the HTTP upgrade request */
  /* Check request line starts with GET */
  if (len < 3 || strncasecmp((const char*)data, "GET", 3) != 0) {
    const char* bad_response = "HTTP/1.1 400 Bad Request\r\n\r\n";
    _ws_connection_send_raw(conn, (const uint8_t*)bad_response, strlen(bad_response));
    _connection_stop_watcher(conn);
    _connection_close_fd(conn);
    return;
  }

  /* Check for required headers: Upgrade: websocket, Connection: Upgrade */
  if (!_header_contains(data, len, "Upgrade", "websocket") ||
      !_header_contains(data, len, "Connection", "Upgrade")) {
    const char* bad_response = "HTTP/1.1 400 Bad Request\r\n\r\n";
    _ws_connection_send_raw(conn, (const uint8_t*)bad_response, strlen(bad_response));
    _connection_stop_watcher(conn);
    _connection_close_fd(conn);
    return;
  }

  /* Extract Sec-WebSocket-Key header */
  char* client_key = _header_get_value(data, len, "Sec-WebSocket-Key");
  if (client_key == NULL) {
    const char* bad_response = "HTTP/1.1 400 Bad Request\r\n\r\n";
    _ws_connection_send_raw(conn, (const uint8_t*)bad_response, strlen(bad_response));
    _connection_stop_watcher(conn);
    _connection_close_fd(conn);
    return;
  }

  /* Compute accept key */
  char* accept_key = _compute_ws_accept_key(client_key);
  free(client_key);

  /* Build and send the 101 response */
  char response[512];
  int response_len = snprintf(response, sizeof(response),
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n"
    "\r\n",
    accept_key);
  free(accept_key);

  _ws_connection_send_raw(conn, (const uint8_t*)response, (size_t)response_len);

  /* Transition to connected state */
  conn->state = WS_STATE_CONNECTED;
  if (conn->upgrade_buf != NULL) {
    DESTROY(conn->upgrade_buf, buffer);
    conn->upgrade_buf = NULL;
  }
}

/* --- WebSocket frame handler (connected state) --- */

static void _ws_handle_frame(ws_connection_t* conn, ws_frame_t* frame) {
  switch (frame->opcode) {
    case WS_OPCODE_BINARY: {
      /* Payload is a CBOR frame — parse and dispatch */
      if (frame->payload == NULL || frame->payload_len == 0) {
        _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Empty binary frame");
        break;
      }
      struct cbor_load_result load_result;
      cbor_item_t* cbor_item = cbor_load(frame->payload, frame->payload_len, &load_result);
      if (cbor_item == NULL || load_result.error.code != CBOR_ERR_NONE) {
        if (cbor_item != NULL) {
          cbor_decref(&cbor_item);
        }
        _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid CBOR frame");
        break;
      }
      uint8_t type = client_api_wire_get_type(cbor_item);
      _ws_dispatch_frame(conn, type, cbor_item);
      cbor_decref(&cbor_item);
      break;
    }

    case WS_OPCODE_TEXT: {
      /* We only accept binary frames — send close with unsupported data */
      const char* unsupported_msg = "Binary frames only";
      size_t close_payload_len = 2 + strlen(unsupported_msg);
      uint8_t* close_payload = get_memory(close_payload_len);
      close_payload[0] = 0x03; /* 1003 = unsupported data */
      close_payload[1] = 0xEB;
      memcpy(close_payload + 2, unsupported_msg, strlen(unsupported_msg));
      size_t ws_len;
      uint8_t* ws_frame = ws_frame_build(WS_OPCODE_CLOSE, close_payload, close_payload_len, &ws_len);
      free(close_payload);
      if (ws_frame != NULL) {
        _ws_connection_send_raw(conn, ws_frame, ws_len);
        free(ws_frame);
      }
      conn->is_closing = 1;
      break;
    }

    case WS_OPCODE_PING: {
      /* Respond with PONG — echo the payload back */
      size_t ws_len;
      uint8_t* ws_frame = ws_frame_build(WS_OPCODE_PONG, frame->payload, frame->payload_len, &ws_len);
      if (ws_frame != NULL) {
        _ws_connection_send_raw(conn, ws_frame, ws_len);
        free(ws_frame);
      }
      break;
    }

    case WS_OPCODE_CLOSE: {
      /* Send CLOSE frame back, then close connection */
      size_t ws_len;
      uint8_t* ws_frame = ws_frame_build(WS_OPCODE_CLOSE, frame->payload, frame->payload_len, &ws_len);
      if (ws_frame != NULL) {
        _ws_connection_send_raw(conn, ws_frame, ws_len);
        free(ws_frame);
      }
      conn->is_closing = 1;
      _connection_stop_watcher(conn);
      _connection_close_fd(conn);
      break;
    }

    default:
      /* Unknown opcode — send close with protocol error */
      {
        uint8_t close_reason[2];
        close_reason[0] = 0x03; /* 1002 = protocol error */
        close_reason[1] = 0xEA;
        size_t ws_len;
        uint8_t* ws_frame = ws_frame_build(WS_OPCODE_CLOSE, close_reason, 2, &ws_len);
        if (ws_frame != NULL) {
          _ws_connection_send_raw(conn, ws_frame, ws_len);
          free(ws_frame);
        }
        conn->is_closing = 1;
      }
      break;
  }
}

/* --- GET pipeline callbacks --- */

static void _ws_get_on_tuple(void* ctx, void* data) {
  ws_get_pipeline_t* pipeline = (ws_get_pipeline_t*)ctx;
  tuple_t* tuple = (tuple_t*)data;
  readable_off_stream_write(pipeline->rs, tuple);
}

static void _ws_get_on_rs_data(void* ctx, void* data) {
  ws_get_pipeline_t* pipeline = (ws_get_pipeline_t*)ctx;
  buffer_t* buf = (buffer_t*)data;
  client_api_get_data_t get_data_msg;
  get_data_msg.data = buf->data;
  get_data_msg.data_size = buf->size;
  cbor_item_t* frame = client_api_get_data_encode(&get_data_msg);
  _ws_connection_send_frame(pipeline->connection, frame);
}

static void _ws_get_on_rs_close(void* ctx, void* unused) {
  (void)unused;
  ws_get_pipeline_t* pipeline = (ws_get_pipeline_t*)ctx;
  cbor_item_t* frame = client_api_get_end_encode();
  _ws_connection_send_frame(pipeline->connection, frame);
  stream_deferred_deref((stream_t*)pipeline->rs);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _ws_get_on_rs_error(void* ctx, void* error) {
  (void)error;
  ws_get_pipeline_t* pipeline = (ws_get_pipeline_t*)ctx;
  _ws_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "Stream error");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
}

static void _ws_get_on_desc_close(void* ctx, void* unused) {
  (void)unused;
  ws_get_pipeline_t* pipeline = (ws_get_pipeline_t*)ctx;
  stream_deferred_deref((stream_t*)pipeline->desc);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _ws_get_on_desc_error(void* ctx, void* error) {
  (void)error;
  ws_get_pipeline_t* pipeline = (ws_get_pipeline_t*)ctx;
  _ws_connection_send_error(pipeline->connection, CLIENT_API_STATUS_NOT_FOUND, "Not found");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
  stream_deactivate((stream_t*)pipeline->desc, NULL);
}

static ws_get_pipeline_t* _ws_get_pipeline_create(ws_connection_t* conn, ori_t* ori) {
  ws_get_pipeline_t* pipeline = get_clear_memory(sizeof(ws_get_pipeline_t));
  refcounter_init((refcounter_t*)pipeline);
  pipeline->connection = conn;
  pipeline->ori = ori;
  pipeline->tc = conn->tc;
  return pipeline;
}

/* --- PUT pipeline callbacks --- */

static void _ws_put_on_descriptor_close(void* ctx, void* unused) {
  (void)unused;
  ws_put_pipeline_t* pipeline = (ws_put_pipeline_t*)ctx;

  /* Build ORI string from the completed PUT */
  off_url_t* url = off_url_create();
  free(url->content_type);
  url->content_type = strdup(pipeline->content_type);
  url->file_name = strdup(pipeline->file_name);
  url->stream_length = pipeline->stream_length;
  if (pipeline->server_address != NULL) {
    free(url->server_address);
    url->server_address = strdup(pipeline->server_address);
  }
  url->file_hash = buffer_copy(pipeline->file_hash);
  url->descriptor_hash = buffer_copy(pipeline->descriptor_hash);

  char* ori_string = off_url_to_string(url);

  client_api_put_response_t response;
  response.ori_string = ori_string;
  cbor_item_t* frame = client_api_put_response_encode(&response);
  _ws_connection_send_frame(pipeline->connection, frame);
  free(ori_string);
  off_url_destroy(url);

  /* Deferred deref the streams, then free pipeline if refcount reaches zero */
  refcounter_dereference((refcounter_t*)pipeline->recipe);
  scheduler_pool_defer_cleanup(((stream_t*)pipeline->ws)->pool, pipeline->recipe,
                               (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)pipeline->desc);
  stream_deferred_deref((stream_t*)pipeline->ws);

  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    if (pipeline->content_type != NULL) {
      free(pipeline->content_type);
    }
    if (pipeline->file_name != NULL) {
      free(pipeline->file_name);
    }
    if (pipeline->server_address != NULL) {
      free(pipeline->server_address);
    }
    if (pipeline->file_hash != NULL) {
      DESTROY(pipeline->file_hash, buffer);
    }
    if (pipeline->descriptor_hash != NULL) {
      DESTROY(pipeline->descriptor_hash, buffer);
    }
    free(pipeline);
  }
}

static void _ws_put_on_descriptor_data(void* ctx, void* data) {
  ws_put_pipeline_t* pipeline = (ws_put_pipeline_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  /* The descriptor's data_event carries the descriptor hash */
  if (pipeline->descriptor_hash != NULL) {
    DESTROY(pipeline->descriptor_hash, buffer);
  }
  pipeline->descriptor_hash = REFERENCE(payload, buffer_t);
}

static void _ws_put_on_stream_close(void* ctx, void* unused) {
  (void)unused;
  ws_put_pipeline_t* pipeline = (ws_put_pipeline_t*)ctx;
  writeable_descriptor_close(pipeline->desc);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    refcounter_dereference((refcounter_t*)pipeline->recipe);
    scheduler_pool_defer_cleanup(((stream_t*)pipeline->ws)->pool, pipeline->recipe,
                                 (void (*)(void*))new_blocks_recipe_destroy);
    stream_deferred_deref((stream_t*)pipeline->desc);
    stream_deferred_deref((stream_t*)pipeline->ws);
    if (pipeline->content_type != NULL) free(pipeline->content_type);
    if (pipeline->file_name != NULL) free(pipeline->file_name);
    if (pipeline->server_address != NULL) free(pipeline->server_address);
    if (pipeline->file_hash != NULL) DESTROY(pipeline->file_hash, buffer);
    if (pipeline->descriptor_hash != NULL) DESTROY(pipeline->descriptor_hash, buffer);
    free(pipeline);
  }
}

static void _ws_put_on_stream_data(void* ctx, void* data) {
  ws_put_pipeline_t* pipeline = (ws_put_pipeline_t*)ctx;
  buffer_t* buf = (buffer_t*)data;

  /* First buffer of exactly 32 bytes from writeable_off_stream is the file hash */
  if (buf->size == 32 && pipeline->file_hash == NULL) {
    pipeline->file_hash = REFERENCE(buf, buffer_t);
  } else {
    /* Otherwise it's a tuple — feed it to the descriptor */
    tuple_t* tuple = REFERENCE(buf, tuple_t);
    writeable_descriptor_write(pipeline->desc, tuple);
    DESTROY(tuple, tuple);
  }
}

static void _ws_put_on_stream_error(void* ctx, void* error) {
  (void)error;
  ws_put_pipeline_t* pipeline = (ws_put_pipeline_t*)ctx;
  _ws_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT stream error");
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}

static void _ws_put_on_descriptor_error(void* ctx, void* error) {
  (void)error;
  ws_put_pipeline_t* pipeline = (ws_put_pipeline_t*)ctx;
  _ws_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT descriptor error");
  stream_deactivate((stream_t*)pipeline->desc, NULL);
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}

/* --- Frame handlers --- */

static void _ws_handle_get(ws_connection_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  client_api_get_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_get_request_decode(frame, &msg) != 0) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid GET request");
    return;
  }

  off_url_t* url = off_url_parse(msg.ori_string);
  client_api_get_request_destroy(&msg);

  if (url == NULL) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid OFF URL");
    return;
  }

  /* Check if this is a directory that needs OFD resolution */
  if (url->content_type != NULL && strstr(url->content_type, "offsystem/directory") != NULL) {
    /* Async directory resolution */
    conn->resolve_url = url;
    conn->get_phase = WS_GET_RESOLVE_DIR;
    buffer_t* root_hash = REFERENCE(url->file_hash, buffer_t);
    char* path = url->file_name != NULL ? get_memory(strlen(url->file_name) + 1) : NULL;
    if (path != NULL) {
      memcpy(path, url->file_name, strlen(url->file_name) + 1);
    }
    conn->resolve_path = path;
    ofd_cache_resolve(conn->ofd_cache, root_hash, path, &conn->actor);
    DESTROY(root_hash, buffer);
    return;
  }

  /* Synchronous GET path */
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

  /* Send GET_RESPONSE_START */
  client_api_get_response_start_t response_start;
  response_start.content_type = "application/octet-stream";
  response_start.content_length = ori->final_byte;
  response_start.has_range = 0;
  response_start.range_start = 0;
  response_start.range_end = 0;
  cbor_item_t* start_frame = client_api_get_response_start_encode(&response_start);
  _ws_connection_send_frame(conn, start_frame);

  /* Create pipeline */
  ws_get_pipeline_t* pipeline = _ws_get_pipeline_create(conn, ori);
  REFERENCE(pipeline, ws_get_pipeline_t);

  size_t descriptor_pad = 32;

  readable_off_stream_t* rs = readable_off_stream_create(
    conn->pool, conn->bc, conn->tc, REFERENCE(ori, ori_t), descriptor_pad, NULL);
  readable_descriptor_t* desc = readable_descriptor_create(
    conn->pool, conn->bc, REFERENCE(ori, ori_t), descriptor_pad, NULL);

  pipeline->rs = rs;
  pipeline->desc = desc;

  /* Subscribe to stream events (5-arg signature) */
  stream_subscribe((stream_t*)rs, data_event, pipeline, _ws_get_on_rs_data, NULL);
  stream_subscribe((stream_t*)rs, close_event, pipeline, _ws_get_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, error_event, pipeline, _ws_get_on_rs_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _ws_get_on_desc_close, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _ws_get_on_desc_error, NULL);

  /* Pipe: descriptor provides tuples to the off_stream */
  stream_subscribe((stream_t*)desc, data_event, pipeline, _ws_get_on_tuple, NULL);

  /* Start the descriptor pull */
  readable_descriptor_push(desc);
}

static void _ws_handle_put(ws_connection_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_put_request_decode(frame, &msg) != 0) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid PUT request");
    return;
  }

  if (msg.content_type == NULL || msg.file_name == NULL) {
    client_api_put_request_destroy(&msg);
    _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Missing content_type or file_name");
    return;
  }

  /* Create pipeline context */
  ws_put_pipeline_t* pipeline = get_clear_memory(sizeof(ws_put_pipeline_t));
  refcounter_init((refcounter_t*)pipeline);
  pipeline->connection = conn;
  pipeline->content_type = msg.content_type;
  pipeline->file_name = msg.file_name;
  pipeline->stream_length = msg.stream_length;
  pipeline->server_address = msg.server_address;
  pipeline->file_hash = NULL;
  pipeline->descriptor_hash = NULL;
  pipeline->file_hash_offset = 0;

  /* Create writeable streams */
  block_size_e block_type = standard;
  size_t tuple_size = 3;
  size_t descriptor_pad = 32;

  new_blocks_recipe_t* recipe = new_blocks_recipe_create(conn->pool, conn->bc, block_type);
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  vec_push(&recipes, (block_recipe_t*)recipe);

  writeable_off_stream_t* ws = writeable_off_stream_create(
    conn->pool, conn->bc, conn->tc, block_type, tuple_size, descriptor_pad, recipes, NULL);
  writeable_descriptor_t* desc = writeable_descriptor_create(
    conn->pool, conn->bc, block_type, descriptor_pad, tuple_size, msg.stream_length, NULL);

  pipeline->ws = ws;
  pipeline->desc = desc;
  pipeline->recipe = recipe;
  /* Reference count: 1 base + 1 for error callbacks that may both fire */
  refcounter_reference((refcounter_t*)pipeline);

  /* Subscribe to stream events (5-arg signature) */
  stream_subscribe((stream_t*)ws, close_event, pipeline, _ws_put_on_stream_close, NULL);
  stream_subscribe((stream_t*)ws, data_event, pipeline, _ws_put_on_stream_data, NULL);
  stream_subscribe((stream_t*)ws, error_event, pipeline, _ws_put_on_stream_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _ws_put_on_descriptor_close, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _ws_put_on_descriptor_data, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _ws_put_on_descriptor_error, NULL);

  if (msg.data != NULL && msg.data_size > 0) {
    /* Buffered PUT — write data and finalize immediately */
    buffer_t* data = buffer_create_from_pointer_copy(msg.data, msg.data_size);
    free(msg.data);
    msg.data = NULL;
    msg.data_size = 0;
    writeable_off_stream_write(ws, data);
    DESTROY(data, buffer);
    writeable_off_stream_finalize(ws);
  } else {
    /* Streaming PUT — store copies of strings in connection for subsequent PUT_DATA frames.
     * The pipeline owns the original pointers; the connection needs its own copies
     * to avoid double-free when the pipeline frees its strings. */
    conn->put_ws = ws;
    conn->put_desc = desc;
    conn->put_streaming = 1;
    conn->put_content_type = strdup(pipeline->content_type);
    conn->put_file_name = strdup(pipeline->file_name);
    conn->put_stream_length = msg.stream_length;
    conn->put_server_address = msg.server_address != NULL ? strdup(msg.server_address) : NULL;
  }

  /* msg.content_type / file_name / server_address are now owned by pipeline,
     don't let client_api_put_request_destroy free them */
  msg.content_type = NULL;
  msg.file_name = NULL;
  msg.server_address = NULL;
  client_api_put_request_destroy(&msg);
}

static void _ws_handle_put_data(ws_connection_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  if (!conn->put_streaming || conn->put_ws == NULL) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "No streaming PUT in progress");
    return;
  }

  client_api_put_data_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_put_data_decode(frame, &msg) != 0) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid PUT_DATA frame");
    free(msg.data);
    return;
  }

  if (msg.data != NULL && msg.data_size > 0) {
    buffer_t* data = buffer_create_from_pointer_copy(msg.data, msg.data_size);
    writeable_off_stream_write(conn->put_ws, data);
    DESTROY(data, buffer);
  }

  free(msg.data);
}

static void _ws_handle_put_end(ws_connection_t* conn) {
  if (!conn->is_authenticated) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  if (!conn->put_streaming || conn->put_ws == NULL) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "No streaming PUT in progress");
    return;
  }

  writeable_off_stream_finalize(conn->put_ws);
  conn->put_streaming = 0;
  conn->put_ws = NULL;
  conn->put_desc = NULL;
}

static void _ws_handle_auth(ws_connection_t* conn, cbor_item_t* frame) {
  if (conn->transport == NULL || conn->transport->api_key_hash == NULL) {
    conn->is_authenticated = 1;
    conn->block_ctx.is_authenticated = 1;
    return;
  }

  client_api_auth_request_t auth;
  if (client_api_auth_request_decode(frame, &auth) != 0) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Invalid auth request");
    return;
  }

  char* key = get_memory(auth.api_key_len + 1);
  memcpy(key, auth.api_key, auth.api_key_len);
  key[auth.api_key_len] = '\0';

  if (bcrypt_check(key, conn->transport->api_key_hash) == 0) {
    conn->is_authenticated = 1;
    conn->block_ctx.is_authenticated = 1;
  } else {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication failed");
  }

  free(key);
  client_api_auth_request_destroy(&auth);
}

static void _ws_dispatch_frame(ws_connection_t* conn, uint8_t type, cbor_item_t* frame) {
  switch (type) {
    case CLIENT_API_GET_REQUEST:
      _ws_handle_get(conn, frame);
      break;
    case CLIENT_API_PUT_REQUEST:
      _ws_handle_put(conn, frame);
      break;
    case CLIENT_API_PUT_DATA:
      _ws_handle_put_data(conn, frame);
      break;
    case CLIENT_API_PUT_END:
      _ws_handle_put_end(conn);
      break;
    case CLIENT_API_AUTH_REQUEST:
      _ws_handle_auth(conn, frame);
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
    default:
      _ws_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Unknown message type");
      break;
  }
}

/* --- Async GET resolution (OFD cache) --- */

static void _ws_handle_ofd_resolve_result(ws_connection_t* conn, message_t* msg) {
  ofd_resolve_result_t* result = (ofd_resolve_result_t*)msg->payload;
  if (result == NULL) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_NOT_FOUND, "Directory resolution failed");
    if (conn->resolve_url != NULL) {
      off_url_destroy(conn->resolve_url);
      conn->resolve_url = NULL;
    }
    if (conn->resolve_path != NULL) {
      free(conn->resolve_path);
      conn->resolve_path = NULL;
    }
    return;
  }

  if (result->ori == NULL) {
    _ws_connection_send_error(conn, CLIENT_API_STATUS_NOT_FOUND, "Directory entry not found");
    if (conn->resolve_url != NULL) {
      off_url_destroy(conn->resolve_url);
      conn->resolve_url = NULL;
    }
    if (conn->resolve_path != NULL) {
      free(conn->resolve_path);
      conn->resolve_path = NULL;
    }
    return;
  }

  /* Continue the GET with the resolved ORI */
  ori_t* resolved_ori = REFERENCE(result->ori, ori_t);

  /* Send GET_RESPONSE_START */
  client_api_get_response_start_t response_start;
  response_start.content_type = "application/octet-stream";
  response_start.content_length = resolved_ori->final_byte;
  response_start.has_range = 0;
  response_start.range_start = 0;
  response_start.range_end = 0;
  cbor_item_t* start_frame = client_api_get_response_start_encode(&response_start);
  _ws_connection_send_frame(conn, start_frame);

  /* Create pipeline */
  ws_get_pipeline_t* pipeline = _ws_get_pipeline_create(conn, resolved_ori);
  REFERENCE(pipeline, ws_get_pipeline_t);

  size_t descriptor_pad = 32;

  readable_off_stream_t* rs = readable_off_stream_create(
    conn->pool, conn->bc, conn->tc, REFERENCE(resolved_ori, ori_t), descriptor_pad, NULL);
  readable_descriptor_t* desc = readable_descriptor_create(
    conn->pool, conn->bc, REFERENCE(resolved_ori, ori_t), descriptor_pad, NULL);

  pipeline->rs = rs;
  pipeline->desc = desc;

  stream_subscribe((stream_t*)rs, data_event, pipeline, _ws_get_on_rs_data, NULL);
  stream_subscribe((stream_t*)rs, close_event, pipeline, _ws_get_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, error_event, pipeline, _ws_get_on_rs_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _ws_get_on_desc_close, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _ws_get_on_desc_error, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _ws_get_on_tuple, NULL);

  readable_descriptor_push(desc);

  DEREFERENCE(pipeline);

  /* Cleanup resolution state */
  if (conn->resolve_url != NULL) {
    off_url_destroy(conn->resolve_url);
    conn->resolve_url = NULL;
  }
  if (conn->resolve_path != NULL) {
    free(conn->resolve_path);
    conn->resolve_path = NULL;
  }
  DESTROY(resolved_ori, ori);
}

/* --- Connection dispatch (runs on scheduler worker threads) --- */

void ws_connection_dispatch(void* state, message_t* msg) {
  ws_connection_t* connection = (ws_connection_t*)state;

  if (connection->is_closing) {
    return;
  }

  switch (msg->type) {
    case CACHE_PUT_RESULT:
    case CACHE_GET_RESULT:
    case CACHE_REMOVE_RESULT:
      if (block_handle_cache_result(&connection->block_ctx, msg)) break;
      break;
    case WS_CONNECTION_DATA: {
      buffer_t* data = (buffer_t*)msg->payload;
      msg->payload = NULL;
      if (connection->sock == NULL) {
        DESTROY(data, buffer);
        break;
      }

      if (connection->state == WS_STATE_UPGRADING) {
        /* Accumulate data until we see \r\n\r\n (end of HTTP headers) */
        if (connection->upgrade_buf == NULL) {
          connection->upgrade_buf = buffer_create_with_capacity(0, data->size + WS_UPGRADE_BUFFER_INITIAL);
          memcpy(connection->upgrade_buf->data, data->data, data->size);
          connection->upgrade_buf->size = data->size;
        } else {
          buffer_ensure_capacity(connection->upgrade_buf, connection->upgrade_buf->size + data->size);
          memcpy(connection->upgrade_buf->data + connection->upgrade_buf->size, data->data, data->size);
          connection->upgrade_buf->size += data->size;
        }
        DESTROY(data, buffer);

        int headers_end = _find_headers_end(connection->upgrade_buf->data, connection->upgrade_buf->size);
        if (headers_end > 0) {
          /* Save remaining data (after headers) before _ws_handle_upgrade frees upgrade_buf */
          size_t remaining = connection->upgrade_buf->size - (size_t)headers_end;
          buffer_t* post_headers_data = NULL;
          if (remaining > 0) {
            post_headers_data = buffer_create_from_pointer_copy(
              connection->upgrade_buf->data + headers_end, remaining);
          }

          /* We have a complete HTTP upgrade request — process it */
          _ws_handle_upgrade(connection, connection->upgrade_buf->data, (size_t)headers_end);

          /* _ws_handle_upgrade destroys upgrade_buf, so don't access it anymore */
          connection->upgrade_buf = NULL;

          /* Any data after the headers is WebSocket frame data */
          if (post_headers_data != NULL && connection->state == WS_STATE_CONNECTED) {
            connection->recv_buf = post_headers_data;
            post_headers_data = NULL;
          } else if (post_headers_data != NULL) {
            DESTROY(post_headers_data, buffer);
          }

          /* Process any buffered recv data as WebSocket frames */
          if (connection->recv_buf != NULL && connection->recv_buf->size > 0) {
            size_t ws_offset = 0;
            while (ws_offset < connection->recv_buf->size) {
              ws_frame_t frame;
              size_t needed = 0;
              ssize_t consumed = ws_frame_parse(
                connection->recv_buf->data + ws_offset,
                connection->recv_buf->size - ws_offset,
                &frame, &needed);
              if (consumed == 0) {
                /* Incomplete frame — stop parsing */
                break;
              } else if (consumed < 0) {
                /* Protocol error */
                DESTROY(connection->recv_buf, buffer);
                connection->recv_buf = NULL;
                _connection_stop_watcher(connection);
                _connection_close_fd(connection);
                break;
              } else {
                _ws_handle_frame(connection, &frame);
                ws_frame_destroy(&frame);
                ws_offset += (size_t)consumed;
              }
            }
            /* Compact recv_buf */
            if (connection->recv_buf != NULL) {
              if (ws_offset == 0) {
                /* Nothing consumed — keep buffer as is */
              } else if (ws_offset >= connection->recv_buf->size) {
                DESTROY(connection->recv_buf, buffer);
                connection->recv_buf = NULL;
              } else {
                size_t leftover = connection->recv_buf->size - ws_offset;
                memmove(connection->recv_buf->data, connection->recv_buf->data + ws_offset, leftover);
                connection->recv_buf->size = leftover;
              }
            }
          }
        }
        /* If headers not complete yet, we'll accumulate more data on next read */
        break;
      }

      /* WS_STATE_CONNECTED: buffer data and parse WebSocket frames */
      if (connection->recv_buf == NULL) {
        connection->recv_buf = buffer_create_with_capacity(0, data->size);
        memcpy(connection->recv_buf->data, data->data, data->size);
        connection->recv_buf->size = data->size;
      } else {
        buffer_ensure_capacity(connection->recv_buf, connection->recv_buf->size + data->size);
        memcpy(connection->recv_buf->data + connection->recv_buf->size, data->data, data->size);
        connection->recv_buf->size += data->size;
      }
      DESTROY(data, buffer);

      /* Parse as many complete WebSocket frames as possible */
      size_t offset = 0;
      while (offset < connection->recv_buf->size) {
        ws_frame_t frame;
        size_t needed = 0;
        ssize_t consumed = ws_frame_parse(
          connection->recv_buf->data + offset,
          connection->recv_buf->size - offset,
          &frame, &needed);
        if (consumed == 0) {
          /* Incomplete frame — stop parsing, keep remaining bytes */
          break;
        } else if (consumed < 0) {
          /* Protocol error — close the connection */
          DESTROY(connection->recv_buf, buffer);
          connection->recv_buf = NULL;
          _connection_stop_watcher(connection);
          _connection_close_fd(connection);
          break;
        } else {
          _ws_handle_frame(connection, &frame);
          ws_frame_destroy(&frame);
          offset += (size_t)consumed;
        }
      }
      /* Compact recv_buf: shift any remaining bytes to the front */
      if (connection->recv_buf != NULL) {
        if (offset == 0) {
          /* Nothing consumed — keep buffer as is (incomplete frame) */
        } else if (offset >= connection->recv_buf->size) {
          /* All data consumed — free the buffer */
          DESTROY(connection->recv_buf, buffer);
          connection->recv_buf = NULL;
        } else {
          /* Partial consumption — shift remaining data to front */
          size_t leftover = connection->recv_buf->size - offset;
          memmove(connection->recv_buf->data, connection->recv_buf->data + offset, leftover);
          connection->recv_buf->size = leftover;
        }
      }
      break;
    }

    case WS_CONNECTION_HANGUP:
    case WS_CONNECTION_ERROR: {
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    case WS_CONNECTION_WRITE: {
      buffer_t* buf = (buffer_t*)msg->payload;
      msg->payload = NULL;
      if (connection->sock == NULL) {
        DESTROY(buf, buffer);
        break;
      }
      if (connection->write_buffer != NULL && connection->write_buffer->size > 0) {
        buffer_ensure_capacity(connection->write_buffer, connection->write_buffer->size + buf->size);
        memcpy(connection->write_buffer->data + connection->write_buffer->size,
               buf->data, buf->size);
        connection->write_buffer->size += buf->size;
        DESTROY(buf, buffer);
        break;
      }
      ssize_t sent;
      if (connection->is_ssl && connection->ssl != NULL) {
        sent = SSL_write(connection->ssl, buf->data, (int)buf->size);
      } else {
        sent = platform_socket_send(connection->sock, buf->data, buf->size);
      }
      if (sent < 0) {
        if (connection->is_ssl && connection->ssl != NULL) {
          int ssl_err = SSL_get_error(connection->ssl, (int)sent);
          if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
            connection->write_buffer = buf;
            connection->write_pending = 1;
            _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
          } else {
            DESTROY(buf, buffer);
            _connection_stop_watcher(connection);
            _connection_close_fd(connection);
          }
        } else {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            connection->write_buffer = buf;
            connection->write_pending = 1;
            _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
          } else {
            DESTROY(buf, buffer);
            _connection_stop_watcher(connection);
            _connection_close_fd(connection);
          }
        }
      } else if (sent == 0) {
        DESTROY(buf, buffer);
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      } else if ((size_t)sent < buf->size) {
        size_t remaining = buf->size - (size_t)sent;
        connection->write_buffer = buffer_create(remaining);
        memcpy(connection->write_buffer->data, buf->data + sent, remaining);
        connection->write_buffer->size = remaining;
        connection->write_pending = 1;
        _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        DESTROY(buf, buffer);
      } else {
        DESTROY(buf, buffer);
      }
      break;
    }

    case WS_CONNECTION_WRITABLE: {
      if (connection->sock == NULL) {
        break;
      }
      if (connection->write_buffer == NULL || connection->write_buffer->size == 0) {
        connection->write_pending = 0;
        _connection_update_watcher(connection, PD_EVENT_READ);
        break;
      }
      ssize_t sent;
      if (connection->is_ssl && connection->ssl != NULL) {
        sent = SSL_write(connection->ssl, connection->write_buffer->data,
                         (int)connection->write_buffer->size);
      } else {
        sent = platform_socket_send(connection->sock, connection->write_buffer->data,
                     connection->write_buffer->size);
      }
      if (sent > 0) {
        if ((size_t)sent >= connection->write_buffer->size) {
          DESTROY(connection->write_buffer, buffer);
          connection->write_buffer = NULL;
          connection->write_pending = 0;
          if (connection->is_closing) {
            if (connection->ssl != NULL) {
              SSL_shutdown(connection->ssl);
              SSL_free(connection->ssl);
              connection->ssl = NULL;
            }
            if (connection->sock != NULL) {
              platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
            }
            _connection_stop_watcher(connection);
            _connection_close_fd(connection);
            break;
          }
          _connection_update_watcher(connection, PD_EVENT_READ);
        } else {
          size_t remaining = connection->write_buffer->size - (size_t)sent;
          memmove(connection->write_buffer->data,
                  connection->write_buffer->data + sent, remaining);
          connection->write_buffer->size = remaining;
        }
      } else if (sent == 0) {
        DESTROY(connection->write_buffer, buffer);
        connection->write_buffer = NULL;
        connection->write_pending = 0;
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      } else {
        if (connection->is_ssl && connection->ssl != NULL) {
          int ssl_err = SSL_get_error(connection->ssl, (int)sent);
          if (ssl_err != SSL_ERROR_WANT_WRITE && ssl_err != SSL_ERROR_WANT_READ) {
            DESTROY(connection->write_buffer, buffer);
            connection->write_buffer = NULL;
            connection->write_pending = 0;
            _connection_stop_watcher(connection);
            _connection_close_fd(connection);
          }
        } else {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            DESTROY(connection->write_buffer, buffer);
            connection->write_buffer = NULL;
            connection->write_pending = 0;
            _connection_stop_watcher(connection);
            _connection_close_fd(connection);
          }
        }
      }
      break;
    }

    case WS_CONNECTION_CLOSE: {
      if (connection->write_pending) {
        connection->is_closing = 1;
        _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        break;
      }
      if (connection->ssl != NULL) {
        SSL_shutdown(connection->ssl);
        SSL_free(connection->ssl);
        connection->ssl = NULL;
      }
      if (connection->sock != NULL) {
        platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    case OFD_CACHE_RESOLVE_RESULT: {
      _ws_handle_ofd_resolve_result(connection, msg);
      break;
    }

    default:
      break;
  }
}

/* --- Read callback (runs on I/O thread) --- */

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  ws_connection_t* connection = (ws_connection_t*)user_data;

  if (events & PD_EVENT_WRITE) {
    message_t writable_msg;
    writable_msg.type = WS_CONNECTION_WRITABLE;
    writable_msg.payload = NULL;
    writable_msg.payload_destroy = NULL;
    actor_send(&connection->actor, &writable_msg);
  }

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? WS_CONNECTION_HANGUP : WS_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    pd_watcher_t* claimed = ATOMIC_EXCHANGE(&connection->watcher, NULL);
    if (claimed != NULL) {
      pd_watcher_stop(claimed);
      /* Defer watcher destruction through server actor's destroy stack */
      if (connection->transport != NULL) {
        ws_watcher_update_payload_t* payload = get_clear_memory(sizeof(ws_watcher_update_payload_t));
        payload->watcher = claimed;
        payload->events = 0;
        message_t stop_msg;
        stop_msg.type = WS_SERVER_STOP_WATCHER;
        stop_msg.payload = payload;
        stop_msg.payload_destroy = free;
        actor_send(&connection->transport->actor, &stop_msg);
      } else {
        pd_watcher_destroy(claimed);
      }
    }
    return;
  }

  if (events & PD_EVENT_READ) {
    uint8_t read_buffer[READ_BUFFER_SIZE];
    ssize_t bytes_read;

    if (connection->is_ssl && connection->ssl != NULL) {
      bytes_read = SSL_read(connection->ssl, read_buffer, sizeof(read_buffer));
      if (bytes_read <= 0) {
        int ssl_err = SSL_get_error(connection->ssl, (int)bytes_read);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
          return;
        }
        if (ssl_err == SSL_ERROR_ZERO_RETURN || ssl_err == SSL_ERROR_SYSCALL) {
          message_t msg;
          msg.type = WS_CONNECTION_HANGUP;
          msg.payload = NULL;
          msg.payload_destroy = NULL;
          actor_send(&connection->actor, &msg);
          return;
        }
        message_t msg;
        msg.type = WS_CONNECTION_ERROR;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
        return;
      }
    } else {
      bytes_read = platform_socket_recv(connection->sock, read_buffer, sizeof(read_buffer));
      if (bytes_read <= 0) {
        if (bytes_read == 0) {
          message_t msg;
          msg.type = WS_CONNECTION_HANGUP;
          msg.payload = NULL;
          msg.payload_destroy = NULL;
          actor_send(&connection->actor, &msg);
          return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        message_t msg;
        msg.type = WS_CONNECTION_ERROR;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
        return;
      }
    }

    /* Always send raw data to actor thread — it handles upgrade parsing and WS frame parsing */
    buffer_t* data = buffer_create_from_pointer_copy(read_buffer, (size_t)bytes_read);
    message_t msg;
    msg.type = WS_CONNECTION_DATA;
    msg.payload = data;
    msg.payload_destroy = (void (*)(void*))buffer_destroy;
    actor_send(&connection->actor, &msg);
  }
}

/* --- Create / Destroy --- */

ws_connection_t* ws_connection_create(ws_transport_t* transport, platform_socket_t* sock) {
  ws_connection_t* connection = get_clear_memory(sizeof(ws_connection_t));
  refcounter_init((refcounter_t*)connection);
  connection->transport = transport;
  connection->sock = sock;
  connection->pool = transport->pool;
  connection->bc = transport->bc;
  connection->ofd_cache = transport->ofd_cache;
  connection->tc = transport->tc;
  connection->write_buffer = NULL;
  connection->write_pending = 0;
  connection->is_closing = 0;
  connection->is_ssl = 0;
  connection->is_authenticated = (transport->api_key_hash == NULL) ? 1 : 0;
  connection->ssl = NULL;
  connection->state = WS_STATE_UPGRADING;
  connection->upgrade_buf = NULL;
  connection->recv_buf = NULL;
  connection->put_streaming = 0;
  connection->put_ws = NULL;
  connection->put_desc = NULL;
  connection->put_recipe = NULL;
  connection->put_content_type = NULL;
  connection->put_file_name = NULL;
  connection->put_stream_length = 0;
  connection->put_server_address = NULL;
  connection->put_file_hash = NULL;
  connection->put_descriptor_hash = NULL;
  connection->resolve_url = NULL;
  connection->resolve_path = NULL;
  connection->get_phase = WS_GET_RESOLVE_DIR;

  /* Set up SSL if the transport has an SSL_CTX */
  if (transport->ssl_ctx != NULL) {
    connection->ssl = SSL_new(transport->ssl_ctx);
    if (connection->ssl != NULL) {
      SSL_set_fd(connection->ssl, platform_socket_fd(sock));
      SSL_set_accept_state(connection->ssl);
      connection->is_ssl = 1;
    }
  }

  connection->block_ctx.conn = (block_connection_t*)connection;
  connection->block_ctx.bc = transport->bc;
  connection->block_ctx.actor = &connection->actor;
  connection->block_ctx.is_authenticated = connection->is_authenticated;
  connection->block_ctx.send_frame = (block_send_frame_fn)_ws_connection_send_frame;
  connection->block_ctx.send_error = (block_send_error_fn)_ws_connection_send_error;
  connection->block_ctx.pending_op = BLOCK_OP_NONE;

  actor_init(&connection->actor, connection, ws_connection_dispatch, transport->pool);

  platform_socket_set_nonblocking(sock);

  ATOMIC_STORE(&connection->watcher, pd_watcher_create(transport->loop, platform_socket_fd(sock),
    PD_EVENT_READ, _connection_read_callback, connection));
  if (ATOMIC_LOAD(&connection->watcher) != NULL) {
    pd_watcher_start(ATOMIC_LOAD(&connection->watcher));
  }

  return connection;
}

void ws_connection_destroy(ws_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*)connection)) {
    if (connection->transport != NULL) {
      atomic_fetch_sub(&connection->transport->active_connections, 1);
      vec_remove(&connection->transport->connections, connection);
    }
    actor_destroy(&connection->actor);
    if (ATOMIC_LOAD(&connection->watcher) != NULL) {
      pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
      if (watcher != NULL) {
        if (connection->transport != NULL) {
          ws_watcher_update_payload_t* payload = get_clear_memory(sizeof(ws_watcher_update_payload_t));
          payload->watcher = watcher;
          payload->events = 0;
          message_t msg;
          msg.type = WS_SERVER_STOP_WATCHER;
          msg.payload = payload;
          msg.payload_destroy = free;
          actor_send(&connection->transport->actor, &msg);
        } else {
          pd_watcher_stop(watcher);
          pd_watcher_destroy(watcher);
        }
      }
    }
    if (connection->ssl != NULL) {
      SSL_free(connection->ssl);
    }
    if (connection->sock != NULL) {
      platform_socket_destroy(connection->sock);
      connection->sock = NULL;
    }
    if (connection->upgrade_buf != NULL) {
      DESTROY(connection->upgrade_buf, buffer);
    }
    if (connection->recv_buf != NULL) {
      DESTROY(connection->recv_buf, buffer);
    }
    if (connection->write_buffer != NULL) {
      DESTROY(connection->write_buffer, buffer);
    }
    if (connection->put_content_type != NULL) {
      free(connection->put_content_type);
    }
    if (connection->put_file_name != NULL) {
      free(connection->put_file_name);
    }
    if (connection->put_server_address != NULL) {
      free(connection->put_server_address);
    }
    if (connection->put_file_hash != NULL) {
      DESTROY(connection->put_file_hash, buffer);
    }
    if (connection->put_descriptor_hash != NULL) {
      DESTROY(connection->put_descriptor_hash, buffer);
    }
    if (connection->resolve_url != NULL) {
      off_url_destroy(connection->resolve_url);
    }
    if (connection->resolve_path != NULL) {
      free(connection->resolve_path);
    }
    free(connection);
  }
}

void ws_connection_write(ws_connection_t* connection, const uint8_t* data, size_t length) {
  if (connection == NULL || connection->sock == NULL) {
    return;
  }
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, length);
  message_t msg;
  msg.type = WS_CONNECTION_WRITE;
  msg.payload = buf;
  msg.payload_destroy = (void (*)(void*))buffer_destroy;
  actor_send(&connection->actor, &msg);
}

void ws_connection_close(ws_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  message_t msg;
  msg.type = WS_CONNECTION_CLOSE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&connection->actor, &msg);
}