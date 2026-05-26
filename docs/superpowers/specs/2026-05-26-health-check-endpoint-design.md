# Health Check Endpoint Design

## Overview

Add a transport-agnostic health check endpoint across all 5 Client API transports
(HTTP, TCP, Unix, WS, WT). The health check returns a JSON status dump including
node status, peer count, topology metrics, block cache stats, and uptime.

Also add the missing block cache client operations (PUT/GET/DELETE) to the C client
library, which were defined in the wire protocol and handled server-side but never
exposed in the client API.

## Shared Health Module

### `src/ClientAPI/health_handler.h/.c`

A `health_context_t` holds nullable pointers to all data sources:

```c
typedef struct health_context_t {
  topology_metrics_t* topology_metrics;  // nullable
  block_cache_t*      block_cache;       // nullable
  node_id_t*          node_id;           // nullable
  uint64_t*           start_time_ms;     // nullable
  uint8_t*            running;           // nullable
  uint8_t*            draining;          // nullable
} health_context_t;
```

Two public functions:

```c
health_data_t health_data_collect(const health_context_t* ctx);
size_t health_data_to_json(const health_data_t* data, char* buf, size_t buf_size);
```

If a data source is NULL, the corresponding JSON fields are omitted or zeroed.
Scalar fields from `topology_metrics_t` are read directly — on x86/ARM64 aligned
reads are hardware-atomic. Pointer arrays (peer_snapshots, ring_entries) are not
iterated; only aggregate counts and scalars are used.

### JSON Response

```json
{
  "status": "running",
  "uptime_seconds": 3600,
  "node_id": "1A2B3C...",
  "peer_count": 12,
  "total_connections": 45,
  "avg_hebbian_weight": 0.73,
  "block_cache": {
    "current_bytes": 1048576,
    "max_bytes": 1073741824,
    "block_count": 128
  },
  "rate_limits": [
    {"type": "ping", "accepted": 1000, "rejected": 5, "avg_tokens": 9.5, "effective_rate": 1.2},
    ...
  ],
  "rpc_calls": [
    {"name": "find_block", "count": 5000},
    ...
  ]
}
```

RPC type names and rate limit type names are defined as static string tables
keyed by the same enum indices used in `topology_metrics_t`.

## HTTP Integration

### `src/ClientAPI/HTTP/health_routes.h/.c`

```c
void health_routes_register(http_server_t* server, const health_context_t* ctx);
```

Registered as middleware (not a route) so it runs before draining and auth
middleware. Checks `method == GET && path == "/health"`. On match: collects
health data, serializes to JSON, returns `200 application/json`, stops chain.
On non-match: returns 0 to continue the middleware/route chain.

This means health always responds (even while draining, where it reports
`"status": "draining"`) and bypasses authentication automatically.

## Wire Protocol

### New message types in `client_api_wire.h/.c`

```c
#define CLIENT_API_HEALTH_REQUEST  19   // [19] — no payload
#define CLIENT_API_HEALTH_RESPONSE 20   // [20, json_string: tstr]
```

```c
typedef struct {
  char* json_data;  // caller must free
} client_api_health_response_t;

cbor_item_t* client_api_health_request_encode(void);
cbor_item_t* client_api_health_response_encode(const client_api_health_response_t* msg);
int client_api_health_response_decode(cbor_item_t* item, client_api_health_response_t* msg);
void client_api_health_response_destroy(client_api_health_response_t* msg);
```

## Server Transport Wiring

Each transport (TCP, Unix, WS, WT) gets:

- `health_context_t* health_ctx` field added to the transport struct
- `health_context_t*` parameter added to the `_create()` function
- A `CLIENT_API_HEALTH_REQUEST` case in the connection dispatch handler

The dispatch handler for health:

```
case CLIENT_API_HEALTH_REQUEST:
  health_data_t data = health_data_collect(connection->transport->health_ctx);
  char json[4096];
  health_data_to_json(&data, json, sizeof(json));
  // encode as CLIENT_API_HEALTH_RESPONSE, send frame
  break;
```

If `health_ctx` is NULL on a transport, the health request still succeeds but
returns only the fields available from non-NULL sources (all zeros/defaults).

## C Client Library

### Health check function

