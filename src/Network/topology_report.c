/*
 * topology_report.c — CBOR wire protocol for topology metrics reports
 */

#include "topology_report.h"
#include "../Util/log.h"
#include "../Platform/platform_posix_compat.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#endif

static cbor_item_t* _node_id_to_cbor(const node_id_t* node_id) {
  return cbor_build_bytestring(node_id->hash, NODE_ID_HASH_SIZE);
}

static int _cbor_to_node_id(cbor_item_t* item, node_id_t* out) {
  if (!cbor_isa_bytestring(item)) return -1;
  size_t byte_len = cbor_bytestring_length(item);
  if (byte_len < NODE_ID_HASH_SIZE) return -1;
  const unsigned char* data = cbor_bytestring_handle(item);
  if (data == NULL) return -1;
  memcpy(out->hash, data, NODE_ID_HASH_SIZE);
  snprintf(out->str, NODE_ID_STRING_SIZE, "%02x%02x%02x%02x...",
           data[0], data[1], data[2], data[3]);
  return 0;
}

cbor_item_t* topology_report_encode(const node_id_t* reporter_id,
                                    uint64_t timestamp_ms,
                                    const topology_metrics_t* metrics) {
  cbor_item_t* root = cbor_new_definite_map(11);

  /* reporter node_id */
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("node_id")),
    .value = cbor_move(_node_id_to_cbor(reporter_id))
  });

  /* timestamp */
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("timestamp_ms")),
    .value = cbor_move(cbor_build_uint64(timestamp_ms))
  });

  /* total_connections */
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("total_connections")),
    .value = cbor_move(cbor_build_uint64(metrics->total_connections))
  });

  /* avg_hebbian_weight */
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("avg_hebbian_weight")),
    .value = cbor_move(cbor_build_float8(metrics->avg_hebbian_weight))
  });

  /* total_rpc_calls array [PEER_RPC_TYPE_COUNT] */
  cbor_item_t* rpc_calls = cbor_new_definite_array(PEER_RPC_TYPE_COUNT);
  for (int index = 0; index < PEER_RPC_TYPE_COUNT; index++) {
    cbor_array_push(rpc_calls,
                    cbor_move(cbor_build_uint64(metrics->total_rpc_calls[index])));
  }
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("total_rpc_calls")),
    .value = cbor_move(rpc_calls)
  });

  /* rate limit aggregates */
  cbor_item_t* rl_accepted = cbor_new_definite_array(RPC_TYPE_COUNT);
  cbor_item_t* rl_rejected = cbor_new_definite_array(RPC_TYPE_COUNT);
  cbor_item_t* rl_tokens = cbor_new_definite_array(RPC_TYPE_COUNT);
  cbor_item_t* rl_rate = cbor_new_definite_array(RPC_TYPE_COUNT);
  for (int index = 0; index < RPC_TYPE_COUNT; index++) {
    cbor_array_push(rl_accepted, cbor_move(cbor_build_uint64(metrics->total_rate_limit_accepted[index])));
    cbor_array_push(rl_rejected, cbor_move(cbor_build_uint64(metrics->total_rate_limit_rejected[index])));
    cbor_array_push(rl_tokens, cbor_move(cbor_build_float8(metrics->avg_rate_limit_tokens[index])));
    cbor_array_push(rl_rate, cbor_move(cbor_build_float8(metrics->effective_rate[index])));
  }
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("rl_accepted")), .value = cbor_move(rl_accepted)});
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("rl_rejected")), .value = cbor_move(rl_rejected)});
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("rl_tokens")), .value = cbor_move(rl_tokens)});
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("rl_rate")), .value = cbor_move(rl_rate)});

  /* peers array */
  cbor_item_t* peers = cbor_new_definite_array(metrics->peer_snapshot_count);
  for (size_t peer_index = 0; peer_index < metrics->peer_snapshot_count; peer_index++) {
    const peer_metrics_snapshot_t* snap = &metrics->peer_snapshots[peer_index];
    cbor_item_t* peer_map = cbor_new_definite_map(11);

    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("node_id")),
      .value = cbor_move(_node_id_to_cbor(&snap->node_id))});
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("hebbian_weight")),
      .value = cbor_move(cbor_build_float8(snap->hebbian_weight))});
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("rtt_ewma_ms")),
      .value = cbor_move(cbor_build_float8((float)snap->rtt_ewma_ms))});
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("connected")),
      .value = cbor_move(cbor_build_bool(snap->connected))});
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("connected_at_ms")),
      .value = cbor_move(cbor_build_uint64((uint64_t)snap->connected_at_ms))});

    /* Per-peer RPC counts (first RPC_TYPE_COUNT entries) */
    cbor_item_t* peer_rpc = cbor_new_definite_array(RPC_TYPE_COUNT);
    for (int rpc_index = 0; rpc_index < RPC_TYPE_COUNT; rpc_index++) {
      cbor_array_push(peer_rpc, cbor_move(cbor_build_uint64(snap->rpc_count[rpc_index])));
    }
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("rpc_count")), .value = cbor_move(peer_rpc)});

    /* Per-peer rate limits */
    cbor_item_t* prl_tokens = cbor_new_definite_array(RPC_TYPE_COUNT);
    cbor_item_t* prl_accepted = cbor_new_definite_array(RPC_TYPE_COUNT);
    cbor_item_t* prl_rejected = cbor_new_definite_array(RPC_TYPE_COUNT);
    cbor_item_t* prl_rate = cbor_new_definite_array(RPC_TYPE_COUNT);
    for (int rl_index = 0; rl_index < RPC_TYPE_COUNT; rl_index++) {
      cbor_array_push(prl_tokens, cbor_move(cbor_build_float8(snap->rate_limit_tokens[rl_index])));
      cbor_array_push(prl_accepted, cbor_move(cbor_build_uint64(snap->rate_limit_accepted[rl_index])));
      cbor_array_push(prl_rejected, cbor_move(cbor_build_uint64(snap->rate_limit_rejected[rl_index])));
      cbor_array_push(prl_rate, cbor_move(cbor_build_float8(snap->rate_limit_effective_rate[rl_index])));
    }
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("rl_tokens")), .value = cbor_move(prl_tokens)});
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("rl_accepted")), .value = cbor_move(prl_accepted)});
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("rl_rejected")), .value = cbor_move(prl_rejected)});
    cbor_map_add(peer_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("rl_rate")), .value = cbor_move(prl_rate)});

    cbor_array_push(peers, cbor_move(peer_map));
  }
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("peers")), .value = cbor_move(peers)});

  /* rings array */
  cbor_item_t* rings = cbor_new_definite_array(metrics->ring_entry_count);
  for (size_t ring_index = 0; ring_index < metrics->ring_entry_count; ring_index++) {
    const ring_topology_entry_t* entry = &metrics->ring_entries[ring_index];
    cbor_item_t* ring_map = cbor_new_definite_map(5);

    cbor_map_add(ring_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("node_id")),
      .value = cbor_move(_node_id_to_cbor(&entry->node_id))});
    cbor_map_add(ring_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("ring_level")),
      .value = cbor_move(cbor_build_uint32(entry->ring_level))});
    cbor_map_add(ring_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("rtt_ms")),
      .value = cbor_move(cbor_build_float8((float)entry->rtt_ms))});
    cbor_map_add(ring_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("capacity")),
      .value = cbor_move(cbor_build_float8(entry->capacity))});
    cbor_map_add(ring_map, (struct cbor_pair){
      .key = cbor_move(cbor_build_string("is_active")),
      .value = cbor_move(cbor_build_bool(entry->is_active_connection))});

    cbor_array_push(rings, cbor_move(ring_map));
  }
  cbor_map_add(root, (struct cbor_pair){
    .key = cbor_move(cbor_build_string("rings")), .value = cbor_move(rings)});

  return root;
}

