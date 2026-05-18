//
// Created by victor on 5/16/26.
//

#define _GNU_SOURCE
#include "test_control_protocol.h"
#include "../src/Network/network.h"
#include "../src/Network/authority.h"
#include "../src/Network/quic_listener.h"
#include "../src/Network/relay_client.h"
#include "../src/Network/conn_state.h"
#include "../src/Network/find_block.h"
#include "../src/Util/log.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/readable_off_stream.h"
#include "../src/OFFStreams/writeable_descriptor.h"
#include "../src/OFFStreams/readable_descriptor.h"
#include "../src/OFFStreams/ori.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/block_recipe.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Configuration/config.h"
#include "../src/Streams/stream.h"
#include "../src/Buffer/buffer.h"
#include "../src/Util/allocator.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../deps/BLAKE3/c/blake3.h"
#ifdef OFFS_TEST
#include "../src/Network/message_log.h"
#include "../src/Network/wire.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/* ---- BLAKE3 helpers ---- */

static void blake3_hash_buffer(const uint8_t* data, size_t length, uint8_t* out) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data, length);
  blake3_hasher_finalize(&hasher, out, 32);
}

static void buffer_to_hex(const uint8_t* buf, size_t len, char* hex) {
  for (size_t idx = 0; idx < len; idx++) {
    snprintf(hex + (idx * 2), 3, "%02x", buf[idx]);
  }
  hex[len * 2] = '\0';
}

static int hex_to_buffer(const char* hex, uint8_t* buf, size_t buf_len) {
  size_t hex_len = strlen(hex);
  if (hex_len != buf_len * 2) {
    return -1;
  }
  for (size_t idx = 0; idx < buf_len; idx++) {
    unsigned int byte_val;
    if (sscanf(hex + (idx * 2), "%02x", &byte_val) != 1) {
      return -1;
    }
    buf[idx] = (uint8_t)byte_val;
  }
  return 0;
}

/* ---- Node state ---- */

typedef struct {
  scheduler_pool_t* pool;
  authority_t* authority;
  timer_actor_t* timer;
  block_cache_t* block_cache;
  tuple_cache_t* tuple_cache;
  network_t* network;
#ifdef HAS_MSQUIC
  quic_listener_t* listener;
#endif
  config_t config;
  char* cache_dir;
  uint16_t port;
  uint16_t control_port;
  char* relay_host;
  uint16_t relay_port;
  char* cert_path;
  char* key_path;
  volatile int running;
  /* Control socket */
  int control_fd;
  /* Store/Fetch synchronization */
  pthread_mutex_t state_lock;
  pthread_cond_t state_cond;
} node_state_t;

static node_state_t g_node;

static void node_state_init(node_state_t* state) {
  memset(state, 0, sizeof(node_state_t));
  state->running = 1;
  state->control_fd = -1;
  pthread_mutex_init(&state->state_lock, NULL);
  pthread_cond_init(&state->state_cond, NULL);
}

static void node_state_destroy(node_state_t* state) {
  pthread_cond_destroy(&state->state_cond);
  pthread_mutex_destroy(&state->state_lock);
  if (state->cache_dir) {
    free(state->cache_dir);
  }
  if (state->relay_host) {
    free(state->relay_host);
  }
  if (state->cert_path) {
    free(state->cert_path);
  }
  if (state->key_path) {
    free(state->key_path);
  }
}

/* ---- Control socket helpers ---- */

static void send_response(int client_fd, const char* response) {
  size_t length = strlen(response);
  ssize_t sent = send(client_fd, response, length, MSG_NOSIGNAL);
  if (sent < 0) {
    fprintf(stderr, "node: send failed: %s\n", strerror(errno));
    return;
  }
  sent = send(client_fd, "\n", 1, MSG_NOSIGNAL);
  if (sent < 0) {
    fprintf(stderr, "node: send newline failed: %s\n", strerror(errno));
  }
}

static size_t get_peer_count(void) {
  if (g_node.network) {
    return g_node.network->conn_mgr.peer_count;
  }
  return 0;
}

static size_t get_block_count(void) {
  if (g_node.block_cache && g_node.block_cache->index) {
    return index_count(g_node.block_cache->index);
  }
  return 0;
}

static int is_relay_connected(void) {
  if (g_node.network && g_node.network->relay) {
    return g_node.network->relay->connected;
  }
  return 0;
}

static const char* nat_type_string(nat_type_e type) {
  switch (type) {
    case NAT_TYPE_OPEN:
      return "open";
    case NAT_TYPE_FULL_CONE:
      return "full_cone";
    case NAT_TYPE_RESTRICTED_CONE:
      return "restricted_cone";
    case NAT_TYPE_PORT_RESTRICTED_CONE:
      return "port_restricted_cone";
    case NAT_TYPE_SYMMETRIC:
      return "symmetric";
    default:
      return "unknown";
  }
}

/* ---- Control command handlers ---- */

static void handle_status(int client_fd) {
  char response[512];
  size_t peers = get_peer_count();
  size_t blocks = get_block_count();
  const char* relay_status = is_relay_connected() ? "connected" : "disconnected";
  const char* nat = "unknown";
  if (g_node.network) {
    nat = nat_type_string(g_node.network->local_nat_type);
  }
  const char* node_id_str = g_node.authority ? g_node.authority->local_id.str : "null";
  snprintf(response, sizeof(response), "%s node_id=%s peers=%zu blocks=%zu relay=%s nat=%s endpoint=%u",
           CTRL_RESP_STATUS, node_id_str, peers, blocks, relay_status, nat,
           g_node.network->relay ? g_node.network->relay->local_endpoint_id : 0);
  send_response(client_fd, response);
}