```c
typedef void (*offs_health_cb_t)(void* ctx, const char* json_response);

int offs_client_health(offs_client_t* client,
                       offs_health_cb_t callback, void* ctx);
```

Sends `CLIENT_API_HEALTH_REQUEST`, receives `CLIENT_API_HEALTH_RESPONSE`,
decodes it, fires callback with the JSON string. Works across all 4 binary
transports (Unix, TCP, WS, WT). The `offs_client_t` struct gains a
`health_cb`/`health_cb_ctx` callback slot pair.

### Block cache functions

```c
typedef void (*offs_block_put_cb_t)(void* ctx, uint8_t status,
    const uint8_t* hash_data, size_t hash_len, uint8_t hash_is_text);
typedef void (*offs_block_get_cb_t)(void* ctx, uint8_t status,
    const uint8_t* data, size_t data_len);
typedef void (*offs_block_delete_cb_t)(void* ctx, uint8_t status);

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

The `_handle_frame()` switch gains 4 new cases for block and health responses.
The `offs_client_t` struct gains 4 new callback slot pairs.

### Flutter client

```dart
// off_api.dart
Future<Map<String, dynamic>> healthCheck() async {
  final response = await http.get(Uri.parse('$baseUrl/health'));
  return json.decode(response.body);
}
```

## Node Integration

`offs_node_t` gains `uint64_t start_time_ms`, set via `gettimeofday()` in
`offs_node_start()`. The `health_context_t` for a full node points to
`node->network->topology_metrics`, `node->block_cache`,
`node->authority->local_id`, `&node->start_time_ms`, `&node->running`,
`&node->draining`.

For the example server (which doesn't use `offs_node_t`), a static
`uint64_t server_start_ms` is set in `main()` and passed to the
`health_context_t`.

## Uptime Tracking

Computed as `(now_ms - start_time_ms) / 1000` in the health handler at
request time. Uses `gettimeofday()` for both start and current timestamps.

## Files Changed

| File | Change |
|------|--------|
| `src/ClientAPI/health_handler.h` | NEW |
| `src/ClientAPI/health_handler.c` | NEW |
| `src/ClientAPI/HTTP/health_routes.h` | NEW |
| `src/ClientAPI/HTTP/health_routes.c` | NEW |
| `src/ClientAPI/client_api_wire.h` | Add HEALTH_REQUEST/RESPONSE + struct + decls |
| `src/ClientAPI/client_api_wire.c` | Add encode/decode/destroy |
| `src/ClientLibs/c/offs_client.h` | Add health + block callbacks + function decls |
| `src/ClientLibs/c/offs_client.c` | Add health + block impl + 4 frame handlers |
| `src/ClientAPI/TCP/tcp_transport.h` | Add health_ctx field + create param |
| `src/ClientAPI/TCP/tcp_connection.c` | Handle CLIENT_API_HEALTH_REQUEST |
| `src/ClientAPI/Unix/unix_transport.h` | Add health_ctx field + create param |
| `src/ClientAPI/Unix/unix_connection.c` | Handle CLIENT_API_HEALTH_REQUEST |
| `src/ClientAPI/WS/ws_transport.h` | Add health_ctx field + create param |
| `src/ClientAPI/WS/ws_connection.c` | Handle CLIENT_API_HEALTH_REQUEST |
| `src/ClientAPI/WT/wt_transport.h` | Add health_ctx field + create param |
| `src/ClientAPI/WT/wt_connection.c` | Handle CLIENT_API_HEALTH_REQUEST |
| `examples/off_client/lib/services/off_api.dart` | Add healthCheck() method |
| `src/Node/node.h` | Add start_time_ms field |
| `src/Node/node.c` | Set start_time_ms in offs_node_start() |
| `examples/off_server/main.c` | Wire health routes + health context |

## Tests

| Test | What it covers |
|------|---------------|
| `test_health_handler.c` | health_data_collect() with full/partial/NULL ctx, JSON output |
| `test_health_wire.c` | CBOR encode/decode round-trip for health messages |
| `test_health_http.c` | HTTP GET /health → 200 + valid JSON + expected fields |
| `test_health_tcp.c` | TCP health request → response round-trip |
| `test_offs_client.cpp` | offs_client_health() via Unix transport |
