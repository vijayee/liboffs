//
// Created by victor on 5/20/26.
//
#include "tcp_connection.h"
#include "tcp_transport.h"
#include "../client_api_wire.h"
#include <cJSON.h>
#include <stdlib.h>
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
#include "../../Network/stream_framer.h"
#include "../../Actor/actor.h"
#include "../../Actor/message.h"
#include "../../Scheduler/scheduler.h"
#include "../../RefCounter/refcounter.h"
#include "../../Streams/stream.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "../../Util/bcrypt.h"

#define READ_BUFFER_SIZE 65536

/* Pipeline context for GET requests */
typedef struct {
  refcounter_t refcounter;
  readable_descriptor_t* desc;
  readable_off_stream_t* rs;
  tuple_cache_t* tc;
  tcp_connection_t* connection;
  ori_t* ori;
} tcp_get_pipeline_t;

/* Pipeline context for PUT requests */
typedef struct {
  refcounter_t refcounter;
  tcp_connection_t* connection;
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
} tcp_put_pipeline_t;

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data);
static void _tcp_dispatch_frame(tcp_connection_t* conn, uint8_t type, cbor_item_t* frame);

/* Helper: serialize a CBOR item to a length-prefixed frame and write to connection */
static void _tcp_connection_send_frame(tcp_connection_t* conn, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);

  if (cbor_buf == NULL || cbor_len == 0) return;

  size_t framed_len;
  uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
  free(cbor_buf);

  if (framed != NULL) {
    tcp_connection_write(conn, framed, framed_len);
    free(framed);
  }
}

/* Helper: send an error frame */
static void _tcp_connection_send_error(tcp_connection_t* conn, uint8_t status_code, const char* message) {
  client_api_error_t error_msg;
  error_msg.status_code = status_code;
  error_msg.message = (char*)message;
  cbor_item_t* frame = client_api_error_encode(&error_msg);
  _tcp_connection_send_frame(conn, frame);
}

/* --- Watcher helpers (follow http_connection pattern) --- */

static void _connection_update_watcher(tcp_connection_t* connection, pd_event_t events) {
  if (connection->transport == NULL) return;
  pd_watcher_t* watcher = ATOMIC_LOAD(&connection->watcher);
  if (watcher == NULL) return;
  tcp_watcher_update_payload_t* payload = get_clear_memory(sizeof(tcp_watcher_update_payload_t));
  payload->watcher = watcher;
  payload->events = events;
  message_t msg;
  msg.type = TCP_SERVER_UPDATE_WATCHER;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&connection->actor, &msg);
}

static void _connection_stop_watcher(tcp_connection_t* connection) {
  pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
  if (watcher == NULL) return;
  if (connection->transport != NULL) {
    tcp_watcher_update_payload_t* payload = get_clear_memory(sizeof(tcp_watcher_update_payload_t));
    payload->watcher = watcher;
    payload->events = 0;
    message_t msg;
    msg.type = TCP_SERVER_STOP_WATCHER;
    msg.payload = payload;
    msg.payload_destroy = free;
    actor_send(&connection->transport->actor, &msg);
  } else {
    pd_watcher_stop(watcher);
    pd_watcher_destroy(watcher);
  }
}