int topology_report_decode(cbor_item_t* item,
                           node_id_t* reporter_id_out,
                           uint64_t* timestamp_ms_out,
                           topology_metrics_t* metrics_out) {
  if (!cbor_isa_map(item)) return -1;
  memset(metrics_out, 0, sizeof(*metrics_out));

  size_t map_size = cbor_map_size(item);
  struct cbor_pair* pairs = cbor_map_handle(item);

  for (size_t index = 0; index < map_size; index++) {
    const char* key = (const char*)cbor_string_handle(pairs[index].key);
    size_t key_len = cbor_string_length(pairs[index].key);

    if (key_len == 7 && strncmp(key, "node_id", 7) == 0) {
      _cbor_to_node_id(pairs[index].value, reporter_id_out);
    } else if (key_len == 12 && strncmp(key, "timestamp_ms", 12) == 0) {
      *timestamp_ms_out = cbor_get_uint64(pairs[index].value);
    } else if (key_len == 17 && strncmp(key, "total_connections", 17) == 0) {
      metrics_out->total_connections = (size_t)cbor_get_uint64(pairs[index].value);
    } else if (key_len == 18 && strncmp(key, "avg_hebbian_weight", 18) == 0) {
      metrics_out->avg_hebbian_weight = (float)cbor_float_get_float8(pairs[index].value);
    } else if (key_len == 5 && strncmp(key, "peers", 5) == 0) {
      cbor_item_t* peers = pairs[index].value;
      if (cbor_isa_array(peers)) {
        size_t peer_count = cbor_array_size(peers);
        metrics_out->peer_snapshots = get_clear_memory(
          peer_count * sizeof(peer_metrics_snapshot_t));
        if (metrics_out->peer_snapshots != NULL) {
          metrics_out->peer_snapshot_count = peer_count;
          metrics_out->peer_snapshot_capacity = peer_count;
          for (size_t peer_index = 0; peer_index < peer_count; peer_index++) {
            cbor_item_t* peer_item = cbor_array_get(peers, peer_index);
            if (!cbor_isa_map(peer_item)) continue;
            peer_metrics_snapshot_t* snap = &metrics_out->peer_snapshots[peer_index];
            size_t pmap_size = cbor_map_size(peer_item);
            struct cbor_pair* ppairs = cbor_map_handle(peer_item);
            for (size_t pidx = 0; pidx < pmap_size; pidx++) {
              const char* pkey = (const char*)cbor_string_handle(ppairs[pidx].key);
              size_t pklen = cbor_string_length(ppairs[pidx].key);
              if (pklen == 7 && strncmp(pkey, "node_id", 7) == 0) {
                _cbor_to_node_id(ppairs[pidx].value, &snap->node_id);
              } else if (pklen == 14 && strncmp(pkey, "hebbian_weight", 14) == 0) {
                snap->hebbian_weight = (float)cbor_float_get_float8(ppairs[pidx].value);
              } else if (pklen == 10 && strncmp(pkey, "rtt_ewma_ms", 10) == 0) {
                snap->rtt_ewma_ms = cbor_float_get_float8(ppairs[pidx].value);
              } else if (pklen == 9 && strncmp(pkey, "connected", 9) == 0) {
                snap->connected = cbor_get_bool(ppairs[pidx].value);
              } else if (pklen == 14 && strncmp(pkey, "connected_at_ms", 14) == 0) {
                snap->connected_at_ms = (int64_t)cbor_get_uint64(ppairs[pidx].value);
              }
            }
          }
        }
      }
    }
  }
  return 0;
}

