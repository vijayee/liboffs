# Health Check Endpoint + Client Library Block API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a transport-agnostic health check endpoint across all 5 Client API transports, plus add the missing block cache operations and health check to the C client library.

**Architecture:** A shared `health_handler` module collects system health data from nullable data-source pointers and serializes to JSON. Each transport wires it in its own way: HTTP as middleware, TCP/Unix/WS/WT as a new wire protocol message type. The C client library gains `offs_client_block_put/get/delete` and `offs_client_health` functions.

**Tech Stack:** C (C11), CBOR (libcbor), POSIX regex, http-parser, poll-dancer, MsQuic

---

### Task 1: Health handler shared module

**Files:**
- Create: `src/ClientAPI/health_handler.h`
- Create: `src/ClientAPI/health_handler.c`

- [ ] **Step 1: Create health_handler.h**

Write `src/ClientAPI/health_handler.h`:

```c
#ifndef OFFS_HEALTH_HANDLER_H
#define OFFS_HEALTH_HANDLER_H

#include <stdint.h>
#include <stddef.h>
#include "../Network/topology_metrics.h"
#include "../Network/node_id.h"
#include "../BlockCache/block_cache.h"

typedef struct health_context_t {
  topology_metrics_t* topology_metrics;
  block_cache_t*      block_cache;
  node_id_t*          node_id;
  uint64_t*           start_time_ms;
  uint8_t*            running;
  uint8_t*            draining;
} health_context_t;

typedef struct health_data_t {
  const char* status;
  uint64_t    uptime_seconds;
  char        node_id_str[64];
  size_t      peer_count;
  size_t      total_connections;
  float       avg_hebbian_weight;
  size_t      block_cache_current_bytes;
  size_t      block_cache_max_bytes;
  size_t      block_cache_block_count;
  uint64_t    rate_limit_accepted[5];
  uint64_t    rate_limit_rejected[5];
  float       avg_rate_limit_tokens[5];
  float       effective_rate[5];
  uint64_t    total_rpc_calls[20];
} health_data_t;

health_data_t health_data_collect(const health_context_t* ctx);
size_t health_data_to_json(const health_data_t* data, char* buf, size_t buf_size);

#endif
```

- [ ] **Step 2: Create health_handler.c with string tables and collect function**

Write `src/ClientAPI/health_handler.c`:

```c
#include "health_handler.h"
#include "../Network/rate_limit.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

static const char* _rate_limit_type_names[] = {
  "find_block",
  "store_block",
  "seeking_blocks",
  "ping_capacity",
  "ping",
};

static const char* _rpc_type_names[] = {
  NULL,
  "ping",
  "ping_response",
  "ping_capacity",
  "ping_capacity_response",
  "ping_block",
  "ping_block_response",
  "find_block",
  "find_block_response",
  "find_node",
  "find_node_response",
  "store_block",
  "store_block_response",
  "seeking_blocks",
  "seeking_blocks_response",
  "rank_block",
  "recall_block",
  "recall_accept",
  "recall_decline",
  "rate_limited",
  "salutation",
};

health_data_t health_data_collect(const health_context_t* ctx) {
  health_data_t data;
  memset(&data, 0, sizeof(data));

  if (ctx == NULL) {
    data.status = "unknown";
    return data;
  }

  if (ctx->running != NULL && ctx->draining != NULL) {
    data.status = *ctx->draining ? "draining" : (*ctx->running ? "running" : "stopped");
  } else if (ctx->running != NULL) {
    data.status = *ctx->running ? "running" : "stopped";
  } else {
    data.status = "unknown";
  }

  if (ctx->start_time_ms != NULL) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
    data.uptime_seconds = (now_ms - *ctx->start_time_ms) / 1000ULL;
  }

  if (ctx->node_id != NULL) {
    node_id_to_string(ctx->node_id, data.node_id_str, sizeof(data.node_id_str));
  }

  if (ctx->topology_metrics != NULL) {
    topology_metrics_t* tm = ctx->topology_metrics;
    data.peer_count = tm->peer_snapshot_count;
    data.total_connections = tm->total_connections;
    data.avg_hebbian_weight = tm->avg_hebbian_weight;
    memcpy(data.rate_limit_accepted, tm->total_rate_limit_accepted, sizeof(data.rate_limit_accepted));
    memcpy(data.rate_limit_rejected, tm->total_rate_limit_rejected, sizeof(data.rate_limit_rejected));
    memcpy(data.avg_rate_limit_tokens, tm->avg_rate_limit_tokens, sizeof(data.avg_rate_limit_tokens));
    memcpy(data.effective_rate, tm->effective_rate, sizeof(data.effective_rate));
    memcpy(data.total_rpc_calls, tm->total_rpc_calls, sizeof(data.total_rpc_calls));
  }

  if (ctx->block_cache != NULL) {
    data.block_cache_current_bytes = ctx->block_cache->current_bytes;
    data.block_cache_max_bytes = ctx->block_cache->max_capacity_bytes;
    data.block_cache_block_count = block_cache_count(ctx->block_cache);
  }

  return data;
}

size_t health_data_to_json(const health_data_t* data, char* buf, size_t buf_size) {
  size_t offset = 0;

  #define APPEND(...) do { \
    int _w = snprintf(buf + offset, buf_size - offset, __VA_ARGS__); \
    if (_w < 0 || (size_t)_w >= buf_size - offset) return offset; \
    offset += (size_t)_w; \
  } while(0)

  APPEND("{");

  APPEND("\"status\":\"%s\"", data->status);
  APPEND(",\"uptime_seconds\":%llu", (unsigned long long)data->uptime_seconds);

  if (data->node_id_str[0] != '\0') {
    APPEND(",\"node_id\":\"%s\"", data->node_id_str);
  }

  APPEND(",\"peer_count\":%zu", data->peer_count);
  APPEND(",\"total_connections\":%zu", data->total_connections);
  APPEND(",\"avg_hebbian_weight\":%.4f", data->avg_hebbian_weight);

  APPEND(",\"block_cache\":{\"current_bytes\":%zu,\"max_bytes\":%zu,\"block_count\":%zu}",
         data->block_cache_current_bytes, data->block_cache_max_bytes, data->block_cache_block_count);

  APPEND(",\"rate_limits\":[");
  for (int i = 0; i < 5; i++) {
    if (i > 0) APPEND(",");
    APPEND("{\"type\":\"%s\",\"accepted\":%llu,\"rejected\":%llu,\"avg_tokens\":%.4f,\"effective_rate\":%.4f}",
           _rate_limit_type_names[i],
           (unsigned long long)data->rate_limit_accepted[i],
           (unsigned long long)data->rate_limit_rejected[i],
           data->avg_rate_limit_tokens[i],
           data->effective_rate[i]);
  }
  APPEND("]");

  APPEND(",\"rpc_calls\":[");
  uint8_t first = 1;
  for (int i = 1; i <= 20; i++) {
    if (data->total_rpc_calls[i - 1] == 0) continue;
    if (!first) APPEND(",");
    first = 0;
    APPEND("{\"name\":\"%s\",\"count\":%llu}",
           _rpc_type_names[i],
           (unsigned long long)data->total_rpc_calls[i - 1]);
  }
  APPEND("]");

  APPEND("}");

  #undef APPEND
  return offset;
}
```

