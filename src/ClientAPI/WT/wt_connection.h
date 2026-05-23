//
// Created by victor on 5/20/26.
//
#ifndef OFFS_WT_CONNECTION_H
#define OFFS_WT_CONNECTION_H

#include <stdint.h>
#include <stddef.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#include "../../RefCounter/refcounter.h"
#include "../../Buffer/buffer.h"
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
#include "../../Network/stream_framer.h"

typedef struct wt_transport_t wt_transport_t;

/* Send complete context for async QUIC stream sends */
typedef struct {
  uint8_t* frame;
  QUIC_BUFFER buf;
} send_complete_context_t;

typedef enum {
  WT_GET_RESOLVE_DIR,
  WT_GET_RESOLVE_INDEX,
  WT_GET_FETCH_RAW_OFD
} wt_get_phase_e;

typedef struct wt_connection_t {
  refcounter_t refcounter;
  actor_t actor;
  HQUIC connection;
  HQUIC stream;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  uint8_t is_closing;
  uint8_t is_authenticated;
  wt_transport_t* transport;
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
  wt_get_phase_e get_phase;
} wt_connection_t;

wt_connection_t* wt_connection_create(wt_transport_t* transport, HQUIC connection, HQUIC stream);
void wt_connection_destroy(wt_connection_t* connection);
void wt_connection_dispatch(void* state, message_t* msg);
void wt_connection_send_frame(wt_connection_t* connection, cbor_item_t* frame);
void wt_connection_send_error(wt_connection_t* connection, uint8_t status_code, const char* message);

#endif /* HAS_MSQUIC */
#endif /* OFFS_WT_CONNECTION_H */