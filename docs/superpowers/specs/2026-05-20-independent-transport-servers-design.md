# Independent Transport Server Design

## Overview

Each network transport (Unix socket, TCP/TLS, WebSocket, WebTransport) is an independent server actor with its own listener thread, event loop, and per-connection actors. Transports directly access `block_cache_t*`, `ofd_cache_t*`, `tuple_cache_t*`, and `scheduler_pool_t*` — no shared handler layer, no adapter pattern.

The HTTP server (`off_routes.c`) stays unchanged. It doesn't use CBOR wire format.

## Shared Code

Only `client_api_wire.h/.c` (CBOR encode/decode) is shared across transports. Each transport has its own pipeline logic, request dispatch, and response encoding.

## Architecture Pattern

Each transport follows the same event-driven pattern as `http_server.c`:

```
Server Actor (transport_t)
  ├── actor_t actor                    // scheduler pool dispatch
  ├── pd_loop_t* loop                  // event loop for this thread
  ├── PLATFORMTHREADTYPE thread        // listener thread
  ├── ATOMIC(uint8_t) running          // shutdown flag
  ├── PLATFORMLOCKTYPE(destroy_lock)   // watcher destroy stack lock
  ├── int listen_fd                    // bound socket
  ├── pd_watcher_t* listen_watcher     // accept watcher
  ├── vec_connection_t connections     // active connections
  ├── block_cache_t* bc                // direct pointer
  ├── ofd_cache_t* ofd_cache           // direct pointer
  ├── tuple_cache_t* tc                // shared tuple cache
  ├── scheduler_pool_t* pool           // scheduler
  └── destroy_head                     // deferred watcher cleanup

Per-Connection Actor (connection_t)
  ├── refcounter_t refcounter
  ├── actor_t actor                     // scheduler pool dispatch
  ├── int fd                           // client socket
  ├── ATOMIC(pd_watcher_t*) watcher    // I/O watcher
  ├── stream_framer_t* framer          // CBOR length-prefix framing
  ├── buffer_t* write_buffer           // pending writes
  ├── transport_t* transport           // back-reference
  ├── block_cache_t* bc                // direct pointer
  ├── ofd_cache_t* ofd_cache           // direct pointer
  ├── tuple_cache_t* tc                // direct pointer
  └── put_context_t* put_context       // streaming PUT state
```

### Key behaviors

- **Accept callback**: `pd_watcher_t` on `listen_fd` for `PD_EVENT_READ` → `accept()` → create connection actor
- **Connection I/O**: `pd_watcher_t` on `client_fd` → `read()` → `stream_framer_feed()` → extract CBOR frames → dispatch
- **Destroy stack**: `PLATFORMLOCKTYPE(destroy_lock)` + linked list of watchers to defer destruction off the I/O thread
- **Actor dispatch**: Messages like `UPDATE_WATCHER`, `STOP_WATCHER` processed on scheduler pool threads
- **Connection cleanup**: `refcounter_t` + `ACTOR_FLAG_DESTROY` + `scheduler_pool_defer_cleanup` pattern from `http_connection`

## Wire Protocol

All binary transports use the same CBOR wire format defined in `client_api_wire.h`:

- **Framing**: 4-byte big-endian length prefix + CBOR payload (via `stream_framer_t`)
- **Message format**: CBOR array with type tag at index 0
- **Types**: PUT_REQUEST(1), PUT_DATA(2), PUT_END(3), PUT_RESPONSE(4), GET_REQUEST(5), GET_RESPONSE_START(6), GET_DATA(7), GET_END(8), ERROR(11)
- **No DELETE**: Blocks are shared across files — deleting blocks for one file would corrupt others. DELETE is not implemented in any transport.

WebSocket and WebTransport don't need the length prefix (they have their own framing).

## File Structure