Check: `node_id_to_string` function exists — let me verify before writing. If it doesn't exist, we'll add a simple formatter.

- [ ] **Step 3: Build and fix compilation errors**

Run: `cd build && make -j$(nproc) 2>&1 | head -30`
Expected: compiles successfully, or fix any missing includes/declarations.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/health_handler.h src/ClientAPI/health_handler.c
git commit -m "feat: add shared health handler module"
```

---

### Task 2: Wire protocol health messages

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h`
- Modify: `src/ClientAPI/client_api_wire.c`

- [ ] **Step 1: Add message type defines and struct to client_api_wire.h**

After `#define CLIENT_API_BLOCK_DELETE_RESPONSE 18`, add:

```c
#define CLIENT_API_HEALTH_REQUEST   19
#define CLIENT_API_HEALTH_RESPONSE  20
```

After the `client_api_block_delete_response_t` struct, add:

```c
typedef struct {
  char* json_data;
} client_api_health_response_t;
```

After the existing encode/decode/destroy declarations, add:

```c
cbor_item_t* client_api_health_request_encode(void);
cbor_item_t* client_api_health_response_encode(const client_api_health_response_t* msg);
int client_api_health_response_decode(cbor_item_t* item, client_api_health_response_t* msg);
void client_api_health_response_destroy(client_api_health_response_t* msg);
```

- [ ] **Step 2: Add encode/decode/destroy implementations to client_api_wire.c**

After the block_delete_response implementations, add:

```c
cbor_item_t* client_api_health_request_encode(void) {
  cbor_item_t* arr = cbor_new_definite_array(1);
  cbor_array_push(arr, cbor_move(cbor_build_uint8(CLIENT_API_HEALTH_REQUEST)));
  return arr;
}

cbor_item_t* client_api_health_response_encode(const client_api_health_response_t* msg) {
  cbor_item_t* arr = cbor_new_definite_array(2);
  cbor_array_push(arr, cbor_move(cbor_build_uint8(CLIENT_API_HEALTH_RESPONSE)));
  if (msg->json_data != NULL) {
    cbor_array_push(arr, cbor_move(cbor_build_string(msg->json_data)));
  } else {
    cbor_array_push(arr, cbor_move(cbor_build_string("")));
  }
  return arr;
}

int client_api_health_response_decode(cbor_item_t* item, client_api_health_response_t* msg) {
  memset(msg, 0, sizeof(*msg));
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;

  cbor_item_t* json_item = cbor_array_get(item, 1);
  if (cbor_isa_string(json_item)) {
    size_t len = cbor_string_length(json_item);
    msg->json_data = get_memory(len + 1);
    memcpy(msg->json_data, cbor_string_handle(json_item), len);
    msg->json_data[len] = '\0';
  } else {
    msg->json_data = get_memory(1);
    msg->json_data[0] = '\0';
  }
  return 0;
}

void client_api_health_response_destroy(client_api_health_response_t* msg) {
  if (msg != NULL && msg->json_data != NULL) {
    free(msg->json_data);
    msg->json_data = NULL;
  }
}
```

- [ ] **Step 3: Build and fix compilation errors**

Run: `cd build && make -j$(nproc) 2>&1 | head -30`

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c
git commit -m "feat: add health check wire protocol messages"
```

---

### Task 3: HTTP health routes

**Files:**
- Create: `src/ClientAPI/HTTP/health_routes.h`
- Create: `src/ClientAPI/HTTP/health_routes.c`

- [ ] **Step 1: Create health_routes.h**

```c
#ifndef OFFS_HEALTH_ROUTES_H
#define OFFS_HEALTH_ROUTES_H

#include "../health_handler.h"
#include "http_server.h"

void health_routes_register(http_server_t* server, const health_context_t* ctx);

#endif
```

- [ ] **Step 2: Create health_routes.c**

```c
#include "health_routes.h"
#include "http_request.h"
#include "http_response.h"
#include "http_status.h"
#include <string.h>