static void _connection_close_fd(tcp_connection_t* connection) {
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

/* --- GET pipeline callbacks --- */

static void _tcp_get_on_tuple(void* ctx, void* data) {
  tcp_get_pipeline_t* pipeline = (tcp_get_pipeline_t*)ctx;
  tuple_t* tuple = (tuple_t*)data;
  readable_off_stream_write(pipeline->rs, tuple);
}

static void _tcp_get_on_rs_data(void* ctx, void* data) {
  tcp_get_pipeline_t* pipeline = (tcp_get_pipeline_t*)ctx;
  buffer_t* buf = (buffer_t*)data;
  client_api_get_data_t get_data_msg;
  get_data_msg.data = buf->data;
  get_data_msg.data_size = buf->size;
  cbor_item_t* frame = client_api_get_data_encode(&get_data_msg);
  _tcp_connection_send_frame(pipeline->connection, frame);
}

static void _tcp_get_on_rs_close(void* ctx, void* unused) {
  (void)unused;
  tcp_get_pipeline_t* pipeline = (tcp_get_pipeline_t*)ctx;
  cbor_item_t* frame = client_api_get_end_encode();
  _tcp_connection_send_frame(pipeline->connection, frame);
  stream_deferred_deref((stream_t*)pipeline->rs);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _tcp_get_on_rs_error(void* ctx, void* error) {
  (void)error;
  tcp_get_pipeline_t* pipeline = (tcp_get_pipeline_t*)ctx;
  _tcp_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "Stream error");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
}

static void _tcp_get_on_desc_close(void* ctx, void* unused) {
  (void)unused;
  tcp_get_pipeline_t* pipeline = (tcp_get_pipeline_t*)ctx;
  stream_deferred_deref((stream_t*)pipeline->desc);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _tcp_get_on_desc_error(void* ctx, void* error) {
  (void)error;
  tcp_get_pipeline_t* pipeline = (tcp_get_pipeline_t*)ctx;
  _tcp_connection_send_error(pipeline->connection, CLIENT_API_STATUS_NOT_FOUND, "Not found");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
  stream_deactivate((stream_t*)pipeline->desc, NULL);
}

static tcp_get_pipeline_t* _tcp_get_pipeline_create(tcp_connection_t* conn, ori_t* ori) {
  tcp_get_pipeline_t* pipeline = get_clear_memory(sizeof(tcp_get_pipeline_t));
  refcounter_init((refcounter_t*)pipeline);
  pipeline->connection = conn;
  pipeline->ori = ori;
  pipeline->tc = conn->tc;
  return pipeline;
}

/* --- PUT pipeline callbacks --- */

static void _tcp_put_on_descriptor_close(void* ctx, void* unused) {
  (void)unused;
  tcp_put_pipeline_t* pipeline = (tcp_put_pipeline_t*)ctx;

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
  _tcp_connection_send_frame(pipeline->connection, frame);
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

static void _tcp_put_on_descriptor_data(void* ctx, void* data) {
  tcp_put_pipeline_t* pipeline = (tcp_put_pipeline_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  /* The descriptor's data_event carries the descriptor hash */
  if (pipeline->descriptor_hash != NULL) {
    DESTROY(pipeline->descriptor_hash, buffer);
  }
  pipeline->descriptor_hash = REFERENCE(payload, buffer_t);
}

static void _tcp_put_on_stream_close(void* ctx, void* unused) {
  (void)unused;
  tcp_put_pipeline_t* pipeline = (tcp_put_pipeline_t*)ctx;
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

static void _tcp_put_on_stream_data(void* ctx, void* data) {
  tcp_put_pipeline_t* pipeline = (tcp_put_pipeline_t*)ctx;
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

static void _tcp_put_on_stream_error(void* ctx, void* error) {
  (void)error;
  tcp_put_pipeline_t* pipeline = (tcp_put_pipeline_t*)ctx;
  _tcp_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT stream error");
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}

static void _tcp_put_on_descriptor_error(void* ctx, void* error) {
  (void)error;
  tcp_put_pipeline_t* pipeline = (tcp_put_pipeline_t*)ctx;
  _tcp_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT descriptor error");
  stream_deactivate((stream_t*)pipeline->desc, NULL);
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}

/* --- Frame handlers --- */

static void _tcp_handle_get(tcp_connection_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  client_api_get_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_get_request_decode(frame, &msg) != 0) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid GET request");
    return;
  }

  off_url_t* url = off_url_parse(msg.ori_string);
  client_api_get_request_destroy(&msg);

  if (url == NULL) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid OFF URL");
    return;
  }

  /* Check if this is a directory that needs OFD resolution */
  if (url->content_type != NULL && strstr(url->content_type, "offsystem/directory") != NULL) {
    /* Async directory resolution */
    conn->resolve_url = url;
    conn->get_phase = TCP_GET_RESOLVE_DIR;
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
  _tcp_connection_send_frame(conn, start_frame);

  /* Create pipeline */
  tcp_get_pipeline_t* pipeline = _tcp_get_pipeline_create(conn, ori);
  REFERENCE(pipeline, tcp_get_pipeline_t);

  size_t descriptor_pad = 32;

  readable_off_stream_t* rs = readable_off_stream_create(
    conn->pool, conn->bc, conn->tc, REFERENCE(ori, ori_t), descriptor_pad, NULL);
  readable_descriptor_t* desc = readable_descriptor_create(
    conn->pool, conn->bc, REFERENCE(ori, ori_t), descriptor_pad, NULL);

  pipeline->rs = rs;
  pipeline->desc = desc;

  /* Subscribe to stream events (5-arg signature) */
  stream_subscribe((stream_t*)rs, data_event, pipeline, _tcp_get_on_rs_data, NULL);
  stream_subscribe((stream_t*)rs, close_event, pipeline, _tcp_get_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, error_event, pipeline, _tcp_get_on_rs_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _tcp_get_on_desc_close, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _tcp_get_on_desc_error, NULL);

  /* Pipe: descriptor provides tuples to the off_stream */
  stream_subscribe((stream_t*)desc, data_event, pipeline, _tcp_get_on_tuple, NULL);

  /* Start the descriptor pull */
  readable_descriptor_push(desc);
}

static void _tcp_handle_put(tcp_connection_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_put_request_decode(frame, &msg) != 0) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid PUT request");
    return;
  }

  if (msg.content_type == NULL || msg.file_name == NULL) {
    client_api_put_request_destroy(&msg);
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Missing content_type or file_name");
    return;
  }

  /* Create pipeline context */
  tcp_put_pipeline_t* pipeline = get_clear_memory(sizeof(tcp_put_pipeline_t));
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
  stream_subscribe((stream_t*)ws, close_event, pipeline, _tcp_put_on_stream_close, NULL);
  stream_subscribe((stream_t*)ws, data_event, pipeline, _tcp_put_on_stream_data, NULL);
  stream_subscribe((stream_t*)ws, error_event, pipeline, _tcp_put_on_stream_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _tcp_put_on_descriptor_close, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _tcp_put_on_descriptor_data, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _tcp_put_on_descriptor_error, NULL);

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

static void _tcp_handle_put_data(tcp_connection_t* conn, cbor_item_t* frame) {
  if (!conn->is_authenticated) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  if (!conn->put_streaming || conn->put_ws == NULL) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "No streaming PUT in progress");
    return;
  }

  client_api_put_data_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_put_data_decode(frame, &msg) != 0) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid PUT_DATA frame");
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

static void _tcp_handle_put_end(tcp_connection_t* conn) {
  if (!conn->is_authenticated) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }
  if (!conn->put_streaming || conn->put_ws == NULL) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "No streaming PUT in progress");
    return;
  }

  writeable_off_stream_finalize(conn->put_ws);
  conn->put_streaming = 0;
  conn->put_ws = NULL;
  conn->put_desc = NULL;
}