static void handle_shutdown(int client_fd) {
  g_node.running = 0;
  send_response(client_fd, CTRL_RESP_OK);
}

/* ---- STORE_FILE handler ---- */

typedef struct {
  node_state_t* node;
  buffer_t* file_hash;
  buffer_t* descriptor_hash;
  size_t final_byte;
  writeable_descriptor_t* desc;
  writeable_off_stream_t* ws;
  new_blocks_recipe_t* recipe;
  volatile int complete;
  uint8_t stored_checksum[32];
} node_put_context_t;

static void node_put_on_stream_data(void* ctx, void* data) {
  node_put_context_t* put_ctx = (node_put_context_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (payload->size == 32 && put_ctx->file_hash == NULL) {
    put_ctx->file_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
  } else {
    tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)payload);
    writeable_descriptor_write(put_ctx->desc, tuple);
    tuple_destroy(tuple);
  }
}

static void node_put_on_stream_close(void* ctx, void* unused) {
  (void)unused;
  node_put_context_t* put_ctx = (node_put_context_t*)ctx;
  writeable_descriptor_close(put_ctx->desc);
}

static void node_put_on_descriptor_data(void* ctx, void* data) {
  node_put_context_t* put_ctx = (node_put_context_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (put_ctx->descriptor_hash != NULL) {
    buffer_destroy(put_ctx->descriptor_hash);
  }
  put_ctx->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
}

static void node_put_on_descriptor_close(void* ctx, void* unused) {
  (void)unused;
  node_put_context_t* put_ctx = (node_put_context_t*)ctx;
  node_state_t* node = put_ctx->node;

  put_ctx->complete = 1;

  /* Order matters: pending list is LIFO. Add recipe first so its destructor
     runs LAST -- after ws's destructor drops its recipe ref. */
  scheduler_pool_defer_cleanup(((stream_t*)put_ctx->ws)->pool, put_ctx->recipe,
                                (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)put_ctx->desc);
  stream_deferred_deref((stream_t*)put_ctx->ws);

  pthread_mutex_lock(&node->state_lock);
  pthread_cond_signal(&node->state_cond);
  pthread_mutex_unlock(&node->state_lock);
}

static void handle_store_file(int client_fd, const char* args) {
  size_t stream_length = 0;
  int block_type_int = 0;
  size_t tuple_size = 0;
  if (sscanf(args, "%zu %d %zu", &stream_length, &block_type_int, &tuple_size) != 3) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid STORE_FILE args");
    return;
  }
  if (stream_length == 0) {
    send_response(client_fd, CTRL_RESP_ERROR " zero stream_length");
    return;
  }

  block_size_e block_type;
  switch (block_type_int) {
    case 0:  block_type = nano; break;
    case 1:  block_type = mini; break;
    case 2:  block_type = standard; break;
    case 3:  block_type = mega; break;
    default:
      send_response(client_fd, CTRL_RESP_ERROR " invalid block_type");
      return;
  }

  /* Generate random data */
  uint8_t* file_data = get_clear_memory(stream_length);
  srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
  for (size_t idx = 0; idx < stream_length; idx++) {
    file_data[idx] = (uint8_t)(rand() & 0xFF);
  }

  /* Compute BLAKE3 checksum of original data */
  uint8_t original_checksum[32];
  blake3_hash_buffer(file_data, stream_length, original_checksum);

  /* Create writeable off-stream pipeline */
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(g_node.pool, g_node.block_cache, block_type);
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  vec_push(&recipes, (block_recipe_t*)recipe);

  writeable_off_stream_t* ws = writeable_off_stream_create(
      g_node.pool, g_node.block_cache, g_node.tuple_cache,
      block_type, tuple_size, 32, recipes, g_node.network);

  writeable_descriptor_t* desc = writeable_descriptor_create(
      g_node.pool, g_node.block_cache, block_type, 32, tuple_size,
      stream_length, g_node.network);

  node_put_context_t* put_ctx = get_clear_memory(sizeof(node_put_context_t));
  put_ctx->node = &g_node;
  put_ctx->desc = desc;
  put_ctx->ws = ws;
  put_ctx->recipe = recipe;
  put_ctx->final_byte = stream_length;
  memcpy(put_ctx->stored_checksum, original_checksum, 32);

  stream_subscribe((stream_t*)ws, data_event, put_ctx,
                   (void (*)(void*, void*))node_put_on_stream_data, NULL);
  stream_subscribe((stream_t*)ws, close_event, put_ctx,
                   (void (*)(void*, void*))node_put_on_stream_close, NULL);
  stream_subscribe((stream_t*)desc, data_event, put_ctx,
                   (void (*)(void*, void*))node_put_on_descriptor_data, NULL);
  stream_once((stream_t*)desc, close_event, put_ctx,
              (void (*)(void*, void*))node_put_on_descriptor_close, NULL);

  /* Write data and finalize */
  buffer_t* data_buf = buffer_create_from_pointer_copy(file_data, stream_length);
  writeable_off_stream_write(ws, data_buf);
  buffer_destroy(data_buf);
  writeable_off_stream_finalize(ws);

  free(file_data);

  /* Wait for completion with timeout */
  struct timespec timeout;
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 10;

  pthread_mutex_lock(&g_node.state_lock);
  while (!put_ctx->complete) {
    int wait_result = pthread_cond_timedwait(&g_node.state_cond, &g_node.state_lock, &timeout);
    if (wait_result == ETIMEDOUT) {
      break;
    }
  }
  pthread_mutex_unlock(&g_node.state_lock);

  if (!put_ctx->complete) {
    send_response(client_fd, CTRL_RESP_ERROR " STORE_FILE timed out");
    /* Close streams to trigger deferred cleanup, then wait for completion */
    if (put_ctx->desc != NULL) {
      writeable_descriptor_close(put_ctx->desc);
    }
    /* Wait for descriptor close callback to signal completion */
    struct timespec cleanup_timeout;
    clock_gettime(CLOCK_REALTIME, &cleanup_timeout);
    cleanup_timeout.tv_sec += 5;
    pthread_mutex_lock(&g_node.state_lock);
    while (!put_ctx->complete) {
      int wait_result = pthread_cond_timedwait(&g_node.state_cond, &g_node.state_lock, &cleanup_timeout);
      if (wait_result == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_node.state_lock);
    if (put_ctx->file_hash) buffer_destroy(put_ctx->file_hash);
    if (put_ctx->descriptor_hash) buffer_destroy(put_ctx->descriptor_hash);
    free(put_ctx);
    return;
  }

  /* Format HASH response */
  char desc_hash_hex[65];
  char file_hash_hex[65];
  char stored_hex[65];

  if (put_ctx->descriptor_hash && put_ctx->descriptor_hash->data) {
    buffer_to_hex(put_ctx->descriptor_hash->data, 32, desc_hash_hex);
  } else {
    memset(desc_hash_hex, '0', 64);
    desc_hash_hex[64] = '\0';
  }

  if (put_ctx->file_hash && put_ctx->file_hash->data) {
    buffer_to_hex(put_ctx->file_hash->data, 32, file_hash_hex);
  } else {
    memset(file_hash_hex, '0', 64);
    file_hash_hex[64] = '\0';
  }

  buffer_to_hex(put_ctx->stored_checksum, 32, stored_hex);

  char response[256];
  snprintf(response, sizeof(response), "%s %s %s %zu %s",
           CTRL_RESP_HASH, desc_hash_hex, file_hash_hex,
           put_ctx->final_byte, stored_hex);
  send_response(client_fd, response);

  /* Clean up put_ctx fields */
  if (put_ctx->file_hash) {
    buffer_destroy(put_ctx->file_hash);
  }
  if (put_ctx->descriptor_hash) {
    buffer_destroy(put_ctx->descriptor_hash);
  }
  free(put_ctx);
}

/* ---- FETCH_FILE handler ---- */

typedef struct {
  node_state_t* node;
  readable_descriptor_t* desc;
  readable_off_stream_t* rs;
  ori_t* ori;
  volatile int complete;
  uint8_t checksum[32];
  size_t total_bytes;
  blake3_hasher hasher;
  int hasher_initialized;
} node_get_pipeline_v2_t;

static void node_get_v2_on_tuple(void* ctx, void* data) {
  node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
  tuple_t* tuple_data = (tuple_t*)data;
  readable_off_stream_write(pipeline->rs, tuple_data);
}

static void node_get_v2_on_rs_data(void* ctx, void* data) {
  node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
  buffer_t* chunk = (buffer_t*)data;
  if (!pipeline->hasher_initialized) {
    blake3_hasher_init(&pipeline->hasher);
    pipeline->hasher_initialized = 1;
  }
  blake3_hasher_update(&pipeline->hasher, chunk->data, chunk->size);
  pipeline->total_bytes += chunk->size;
}

static void node_get_v2_on_desc_close(void* ctx, void* unused) {
  (void)unused;
  node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
  stream_deferred_deref((stream_t*)pipeline->desc);
}

static void node_get_v2_on_desc_error(void* ctx, void* error) {
  (void)error;
  node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
  stream_deactivate((stream_t*)pipeline->rs, NULL);
  // Don't defer-deref desc here — stream_deactivate sends close_event too,
  // and node_get_v2_on_desc_close will handle the deref.
}

static void node_get_v2_on_rs_close(void* ctx, void* unused) {
  (void)unused;
  node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
  node_state_t* node = pipeline->node;

  if (pipeline->hasher_initialized) {
    blake3_hasher_finalize(&pipeline->hasher, pipeline->checksum, 32);
  }

  pipeline->complete = 1;

  stream_deferred_deref((stream_t*)pipeline->rs);
  DESTROY(pipeline->ori, ori);

  pthread_mutex_lock(&node->state_lock);
  pthread_cond_signal(&node->state_cond);
  pthread_mutex_unlock(&node->state_lock);
}

static void handle_fetch_file(int client_fd, const char* args) {
  char desc_hash_hex[65];
  char file_hash_hex[65];
  size_t final_byte = 0;
  int block_type_int = 0;
  size_t tuple_size = 0;

  if (sscanf(args, "%64s %64s %zu %d %zu",
             desc_hash_hex, file_hash_hex, &final_byte,
             &block_type_int, &tuple_size) != 5) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid FETCH_FILE args");
    return;
  }

  uint8_t desc_hash_bytes[32];
  uint8_t file_hash_bytes[32];
  if (hex_to_buffer(desc_hash_hex, desc_hash_bytes, 32) != 0) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid descriptor_hash hex");
    return;
  }
  if (hex_to_buffer(file_hash_hex, file_hash_bytes, 32) != 0) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid file_hash hex");
    return;
  }

  block_size_e block_type;
  switch (block_type_int) {
    case 0:  block_type = nano; break;
    case 1:  block_type = mini; break;
    case 2:  block_type = standard; break;
    case 3:  block_type = mega; break;
    default:
      send_response(client_fd, CTRL_RESP_ERROR " invalid block_type");
      return;
  }

  /* Create ori */
  ori_t* stream_ori = ori_create(final_byte);
  stream_ori->descriptor_hash = buffer_create_from_pointer_copy(desc_hash_bytes, 32);
  stream_ori->file_hash = buffer_create_from_pointer_copy(file_hash_bytes, 32);
  stream_ori->block_type = block_type;
  stream_ori->tuple_size = tuple_size;

  /* Create readable pipeline */
  readable_off_stream_t* rs = readable_off_stream_create(
      g_node.pool, g_node.block_cache, g_node.tuple_cache,
      stream_ori, 32, g_node.network);

  readable_descriptor_t* desc = readable_descriptor_create(
      g_node.pool, g_node.block_cache, stream_ori, 32, g_node.network);

  node_get_pipeline_v2_t* pipeline = get_clear_memory(sizeof(node_get_pipeline_v2_t));
  pipeline->node = &g_node;
  pipeline->desc = desc;
  pipeline->rs = rs;
  pipeline->ori = stream_ori;
  pipeline->hasher_initialized = 0;

  stream_subscribe((stream_t*)desc, data_event, pipeline,
                   (void (*)(void*, void*))node_get_v2_on_tuple, NULL);
  stream_once((stream_t*)desc, close_event, pipeline,
              (void (*)(void*, void*))node_get_v2_on_desc_close, NULL);
  stream_once((stream_t*)desc, error_event, pipeline,
              (void (*)(void*, void*))node_get_v2_on_desc_error, NULL);
  stream_once((stream_t*)rs, close_event, pipeline,
              (void (*)(void*, void*))node_get_v2_on_rs_close, NULL);
  stream_subscribe((stream_t*)rs, data_event, pipeline,
                   (void (*)(void*, void*))node_get_v2_on_rs_data, NULL);

  /* Push descriptor to start the pipeline */
  readable_descriptor_push(desc);

  /* Wait for completion with timeout */
  struct timespec timeout;
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += (CTRL_TRANSFER_TIMEOUT_MS / 1000);

  pthread_mutex_lock(&g_node.state_lock);
  while (!pipeline->complete) {
    int wait_result = pthread_cond_timedwait(&g_node.state_cond, &g_node.state_lock, &timeout);
    if (wait_result == ETIMEDOUT) {
      break;
    }
  }
  int completed = pipeline->complete;
  uint8_t checksum[32];
  size_t total_bytes = pipeline->total_bytes;
  if (completed) {
    memcpy(checksum, pipeline->checksum, 32);
  }
  pthread_mutex_unlock(&g_node.state_lock);

  if (!completed) {
    send_response(client_fd, CTRL_RESP_ERROR " FETCH_FILE timed out");
    /* Deactivate descriptor to trigger pipeline completion and callback cleanup */
    if (pipeline->desc != NULL) {
      stream_deactivate((stream_t*)pipeline->desc, NULL);
    }
    struct timespec cleanup_timeout;
    clock_gettime(CLOCK_REALTIME, &cleanup_timeout);
    cleanup_timeout.tv_sec += 3;
    pthread_mutex_lock(&g_node.state_lock);
    while (!pipeline->complete) {
      if (pthread_cond_timedwait(&g_node.state_cond, &g_node.state_lock, &cleanup_timeout) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_node.state_lock);
    free(pipeline);
    return;
  }

  free(pipeline);

  /* Format DATA response */
  char checksum_hex[65];
  buffer_to_hex(checksum, 32, checksum_hex);

  char response[256];
  snprintf(response, sizeof(response), "%s %s %zu",
           CTRL_RESP_DATA, checksum_hex, total_bytes);
  send_response(client_fd, response);
}