static int _health_middleware(http_request_t* request, http_response_t* response,
                              void* user_data) {
  (void)request;
  if (request->method != HTTP_GET || strcmp(request->path, "/health") != 0) {
    return 0;
  }

  const health_context_t* ctx = (const health_context_t*)user_data;
  health_data_t data = health_data_collect(ctx);

  char json[8192];
  health_data_to_json(&data, json, sizeof(json));

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, (const uint8_t*)json, strlen(json));
  http_response_end(response);
  return 1;
}

void health_routes_register(http_server_t* server, const health_context_t* ctx) {
  http_server_use(server, _health_middleware, (void*)ctx, NULL);
}
```

Note: Verify the exact names for `HTTP_GET` (or `HTTP_GET` enum) and `HTTP_STATUS_OK` from `http_status.h` and `http_request.h`. Use whatever the existing codebase uses.

- [ ] **Step 3: Build and fix compilation errors**

Run: `cd build && make -j$(nproc) 2>&1 | head -30`

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/HTTP/health_routes.h src/ClientAPI/HTTP/health_routes.c
git commit -m "feat: add HTTP health check middleware"
```

---

### Task 4: TCP transport health wiring

**Files:**
- Modify: `src/ClientAPI/TCP/tcp_transport.h`
- Modify: `src/ClientAPI/TCP/tcp_transport.c`
- Modify: `src/ClientAPI/TCP/tcp_connection.c`

- [ ] **Step 1: Add health_ctx to tcp_transport_t**

In `tcp_transport.h`, add `#include "../health_handler.h"` at the top.

Add to the `tcp_transport_t` struct (before the closing `}`):
```c
  health_context_t* health_ctx;
```

Add `health_context_t* health_ctx` parameter to `tcp_transport_create()` declaration (after `api_key_hash`):
```c
tcp_transport_t* tcp_transport_create(scheduler_pool_t* pool,
                                       block_cache_t* bc,
                                       ofd_cache_t* ofd_cache,
                                       tuple_cache_t* tc,
                                       const char* host,
                                       uint16_t port,
                                       const char* cert_path,
                                       const char* key_path,
                                       const char* api_key_hash,
                                       health_context_t* health_ctx);
```

- [ ] **Step 2: Update tcp_transport.c**

In `tcp_transport_create()`, add the parameter and store it:
```c
transport->health_ctx = health_ctx;
```

Update the function signature to match the header.

- [ ] **Step 3: Add health dispatch to tcp_connection.c**

In `_tcp_dispatch_frame()`, before the `default:` case, add:

```c
    case CLIENT_API_HEALTH_REQUEST: {
      if (conn->transport == NULL || conn->transport->health_ctx == NULL) {
        _tcp_connection_send_error(conn, CLIENT_API_STATUS_INTERNAL_ERROR,
                                   "Health check not configured");
        break;
      }
      health_data_t data = health_data_collect(conn->transport->health_ctx);
      char json[8192];
      health_data_to_json(&data, json, sizeof(json));
      client_api_health_response_t resp;
      resp.json_data = json;
      cbor_item_t* frame = client_api_health_response_encode(&resp);
      _tcp_connection_send_frame(conn, frame);
      break;
    }
```

Add `#include "../health_handler.h"` to the includes in `tcp_connection.c`.

- [ ] **Step 4: Update all callers of tcp_transport_create**

Search for calls to `tcp_transport_create` and add `NULL` as the last argument:

Run: `grep -rn "tcp_transport_create" src/ test/ examples/`

Update each caller to pass `NULL` for `health_ctx`.

- [ ] **Step 5: Build and fix compilation errors**

Run: `cd build && make -j$(nproc) 2>&1 | head -40`

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/TCP/tcp_transport.h src/ClientAPI/TCP/tcp_transport.c \
        src/ClientAPI/TCP/tcp_connection.c
# also add any caller files that were updated
git commit -m "feat: add health check handler to TCP transport"
```

---

### Task 5: Unix transport health wiring

**Files:**
- Modify: `src/ClientAPI/Unix/unix_transport.h`
- Modify: `src/ClientAPI/Unix/unix_transport.c`
- Modify: `src/ClientAPI/Unix/unix_connection.c`

- [ ] **Step 1: Add health_ctx to unix_transport_t**

In `unix_transport.h`, add `#include "../health_handler.h"`.

Add `health_context_t* health_ctx` field to struct.

Add `health_context_t* health_ctx` parameter to `unix_transport_create()`.

- [ ] **Step 2: Update unix_transport.c**

Store `health_ctx` in `unix_transport_create()`, update signature.

- [ ] **Step 3: Add health dispatch to unix_connection.c**

Add `#include "../health_handler.h"`.

In `_unix_dispatch_frame()`, before `default:`, add the same `CLIENT_API_HEALTH_REQUEST` case as TCP but using `_unix_connection_send_frame` and `conn->transport->health_ctx`.

- [ ] **Step 4: Update all callers of unix_transport_create**

Search: `grep -rn "unix_transport_create" src/ test/ examples/`

Add `NULL` for `health_ctx` parameter at each call site.

- [ ] **Step 5: Build, fix errors, commit**

```bash
cd build && make -j$(nproc) 2>&1 | head -40
git add src/ClientAPI/Unix/ CMakeLists.txt
git commit -m "feat: add health check handler to Unix transport"
```

---

### Task 6: WS transport health wiring

**Files:**
- Modify: `src/ClientAPI/WS/ws_transport.h`
- Modify: `src/ClientAPI/WS/ws_transport.c`
- Modify: `src/ClientAPI/WS/ws_connection.c`

- [ ] **Step 1-5: Same pattern as Task 4/5, applied to WS**

- Add `health_context_t* health_ctx` to `ws_transport_t` struct and `ws_transport_create()` parameter list.
- Store in create function.
- In `_ws_dispatch_frame()`, add `CLIENT_API_HEALTH_REQUEST` case using `_ws_connection_send_frame` and `conn->transport->health_ctx`.
- Update callers to pass `NULL`.
- Build, fix errors, commit.

