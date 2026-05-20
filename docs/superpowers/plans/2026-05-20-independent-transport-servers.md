# Independent Transport Servers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build independent server-actor-based transports (Unix, TCP, WS, WT) for the OFF system, each with direct block_cache access, plus a C client library — and disable the dangerous DELETE operation.

**Architecture:** Each transport is a standalone server actor (pd_loop thread, accept watcher, destroy stack, per-connection actors). They share only `client_api_wire.h/.c` for CBOR encode/decode. No adapter layer. The HTTP server stays unchanged except DELETE is disabled. A C ClientLib connects to any transport via URL scheme.

**Tech Stack:** C11, libcbor, poll-dancer, OpenSSL (for TLS), scheduler/actor system, stream_framer for length-prefix framing

---

## File Structure

### New files to create:
- `src/ClientAPI/Unix/unix_transport.h` — Unix server actor header
- `src/ClientAPI/Unix/unix_transport.c` — Unix server actor implementation
- `src/ClientAPI/Unix/unix_connection.h` — Per-connection actor header
- `src/ClientAPI/Unix/unix_connection.c` — Per-connection actor implementation
- `src/ClientLibs/c/offs_client.h` — Public C client header
- `src/ClientLibs/c/offs_client.c` — C client implementation
- `src/ClientLibs/c/CMakeLists.txt` — Build static/shared library

### Files to delete:
- `src/ClientAPI/client_api_handler.h`
- `src/ClientAPI/client_api_handler.c`
- `src/ClientAPI/client_api_session.h`
- `src/ClientAPI/client_api_session.c`
- `src/ClientAPI/HTTP/http_api_adapter.h`
- `src/ClientAPI/HTTP/http_api_adapter.c`

### Files to modify:
- `src/ClientAPI/client_api_wire.h` — Remove DELETE types
- `src/ClientAPI/client_api_wire.c` — Remove DELETE encode/decode
- `src/ClientAPI/client_api_transport.h` — Remove HTTP type, clean up
- `src/ClientAPI/HTTP/off_routes.c` — Remove DELETE handler and route registration
- `examples/off_server/main.c` — Remove DELETE route registration

---

## Task 1: Remove DELETE operation from all code

Disable DELETE across the board: wire format, handler, session, HTTP routes, and adapter. This is a safety fix — deleting shared blocks corrupts other files.

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h`
- Modify: `src/ClientAPI/client_api_wire.c`
- Modify: `src/ClientAPI/HTTP/off_routes.c`
- Delete: `src/ClientAPI/client_api_handler.h`
- Delete: `src/ClientAPI/client_api_handler.c`
- Delete: `src/ClientAPI/client_api_session.h`
- Delete: `src/ClientAPI/client_api_session.c`
- Delete: `src/ClientAPI/HTTP/http_api_adapter.h`
- Delete: `src/ClientAPI/HTTP/http_api_adapter.c`

- [ ] **Step 1: Remove DELETE wire format types from client_api_wire.h**

Remove these lines:
- `#define CLIENT_API_DELETE_REQUEST 9`
- `#define CLIENT_API_DELETE_RESPONSE 10`
- The `client_api_delete_request_t` struct
- The `client_api_delete_response_t` struct
- The encode/decode/destroy function declarations for delete_request and delete_response

Renumber `CLIENT_API_ERROR` from 11 to 9 (or just keep it at 11 — either works since these are identifiers not sequential indices). Keep it at 11 to avoid wire compatibility confusion.

- [ ] **Step 2: Remove DELETE encode/decode implementations from client_api_wire.c**

Remove the `client_api_delete_request_encode`, `client_api_delete_request_decode`, `client_api_delete_request_destroy`, `client_api_delete_response_encode`, `client_api_delete_response_decode`, `client_api_delete_response_destroy` function implementations.

Remove the `CLIENT_API_DELETE_REQUEST` case from `client_api_wire_get_type()` if it exists.

- [ ] **Step 3: Remove DELETE handler from off_routes.c**

In `src/ClientAPI/HTTP/off_routes.c`:
- Remove the `_off_delete_handler` static function (lines ~787-801)
- Remove the `http_server_delete_with_data` route registration (lines ~836-837)

- [ ] **Step 4: Delete adapter and session files**