/* ---- Event log and Hebbian query handlers (OFFS_TEST only) ---- */

#ifdef OFFS_TEST
static void handle_get_events(int client_fd, size_t cursor) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  message_event_t events[64];
  size_t count = message_log_query(&g_node.network->log, cursor, events, 64);

  char response[8192];
  int offset = snprintf(response, sizeof(response), "%s %zu|",
                        CTRL_RESP_EVENTS, g_node.network->log.count);

  for (size_t idx = 0; idx < count; idx++) {
    message_event_t* ev = &events[idx];
    char peer_hex[NODE_ID_STRING_SIZE];
    strncpy(peer_hex, ev->peer_id.str, NODE_ID_STRING_SIZE);

    /* Format first 8 bytes of block_hash as hex prefix for correlation */
    char hash_prefix[17];
    for (int byte = 0; byte < 8 && byte < 32; byte++) {
      snprintf(hash_prefix + byte * 2, 3, "%02x", ev->block_hash[byte]);
    }
    hash_prefix[16] = '\0';

    /* Check if peer_id is all-zero (null peer) */
    int peer_is_null = 1;
    for (int idx2 = 0; idx2 < 4; idx2++) {
      if (ev->peer_id.hash[idx2] != 0) {
        peer_is_null = 0;
        break;
      }
    }

    if (peer_is_null) {
      offset += snprintf(response + offset, sizeof(response) - offset,
                         "%zu:%u,%u,0,%llu,%s,%u,%.4f",
                         cursor + idx, ev->type, ev->direction,
                         (unsigned long long)ev->message_id,
                         hash_prefix, ev->result, ev->hebbian_weight);
    } else {
      offset += snprintf(response + offset, sizeof(response) - offset,
                         "%zu:%u,%u,%s,%llu,%s,%u,%.4f",
                         cursor + idx, ev->type, ev->direction,
                         peer_hex, (unsigned long long)ev->message_id,
                         hash_prefix, ev->result, ev->hebbian_weight);
    }

    if (idx + 1 < count) {
      offset += snprintf(response + offset, sizeof(response) - offset, ";");
    }
  }

  send_response(client_fd, response);
}