---

### Task 7: WT transport health wiring

**Files:**
- Modify: `src/ClientAPI/WT/wt_transport.h`
- Modify: `src/ClientAPI/WT/wt_transport.c`
- Modify: `src/ClientAPI/WT/wt_connection.c`

- [ ] **Step 1-5: Same pattern as Task 4/5, applied to WT**

- Add `health_context_t* health_ctx` to `wt_transport_t` struct and `wt_transport_create()` parameter list.
- Store in create function.
- In `_wt_dispatch_frame()`, add `CLIENT_API_HEALTH_REQUEST` case using `wt_connection_send_frame` (public function) and `conn->transport->health_ctx`.
- Wrap new code in `#ifdef HAS_MSQUIC`.
- Update callers to pass `NULL`.
- Build, fix errors, commit.

---

### Task 8: C client library block cache API

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h`
- Modify: `src/ClientLibs/c/offs_client.c`

- [ ] **Step 1: Add callback types and function declarations to offs_client.h**

After the existing callback typedefs, add:

```c
typedef void (*offs_block_put_cb_t)(void* ctx, uint8_t status,
    const uint8_t* hash_data, size_t hash_len, uint8_t hash_is_text);
typedef void (*offs_block_get_cb_t)(void* ctx, uint8_t status,
    const uint8_t* data, size_t data_len);
typedef void (*offs_block_delete_cb_t)(void* ctx, uint8_t status);
```

After the existing function declarations, add:

```c
int offs_client_block_put(offs_client_t* client,
    const uint8_t* data, size_t data_len, uint8_t encoding,
    offs_block_put_cb_t callback, void* ctx);

int offs_client_block_get(offs_client_t* client,
    const uint8_t* hash_data, size_t hash_len,
    offs_block_get_cb_t callback, void* ctx);

int offs_client_block_delete(offs_client_t* client,
    const uint8_t* hash_data, size_t hash_len,
    offs_block_delete_cb_t callback, void* ctx);
```

- [ ] **Step 2: Add callback slots to offs_client_t struct in offs_client.c**

After the existing callback slots (around line 124), add:

```c
  offs_block_put_cb_t block_put_cb;
  void* block_put_cb_ctx;
  offs_block_get_cb_t block_get_cb;
  void* block_get_cb_ctx;
  offs_block_delete_cb_t block_delete_cb;
  void* block_delete_cb_ctx;
```

Zero them in `_connect_attempt()` after the existing callback zeroing (around line 740):

```c
  client->block_put_cb = NULL;
  client->block_put_cb_ctx = NULL;
  client->block_get_cb = NULL;
  client->block_get_cb_ctx = NULL;
  client->block_delete_cb = NULL;
  client->block_delete_cb_ctx = NULL;
```

- [ ] **Step 3: Add response handlers to _handle_frame()**

In the `_handle_frame()` switch, after the existing block cases snapshot, add block response callbacks to the lock snapshot section. Add before `platform_mutex_lock`:

```c
  offs_block_put_cb_t block_put_cb = client->block_put_cb;
  void* block_put_cb_ctx = client->block_put_cb_ctx;
  offs_block_get_cb_t block_get_cb = client->block_get_cb;
  void* block_get_cb_ctx = client->block_get_cb_ctx;
  offs_block_delete_cb_t block_delete_cb = client->block_delete_cb;
  void* block_delete_cb_ctx = client->block_delete_cb_ctx;
```

Add to the switch statement, before `default:`:

```c
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
```

- [ ] **Step 4: Add block_put, block_get, block_delete functions**

After `offs_client_get()`, add:

```c
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
```

- [ ] **Step 5: Build, fix errors, commit**

```bash
cd build && make -j$(nproc) 2>&1 | head -40
git add src/ClientLibs/c/offs_client.h src/ClientLibs/c/offs_client.c
git commit -m "feat: add block cache API to C client library"
```

---

### Task 9: C client library health check API

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h`
- Modify: `src/ClientLibs/c/offs_client.c`

- [ ] **Step 1: Add callback type and function decl to offs_client.h**

```c
typedef void (*offs_health_cb_t)(void* ctx, const char* json_response);

int offs_client_health(offs_client_t* client,
    offs_health_cb_t callback, void* ctx);
```

- [ ] **Step 2: Add callback slots to offs_client_t**

```c
  offs_health_cb_t health_cb;
  void* health_cb_ctx;
```

Zero in `_connect_attempt()`.

- [ ] **Step 3: Snapshot health callback in _handle_frame() lock section**

Add to the lock snapshot block:
```c
  offs_health_cb_t health_cb = client->health_cb;
  void* health_cb_ctx = client->health_cb_ctx;
```

- [ ] **Step 4: Add CLIENT_API_HEALTH_RESPONSE case to _handle_frame() switch**

Before `default:`:

```c
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
```

- [ ] **Step 5: Add offs_client_health() function**

After the block functions:

```c
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
```

- [ ] **Step 6: Build, fix errors, commit**

```bash
cd build && make -j$(nproc) 2>&1 | head -40
git add src/ClientLibs/c/offs_client.h src/ClientLibs/c/offs_client.c
git commit -m "feat: add health check API to C client library"
```

---

### Task 10: Node uptime tracking

**Files:**
- Modify: `src/Node/node.h`
- Modify: `src/Node/node.c`

- [ ] **Step 1: Add start_time_ms to offs_node_t**

In `node.h`, add after the `draining` field:
```c
  uint64_t start_time_ms;
```

- [ ] **Step 2: Set start_time_ms in offs_node_start()**

