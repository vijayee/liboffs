//
// Created by victor on 5/20/26.
//
#include "wt_connection.h"
#include "wt_transport.h"

#ifdef HAS_MSQUIC

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
#include <stdlib.h>

/* Pipeline context for GET requests */
typedef struct {
  refcounter_t refcounter;
  readable_descriptor_t* desc;
  readable_off_stream_t* rs;
  tuple_cache_t* tc;
  wt_connection_t* connection;
  ori_t* ori;
} wt_get_pipeline_t;

/* Pipeline context for PUT requests */
typedef struct {
  refcounter_t refcounter;
  wt_connection_t* connection;
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
} wt_put_pipeline_t;

static void _wt_dispatch_frame(wt_connection_t* conn, uint8_t type, cbor_item_t* frame);

/* --- Send helpers --- */

void wt_connection_send_frame(wt_connection_t* connection, cbor_item_t* frame) {
  if (connection == NULL || connection->stream == NULL || connection->is_closing) {
    if (frame != NULL) {
      cbor_decref(&frame);
    }
    return;
  }

  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);

  if (cbor_buf == NULL || cbor_len == 0) return;

  /* Wrap in 4-byte length-prefix frame */
  size_t framed_len;
  uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
  free(cbor_buf);

  if (framed == NULL) return;

  /* Send via QUIC stream */
  send_complete_context_t* send_ctx = get_clear_memory(sizeof(send_complete_context_t));
  send_ctx->frame = framed;
  send_ctx->buf.Buffer = framed;
  send_ctx->buf.Length = (uint32_t)framed_len;

  QUIC_STATUS status = connection->transport->msquic->StreamSend(
    connection->stream, &send_ctx->buf, 1, QUIC_SEND_FLAG_NONE, send_ctx);

  if (QUIC_FAILED(status)) {
    free(framed);
    free(send_ctx);
  }
}

void wt_connection_send_error(wt_connection_t* connection, uint8_t status_code, const char* message) {
  client_api_error_t error_msg;
  error_msg.status_code = status_code;
  error_msg.message = (char*)message;
  cbor_item_t* frame = client_api_error_encode(&error_msg);
  wt_connection_send_frame(connection, frame);
}

/* --- GET pipeline callbacks --- */

static void _wt_get_on_tuple(void* ctx, void* data) {
  wt_get_pipeline_t* pipeline = (wt_get_pipeline_t*)ctx;
  tuple_t* tuple = (tuple_t*)data;
  readable_off_stream_write(pipeline->rs, tuple);
}

static void _wt_get_on_rs_data(void* ctx, void* data) {
  wt_get_pipeline_t* pipeline = (wt_get_pipeline_t*)ctx;
  buffer_t* buf = (buffer_t*)data;
  client_api_get_data_t get_data_msg;
  get_data_msg.data = buf->data;
  get_data_msg.data_size = buf->size;
  cbor_item_t* frame = client_api_get_data_encode(&get_data_msg);
  wt_connection_send_frame(pipeline->connection, frame);
}

static void _wt_get_on_rs_close(void* ctx, void* unused) {
  (void)unused;
  wt_get_pipeline_t* pipeline = (wt_get_pipeline_t*)ctx;
  cbor_item_t* frame = client_api_get_end_encode();
  wt_connection_send_frame(pipeline->connection, frame);
  stream_deferred_deref((stream_t*)pipeline->rs);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _wt_get_on_rs_error(void* ctx, void* error) {
  (void)error;
  wt_get_pipeline_t* pipeline = (wt_get_pipeline_t*)ctx;
  wt_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "Stream error");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
}

static void _wt_get_on_desc_close(void* ctx, void* unused) {
  (void)unused;
  wt_get_pipeline_t* pipeline = (wt_get_pipeline_t*)ctx;
  stream_deferred_deref((stream_t*)pipeline->desc);
  ori_destroy(pipeline->ori);
  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    DESTROY(pipeline->ori, ori);
    free(pipeline);
  }
}

static void _wt_get_on_desc_error(void* ctx, void* error) {
  (void)error;
  wt_get_pipeline_t* pipeline = (wt_get_pipeline_t*)ctx;
  wt_connection_send_error(pipeline->connection, CLIENT_API_STATUS_NOT_FOUND, "Not found");
  stream_deactivate((stream_t*)pipeline->rs, NULL);
  stream_deactivate((stream_t*)pipeline->desc, NULL);
}

static wt_get_pipeline_t* _wt_get_pipeline_create(wt_connection_t* conn, ori_t* ori) {
  wt_get_pipeline_t* pipeline = get_clear_memory(sizeof(wt_get_pipeline_t));
  refcounter_init((refcounter_t*)pipeline);
  pipeline->connection = conn;
  pipeline->ori = ori;
  pipeline->tc = conn->tc;
  return pipeline;
}