static void handle_find_block_cmd(int client_fd, const char* hash_hex) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  /* Parse 64-char hex hash */
  uint8_t block_hash[32];
  if (strlen(hash_hex) != 64) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid hash length");
    return;
  }
  for (int idx = 0; idx < 32; idx++) {
    unsigned int byte;
    if (sscanf(hash_hex + idx * 2, "%02x", &byte) != 1) {
      send_response(client_fd, CTRL_RESP_ERROR " invalid hash hex");
      return;
    }
    block_hash[idx] = (uint8_t)byte;
  }

  /* Create a local FindBlock request and dispatch it */
  network_local_find_block_payload_t* payload =
      get_clear_memory(sizeof(network_local_find_block_payload_t));
  if (payload == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " allocation failed");
    return;
  }
  payload->hash = buffer_create_from_pointer_copy(block_hash, 32);
  if (payload->hash == NULL) {
    free(payload);
    send_response(client_fd, CTRL_RESP_ERROR " allocation failed");
    return;
  }
  payload->reply_to = &g_node.network->actor;

  message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = NETWORK_LOCAL_FIND_BLOCK;
  msg.payload = payload;
  msg.payload_destroy = network_local_find_block_payload_destroy;

  actor_send(&g_node.network->actor, &msg);

  char response[128];
  snprintf(response, sizeof(response), "%s find_block_injected",
           CTRL_RESP_OK);
  send_response(client_fd, response);
}