static void _tcp_handle_auth(tcp_connection_t* conn, cbor_item_t* frame) {
  if (conn->transport == NULL || conn->transport->api_key_hash == NULL) {
    conn->is_authenticated = 1;
    conn->block_ctx.is_authenticated = 1;
    return;
  }

  client_api_auth_request_t auth;
  if (client_api_auth_request_decode(frame, &auth) != 0) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Invalid auth request");
    return;
  }

  char* key = get_memory(auth.api_key_len + 1);
  memcpy(key, auth.api_key, auth.api_key_len);
  key[auth.api_key_len] = '\0';

  if (bcrypt_check(key, conn->transport->api_key_hash) == 0) {
    conn->is_authenticated = 1;
    conn->block_ctx.is_authenticated = 1;
  } else {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication failed");
  }

  free(key);
  client_api_auth_request_destroy(&auth);
}

static void _tcp_dispatch_frame(tcp_connection_t* conn, uint8_t type, cbor_item_t* frame) {
  switch (type) {
    case CLIENT_API_GET_REQUEST:
      _tcp_handle_get(conn, frame);
      break;
    case CLIENT_API_PUT_REQUEST:
      _tcp_handle_put(conn, frame);
      break;
    case CLIENT_API_PUT_DATA:
      _tcp_handle_put_data(conn, frame);
      break;
    case CLIENT_API_PUT_END:
      _tcp_handle_put_end(conn);
      break;
    case CLIENT_API_AUTH_REQUEST:
      _tcp_handle_auth(conn, frame);
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
        _tcp_connection_send_error(conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Health data serialization failed");
        break;
      }
      client_api_health_response_t resp;
      resp.json_data = json_str;
      cbor_item_t* health_frame = client_api_health_response_encode(&resp);
      _tcp_connection_send_frame(conn, health_frame);
      free(json_str);
      break;
    }
    default:
      _tcp_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Unknown message type");
      break;
  }
}