/* --- PUT pipeline callbacks --- */

static void _wt_put_on_descriptor_close(void* ctx, void* unused) {
  (void)unused;
  wt_put_pipeline_t* pipeline = (wt_put_pipeline_t*)ctx;

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
  wt_connection_send_frame(pipeline->connection, frame);
  free(ori_string);
  off_url_destroy(url);

  refcounter_dereference((refcounter_t*)pipeline->recipe);
  scheduler_pool_defer_cleanup(((stream_t*)pipeline->ws)->pool, pipeline->recipe,
                               (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)pipeline->desc);
  stream_deferred_deref((stream_t*)pipeline->ws);

  if (refcounter_dereference_is_zero((refcounter_t*)pipeline)) {
    if (pipeline->content_type != NULL) free(pipeline->content_type);
    if (pipeline->file_name != NULL) free(pipeline->file_name);
    if (pipeline->server_address != NULL) free(pipeline->server_address);
    if (pipeline->file_hash != NULL) DESTROY(pipeline->file_hash, buffer);
    if (pipeline->descriptor_hash != NULL) DESTROY(pipeline->descriptor_hash, buffer);
    free(pipeline);
  }
}

static void _wt_put_on_descriptor_data(void* ctx, void* data) {
  wt_put_pipeline_t* pipeline = (wt_put_pipeline_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (pipeline->descriptor_hash != NULL) {
    DESTROY(pipeline->descriptor_hash, buffer);
  }
  pipeline->descriptor_hash = REFERENCE(payload, buffer_t);
}

static void _wt_put_on_stream_close(void* ctx, void* unused) {
  (void)unused;
  wt_put_pipeline_t* pipeline = (wt_put_pipeline_t*)ctx;
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

static void _wt_put_on_stream_data(void* ctx, void* data) {
  wt_put_pipeline_t* pipeline = (wt_put_pipeline_t*)ctx;
  buffer_t* buf = (buffer_t*)data;

  if (buf->size == 32 && pipeline->file_hash == NULL) {
    pipeline->file_hash = REFERENCE(buf, buffer_t);
  } else {
    tuple_t* tuple = REFERENCE(buf, tuple_t);
    writeable_descriptor_write(pipeline->desc, tuple);
    DESTROY(tuple, tuple);
  }
}

static void _wt_put_on_stream_error(void* ctx, void* error) {
  (void)error;
  wt_put_pipeline_t* pipeline = (wt_put_pipeline_t*)ctx;
  wt_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT stream error");
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}

static void _wt_put_on_descriptor_error(void* ctx, void* error) {
  (void)error;
  wt_put_pipeline_t* pipeline = (wt_put_pipeline_t*)ctx;
  wt_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT descriptor error");
  stream_deactivate((stream_t*)pipeline->desc, NULL);
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}

/* --- Frame handlers --- */

static void _wt_handle_get(wt_connection_t* conn, cbor_item_t* frame) {
  client_api_get_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_get_request_decode(frame, &msg) != 0) {
    wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid GET request");
    return;
  }

  off_url_t* url = off_url_parse(msg.ori_string);
  client_api_get_request_destroy(&msg);

  if (url == NULL) {
    wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid OFF URL");
    return;
  }

  if (url->content_type != NULL && strstr(url->content_type, "offsystem/directory") != NULL) {
    conn->resolve_url = url;
    conn->get_phase = WT_GET_RESOLVE_DIR;
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
  cbor_item_t* start_frame = client_api_get_response_start_encode(&response_start);
  wt_connection_send_frame(conn, start_frame);

  wt_get_pipeline_t* pipeline = _wt_get_pipeline_create(conn, ori);
  REFERENCE(pipeline, wt_get_pipeline_t);

  size_t descriptor_pad = 32;
  readable_off_stream_t* rs = readable_off_stream_create(
    conn->pool, conn->bc, conn->tc, REFERENCE(ori, ori_t), descriptor_pad, NULL);
  readable_descriptor_t* desc = readable_descriptor_create(
    conn->pool, conn->bc, REFERENCE(ori, ori_t), descriptor_pad, NULL);

  pipeline->rs = rs;
  pipeline->desc = desc;

  stream_subscribe((stream_t*)rs, data_event, pipeline, _wt_get_on_rs_data, NULL);
  stream_subscribe((stream_t*)rs, close_event, pipeline, _wt_get_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, error_event, pipeline, _wt_get_on_rs_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _wt_get_on_desc_close, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _wt_get_on_desc_error, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _wt_get_on_tuple, NULL);

  readable_descriptor_push(desc);
}

static void _wt_handle_put(wt_connection_t* conn, cbor_item_t* frame) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_put_request_decode(frame, &msg) != 0) {
    wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid PUT request");
    return;
  }

  if (msg.content_type == NULL || msg.file_name == NULL) {
    client_api_put_request_destroy(&msg);
    wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Missing content_type or file_name");
    return;
  }

  wt_put_pipeline_t* pipeline = get_clear_memory(sizeof(wt_put_pipeline_t));
  refcounter_init((refcounter_t*)pipeline);
  pipeline->connection = conn;
  pipeline->content_type = msg.content_type;
  pipeline->file_name = msg.file_name;
  pipeline->stream_length = msg.stream_length;
  pipeline->server_address = msg.server_address;
  pipeline->file_hash = NULL;
  pipeline->descriptor_hash = NULL;
  pipeline->file_hash_offset = 0;

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
  refcounter_reference((refcounter_t*)pipeline);

  stream_subscribe((stream_t*)ws, close_event, pipeline, _wt_put_on_stream_close, NULL);
  stream_subscribe((stream_t*)ws, data_event, pipeline, _wt_put_on_stream_data, NULL);
  stream_subscribe((stream_t*)ws, error_event, pipeline, _wt_put_on_stream_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _wt_put_on_descriptor_close, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _wt_put_on_descriptor_data, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _wt_put_on_descriptor_error, NULL);

  if (msg.data != NULL && msg.data_size > 0) {
    buffer_t* data = buffer_create_from_pointer_copy(msg.data, msg.data_size);
    free(msg.data);
    msg.data = NULL;
    msg.data_size = 0;
    writeable_off_stream_write(ws, data);
    DESTROY(data, buffer);
    writeable_off_stream_finalize(ws);
  } else {
    conn->put_ws = ws;
    conn->put_desc = desc;
    conn->put_streaming = 1;
    conn->put_content_type = strdup(pipeline->content_type);
    conn->put_file_name = strdup(pipeline->file_name);
    conn->put_stream_length = msg.stream_length;
    conn->put_server_address = msg.server_address != NULL ? strdup(msg.server_address) : NULL;
  }

  msg.content_type = NULL;
  msg.file_name = NULL;
  msg.server_address = NULL;
  client_api_put_request_destroy(&msg);
}