static void handle_ping_peer_cmd(int client_fd, const char* node_id_hex) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  node_id_t peer_id;
  memset(&peer_id, 0, sizeof(peer_id));
  if (node_id_from_string((char*)node_id_hex, &peer_id) != 0) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid node_id");
    return;
  }

  peer_connection_t* peer = connection_manager_lookup(&g_node.network->conn_mgr, &peer_id);
  if (peer == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " peer not found");
    return;
  }

  wire_ping_t ping;
  memset(&ping, 0, sizeof(ping));
  ping.message_id = (uint64_t)(time(NULL)) ^ ((uint64_t)rand() << 32);
  memcpy(&ping.sender_id, &g_node.network->authority->local_id, sizeof(node_id_t));
  ping.timestamp = (uint64_t)(time(NULL) * 1000);

  cbor_item_t* cbor = wire_ping_encode(&ping);
  if (cbor == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " encode failed");
    return;
  }

  int result = conn_state_send(g_node.network, peer, cbor);
  cbor_decref(&cbor);

  if (result == 0) {
    char response[128];
    snprintf(response, sizeof(response), "%s %llu",
             CTRL_RESP_OK, (unsigned long long)ping.message_id);
    send_response(client_fd, response);
  } else {
    send_response(client_fd, CTRL_RESP_ERROR " send failed");
  }
}

static void handle_hebbian_cmd(int client_fd) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  hebbian_table_t* table = &g_node.network->hebbian;
  char response[4096];
  int offset = snprintf(response, sizeof(response), "%s %zu|",
                        CTRL_RESP_HEBBIAN, table->count);

  for (size_t idx = 0; idx < table->count; idx++) {
    char node_hex[NODE_ID_STRING_SIZE];
    strncpy(node_hex, table->entries[idx].peer_id.str, NODE_ID_STRING_SIZE);
    offset += snprintf(response + offset, sizeof(response) - offset,
                      "%s:%.4f", node_hex, table->entries[idx].weight);
    if (idx + 1 < table->count) {
      offset += snprintf(response + offset, sizeof(response) - offset, ";");
    }
  }

  send_response(client_fd, response);
}
#endif // OFFS_TEST

/* ---- Command dispatcher ---- */