/* --- Async GET resolution (OFD cache) --- */

static void _tcp_handle_ofd_resolve_result(tcp_connection_t* conn, message_t* msg) {
  ofd_resolve_result_t* result = (ofd_resolve_result_t*)msg->payload;
  if (result == NULL) {
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_NOT_FOUND, "Directory resolution failed");
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
    _tcp_connection_send_error(conn, CLIENT_API_STATUS_NOT_FOUND, "Directory entry not found");
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
  _tcp_connection_send_frame(conn, start_frame);

  /* Create pipeline */
  tcp_get_pipeline_t* pipeline = _tcp_get_pipeline_create(conn, resolved_ori);
  REFERENCE(pipeline, tcp_get_pipeline_t);

  size_t descriptor_pad = 32;

  readable_off_stream_t* rs = readable_off_stream_create(
    conn->pool, conn->bc, conn->tc, REFERENCE(resolved_ori, ori_t), descriptor_pad, NULL);
  readable_descriptor_t* desc = readable_descriptor_create(
    conn->pool, conn->bc, REFERENCE(resolved_ori, ori_t), descriptor_pad, NULL);

  pipeline->rs = rs;
  pipeline->desc = desc;

  stream_subscribe((stream_t*)rs, data_event, pipeline, _tcp_get_on_rs_data, NULL);
  stream_subscribe((stream_t*)rs, close_event, pipeline, _tcp_get_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, error_event, pipeline, _tcp_get_on_rs_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _tcp_get_on_desc_close, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _tcp_get_on_desc_error, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _tcp_get_on_tuple, NULL);

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

void tcp_connection_dispatch(void* state, message_t* msg) {
  tcp_connection_t* connection = (tcp_connection_t*)state;

  if (connection->is_closing) {
    return;
  }

  switch (msg->type) {
    case CACHE_PUT_RESULT:
    case CACHE_GET_RESULT:
    case CACHE_REMOVE_RESULT:
      if (block_handle_cache_result(&connection->block_ctx, msg)) break;
      break;
    case TCP_CONNECTION_DATA: {
      buffer_t* data = (buffer_t*)msg->payload;
      msg->payload = NULL;
      if (connection->sock == NULL) {
        DESTROY(data, buffer);
        break;
      }
      stream_framer_feed(connection->framer, data->data, data->size);
      DESTROY(data, buffer);

      size_t frame_len;
      uint8_t* frame_data;
      while ((frame_data = stream_framer_next(connection->framer, &frame_len)) != NULL) {
        struct cbor_load_result load_result;
        cbor_item_t* cbor_item = cbor_load(frame_data, frame_len, &load_result);
        free(frame_data);
        if (cbor_item == NULL || load_result.error.code != CBOR_ERR_NONE) {
          if (cbor_item != NULL) {
            cbor_decref(&cbor_item);
          }
          continue;
        }
        uint8_t type = client_api_wire_get_type(cbor_item);
        _tcp_dispatch_frame(connection, type, cbor_item);
        cbor_decref(&cbor_item);
      }
      break;
    }

    case TCP_CONNECTION_HANGUP:
    case TCP_CONNECTION_ERROR: {
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    case TCP_CONNECTION_WRITE: {
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

    case TCP_CONNECTION_WRITABLE: {
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

    case TCP_CONNECTION_CLOSE: {
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
      _tcp_handle_ofd_resolve_result(connection, msg);
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
  tcp_connection_t* connection = (tcp_connection_t*)user_data;

  if (events & PD_EVENT_WRITE) {
    message_t writable_msg;
    writable_msg.type = TCP_CONNECTION_WRITABLE;
    writable_msg.payload = NULL;
    writable_msg.payload_destroy = NULL;
    actor_send(&connection->actor, &writable_msg);
  }

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? TCP_CONNECTION_HANGUP : TCP_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    pd_watcher_t* claimed = ATOMIC_EXCHANGE(&connection->watcher, NULL);
    if (claimed != NULL) {
      pd_watcher_stop(claimed);
      /* Defer watcher destruction through server actor's destroy stack */
      if (connection->transport != NULL) {
        tcp_watcher_update_payload_t* payload = get_clear_memory(sizeof(tcp_watcher_update_payload_t));
        payload->watcher = claimed;
        payload->events = 0;
        message_t stop_msg;
        stop_msg.type = TCP_SERVER_STOP_WATCHER;
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
          /* Clean shutdown or connection closed */
          message_t msg;
          msg.type = TCP_CONNECTION_HANGUP;
          msg.payload = NULL;
          msg.payload_destroy = NULL;
          actor_send(&connection->actor, &msg);
          return;
        }
        /* Other SSL errors */
        message_t msg;
        msg.type = TCP_CONNECTION_ERROR;
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
          msg.type = TCP_CONNECTION_HANGUP;
          msg.payload = NULL;
          msg.payload_destroy = NULL;
          actor_send(&connection->actor, &msg);
          return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        message_t msg;
        msg.type = TCP_CONNECTION_ERROR;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
        return;
      }
    }

    buffer_t* data = buffer_create_from_pointer_copy(read_buffer, (size_t)bytes_read);
    message_t msg;
    msg.type = TCP_CONNECTION_DATA;
    msg.payload = data;
    msg.payload_destroy = (void (*)(void*))buffer_destroy;
    actor_send(&connection->actor, &msg);
  }
}

/* --- Create / Destroy --- */

tcp_connection_t* tcp_connection_create(tcp_transport_t* transport, platform_socket_t* sock) {
  tcp_connection_t* connection = get_clear_memory(sizeof(tcp_connection_t));
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
  connection->framer = stream_framer_create();
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
  connection->get_phase = 0;

  /* Set up SSL if the transport has an SSL_CTX */
  if (transport->ssl_ctx != NULL) {
    connection->ssl = SSL_new(transport->ssl_ctx);
    if (connection->ssl != NULL) {
      SSL_set_fd(connection->ssl, platform_socket_fd(sock));
      SSL_set_accept_state(connection->ssl);
      connection->is_ssl = 1;
    }
  }

  /* Initialize block handler context */
  connection->block_ctx.conn = (block_connection_t*)connection;
  connection->block_ctx.bc = transport->bc;
  connection->block_ctx.actor = &connection->actor;
  connection->block_ctx.is_authenticated = connection->is_authenticated;
  connection->block_ctx.send_frame = (block_send_frame_fn)_tcp_connection_send_frame;
  connection->block_ctx.send_error = (block_send_error_fn)_tcp_connection_send_error;
  connection->block_ctx.pending_op = BLOCK_OP_NONE;

  actor_init(&connection->actor, connection, tcp_connection_dispatch, transport->pool);

  platform_socket_set_nonblocking(sock);

  ATOMIC_STORE(&connection->watcher, pd_watcher_create(transport->loop, platform_socket_fd(sock),
    PD_EVENT_READ, _connection_read_callback, connection));
  if (ATOMIC_LOAD(&connection->watcher) != NULL) {
    pd_watcher_start(ATOMIC_LOAD(&connection->watcher));
  }

  return connection;
}

void tcp_connection_destroy(tcp_connection_t* connection) {
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
          tcp_watcher_update_payload_t* payload = get_clear_memory(sizeof(tcp_watcher_update_payload_t));
          payload->watcher = watcher;
          payload->events = 0;
          message_t msg;
          msg.type = TCP_SERVER_STOP_WATCHER;
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
    if (connection->framer != NULL) {
      stream_framer_destroy(connection->framer);
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

void tcp_connection_write(tcp_connection_t* connection, const uint8_t* data, size_t length) {
  if (connection == NULL || connection->sock == NULL) {
    return;
  }
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, length);
  message_t msg;
  msg.type = TCP_CONNECTION_WRITE;
  msg.payload = buf;
  msg.payload_destroy = (void (*)(void*))buffer_destroy;
  actor_send(&connection->actor, &msg);
}

void tcp_connection_close(tcp_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  message_t msg;
  msg.type = TCP_CONNECTION_CLOSE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&connection->actor, &msg);
}