static void _wt_handle_put_data(wt_connection_t* conn, cbor_item_t* frame) {
  if (!conn->put_streaming || conn->put_ws == NULL) {
    wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "No streaming PUT in progress");
    return;
  }

  client_api_put_data_t msg;
  memset(&msg, 0, sizeof(msg));
  if (client_api_put_data_decode(frame, &msg) != 0) {
    wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid PUT_DATA frame");
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

static void _wt_handle_put_end(wt_connection_t* conn) {
  if (!conn->put_streaming || conn->put_ws == NULL) {
    wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "No streaming PUT in progress");
    return;
  }

  writeable_off_stream_finalize(conn->put_ws);
  conn->put_streaming = 0;
  conn->put_ws = NULL;
  conn->put_desc = NULL;
}

static void _wt_dispatch_frame(wt_connection_t* conn, uint8_t type, cbor_item_t* frame) {
  switch (type) {
    case CLIENT_API_GET_REQUEST:
      _wt_handle_get(conn, frame);
      break;
    case CLIENT_API_PUT_REQUEST:
      _wt_handle_put(conn, frame);
      break;
    case CLIENT_API_PUT_DATA:
      _wt_handle_put_data(conn, frame);
      break;
    case CLIENT_API_PUT_END:
      _wt_handle_put_end(conn);
      break;
    default:
      wt_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, "Unknown message type");
      break;
  }
}

/* --- Async OFD resolution --- */

