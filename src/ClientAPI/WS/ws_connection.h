//
// Created by victor on 5/20/26.
//
#ifndef OFFS_WS_CONNECTION_H
#define OFFS_WS_CONNECTION_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include "../../RefCounter/refcounter.h"
#include "../../Buffer/buffer.h"
#include "../../Util/vec.h"
#include "../../Util/atomic_compat.h"
#include "../../Actor/actor.h"
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
#include "../../Scheduler/scheduler.h"
#include "../../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "ws_frame.h"

typedef struct ws_transport_t ws_transport_t;

typedef struct {
  pd_watcher_t* watcher;
  pd_event_t events;
} ws_watcher_update_payload_t;

typedef enum {
  WS_STATE_UPGRADING,    /* Reading HTTP upgrade request */
  WS_STATE_CONNECTED     /* WebSocket frame exchange */
} ws_state_e;

typedef enum {
  WS_GET_RESOLVE_DIR,
  WS_GET_RESOLVE_INDEX,
  WS_GET_FETCH_RAW_OFD
} ws_get_phase_e;

typedef struct ws_connection_t {
  refcounter_t refcounter;
  actor_t actor;
  platform_socket_t* sock;
  ATOMIC(pd_watcher_t*) watcher;
  SSL* ssl;
  uint8_t is_ssl;
  ws_state_e state;              /* UPGRADING or CONNECTED */
  buffer_t* upgrade_buf;         /* Buffer for HTTP upgrade request data */
  buffer_t* recv_buf;            /* Buffer for reassembling partial WebSocket frames */
  buffer_t* write_buffer;
  uint8_t write_pending;
  uint8_t is_closing;
  ws_transport_t* transport;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;

  /* Streaming PUT state */
  writeable_off_stream_t* put_ws;
  writeable_descriptor_t* put_desc;
  new_blocks_recipe_t* put_recipe;
  char* put_content_type;
  char* put_file_name;
  size_t put_stream_length;
  char* put_server_address;
  buffer_t* put_file_hash;
  buffer_t* put_descriptor_hash;
  uint8_t put_streaming;

  /* Async GET state */
  off_url_t* resolve_url;
  char* resolve_path;
  ws_get_phase_e get_phase;
} ws_connection_t;

ws_connection_t* ws_connection_create(ws_transport_t* transport, platform_socket_t* sock);
void ws_connection_destroy(ws_connection_t* connection);

void ws_connection_dispatch(void* state, message_t* msg);
void ws_connection_write(ws_connection_t* connection, const uint8_t* data, size_t length);
void ws_connection_close(ws_connection_t* connection);

#endif // OFFS_WS_CONNECTION_H