static void handle_command(int client_fd, char* line) {
  /* Strip trailing newline/carriage return */
  size_t line_length = strlen(line);
  while (line_length > 0 && (line[line_length - 1] == '\n' || line[line_length - 1] == '\r')) {
    line[--line_length] = '\0';
  }

  if (line_length == 0) {
    return;
  }

  if (strcmp(line, CTRL_STATUS) == 0) {
    handle_status(client_fd);
  } else if (strcmp(line, CTRL_SHUTDOWN) == 0) {
    handle_shutdown(client_fd);
  } else if (strncmp(line, CTRL_PEER_ADD " ", strlen(CTRL_PEER_ADD) + 1) == 0) {
    const char* peer_addr = line + strlen(CTRL_PEER_ADD) + 1;
    /* Parse host:port */
    char host[256];
    uint16_t port = 0;
    char* colon = strchr(peer_addr, ':');
    if (colon) {
      size_t host_len = (size_t)(colon - peer_addr);
      if (host_len >= sizeof(host)) {
        host_len = sizeof(host) - 1;
      }
      memcpy(host, peer_addr, host_len);
      host[host_len] = '\0';
      port = (uint16_t)atoi(colon + 1);
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " invalid PEER_ADD format");
      return;
    }
    if (g_node.network != NULL) {
      int result = network_connect_peer(g_node.network, host, port);
      if (result == 0) {
        send_response(client_fd, CTRL_RESP_OK);
      } else {
        send_response(client_fd, CTRL_RESP_ERROR " peer connect failed");
      }
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " no network");
    }
  } else if (strncmp(line, CTRL_CONNECT_RELAY " ", strlen(CTRL_CONNECT_RELAY) + 1) == 0) {
    const char* relay_addr = line + strlen(CTRL_CONNECT_RELAY) + 1;
    char host[256];
    uint16_t port = 0;
    char* colon = strchr(relay_addr, ':');
    if (colon) {
      size_t host_len = (size_t)(colon - relay_addr);
      if (host_len >= sizeof(host)) {
        host_len = sizeof(host) - 1;
      }
      memcpy(host, relay_addr, host_len);
      host[host_len] = '\0';
      port = (uint16_t)atoi(colon + 1);
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " invalid CONNECT_RELAY format");
      return;
    }
    if (g_node.network) {
      int result = network_connect_relay(g_node.network, host, port);
      if (result == 0) {
        send_response(client_fd, CTRL_RESP_OK);
      } else {
        send_response(client_fd, CTRL_RESP_ERROR " relay connect failed");
      }
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " no network");
    }
  } else if (strncmp(line, CTRL_ADD_PEER " ", strlen(CTRL_ADD_PEER) + 1) == 0) {
    // ADD_PEER <node_id_hex> <relay_endpoint_id>
    // Creates a peer entry in the connection manager without a direct QUIC connection.
    // Used for relay-only testing where peers communicate through the relay server.
    const char* args = line + strlen(CTRL_ADD_PEER) + 1;
    node_id_t peer_id;
    memset(&peer_id, 0, sizeof(peer_id));
    uint32_t relay_endpoint_id = 0;
    char node_id_str[NODE_ID_STRING_SIZE];
    int parsed = sscanf(args, "%47s %u", node_id_str, &relay_endpoint_id);
    if (parsed < 1) {
      send_response(client_fd, CTRL_RESP_ERROR " invalid ADD_PEER format");
      return;
    }
    if (node_id_from_string(node_id_str, &peer_id) != 0) {
      send_response(client_fd, CTRL_RESP_ERROR " invalid node_id");
      return;
    }
    if (g_node.network != NULL) {
      peer_connection_t* existing = connection_manager_lookup(&g_node.network->conn_mgr, &peer_id);
      if (existing != NULL) {
        // Peer already exists — update relay endpoint if provided
        if (parsed >= 2 && relay_endpoint_id != 0) {
          existing->relay_endpoint_id = relay_endpoint_id;
          conn_state_set_peer_nat_type(existing, NAT_TYPE_SYMMETRIC);
        }
        send_response(client_fd, CTRL_RESP_OK);
      } else {
        peer_connection_t* peer = connection_manager_add(&g_node.network->conn_mgr, &peer_id, NULL, g_node.pool);
        if (peer != NULL) {
          if (parsed >= 2 && relay_endpoint_id != 0) {
            peer->relay_endpoint_id = relay_endpoint_id;
            conn_state_set_peer_nat_type(peer, NAT_TYPE_SYMMETRIC);
          }
          // Ensure the peer is in the ring set for routing
          net_node_t* ring_node = net_node_create(&peer_id, 0, 0);
          if (ring_node != NULL) {
            ring_node->weight = FIND_BLOCK_MIN_WEIGHT;
            ring_set_insert(g_node.network->rings, ring_node, 0);
          }
          send_response(client_fd, CTRL_RESP_OK);
        } else {
          send_response(client_fd, CTRL_RESP_ERROR " failed to add peer");
        }
      }
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " no network");
    }
  } else if (strncmp(line, CTRL_WAIT_FOR_PEER " ", strlen(CTRL_WAIT_FOR_PEER) + 1) == 0) {
    size_t target_count = (size_t)atol(line + strlen(CTRL_WAIT_FOR_PEER) + 1);
    int found = 0;
    for (int attempt = 0; attempt < (int)(CTRL_HANDSHAKE_TIMEOUT_MS / CTRL_POLL_INTERVAL_MS); attempt++) {
      if (get_peer_count() >= target_count) {
        found = 1;
        break;
      }
      usleep(CTRL_POLL_INTERVAL_MS * 1000);
    }
    if (found) {
      send_response(client_fd, CTRL_RESP_OK);
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " WAIT_FOR_PEER timed out");
    }
  } else if (strncmp(line, CTRL_STORE_FILE " ", strlen(CTRL_STORE_FILE) + 1) == 0) {
    const char* store_args = line + strlen(CTRL_STORE_FILE) + 1;
    handle_store_file(client_fd, store_args);
  } else if (strncmp(line, CTRL_FETCH_FILE " ", strlen(CTRL_FETCH_FILE) + 1) == 0) {
    const char* fetch_args = line + strlen(CTRL_FETCH_FILE) + 1;
    handle_fetch_file(client_fd, fetch_args);
  } else if (strncmp(line, CTRL_FORCE_RELAY_ENDPOINT " ",
                strlen(CTRL_FORCE_RELAY_ENDPOINT) + 1) == 0) {
    uint32_t endpoint_id = (uint32_t)atol(line + strlen(CTRL_FORCE_RELAY_ENDPOINT) + 1);
    if (g_node.network != NULL) {
      connection_manager_t* mgr = &g_node.network->conn_mgr;
      for (size_t idx = 0; idx < mgr->peer_count; idx++) {
        peer_connection_t* peer = mgr->peers[idx];
        if (peer != NULL && peer->connected) {
          peer->relay_endpoint_id = endpoint_id;
          conn_state_set_peer_nat_type(peer, NAT_TYPE_SYMMETRIC);
        }
      }
      send_response(client_fd, CTRL_RESP_OK);
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " no network");
    }
  } else if (strcmp(line, CTRL_GET_EVENTS) == 0) {
#ifdef OFFS_TEST
    handle_get_events(client_fd, 0);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
  } else if (strncmp(line, CTRL_GET_EVENTS " ",
                strlen(CTRL_GET_EVENTS) + 1) == 0) {
#ifdef OFFS_TEST
    size_t cursor = (size_t)atol(line + strlen(CTRL_GET_EVENTS) + 1);
    handle_get_events(client_fd, cursor);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
  } else if (strcmp(line, CTRL_CLEAR_EVENTS) == 0) {
#ifdef OFFS_TEST
    if (g_node.network != NULL) {
      message_log_clear(&g_node.network->log);
      send_response(client_fd, CTRL_RESP_OK);
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " no network");
    }
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
  } else if (strncmp(line, CTRL_FIND_BLOCK " ",
                strlen(CTRL_FIND_BLOCK) + 1) == 0) {
#ifdef OFFS_TEST
    handle_find_block_cmd(client_fd, line + strlen(CTRL_FIND_BLOCK) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
  } else if (strncmp(line, CTRL_PING_PEER " ",
                strlen(CTRL_PING_PEER) + 1) == 0) {
#ifdef OFFS_TEST
    handle_ping_peer_cmd(client_fd, line + strlen(CTRL_PING_PEER) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
  } else if (strcmp(line, CTRL_HEBBIAN) == 0) {
#ifdef OFFS_TEST
    handle_hebbian_cmd(client_fd);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
  } else {
    send_response(client_fd, CTRL_RESP_ERROR " unknown command");
  }
}

/* ---- Control socket thread ---- */

static void* control_socket_thread(void* arg) {
  (void)arg;
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    fprintf(stderr, "node: control socket create failed: %s\n", strerror(errno));
    return NULL;
  }

  int opt_val = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(g_node.control_port);

  if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "node: control socket bind failed: %s\n", strerror(errno));
    close(server_fd);
    return NULL;
  }

  if (listen(server_fd, 5) < 0) {
    fprintf(stderr, "node: control socket listen failed: %s\n", strerror(errno));
    close(server_fd);
    return NULL;
  }

  g_node.control_fd = server_fd;

  /* Print actual bound port if 0 was requested */
  if (g_node.control_port == 0) {
    socklen_t addr_len = sizeof(addr);
    getsockname(server_fd, (struct sockaddr*)&addr, &addr_len);
    g_node.control_port = ntohs(addr.sin_port);
  }
  printf("CONTROL_PORT %u\n", g_node.control_port);
  fflush(stdout);

  while (g_node.running) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(server_fd, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = CTRL_POLL_INTERVAL_MS * 1000;

    int select_result = select(server_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (select_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "node: control socket select failed: %s\n", strerror(errno));
      break;
    }
    if (select_result == 0) {
      continue;
    }

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "node: control socket accept failed: %s\n", strerror(errno));
      continue;
    }

    /* Read lines from client connection */
    char line_buf[4096];
    ssize_t bytes_read;
    size_t line_offset = 0;

    while (g_node.running && (bytes_read = recv(client_fd, line_buf + line_offset,
                                                 sizeof(line_buf) - line_offset - 1, 0)) > 0) {
      line_offset += (size_t)bytes_read;
      line_buf[line_offset] = '\0';

      /* Process complete lines */
      char* newline;
      while ((newline = strchr(line_buf, '\n')) != NULL) {
        *newline = '\0';
        handle_command(client_fd, line_buf);
        size_t remaining = (size_t)((newline + 1) - line_buf);
        size_t left = line_offset - remaining;
        if (left > 0) {
          memmove(line_buf, newline + 1, left);
        }
        line_offset = left;
        line_buf[line_offset] = '\0';
      }

      if (line_offset >= sizeof(line_buf) - 1) {
        /* Line too long, reset */
        line_offset = 0;
      }
    }

    close(client_fd);
  }

  close(server_fd);
  g_node.control_fd = -1;
  return NULL;
}