In `node.c`, at the start of `offs_node_start()`, before any other logic:
```c
#include <sys/time.h>

  struct timeval tv;
  gettimeofday(&tv, NULL);
  node->start_time_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
```

- [ ] **Step 3: Build, fix errors, commit**

```bash
cd build && make -j$(nproc) 2>&1 | head -30
git add src/Node/node.h src/Node/node.c
git commit -m "feat: add uptime tracking to offs_node_t"
```

---

### Task 11: Example server wiring

**Files:**
- Modify: `examples/off_server/main.c`

- [ ] **Step 1: Add health includes and start time**

Add includes:
```c
#include "ClientAPI/HTTP/health_routes.h"
#include "ClientAPI/health_handler.h"
#include <sys/time.h>
```

In `main()`, after `signal(SIGINT, ...)` setup, add:
```c
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint64_t server_start_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;

  health_context_t health_ctx;
  memset(&health_ctx, 0, sizeof(health_ctx));
  health_ctx.block_cache = bc;
  health_ctx.start_time_ms = &server_start_ms;
  uint8_t running_val = 1;
  uint8_t draining_val = 0;
  health_ctx.running = &running_val;
  health_ctx.draining = &draining_val;
```

- [ ] **Step 2: Register health routes**

After the existing route registrations (`off_routes_register`, `block_routes_register`):
```c
  health_routes_register(server, &health_ctx);
```

- [ ] **Step 3: Pass health_ctx to Unix transport if present**

Update the `unix_transport_create` call to pass `&health_ctx`:
```c
unix_transport = unix_transport_create(pool, bc, ofd_cache, tc, unix_path, NULL, &health_ctx);
```

- [ ] **Step 4: Build, fix errors, commit**

```bash
cd build && make -j$(nproc) 2>&1 | head -30
git add examples/off_server/main.c
git commit -m "feat: wire health check into example server"
```

---

### Task 12: Flutter client health check

**Files:**
- Modify: `examples/off_client/lib/services/off_api.dart`

- [ ] **Step 1: Add healthCheck() method to OffApi class**

Add before the closing `}` of the class:
```dart
  Future<Map<String, dynamic>> healthCheck() async {
    final response = await http.get(Uri.parse('$baseUrl/health'));
    if (response.statusCode == 200) {
      return json.decode(response.body) as Map<String, dynamic>;
    }
    throw HttpException('Health check failed: ${response.statusCode}');
  }
```

Add import at top if not already present:
```dart
import 'dart:io';
```

- [ ] **Step 2: Build Flutter app to verify compilation**

Run: `cd examples/off_client && flutter pub get && flutter analyze`
Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add examples/off_client/lib/services/off_api.dart
git commit -m "feat: add health check to Flutter client"
```

---

### Task 13: Tests — health handler unit tests

**Files:**
- Create: `test/test_health_handler.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write test file**

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <sys/time.h>
#include "ClientAPI/health_handler.h"
#include "BlockCache/block_cache.h"
#include "Scheduler/scheduler.h"
#include "Configuration/config.h"
#include "Timer/timer_actor.h"

extern "C" {
#include "rm_rf.h"
}

TEST(HealthHandler, CollectWithNullContext) {
  health_data_t data = health_data_collect(NULL);
  EXPECT_STREQ(data.status, "unknown");
  EXPECT_EQ(data.uptime_seconds, 0u);
}

TEST(HealthHandler, CollectWithMinimalContext) {
  uint8_t running = 1;
  uint8_t draining = 0;
  uint64_t start_ms = 1000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;
  ctx.start_time_ms = &start_ms;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_STREQ(data.status, "running");
  EXPECT_GT(data.uptime_seconds, 0u);
  EXPECT_EQ(data.peer_count, 0u);
}

TEST(HealthHandler, CollectWithDrainingStatus) {
  uint8_t running = 1;
  uint8_t draining = 1;
  uint64_t start_ms = 1000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;
  ctx.start_time_ms = &start_ms;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_STREQ(data.status, "draining");
}

TEST(HealthHandler, JsonOutputContainsExpectedKeys) {
  uint8_t running = 1;
  uint8_t draining = 0;
  uint64_t start_ms = 1000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;
  ctx.start_time_ms = &start_ms;

  health_data_t data = health_data_collect(&ctx);
  char json[8192];
  size_t len = health_data_to_json(&data, json, sizeof(json));
  EXPECT_GT(len, 0u);
  EXPECT_LT(len, sizeof(json));

  EXPECT_NE(strstr(json, "\"status\":\"running\""), nullptr);
  EXPECT_NE(strstr(json, "\"uptime_seconds\":"), nullptr);
  EXPECT_NE(strstr(json, "\"peer_count\":"), nullptr);
  EXPECT_NE(strstr(json, "\"block_cache\":"), nullptr);
  EXPECT_NE(strstr(json, "\"rate_limits\":"), nullptr);
  EXPECT_NE(strstr(json, "\"rpc_calls\":"), nullptr);
}

TEST(HealthHandler, JsonOutputValidJson) {
  uint8_t running = 1;
  uint8_t draining = 0;
  uint64_t start_ms = 1000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;
  ctx.start_time_ms = &start_ms;

  health_data_t data = health_data_collect(&ctx);
  char json[8192];
  health_data_to_json(&data, json, sizeof(json));

  EXPECT_EQ(json[0], '{');
  EXPECT_EQ(json[strlen(json) - 1], '}');
}
```

- [ ] **Step 2: Add test file to CMakeLists.txt**

In `test/CMakeLists.txt`, add `test_health_handler.cpp` to the `add_executable(testliboffs ...)` list.

- [ ] **Step 3: Build and run tests**

```bash
cd build && make -j$(nproc) && ./test/testliboffs --gtest_filter='*HealthHandler*'
```
Expected: 5/5 tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/test_health_handler.cpp test/CMakeLists.txt
git commit -m "test: add health handler unit tests"
```