Remove these files entirely:
- `src/ClientAPI/client_api_handler.h`
- `src/ClientAPI/client_api_handler.c`
- `src/ClientAPI/client_api_session.h`
- `src/ClientAPI/client_api_session.c`
- `src/ClientAPI/HTTP/http_api_adapter.h`
- `src/ClientAPI/HTTP/http_api_adapter.c`

- [ ] **Step 5: Remove DELETE operation enum from client_api_wire.h**

If `client_api_operation_e` exists in client_api_wire.h with `CLIENT_API_OP_DELETE`, remove it. (It may only be in client_api_handler.h which we're deleting, so check first.)

- [ ] **Step 6: Remove HTTP type from client_api_transport.h**

In `src/ClientAPI/client_api_transport.h`, remove `CLIENT_API_TRANSPORT_HTTP` from the enum. HTTP is not a CBOR transport — it stays independent as `off_routes.c`.

- [ ] **Step 7: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake --build . 2>&1 | head -50`

Expected: Clean build with no errors. The DELETE handler and adapter files are gone, the wire format no longer has DELETE types.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor: remove DELETE operation and adapter layer

DELETE is unsafe — blocks are shared across files, so deleting
blocks for one file corrupts others. Remove DELETE from wire
format, HTTP routes, handler, and session.

Delete the adapter layer (client_api_handler, client_api_session,
http_api_adapter) — each transport will have its own pipeline
logic directly."
```

---

## Task 2: Clean up client_api_transport.h

Simplify the transport interface to just what independent transports need.

**Files:**
- Modify: `src/ClientAPI/client_api_transport.h`

- [ ] **Step 1: Simplify transport.h**

The current `client_api_transport.h` has `client_api_transport_t` with `client_api_context_t* ctx` — but that type was in the deleted handler. Remove the context reference and any dependencies on deleted types. Keep only what's needed:

```c
#ifndef OFFS_CLIENT_API_TRANSPORT_H
#define OFFS_CLIENT_API_TRANSPORT_H

#include "../../Util/atomic_compat.h"
#include "../../Util/threadding.h"
#include <poll-dancer/poll-dancer.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
  CLIENT_API_TRANSPORT_UNIX,
  CLIENT_API_TRANSPORT_TCP,
  CLIENT_API_TRANSPORT_WS,
  CLIENT_API_TRANSPORT_WT
} client_api_transport_type_e;

typedef struct client_api_transport_t client_api_transport_t;

struct client_api_transport_t {
  const char* name;
  client_api_transport_type_e type;
  PLATFORMTHREADTYPE thread;
  pd_loop_t* loop;
  ATOMIC(uint8_t) running;
  PLATFORMLOCKTYPE(destroy_lock);

  int (*start)(client_api_transport_t* self);
  int (*stop)(client_api_transport_t* self);
  void (*destroy)(client_api_transport_t* self);
};

typedef struct {
  const char* host;
  uint16_t port;
  const char* socket_path;
  const char* cert_path;
  const char* key_path;
  size_t max_connections;
} client_api_transport_config_t;

#endif
```

Note: removed `scheduler_pool_t* pool`, `client_api_context_t* ctx`, `send` function pointer, and reference to Actor/actor.h. Each transport struct will embed what it needs directly.

- [ ] **Step 2: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake --build . 2>&1 | head -50`

Expected: Clean build.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "refactor: simplify client_api_transport.h for independent transports

Remove context pointer, send function, and pool reference —
each transport struct will embed what it needs directly."
```

---

## Task 3: Implement Unix domain socket transport

The first independent transport. Follows the http_server pattern exactly: server actor, pd_loop thread, accept watcher, destroy stack, per-connection actors.

**Files:**
- Create: `src/ClientAPI/Unix/unix_transport.h`
- Create: `src/ClientAPI/Unix/unix_transport.c`
- Create: `src/ClientAPI/Unix/unix_connection.h`
- Create: `src/ClientAPI/Unix/unix_connection.c`

- [ ] **Step 1: Create unix_transport.h**

```c
#ifndef OFFS_UNIX_TRANSPORT_H
#define OFFS_UNIX_TRANSPORT_H

#include "../../Scheduler/scheduler.h"
#include "../../BlockCache/block_cache.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/threadding.h"
#include "../../Util/vec.h"
#include <poll-dancer/poll-dancer.h>
#include <stdint.h>
#include <stddef.h>

typedef struct unix_connection_t unix_connection_t;
typedef vec_t(unix_connection_t*) vec_unix_connection_t;

typedef struct server_destroy_node_t {
  pd_watcher_t* watcher;
  struct server_destroy_node_t* next;
} server_destroy_node_t;

typedef struct unix_transport_t {
  actor_t actor;
  pd_loop_t* loop;
  PLATFORMTHREADTYPE thread;
  ATOMIC(uint8_t) running;
  int listen_fd;
  pd_watcher_t* listen_watcher;
  vec_unix_connection_t connections;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  size_t max_connections;
  ATOMIC(size_t) active_connections;
  PLATFORMLOCKTYPE(destroy_lock);
  server_destroy_node_t* destroy_head;
  char* socket_path;
} unix_transport_t;

unix_transport_t* unix_transport_create(scheduler_pool_t* pool,
                                         block_cache_t* bc,
                                         ofd_cache_t* ofd_cache,
                                         tuple_cache_t* tc,
                                         const char* socket_path,
                                         size_t max_connections);
void unix_transport_destroy(unix_transport_t* transport);
void unix_transport_start(unix_transport_t* transport);
void unix_transport_stop(unix_transport_t* transport);

#endif
```

- [ ] **Step 2: Create unix_transport.c — server skeleton**

Follow `http_server.c` pattern exactly. Implement:
- `unix_transport_create()` — allocate, init actor, create pd_loop, bind AF_UNIX socket, listen
- `unix_transport_destroy()` — stop if running, close all connections, free everything
- `_server_thread()` — pd_loop_run_once loop with destroy_stack_drain
- `_accept_callback()` — accept(), create unix_connection_t, push to connections vector
- `_server_dispatch()` — handle UPDATE_WATCHER, STOP_WATCHER messages
- `unix_transport_start()` — set running=1, create thread
- `unix_transport_stop()` — set running=0, pd_loop_async_send, join thread
- Destroy stack: `_destroy_stack_init`, `_destroy_stack_push`, `_destroy_stack_drain`, `_destroy_stack_destroy`

- [ ] **Step 3: Create unix_connection.h**

```c
#ifndef OFFS_UNIX_CONNECTION_H
#define OFFS_UNIX_CONNECTION_H

#include "../../Actor/actor.h"
#include "../../RefCounter/refcounter.h"
#include "../../Network/stream_framer.h"
#include "../../Buffer/buffer.h"
#include "../../Util/atomic_compat.h"
#include <poll-dancer/poll-dancer.h>
#include "../../OFFStreams/writeable_off_stream.h"
#include "../../OFFStreams/writeable_descriptor.h"
#include "../../OFFStreams/block_recipe.h"
#include <stdint.h>
#include <stddef.h>

typedef struct unix_transport_t unix_transport_t;

typedef struct {
  refcounter_t refcounter;
  actor_t actor;
  int fd;
  ATOMIC(pd_watcher_t*) watcher;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  uint8_t write_pending;
  uint8_t is_closing;
  unix_transport_t* transport;
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;

  // Streaming PUT state
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
} unix_connection_t;

unix_connection_t* unix_connection_create(unix_transport_t* transport, int fd);
void unix_connection_destroy(unix_connection_t* connection);
void unix_connection_dispatch(void* state, message_t* msg);

#endif
```

- [ ] **Step 4: Create unix_connection.c — connection skeleton**

Follow `http_connection.c` pattern. Implement:
- `unix_connection_create()` — allocate, init actor, create stream_framer, create write_buffer, start pd_watcher on fd for PD_EVENT_READ
- `unix_connection_destroy()` — check refcounter, cleanup fd, destroy framer, destroy write_buffer, free actor queue nodes, stop/destroy watcher, free connection
- `_read_callback()` — read(fd, buf), stream_framer_feed(), loop stream_framer_next(), for each frame: cbor_load() → decode type byte → dispatch to handler
- `_write_callback()` — write pending data to fd
- `unix_connection_dispatch()` — handle UPDATE_WATCHER, STOP_WATCHER messages
- `unix_connection_write()` — buffer data for writing, update watcher to PD_EVENT_READ|PD_EVENT_WRITE
- `unix_connection_close()` — set is_closing, close fd, schedule cleanup

- [ ] **Step 5: Implement GET handler in unix_connection.c**

```c
// Pipeline context for GET responses
typedef struct {
  refcounter_t refcounter;
  readable_descriptor_t* desc;
  readable_off_stream_t* rs;
  tuple_cache_t* tc;
  unix_connection_t* connection;
  ori_t* ori;
} unix_get_pipeline_t;

static void _unix_get_on_tuple(void* ctx, void* data) {
  unix_get_pipeline_t* pipeline = (unix_get_pipeline_t*)ctx;
  tuple_t* tuple = (tuple_t*)data;
  readable_off_stream_write(pipeline->rs, tuple);
}

static void _unix_get_on_desc_close(void* ctx, void* unused) {
  // deref pipeline, deferred deref desc, free if zero
}

static void _unix_get_on_desc_error(void* ctx, void* error) {
  // encode ERROR frame, send to connection, deactivate stream
}

static void _unix_get_on_rs_data(void* ctx, void* data) {
  // encode GET_DATA CBOR frame, length-prefix it, write to connection fd
}

static void _unix_get_on_rs_close_with_end(void* ctx, void* unused) {
  // encode GET_END CBOR frame, write to connection fd
  // deref pipeline, deferred deref rs, free if zero
}

static void _unix_get_on_rs_error(void* ctx, void* error) {
  // encode ERROR frame, send to connection, deactivate stream
}

static void _unix_handle_get(unix_connection_t* conn, client_api_get_request_t* request) {
  // Parse ORI from request->ori_string
  // Create ori_t from URL
  // Encode GET_RESPONSE_START frame with content type, content length, range info
  // Create readable_off_stream + readable_descriptor
  // Set up pipeline with stream_subscribe/once callbacks
  // readable_descriptor_push(desc)
}
```

The key pattern: pipeline callbacks encode CBOR responses using `client_api_wire.h` functions, then call `stream_frame_encode()` to add the 4-byte length prefix, then write to the connection fd via `unix_connection_write()`.

- [ ] **Step 6: Implement PUT handler in unix_connection.c**

```c
static void _unix_put_on_descriptor_close(void* ctx, void* unused) {
  // Build ORI from file_hash + descriptor_hash
  // Encode PUT_RESPONSE CBOR frame with ORI string
  // Length-prefix and write to connection fd
  // Cleanup: deref recipe, deferred deref desc/ws, free put_ctx fields
}

static void _unix_put_on_descriptor_data(void* ctx, void* data) {
  // Store descriptor hash
}

static void _unix_put_on_stream_close(void* ctx, void* unused) {
  // Close writeable_descriptor
}

static void _unix_put_on_stream_data(void* ctx, void* data) {
  // First 32-byte payload = file_hash, rest = tuples for descriptor
}

static void _unix_handle_put(unix_connection_t* conn, client_api_put_request_t* request) {
  // Create writeable_off_stream + writeable_descriptor pipeline
  // Subscribe to stream and descriptor events
  // If request has data (buffered PUT): write data immediately, finalize
  // If request has no data (streaming PUT): store put_ws/put_desc in connection
  //   set conn->put_streaming = 1, conn->put_* = pipeline context
}
```

- [ ] **Step 7: Implement streaming PUT handlers**

```c
static void _unix_handle_put_data(unix_connection_t* conn, const uint8_t* data, size_t size) {
  // buffer_t* chunk = buffer_create_from_pointer_copy(data, size);
  // writeable_off_stream_write(conn->put_ws, chunk);
  // buffer_destroy(chunk);
}

static void _unix_handle_put_end(unix_connection_t* conn) {
  // writeable_off_stream_finalize(conn->put_ws);
  // conn->put_streaming = 0;
}
```

- [ ] **Step 8: Implement frame dispatch in _read_callback**

In the `_read_callback` function that processes frames from `stream_framer_next()`:

```c
cbor_item_t* frame = cbor_load(data, len, NULL);
uint8_t type = client_api_wire_get_type(frame);

switch (type) {
  case CLIENT_API_PUT_REQUEST: {
    client_api_put_request_t msg;
    if (client_api_put_request_decode(frame, &msg) != 0) {
      // send ERROR frame
      break;
    }
    _unix_handle_put(conn, &msg);
    client_api_put_request_destroy(&msg);
    break;
  }
  case CLIENT_API_PUT_DATA: {
    client_api_put_data_t msg;
    if (client_api_put_data_decode(frame, &msg) != 0) {
      break;
    }
    if (conn->put_streaming) {
      _unix_handle_put_data(conn, msg.data, msg.size);
    }
    client_api_put_data_destroy(&msg);
    break;
  }
  case CLIENT_API_PUT_END: {
    if (conn->put_streaming) {
      _unix_handle_put_end(conn);
    }
    break;
  }
  case CLIENT_API_GET_REQUEST: {
    client_api_get_request_t msg;
    if (client_api_get_request_decode(frame, &msg) != 0) {
      break;
    }
    _unix_handle_get(conn, &msg);
    client_api_get_request_destroy(&msg);
    break;
  }
  default: {
    // Unknown frame type — send ERROR
    break;
  }
}
cbor_decref(&frame);
```

- [ ] **Step 9: Implement async GET (OFD directory resolution)**

For GET requests with directory content types (`offsystem/directory`), the connection actor needs to handle async responses from `ofd_cache_resolve()` and `block_cache_get()`.

```c
// In unix_connection_dispatch():
case OFD_CACHE_RESOLVE_RESULT: {
  // Handle directory resolution result
  // If ORI found: set up stream pipeline with resolved ORI
  // If not found: send ERROR frame
  break;
}
case CACHE_GET_RESULT: {
  // Handle raw OFD block fetch (?ofd=raw equivalent)
  // Encode block data as CBOR and send as GET_DATA
  // Send GET_END
  break;
}
```

The connection actor gets an `actor_t` field precisely for receiving these async messages. The actor's dispatch function switches on message type.

- [ ] **Step 10: Build and test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && cmake --build . 2>&1 | tail -30`

Expected: Clean build. The GLOB_RECURSE picks up the new files automatically.

- [ ] **Step 11: Run valgrind on existing tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && valgrind --leak-check=full ctest --output-on-failure 2>&1 | tail -50`

Expected: No new leaks introduced. Pre-existing leaks (timer_actor 24B, block 16B, index/get_dir 508B, cbor 640B) are acceptable.

- [ ] **Step 12: Commit**

```bash
git add -A
git commit -m "feat: add Unix domain socket transport for OFFS client API

Independent server actor with per-connection actors, direct
block_cache access, CBOR wire format via stream_framer.
Supports buffered PUT, streaming PUT, GET with async directory
resolution. Follows http_server pattern (pd_loop, destroy
stack, actor dispatch)."
```

---

## Task 4: Implement TCP/TLS transport

Nearly identical to Unix transport but uses AF_INET and optional SSL. Same per-connection actor pattern.

**Files:**
- Create: `src/ClientAPI/TCP/tcp_transport.h`
- Create: `src/ClientAPI/TCP/tcp_transport.c`
- Create: `src/ClientAPI/TCP/tcp_connection.h`
- Create: `src/ClientAPI/TCP/tcp_connection.c`

- [ ] **Step 1: Create tcp_transport.h**

Same as `unix_transport.h` but:
- `struct sockaddr_in` instead of `struct sockaddr_un`
- `host` and `port` instead of `socket_path`
- `SSL_CTX* ssl_ctx` for optional TLS
- `host` string stored for bind address

- [ ] **Step 2: Create tcp_transport.c**

Copy `unix_transport.c` and modify:
- `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` instead of `AF_UNIX`
- `bind()` to `struct sockaddr_in` with `inet_pton(host)` and `htons(port)`
- Optional: `SSL_CTX_new()` in create if cert_path/key_path provided
- Same accept callback, connection management, destroy stack pattern

- [ ] **Step 3: Create tcp_connection.h**

Same as `unix_connection.h` but add:
- `SSL* ssl` field for TLS connections
- `uint8_t is_ssl` flag
- Connection dispatch and I/O use `SSL_read()`/`SSL_write()` when `is_ssl == 1`
- SSL handshake in connection creation

- [ ] **Step 4: Create tcp_connection.c**

Copy `unix_connection.c` and modify:
- Read callback: use `SSL_read()` when `conn->is_ssl`, plain `read()` otherwise
- Write callback: use `SSL_write()` when `conn->is_ssl`, plain `write()` otherwise
- Connection create: if `transport->ssl_ctx != NULL`, create `SSL*`, do `SSL_set_fd()` + `SSL_accept()`
- Connection destroy: `SSL_free()` if `conn->ssl != NULL`
- Same CBOR frame dispatch, same PUT/GET pipeline logic (identical to unix_connection.c)

The GET and PUT pipeline logic should be identical between unix and tcp connections. Consider extracting into shared helper functions if the code duplication becomes significant, but for now, keep each transport self-contained per the design.

- [ ] **Step 5: Build and test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && cmake --build . 2>&1 | tail -30`

Expected: Clean build.

- [ ] **Step 6: Run valgrind**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && valgrind --leak-check=full ctest --output-on-failure 2>&1 | tail -50`

Expected: No new leaks.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: add TCP/TLS transport for OFFS client API

Server actor with per-connection actors, optional OpenSSL TLS.
Same pattern as Unix transport: AF_INET, SSL_read/SSL_write,
direct block_cache access, CBOR wire format."
```

---

## Task 5: Implement WebSocket transport

Adds HTTP upgrade handshake + RFC 6455 framing on top of a TCP listener.

**Files:**
- Create: `src/ClientAPI/WS/ws_transport.h`
- Create: `src/ClientAPI/WS/ws_transport.c`
- Create: `src/ClientAPI/WS/ws_connection.h`
- Create: `src/ClientAPI/WS/ws_connection.c`
- Create: `src/ClientAPI/WS/ws_frame.h`
- Create: `src/ClientAPI/WS/ws_frame.c`

- [ ] **Step 1: Create ws_frame.h and ws_frame.c**

RFC 6455 frame parser/builder:

```c
// ws_frame.h
typedef struct {
  uint8_t fin;
  uint8_t opcode;    // 0x1=text, 0x2=binary, 0x8=close, 0x9=ping, 0xA=pong
  uint8_t mask;      // client frames must be masked
  uint64_t payload_len;
  uint8_t* mask_key; // 4 bytes, NULL if not masked
  uint8_t* payload;   // decoded (unmasked) payload data
} ws_frame_t;

// Parse a frame from raw bytes. Returns bytes consumed, or -1 on error.
// If frame is incomplete, returns 0 and sets *frame_len to bytes needed.
ssize_t ws_frame_parse(const uint8_t* data, size_t len, ws_frame_t* frame, size_t* frame_len);

// Build a frame from payload data. Returns heap-allocated buffer, caller must free().
// Server frames are NOT masked.
uint8_t* ws_frame_build(uint8_t opcode, const uint8_t* payload, size_t payload_len, size_t* out_len);

void ws_frame_destroy(ws_frame_t* frame);
```

- [ ] **Step 2: Create ws_transport.h and ws_transport.c**

Same server actor pattern as TCP, but:
- Accepts TCP connections like TCP transport
- Each new connection starts in HTTP upgrade mode
- Uses the existing `http_parser` from `deps/http-parser/` for the initial upgrade request
- On successful upgrade: switches to WebSocket binary frame mode
- Computes `Sec-WebSocket-Accept` response (SHA-1 of key + magic GUID)
- Sends `101 Switching Protocols` response
- After upgrade: each binary WebSocket message is a CBOR frame (no length-prefix needed — WS frames have their own framing)

- [ ] **Step 3: Create ws_connection.h and ws_connection.c**

Two-phase connection:
- Phase 1 (pre-upgrade): raw fd + http_parser, processes HTTP upgrade request
- Phase 2 (post-upgrade): raw fd + ws_frame parser, processes binary WS frames

After upgrade, each binary WS message payload is a CBOR frame — decode directly without stream_framer (no length prefix needed). The connection switches from `_http_upgrade_read_callback` to `_ws_read_callback`.

GET and PUT pipeline logic is identical to Unix/TCP — encode CBOR response frames and send them as binary WS messages.

- [ ] **Step 4: Build and test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && cmake --build . 2>&1 | tail -30`

Expected: Clean build.

- [ ] **Step 5: Run valgrind**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && valgrind --leak-check=full ctest --output-on-failure 2>&1 | tail -50`

Expected: No new leaks.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add WebSocket transport for OFFS client API

HTTP upgrade handshake + RFC 6455 framing. Binary WS messages
carry CBOR frames without length prefix. Two-phase connection:
HTTP parser for upgrade, then ws_frame parser for data."
```

---

## Task 6: Implement WebTransport transport (MsQuic)

Most complex transport — QUIC connections with bidirectional streams. Conditional on HAS_MSQUIC.

**Files:**
- Create: `src/ClientAPI/WT/wt_transport.h`
- Create: `src/ClientAPI/WT/wt_transport.c`

- [ ] **Step 1: Create wt_transport.h**

```c
#ifndef OFFS_WT_TRANSPORT_H
#define OFFS_WT_TRANSPORT_H

#include "../../Scheduler/scheduler.h"
#include "../../BlockCache/block_cache.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../Util/atomic_compat.h"
#include <stdint.h>
#include <stddef.h>

#ifdef HAS_MSQUIC

typedef struct wt_transport_t {
  scheduler_pool_t* pool;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  ATOMIC(uint8_t) running;
  char* host;
  uint16_t port;
  char* cert_path;
  char* key_path;
  size_t max_connections;
  // MsQuic handles: registration, configuration, listener
  void* registration;  // const QUIC_API_TABLE* + HQUIC registration
  void*configuration;   // HQUIC configuration
  void* listener;      // HQUIC listener
  // Per-session state managed by MsQuic callbacks
} wt_transport_t;

wt_transport_t* wt_transport_create(scheduler_pool_t* pool,
                                     block_cache_t* bc,
                                     ofd_cache_t* ofd_cache,
                                     tuple_cache_t* tc,
                                     const char* host,
                                     uint16_t port,
                                     const char* cert_path,
                                     const char* key_path,
                                     size_t max_connections);
void wt_transport_destroy(wt_transport_t* transport);
int wt_transport_start(wt_transport_t* transport);
void wt_transport_stop(wt_transport_t* transport);

#endif // HAS_MSQUIC
#endif // OFFS_WT_TRANSPORT_H
```

- [ ] **Step 2: Create wt_transport.c**

Wrapped in `#ifdef HAS_MSQUIC`. Implementation follows `src/Network/quic_listener.c` patterns:
- Uses `msquic_singleton` for MsQuic API initialization
- Creates `HQUIC` registration and configuration with ALPN `"offs"` 
- Opens listener on specified host:port
- Connection callback creates per-session state
- Stream callback receives data → `stream_framer_feed()` → extract CBOR frames → dispatch
- Response sender writes CBOR frames on QUIC stream
- No pd_loop — MsQuic manages its own worker threads via callbacks

This is the most complex transport. The per-session and per-stream state management is handled by MsQuic callbacks rather than our own actors, but the CBOR dispatch logic is the same.

- [ ] **Step 3: Build (conditional)**

The file compiles to empty when `HAS_MSQUIC` is not defined. When it is defined, ensure MsQuic headers are available and linked.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add WebTransport transport stub (MsQuic, conditional)

QUIC-based transport using MsQuic for connection and stream
management. Per-session state via MsQuic callbacks. CBOR frames
on bidirectional streams. Compiles to empty when HAS_MSQUIC
is not defined."
```

---

## Task 7: Implement C Client Library

A C client that connects to OFFS servers via Unix, TCP, or WebSocket transports. Reuses `client_api_wire.h/.c` for CBOR encoding/decoding.

**Files:**
- Create: `src/ClientLibs/c/offs_client.h`
- Create: `src/ClientLibs/c/offs_client.c`
- Create: `src/ClientLibs/c/CMakeLists.txt`

- [ ] **Step 1: Create offs_client.h**

Public C API (see design doc for full API surface):

```c
#ifndef OFFS_CLIENT_H
#define OFFS_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct offs_client_t offs_client_t;

// Callbacks
typedef void (*offs_put_response_cb_t)(void* ctx, const char* ori_string);
typedef void (*offs_get_data_cb_t)(void* ctx, const uint8_t* data, size_t len);
typedef void (*offs_get_end_cb_t)(void* ctx);
typedef void (*offs_error_cb_t)(void* ctx, uint8_t status_code, const char* message);

// Connection
offs_client_t* offs_client_connect(const char* transport_url);
void offs_client_disconnect(offs_client_t* client);

// Buffered PUT — data included in request
int offs_client_put(offs_client_t* client,
                    const char* content_type,
                    const char* file_name,
                    size_t stream_length,
                    const uint8_t* data,
                    size_t data_len,
                    offs_put_response_cb_t callback,
                    void* ctx);

// Streaming PUT — start, data chunks, end
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

// GET
int offs_client_get(offs_client_t* client,
                     const char* ori_string,
                     offs_get_data_cb_t data_cb,
                     offs_get_end_cb_t end_cb,
                     offs_error_cb_t error_cb,
                     void* ctx);

// GET with range
int offs_client_get_range(offs_client_t* client,
                          const char* ori_string,
                          size_t range_start,
                          size_t range_end,
                          offs_get_data_cb_t data_cb,
                          offs_get_end_cb_t end_cb,
                          offs_error_cb_t error_cb,
                          void* ctx);

// Error callback (global for this connection)
void offs_client_on_error(offs_client_t* client, offs_error_cb_t cb, void* ctx);

#ifdef __cplusplus
}
#endif
#endif // OFFS_CLIENT_H
```

- [ ] **Step 2: Create offs_client.c**

Implementation follows Poseidon's `poseidon_client.c` pattern:

```c
struct offs_client_t {
  int fd;
  uint8_t connected;
  uint8_t is_ws;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  uint8_t write_pending;
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

Key functions:
- `offs_client_connect()`: Parse URL (`unix://`, `tcp://`, `ws://`), create socket, connect, start receive thread
- Receive thread: `read()` → `stream_framer_feed()` → `stream_framer_next()` → decode CBOR → invoke callback
- For WS: handle WS frame parsing after initial HTTP upgrade
- `offs_client_put()`: Encode PUT_REQUEST frame, length-prefix it, send over socket
- `offs_client_get()`: Encode GET_REQUEST frame, set callbacks for GET_RESPONSE_START/GET_DATA/GET_END
- `offs_client_disconnect()`: set running=0, join thread, close fd, free

- [ ] **Step 3: Create CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.10)
project(offs_client C)