/* ---- Argument parsing ---- */

static void parse_args(int argc, char* argv[]) {
  for (int idx = 1; idx < argc; idx++) {
    if (strcmp(argv[idx], "--port") == 0 && idx + 1 < argc) {
      g_node.port = (uint16_t)atoi(argv[++idx]);
    } else if (strcmp(argv[idx], "--control-port") == 0 && idx + 1 < argc) {
      g_node.control_port = (uint16_t)atoi(argv[++idx]);
    } else if (strcmp(argv[idx], "--relay-host") == 0 && idx + 1 < argc) {
      g_node.relay_host = strdup(argv[++idx]);
    } else if (strcmp(argv[idx], "--relay-port") == 0 && idx + 1 < argc) {
      g_node.relay_port = (uint16_t)atoi(argv[++idx]);
    } else if (strcmp(argv[idx], "--cache-dir") == 0 && idx + 1 < argc) {
      g_node.cache_dir = strdup(argv[++idx]);
    } else if (strcmp(argv[idx], "--cert") == 0 && idx + 1 < argc) {
      g_node.cert_path = strdup(argv[++idx]);
    } else if (strcmp(argv[idx], "--key") == 0 && idx + 1 < argc) {
      g_node.key_path = strdup(argv[++idx]);
    }
  }
}