---

### Task 14: Tests — health wire protocol round-trip

**Files:**
- Create: `test/test_health_wire.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write test file**

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "ClientAPI/client_api_wire.h"

TEST(HealthWire, EncodeHealthRequest) {
  cbor_item_t* frame = client_api_health_request_encode();
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(cbor_isa_array(frame));
  EXPECT_EQ(cbor_array_size(frame), 1u);
  uint8_t type = client_api_wire_get_type(frame);
  EXPECT_EQ(type, CLIENT_API_HEALTH_REQUEST);
  cbor_decref(&frame);
}

TEST(HealthWire, HealthResponseRoundTrip) {
  const char* test_json = "{\"status\":\"running\",\"peer_count\":5}";

  client_api_health_response_t msg;
  msg.json_data = (char*)test_json;

  cbor_item_t* frame = client_api_health_response_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_health_response_t decoded;
  int result = client_api_health_response_decode(frame, &decoded);
  EXPECT_EQ(result, 0);
  EXPECT_STREQ(decoded.json_data, test_json);

  client_api_health_response_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(HealthWire, HealthResponseEmptyJson) {
  client_api_health_response_t msg;
  msg.json_data = NULL;

  cbor_item_t* frame = client_api_health_response_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_health_response_t decoded;
  int result = client_api_health_response_decode(frame, &decoded);
  EXPECT_EQ(result, 0);
  EXPECT_STREQ(decoded.json_data, "");

  client_api_health_response_destroy(&decoded);
  cbor_decref(&frame);
}
```

- [ ] **Step 2: Add to CMakeLists.txt, build, run tests**

```bash
cd build && make -j$(nproc) && ./test/testliboffs --gtest_filter='*HealthWire*'
```

- [ ] **Step 3: Commit**

```bash
git add test/test_health_wire.cpp test/CMakeLists.txt
git commit -m "test: add health wire protocol round-trip tests"
```

---

### Task 15: Tests — health HTTP integration

**Files:**
- Create: `test/test_health_http.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write test file**

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "ClientAPI/HTTP/http_server.h"
#include "ClientAPI/HTTP/health_routes.h"
#include "ClientAPI/health_handler.h"
#include "Scheduler/scheduler.h"

class HealthHttpTest : public testing::Test {
protected:
  scheduler_pool_t* pool = NULL;
  http_server_t* server = NULL;
  uint16_t port = 0;

  void SetUp() override {
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    port = 23450 + (getpid() % 100);
    server = http_server_create(pool, "127.0.0.1", port);
    ASSERT_NE(server, nullptr);
  }

  void TearDown() override {
    if (server != NULL) {
      http_server_stop(server);
      scheduler_pool_wait_for_idle(pool);
      scheduler_pool_stop(pool);
      http_server_destroy(server);
    }
    if (pool != NULL) scheduler_pool_destroy(pool);
  }
};

static int _connect_to_server(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  connect(fd, (struct sockaddr*)&addr, sizeof(addr));
  return fd;
}

TEST_F(HealthHttpTest, GetHealthReturns200) {
  uint8_t running = 1;
  uint8_t draining = 0;
  uint64_t start_ms = 1000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;
  ctx.start_time_ms = &start_ms;

  health_routes_register(server, &ctx);
  http_server_listen(server);
  usleep(50000);

  int fd = _connect_to_server(port);
  ASSERT_GE(fd, 0);

  const char* request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  send(fd, request, strlen(request), 0);

  char response[16384];
  memset(response, 0, sizeof(response));
  ssize_t received = recv(fd, response, sizeof(response) - 1, 0);
  close(fd);
  ASSERT_GT(received, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "application/json"), nullptr);
  EXPECT_NE(strstr(response, "\"status\":\"running\""), nullptr);
}

TEST_F(HealthHttpTest, HealthBypassesAuth) {
  uint8_t running = 1;
  uint8_t draining = 0;
  uint64_t start_ms = 1000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;
  ctx.start_time_ms = &start_ms;

  health_routes_register(server, &ctx);
  // No off_routes_register (which adds auth middleware) — health still works
  http_server_listen(server);
  usleep(50000);

  int fd = _connect_to_server(port);
  ASSERT_GE(fd, 0);

  const char* request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  send(fd, request, strlen(request), 0);

  char response[16384];
  memset(response, 0, sizeof(response));
  ssize_t received = recv(fd, response, sizeof(response) - 1, 0);
  close(fd);
  ASSERT_GT(received, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
}
```

- [ ] **Step 2: Add to CMakeLists.txt, build, run tests**

```bash
cd build && make -j$(nproc) && ./test/testliboffs --gtest_filter='*HealthHttp*'
```

- [ ] **Step 3: Commit**

```bash
git add test/test_health_http.cpp test/CMakeLists.txt
git commit -m "test: add HTTP health endpoint integration tests"
```

---

### Task 16: Tests — health TCP integration