/* Parse a URL like "http://host:port/path" into host, port, path components.
 * Returns 0 on success, -1 on parse failure. */
static int _parse_url(const char* url, char* host, size_t host_size,
                       uint16_t* port, char* path, size_t path_size) {
  if (url == NULL) return -1;
  const char* start = url;
  if (strncmp(start, "http://", 7) == 0) {
    start += 7;
    *port = 80;
  } else {
    return -1; /* only http supported */
  }
  const char* colon = strchr(start, ':');
  const char* slash = strchr(start, '/');
  if (colon != NULL && slash != NULL && colon < slash) {
    size_t host_len = (size_t)(colon - start);
    if (host_len >= host_size) host_len = host_size - 1;
    memcpy(host, start, host_len);
    host[host_len] = '\0';
    *port = (uint16_t)atoi(colon + 1);
  } else if (slash != NULL) {
    size_t host_len = (size_t)(slash - start);
    if (host_len >= host_size) host_len = host_size - 1;
    memcpy(host, start, host_len);
    host[host_len] = '\0';
  } else {
    strncpy(host, start, host_size - 1);
    host[host_size - 1] = '\0';
  }
  if (slash != NULL) {
    strncpy(path, slash, path_size - 1);
    path[path_size - 1] = '\0';
  } else {
    strncpy(path, "/", path_size - 1);
    path[path_size - 1] = '\0';
  }
  return 0;
}