```
src/ClientAPI/
  client_api_wire.h/.c          — CBOR encode/decode (shared)
  client_api_transport.h         — Transport vtable (minimal)
  HTTP/                          — Unchanged (off_routes.c stays as-is)
  Unix/
    unix_transport.h/.c          — Server actor + accept loop
    unix_connection.h/.c         — Per-connection actor
  TCP/
    tcp_transport.h/.c           — Server actor + SSL/TLS
    tcp_connection.h/.c          — Per-connection actor
  WS/
    ws_transport.h/.c           — Server actor + HTTP upgrade
    ws_connection.h/.c           — Per-connection actor
    ws_frame.h/.c                — RFC 6455 frame encode/decode
  WT/
    wt_transport.h/.c            — Server actor (MsQuic, conditional)
```

## Removed Files

These adapter-layer files are deleted:
- `client_api_handler.h/.c` — no shared handler, each transport has its own
- `client_api_session.h/.c` — no shared session, each transport has its own connection actor
- `http_api_adapter.h/.c` — no HTTP adapter, HTTP stays as-is

## HTTP DELETE Handler

The existing DELETE handler in `off_routes.c` calls `block_cache_remove()`, which is dangerous: blocks are shared across files, so deleting blocks for one file corrupts others. The DELETE route must be disabled in the HTTP server. No DELETE operation is implemented in any transport.

## Create Functions

Each transport's create function takes direct pointers:

```c
// Unix
unix_transport_t* unix_transport_create(scheduler_pool_t* pool,
                                         block_cache_t* bc,
                                         ofd_cache_t* ofd_cache,
                                         tuple_cache_t* tc,
                                         const char* socket_path,
                                         size_t max_connections);

// TCP
tcp_transport_t* tcp_transport_create(scheduler_pool_t* pool,
                                        block_cache_t* bc,
                                        ofd_cache_t* ofd_cache,
                                        tuple_cache_t* tc,
                                        const char* host,
                                        uint16_t port,
                                        const char* cert_path,   // NULL for plain TCP
                                        const char* key_path,    // NULL for plain TCP
                                        size_t max_connections);

// WebSocket
ws_transport_t* ws_transport_create(scheduler_pool_t* pool,
                                      block_cache_t* bc,
                                      ofd_cache_t* ofd_cache,
                                      tuple_cache_t* tc,
                                      const char* host,
                                      uint16_t port,
                                      const char* cert_path,
                                      const char* key_path,
                                      size_t max_connections);

// WebTransport (conditional: #ifdef HAS_MSQUIC)
wt_transport_t* wt_transport_create(scheduler_pool_t* pool,
                                      block_cache_t* bc,
                                      ofd_cache_t* ofd_cache,
                                      tuple_cache_t* tc,
                                      const char* host,
                                      uint16_t port,
                                      const char* cert_path,
                                      const char* key_path,
                                      size_t max_connections);
```

Each has `start()`, `stop()`, `destroy()` lifecycle methods. No central registry — the application creates and starts whichever transports it needs from `main()`.

## Implementation Order

1. **Unix socket transport** — simplest, proves the pattern. No TLS, no HTTP upgrade.
2. **TCP/TLS transport** — nearly identical to Unix, just `AF_INET` + optional SSL.
3. **WebSocket transport** — adds HTTP upgrade handshake + RFC 6455 framing.
4. **WebTransport transport** — MsQuic integration, most complex.

Each phase must build and pass valgrind before moving to the next.

## Unix Connection Request Flow

1. `accept()` → `unix_connection_create(transport, client_fd)`
2. Read callback: `read(fd, buf)` → `stream_framer_feed(framer, buf, n)` → `stream_framer_next()` loop
3. For each frame: `cbor_load(data, len)` → extract type byte → dispatch:
   - `PUT_REQUEST` → set up `writeable_off_stream` + `writeable_descriptor` pipeline, write data if buffered, store `put_context` if streaming
   - `GET_REQUEST` → parse ORI from CBOR, set up `readable_off_stream` + `readable_descriptor` pipeline, subscribe to data events → encode `GET_DATA` frames → write to fd
   - `PUT_DATA` → `writeable_off_stream_write()` with chunk data
   - `PUT_END` → `writeable_off_stream_finalize()`