**Files:**
- Create: `test/test_health_tcp.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write test file**

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ClientAPI/health_handler.h"
#include "ClientAPI/client_api_wire.h"
#include "ClientAPI/TCP/tcp_transport.h"
#include "Network/stream_framer.h"
#include "BlockCache/block_cache.h"
#include "OFFStreams/ofd_cache.h"
#include "OFFStreams/tuple_cache.h"
#include "Scheduler/scheduler.h"
#include "Timer/timer_actor.h"
#include "Configuration/config.h"

extern "C" {
#include "rm_rf.h"
}

class HealthTcpTest : public testing::Test {
protected:
  scheduler_pool_t* pool = NULL;
  timer_actor_t* timer = NULL;
  block_cache_t* bc = NULL;
  ofd_cache_t* ofd_cache = NULL;
  tuple_cache_t* tc = NULL;
  tcp_transport_t* transport = NULL;
  char* cache_dir = NULL;
  uint16_t port = 0;

  void SetUp() override {
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    timer = timer_actor_create();
    cache_dir = strdup("/tmp/test_health_tcp_XXXXXX");
    mkdtemp(cache_dir);
    config_t config = config_default();
    bc = block_cache_create(config, cache_dir, standard, timer, pool, NULL, 0);
    ofd_cache = ofd_cache_create(pool, bc, 300000);
    tc = tuple_cache_create(100, pool);

    port = 23500 + (getpid() % 100);
    transport = tcp_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, NULL, NULL);
    ASSERT_NE(transport, nullptr);
  }

  void TearDown() override {
    if (transport != NULL) tcp_transport_stop(transport);
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    if (ofd_cache != NULL) ofd_cache_destroy(ofd_cache);
    if (tc != NULL) tuple_cache_destroy(tc);
    if (bc != NULL) block_cache_destroy(bc);
    if (timer != NULL) timer_actor_destroy(timer);
    if (transport != NULL) tcp_transport_destroy(transport);
    if (pool != NULL) scheduler_pool_destroy(pool);
    rm_rf(cache_dir);
    free(cache_dir);
  }
};

static int _send_frame(int fd, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);
  if (cbor_buf == NULL) return -1;
  size_t framed_len;
  uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
  free(cbor_buf);
  if (framed == NULL) return -1;
  ssize_t sent = send(fd, framed, framed_len, MSG_NOSIGNAL);
  free(framed);
  return (sent == (ssize_t)framed_len) ? 0 : -1;
}

static cbor_item_t* _recv_frame(int fd, stream_framer_t* framer, int timeout_ms = 10000) {
  uint8_t buf[65536];
  for (int i = 0; i < timeout_ms / 10; i++) {
    size_t frame_len;
    uint8_t* frame_data = stream_framer_next(framer, &frame_len);
    if (frame_data != NULL) {
      struct cbor_load_result load_result;
      cbor_item_t* item = cbor_load(frame_data, frame_len, &load_result);
      free(frame_data);
      if (item != NULL && load_result.error.code == CBOR_ERR_NONE) return item;
      if (item != NULL) cbor_decref(&item);
      return NULL;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
      ssize_t received = recv(fd, buf, sizeof(buf), 0);
      if (received <= 0) return NULL;
      stream_framer_feed(framer, buf, (size_t)received);
    }
  }
  return NULL;
}

TEST_F(HealthTcpTest, HealthRequestReturnsJsonResponse) {
  uint8_t running = 1;
  uint8_t draining = 0;
  uint64_t start_ms = 1000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;
  ctx.start_time_ms = &start_ms;
  ctx.block_cache = bc;
  transport->health_ctx = &ctx;

  tcp_transport_start(transport);
  usleep(50000);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  ASSERT_GE(connect(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);

  // Send health request
  cbor_item_t* request = client_api_health_request_encode();
  ASSERT_EQ(_send_frame(fd, request), 0);

  // Receive response
  stream_framer_t* framer = stream_framer_create();
  cbor_item_t* response = _recv_frame(fd, framer);
  stream_framer_destroy(framer);
  ASSERT_NE(response, nullptr);

  uint8_t type = client_api_wire_get_type(response);
  EXPECT_EQ(type, CLIENT_API_HEALTH_RESPONSE);

  if (type == CLIENT_API_HEALTH_RESPONSE) {
    client_api_health_response_t msg;
    memset(&msg, 0, sizeof(msg));
    EXPECT_EQ(client_api_health_response_decode(response, &msg), 0);
    EXPECT_NE(strstr(msg.json_data, "\"status\":\"running\""), nullptr);
    client_api_health_response_destroy(&msg);
  }
  cbor_decref(&response);
  close(fd);
}
```

- [ ] **Step 2: Add to CMakeLists.txt, build, run tests**

```bash
cd build && make -j$(nproc) && ./test/testliboffs --gtest_filter='*HealthTcp*'
```

- [ ] **Step 3: Commit**

```bash
git add test/test_health_tcp.cpp test/CMakeLists.txt
git commit -m "test: add TCP health check integration tests"
```

---

### Task 17: Tests — offs_client health + block tests

**Files:**
- Modify: `test/test_offs_client.cpp`

- [ ] **Step 1: Add health check and block cache tests to test_offs_client.cpp**

At the end of the `offs_client_test` namespace (before the closing `}`), add:

```cpp
struct HealthCallbackContext {
  char* json_data;
  int called;
};

static void _health_callback(void* ctx, const char* json_response) {
  auto* hctx = (HealthCallbackContext*)ctx;
  if (hctx->json_data == NULL && json_response != NULL) {
    hctx->json_data = strdup(json_response);
  }
  hctx->called++;
}

TEST_F(OffsClientTest, HealthCheckReturnsJson) {
  transport = unix_transport_create(pool, bc, ofd_cache, tc, socket_path, NULL, NULL);
  ASSERT_NE(transport, nullptr);
  unix_transport_start(transport);

  offs_client_config_t client_config = offs_client_config_default();
  char url[256];
  snprintf(url, sizeof(url), "unix://%s", socket_path);
  offs_client_t* client = offs_client_connect_ex(url, NULL, &client_config);
  ASSERT_NE(client, nullptr);

  HealthCallbackContext health_ctx = {NULL, 0};
  ASSERT_EQ(offs_client_health(client, _health_callback, &health_ctx), 0);

  for (int i = 0; i < 200 && health_ctx.called == 0; i++) {
    usleep(10000);
  }
  EXPECT_GT(health_ctx.called, 0);
  if (health_ctx.json_data != NULL) {
    EXPECT_NE(strstr(health_ctx.json_data, "\"status\""), nullptr);
    free(health_ctx.json_data);
  }

  offs_client_disconnect(client);
}

struct BlockPutCallbackContext {
  uint8_t status;
  uint8_t* hash_data;
  size_t hash_len;
  uint8_t hash_is_text;
  int called;
};

static void _block_put_callback(void* ctx, uint8_t status,
    const uint8_t* hash_data, size_t hash_len, uint8_t hash_is_text) {
  auto* bctx = (BlockPutCallbackContext*)ctx;
  bctx->status = status;
  if (hash_data != NULL && hash_len > 0) {
    bctx->hash_data = (uint8_t*)malloc(hash_len);
    memcpy(bctx->hash_data, hash_data, hash_len);
    bctx->hash_len = hash_len;
  }
  bctx->hash_is_text = hash_is_text;
  bctx->called++;
}

struct BlockGetCallbackContext {
  uint8_t status;
  uint8_t* data;
  size_t data_len;
  int called;
};

static void _block_get_callback(void* ctx, uint8_t status,
    const uint8_t* data, size_t data_len) {
  auto* bctx = (BlockGetCallbackContext*)ctx;
  bctx->status = status;
  if (data != NULL && data_len > 0) {
    bctx->data = (uint8_t*)malloc(data_len);
    memcpy(bctx->data, data, data_len);
    bctx->data_len = data_len;
  }
  bctx->called++;
}

struct BlockDeleteCallbackContext {
  uint8_t status;
  int called;
};

static void _block_delete_callback(void* ctx, uint8_t status) {
  auto* bctx = (BlockDeleteCallbackContext*)ctx;
  bctx->status = status;
  bctx->called++;
}

TEST_F(OffsClientTest, BlockPutGetDeleteRoundTrip) {
  transport = unix_transport_create(pool, bc, ofd_cache, tc, socket_path, "test_key_hash", NULL);
  ASSERT_NE(transport, nullptr);
  unix_transport_start(transport);

  offs_client_config_t client_config = offs_client_config_default();
  char url[256];
  snprintf(url, sizeof(url), "unix://%s", socket_path);
  offs_client_t* client = offs_client_connect_ex(url, "test_key_hash", &client_config);
  ASSERT_NE(client, nullptr);

  // Block PUT
  const uint8_t test_data[] = "hello block cache test data";
  BlockPutCallbackContext put_ctx = {0, NULL, 0, 0, 0};
  ASSERT_EQ(offs_client_block_put(client, test_data, sizeof(test_data) - 1, 0,
                                   _block_put_callback, &put_ctx), 0);

  for (int i = 0; i < 200 && put_ctx.called == 0; i++) usleep(10000);
  EXPECT_GT(put_ctx.called, 0);
  EXPECT_EQ(put_ctx.status, 0);
  ASSERT_GT(put_ctx.hash_len, 0u);

  // Block GET using the returned hash
  BlockGetCallbackContext get_ctx = {0, NULL, 0, 0};
  ASSERT_EQ(offs_client_block_get(client, put_ctx.hash_data, put_ctx.hash_len,
                                   _block_get_callback, &get_ctx), 0);

  for (int i = 0; i < 200 && get_ctx.called == 0; i++) usleep(10000);
  EXPECT_GT(get_ctx.called, 0);
  EXPECT_EQ(get_ctx.status, 0);
  if (get_ctx.data != NULL) {
    EXPECT_EQ(get_ctx.data_len, sizeof(test_data) - 1);
    free(get_ctx.data);
  }

  // Block DELETE
  BlockDeleteCallbackContext del_ctx = {0, 0};
  ASSERT_EQ(offs_client_block_delete(client, put_ctx.hash_data, put_ctx.hash_len,
                                      _block_delete_callback, &del_ctx), 0);

  for (int i = 0; i < 200 && del_ctx.called == 0; i++) usleep(10000);
  EXPECT_GT(del_ctx.called, 0);

  free(put_ctx.hash_data);
  offs_client_disconnect(client);
}
```

Note: These tests live in the existing `offs_client_test` namespace/fixture and reuse its `SetUp`/`TearDown` which creates `pool`, `bc`, `ofd_cache`, `tc`, and `socket_path`. The test class may need `transport` to be declared as a member variable if it isn't already.

- [ ] **Step 2: Build and run tests**

```bash
cd build && make -j$(nproc) && ./test/testliboffs --gtest_filter='*OffsClientTest*'
```

- [ ] **Step 3: Commit**

```bash
git add test/test_offs_client.cpp
git commit -m "test: add health check and block cache API client tests"
```

---

### Task 18: Final verification

- [ ] **Step 1: Run all new tests**

```bash
cd build && ./test/testliboffs --gtest_filter='*Health*:*OffsClientTest*'
```
Expected: all tests pass.

- [ ] **Step 2: Run full test suite**

```bash
cd build && ./test/testliboffs
```
Expected: no regressions.

- [ ] **Step 3: Check for memory leaks (valgrind with DWARF-4)**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-gdwarf-4" && make -j$(nproc)
valgrind --leak-check=full --suppressions=../valgrind.supp ./test/testliboffs --gtest_filter='*Health*'
```
Expected: no new leaks beyond the known pre-existing ones.

- [ ] **Step 4: Close ticket in Harmony**

```bash
$H ticket close OFFS-131 "Implemented health check endpoint across HTTP/TCP/Unix/WS/WT with shared health_handler module, wire protocol messages, C client library functions (health + block cache), Flutter client healthCheck(), and comprehensive tests"
```