int topology_report_post(const char* url, cbor_item_t* report) {
  if (url == NULL || report == NULL) return -1;

  char host[256], path[512];
  uint16_t port = 80;
  if (_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
    log_error("topology_report_post: failed to parse URL %s", url);
    return -1;
  }

  /* Serialize CBOR to buffer */
  size_t cbor_size = cbor_serialized_size(report);
  if (cbor_size == 0) {
    log_error("topology_report_post: CBOR serialized size is 0");
    return -1;
  }
  unsigned char* cbor_buf = malloc(cbor_size);
  if (cbor_buf == NULL) {
    log_error("topology_report_post: malloc failed for %zu bytes", cbor_size);
    return -1;
  }
  cbor_serialize(report, cbor_buf, cbor_size);

  /* Resolve hostname */
  struct hostent* server_host = gethostbyname(host);
  if (server_host == NULL) {
    log_error("topology_report_post: gethostbyname failed for %s", host);
    free(cbor_buf);
    return -1;
  }

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    log_error("topology_report_post: socket creation failed");
    free(cbor_buf);
    return -1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  memcpy(&server_addr.sin_addr.s_addr, server_host->h_addr, (size_t)server_host->h_length);
  server_addr.sin_port = htons(port);

  if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    log_error("topology_report_post: connect to %s:%u failed", host, port);
    CLOSE_SOCKET(sock);
    free(cbor_buf);
    return -1;
  }

  /* Build HTTP/1.1 POST request */
  char request[2048];
  int req_len = snprintf(request, sizeof(request),
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/cbor\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "\r\n",
    path, host, cbor_size);

  if (send(sock, request, (size_t)req_len, 0) != req_len) {
    log_error("topology_report_post: send headers failed");
    CLOSE_SOCKET(sock);
    free(cbor_buf);
    return -1;
  }

  /* Send body */
  size_t sent = 0;
  while (sent < cbor_size) {
    ssize_t n = send(sock, (const char*)cbor_buf + sent, cbor_size - sent, 0);
    if (n <= 0) {
      log_error("topology_report_post: send body failed at %zu/%zu", sent, cbor_size);
      CLOSE_SOCKET(sock);
      free(cbor_buf);
      return -1;
    }
    sent += (size_t)n;
  }

  /* Read response status line */
  char response_buf[256];
  ssize_t received = recv(sock, response_buf, sizeof(response_buf) - 1, 0);
  CLOSE_SOCKET(sock);
  free(cbor_buf);

  if (received <= 0) {
    log_error("topology_report_post: no response from %s:%u", host, port);
    return -1;
  }
  response_buf[received] = '\0';

  /* Check for HTTP 200 */
  if (strncmp(response_buf, "HTTP/1.1 200", 11) == 0 ||
      strncmp(response_buf, "HTTP/1.0 200", 11) == 0) {
    log_info("topology_report_post: report accepted by %s:%u", host, port);
    return 0;
  }

  log_error("topology_report_post: unexpected response from %s:%u: %s",
            host, port, response_buf);
  return -1;
}