4. Pipeline callbacks encode CBOR responses and write to the connection fd
5. On close/error: `stream_deactivate()`, deferred watcher cleanup, `refcounter_dereference`

## Pipeline Callbacks

Each connection defines its own static pipeline callbacks that encode CBOR responses:

```c
// GET pipeline — data callback
static void _unix_get_on_data(void* ctx, void* data) {
    unix_get_pipeline_t* pipeline = (unix_get_pipeline_t*)ctx;
    buffer_t* buf = (buffer_t*)data;
    // Encode GET_DATA(7) CBOR frame: [7, bytestring]
    cbor_item_t* frame = client_api_get_data_encode(buf->data, buf->size);
    size_t len;
    uint8_t* encoded = client_api_wire_encode_frame(frame, &len);
    cbor_decref(&frame);
    // Write length-prefixed frame to connection fd
    unix_connection_write(pipeline->connection, encoded, len);
    free(encoded);
}

// GET pipeline — close callback
static void _unix_get_on_close(void* ctx, void* unused) {
    unix_get_pipeline_t* pipeline = (unix_get_pipeline_t*)ctx;
    // Encode GET_END(8) CBOR frame: [8]
    cbor_item_t* frame = client_api_get_end_encode();
    // ... write and cleanup
}
```

## PUT Pipeline

For buffered PUT (data included in PUT_REQUEST frame):
```c
static void _unix_handle_put(unix_connection_t* conn, client_api_request_t* request) {
    // Create writeable_off_stream + writeable_descriptor
    // Set up pipeline callbacks
    // Write request->put_data to writeable_off_stream
    // Finalize writeable_off_stream
    // On descriptor close: encode PUT_RESPONSE with ORI string
}
```

For streaming PUT (empty data in PUT_REQUEST, followed by PUT_DATA frames):
```c
// Store put_context in conn for subsequent frames
conn->put_context = put_ctx;
conn->put_streaming = 1;

// On PUT_DATA frame:
buffer_t* chunk = buffer_create_from_pointer_copy(data, size);
writeable_off_stream_write(conn->put_context->ws, chunk);
buffer_destroy(chunk);

// On PUT_END frame:
writeable_off_stream_finalize(conn->put_context->ws);
conn->put_context = NULL;
conn->put_streaming = 0;
```

## Async Directory Resolution (GET with ofd_cache)

For OFD directory resolution, the connection actor receives `OFD_CACHE_RESOLVE_RESULT` messages:

```c
static void _unix_connection_dispatch(void* state, message_t* msg) {
    unix_connection_t* conn = (unix_connection_t*)state;
    switch (msg->type) {
        case OFD_CACHE_RESOLVE_RESULT: {
            // Handle async directory resolution
            // Set up stream pipeline with resolved ORI
            break;
        }
        case CACHE_GET_RESULT: {
            // Handle async block fetch (for ?ofd=raw)
            break;
        }
        default:
            break;
    }
}
```

This mirrors the pattern in `off_routes.c`'s `_off_get_dispatch`.

## ClientLib (C)

A C client library that connects to OFFS servers via any transport. Reuses `client_api_wire.h/.c` for CBOR encoding/decoding. Lives in `src/ClientLibs/c/`.

### File Structure

```
src/ClientLibs/c/
  offs_client.h          — Public C header (FFI surface)
  offs_client.c           — Implementation
  CMakeLists.txt          — Builds static + shared library, installs headers
```

### API Surface