/* ---- Main entry point ---- */

int node_main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);

  node_state_init(&g_node);
  parse_args(argc, argv);

  /* Default cache dir */
  if (!g_node.cache_dir) {
    g_node.cache_dir = strdup("/tmp/liboffs_test_bc");
  }

  /* Initialization sequence */
  g_node.pool = scheduler_pool_create(2);
  if (!g_node.pool) {
    fprintf(stderr, "node: scheduler_pool_create failed\n");
    node_state_destroy(&g_node);
    return 1;
  }
  scheduler_pool_start(g_node.pool);

  g_node.config = config_default();
  g_node.authority = authority_create(&g_node.config);
  if (!g_node.authority) {
    fprintf(stderr, "node: authority_create failed\n");
    scheduler_pool_stop(g_node.pool);
    scheduler_pool_destroy(g_node.pool);
    node_state_destroy(&g_node);
    return 1;
  }
  if (g_node.cert_path && g_node.key_path) {
    g_node.authority->node_cert_path = strdup(g_node.cert_path);
    g_node.authority->node_key_path = strdup(g_node.key_path);
  }

  g_node.timer = timer_actor_create();
  if (!g_node.timer) {
    fprintf(stderr, "node: timer_actor_create failed\n");
    authority_destroy(g_node.authority);
    scheduler_pool_stop(g_node.pool);
    scheduler_pool_destroy(g_node.pool);
    node_state_destroy(&g_node);
    return 1;
  }

  g_node.block_cache = block_cache_create(g_node.config, g_node.cache_dir, standard, g_node.timer, g_node.pool);
  if (!g_node.block_cache) {
    fprintf(stderr, "node: block_cache_create failed\n");
    timer_actor_destroy(g_node.timer);
    authority_destroy(g_node.authority);
    scheduler_pool_stop(g_node.pool);
    scheduler_pool_destroy(g_node.pool);
    node_state_destroy(&g_node);
    return 1;
  }

  g_node.tuple_cache = tuple_cache_create(100, g_node.pool);
  if (!g_node.tuple_cache) {
    fprintf(stderr, "node: tuple_cache_create failed\n");
    block_cache_destroy(g_node.block_cache);
    timer_actor_destroy(g_node.timer);
    authority_destroy(g_node.authority);
    scheduler_pool_stop(g_node.pool);
    scheduler_pool_destroy(g_node.pool);
    node_state_destroy(&g_node);
    return 1;
  }

  g_node.network = network_create(g_node.authority, g_node.block_cache, g_node.timer, g_node.pool);
  if (!g_node.network) {
    fprintf(stderr, "node: network_create failed\n");
    tuple_cache_destroy(g_node.tuple_cache);
    block_cache_destroy(g_node.block_cache);
    timer_actor_destroy(g_node.timer);
    authority_destroy(g_node.authority);
    scheduler_pool_stop(g_node.pool);
    scheduler_pool_destroy(g_node.pool);
    node_state_destroy(&g_node);
    return 1;
  }

#ifdef HAS_MSQUIC
  if (g_node.port > 0) {
    g_node.listener = quic_listener_create(g_node.network, g_node.pool);
    if (g_node.listener) {
      int start_result = quic_listener_start(g_node.listener, "0.0.0.0", g_node.port);
      if (start_result != 0) {
        fprintf(stderr, "node: quic_listener_start failed\n");
      }
      g_node.network->quic_listener = g_node.listener;
    }
  }
#endif

  /* Connect to relay if specified */
  if (g_node.relay_host && g_node.relay_port > 0) {
    network_connect_relay(g_node.network, g_node.relay_host, g_node.relay_port);
  }

  /* Start control socket thread */
  pthread_t ctrl_thread;
  pthread_create(&ctrl_thread, NULL, control_socket_thread, NULL);

  /* Main loop: wait for shutdown */
  while (g_node.running) {
    usleep(CTRL_POLL_INTERVAL_MS * 1000);
  }

  /* Wait for control thread to finish */
  pthread_join(ctrl_thread, NULL);

  /* Cleanup in reverse order */
#ifdef HAS_MSQUIC
  if (g_node.listener) {
    quic_listener_stop(g_node.listener);
    quic_listener_destroy(g_node.listener);
  }
#endif

  network_destroy(g_node.network);
  tuple_cache_destroy(g_node.tuple_cache);
  block_cache_destroy(g_node.block_cache);
  timer_actor_destroy(g_node.timer);
  authority_destroy(g_node.authority);
  scheduler_pool_stop(g_node.pool);
  scheduler_pool_destroy(g_node.pool);
  node_state_destroy(&g_node);

  return 0;
}