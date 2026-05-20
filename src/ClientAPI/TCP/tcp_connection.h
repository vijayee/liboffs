//
// Created by victor on 5/20/26.
//
#ifndef OFFS_TCP_CONNECTION_H
#define OFFS_TCP_CONNECTION_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include "../../RefCounter/refcounter.h"
#include "../../Buffer/buffer.h"
#include "../../Util/vec.h"
#include "../../Util/atomic_compat.h"
#include "../../Actor/actor.h"
#include "../../Network/stream_framer.h"
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
#include <poll-dancer/poll-dancer.h>

typedef struct tcp_transport_t tcp_transport_t;

typedef struct {
  pd_watcher_t* watcher;
  pd_event_t events;
} tcp_watcher_update_payload_t;

typedef enum {
  TCP_GET_RESOLVE_DIR,
  TCP_GET_RESOLVE_INDEX,
  TCP_GET_FETCH_RAW_OFD
} tcp_get_phase_t;

typedef struct tcp_connection_t {
  refcounter_t refcounter;
  actor_t actor;
  int fd;
  ATOMIC(pd_watcher_t*) watcher;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  uint8_t write_pending;
  uint8_t is_closing;
  uint8_t is_ssl;
  SSL* ssl;
  tcp_transport_t* transport;
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
  uint8_t get_phase;
} tcp_connection_t;

tcp_connection_t* tcp_connection_create(tcp_transport_t* transport, int fd);
void tcp_connection_destroy(tcp_connection_t* connection);

void tcp_connection_dispatch(void* state, message_t* msg);
void tcp_connection_write(tcp_connection_t* connection, const uint8_t* data, size_t length);
void tcp_connection_close(tcp_connection_t* connection);

#endif // OFFS_TCP_CONNECTION_H