static void _wt_handle_ofd_resolve_result(wt_connection_t* conn, message_t* msg) {
  ofd_resolve_result_t* result = (ofd_resolve_result_t*)msg->payload;
  if (result == NULL || result->ori == NULL) {
    wt_connection_send_error(conn, CLIENT_API_STATUS_NOT_FOUND, "Directory resolution failed");
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

  ori_t* resolved_ori = REFERENCE(result->ori, ori_t);

  client_api_get_response_start_t response_start;
  response_start.content_type = "application/octet-stream";
  response_start.content_length = resolved_ori->final_byte;
  response_start.has_range = 0;
  response_start.range_start = 0;
  response_start.range_end = 0;
  cbor_item_t* start_frame = client_api_get_response_start_encode(&response_start);
  wt_connection_send_frame(conn, start_frame);

  wt_get_pipeline_t* pipeline = _wt_get_pipeline_create(conn, resolved_ori);
  REFERENCE(pipeline, wt_get_pipeline_t);

  size_t descriptor_pad = 32;
  readable_off_stream_t* rs = readable_off_stream_create(
    conn->pool, conn->bc, conn->tc, REFERENCE(resolved_ori, ori_t), descriptor_pad, NULL);
  readable_descriptor_t* desc = readable_descriptor_create(
    conn->pool, conn->bc, REFERENCE(resolved_ori, ori_t), descriptor_pad, NULL);

  pipeline->rs = rs;
  pipeline->desc = desc;

  stream_subscribe((stream_t*)rs, data_event, pipeline, _wt_get_on_rs_data, NULL);
  stream_subscribe((stream_t*)rs, close_event, pipeline, _wt_get_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, error_event, pipeline, _wt_get_on_rs_error, NULL);
  stream_subscribe((stream_t*)desc, close_event, pipeline, _wt_get_on_desc_close, NULL);
  stream_subscribe((stream_t*)desc, error_event, pipeline, _wt_get_on_desc_error, NULL);
  stream_subscribe((stream_t*)desc, data_event, pipeline, _wt_get_on_tuple, NULL);

  readable_descriptor_push(desc);
  DEREFERENCE(pipeline);

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

void wt_connection_dispatch(void* state, message_t* msg) {
  wt_connection_t* connection = (wt_connection_t*)state;

  if (connection->is_closing) {
    return;
  }

  switch (msg->type) {
    case WT_CONNECTION_DATA: {
      buffer_t* data = (buffer_t*)msg->payload;
      msg->payload = NULL;
      if (connection->stream == NULL) {
        DESTROY(data, buffer);
        break;
      }

      stream_framer_feed(connection->framer, data->data, data->size);
      DESTROY(data, buffer);

      uint8_t* frame_data;
      size_t frame_len;
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
        _wt_dispatch_frame(connection, type, cbor_item);
        cbor_decref(&cbor_item);
      }
      break;
    }

    case WT_CONNECTION_HANGUP:
    case WT_CONNECTION_ERROR: {
      connection->is_closing = 1;
      break;
    }

    case OFD_CACHE_RESOLVE_RESULT: {
      _wt_handle_ofd_resolve_result(connection, msg);
      break;
    }

    default:
      break;
  }
}

/* --- Create / Destroy --- */

wt_connection_t* wt_connection_create(wt_transport_t* transport, HQUIC connection, HQUIC stream) {
  wt_connection_t* conn = get_clear_memory(sizeof(wt_connection_t));
  refcounter_init((refcounter_t*)conn);
  conn->transport = transport;
  conn->connection = connection;
  conn->stream = stream;
  conn->pool = transport->pool;
  conn->bc = transport->bc;
  conn->ofd_cache = transport->ofd_cache;
  conn->tc = transport->tc;
  conn->is_closing = 0;
  conn->put_streaming = 0;
  conn->put_ws = NULL;
  conn->put_desc = NULL;
  conn->put_recipe = NULL;
  conn->put_content_type = NULL;
  conn->put_file_name = NULL;
  conn->put_stream_length = 0;
  conn->put_server_address = NULL;
  conn->put_file_hash = NULL;
  conn->put_descriptor_hash = NULL;
  conn->resolve_url = NULL;
  conn->resolve_path = NULL;
  conn->get_phase = WT_GET_RESOLVE_DIR;
  conn->write_buffer = NULL;

  conn->framer = stream_framer_create();

  actor_init(&conn->actor, conn, wt_connection_dispatch, transport->pool);

  return conn;
}

void wt_connection_destroy(wt_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*)connection)) {
    if (connection->transport != NULL) {
      atomic_fetch_sub(&connection->transport->active_connections, 1);
      platform_mutex_lock(connection->transport->conn_lock);
      vec_remove(&connection->transport->connections, connection);
      platform_mutex_unlock(connection->transport->conn_lock);
    }
    actor_destroy(&connection->actor);
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

#else /* !HAS_MSQUIC */

#include "wt_connection.h"

wt_connection_t* wt_connection_create(wt_transport_t* transport, HQUIC connection, HQUIC stream) {
  (void)transport; (void)connection; (void)stream;
  return NULL;
}

void wt_connection_destroy(wt_connection_t* connection) {
  (void)connection;
}

void wt_connection_dispatch(void* state, message_t* msg) {
  (void)state; (void)msg;
}

void wt_connection_send_frame(wt_connection_t* connection, cbor_item_t* frame) {
  (void)connection; (void)frame;
}

void wt_connection_send_error(wt_connection_t* connection, uint8_t status_code, const char* message) {
  (void)connection; (void)status_code; (void)message;
}

#endif /* HAS_MSQUIC */