```c
// Opaque handle
typedef struct offs_client_t offs_client_t;

// Callbacks
typedef void (*offs_put_response_cb_t)(void* ctx, const char* ori_string);
typedef void (*offs_get_data_cb_t)(void* ctx, const uint8_t* data, size_t len);
typedef void (*offs_get_end_cb_t)(void* ctx);
typedef void (*offs_error_cb_t)(void* ctx, uint8_t status_code, const char* message);

// Connection lifecycle
offs_client_t* offs_client_connect(const char* transport_url);  // "unix:///path", "tcp://host:port", "ws://host:port"
void offs_client_disconnect(offs_client_t* client);

// Operations (PUT data in, GET data out)
int offs_client_put(offs_client_t* client,
                    const char* content_type,
                    const char* file_name,
                    size_t stream_length,
                    const uint8_t* data,
                    size_t data_len,
                    offs_put_response_cb_t callback,
                    void* ctx);

int offs_client_put_stream_start(offs_client_t* client,
                                  const char* content_type,
                                  const char* file_name,
                                  size_t stream_length);
int offs_client_put_stream_data(offs_client_t* client,
                                 const uint8_t* data,
                                 size_t len);
int offs_client_put_stream_end(offs_client_t* client,
                                offs_put_response_cb_t callback,
                                void* ctx);

int offs_client_get(offs_client_t* client,
                     const char* ori_string,
                     offs_get_data_cb_t data_cb,
                     offs_get_end_cb_t end_cb,
                     offs_error_cb_t error_cb,
                     void* ctx);

// Optional: range request
int offs_client_get_range(offs_client_t* client,
                          const char* ori_string,
                          size_t range_start,
                          size_t range_end,
                          offs_get_data_cb_t data_cb,
                          offs_get_end_cb_t end_cb,
                          offs_error_cb_t error_cb,
                          void* ctx);

// Error callback (global)
void offs_client_on_error(offs_client_t* client, offs_error_cb_t cb, void* ctx);
```

### Internal Structure

```c
struct offs_client_t {
    int fd;                     // Socket fd (Unix/TCP)
    uint8_t connected;
    uint8_t is_ws;              // 1 if WebSocket transport
    stream_framer_t* framer;    // Length-prefix frame decoder
    buffer_t* write_buffer;    // Pending writes
    PLATFORMLOCKTYPE(lock);
    PLATFORMTHREADTYPE(recv_thread);
    volatile uint8_t running;
    uint8_t read_buf[65536];
    size_t read_len;
    // Callbacks
    offs_put_response_cb_t put_cb;     void* put_cb_ctx;
    offs_get_data_cb_t get_data_cb;    void* get_data_cb_ctx;
    offs_get_end_cb_t get_end_cb;      void* get_end_cb_ctx;
    offs_error_cb_t error_cb;          void* error_cb_ctx;
};
```

### How it works

1. `offs_client_connect()` parses the URL prefix (`unix://`, `tcp://`, `ws://`), opens the appropriate socket, and starts a background receive thread
2. For Unix/TCP: the receive thread reads from `fd`, feeds bytes into `stream_framer_t`, extracts CBOR frames, decodes them, and invokes the appropriate callback
3. For WebSocket: the receive thread handles WS frame parsing (opcodes, masking), extracts the inner CBOR payload
4. `offs_client_put()` encodes a `PUT_REQUEST` frame, sends it (with data for buffered, without for streaming), receives `PUT_RESPONSE` with the ORI string
5. `offs_client_get()` encodes a `GET_REQUEST` frame, then the receive thread calls `get_data_cb` for each `GET_DATA` frame and `get_end_cb` on `GET_END`
6. Streaming PUT: `put_stream_start` → `put_stream_data` (sends `PUT_DATA` frames) → `put_stream_end` (sends `PUT_END`)

### Language Bindings (Future)

The C header is the canonical FFI surface. Future language bindings:
- **Python**: cffi ABI-mode, loads `liboffs_client.so`, wraps in Pythonic class
- **Node.js**: Pure JS WebSocket client that speaks the same CBOR wire format
- **Swift**: XPC or direct TCP connection with native CBOR codec

Each binding reuses `client_api_wire.h/.c` for the wire format but implements its own transport layer.