set(CMAKE_C_STANDARD 11)

add_library(offs_client STATIC
  offs_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ClientAPI/client_api_wire.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../Network/stream_framer.c
)

target_include_directories(offs_client PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ClientAPI
  ${CMAKE_CURRENT_SOURCE_DIR}/../../Network
  ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/libcbor/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/libcbor/build
)
```

- [ ] **Step 4: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && cmake --build . 2>&1 | tail -30`

Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: add C client library for OFFS

Connects via unix://, tcp://, or ws:// URL schemes. Background
receive thread decodes CBOR frames and invokes callbacks.
Reuses client_api_wire for encoding/decoding."
```

---

## Task 8: Update examples/off_server/main.c

Wire the Unix transport into the example server startup alongside the HTTP server.

**Files:**
- Modify: `examples/off_server/main.c`

- [ ] **Step 1: Add Unix transport to main.c**

Add include for `src/ClientAPI/Unix/unix_transport.h`. After the HTTP server is created and routes are registered, add:

```c
tuple_cache_t* tc = tuple_cache_create(100, pool);
unix_transport_t* unix_transport = unix_transport_create(pool, bc, ofd_cache, tc, "/tmp/offs.sock", 128);
unix_transport_start(unix_transport);
```

Add cleanup in the shutdown path:
```c
unix_transport_stop(unix_transport);
unix_transport_destroy(unix_transport);
tuple_cache_destroy(tc);
```

- [ ] **Step 2: Build and test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && cmake --build . 2>&1 | tail -30`

Expected: Clean build.

- [ ] **Step 3: Run valgrind**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && valgrind --leak-check=full ctest --output-on-failure 2>&1 | tail -50`

Expected: No new leaks.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: wire Unix transport into example server

Start Unix domain socket listener alongside HTTP server.
Share tuple_cache across both transports."
```