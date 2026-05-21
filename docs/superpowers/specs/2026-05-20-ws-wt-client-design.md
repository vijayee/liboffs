# WS and WT Client Library Support

**Goal:** Add WebSocket (`ws://`/`wss://`) and WebTransport (`wt://`/`wts://`) transport support to the C client library, so callers can connect to any OFFS server type via `offs_client_connect()`.

**Architecture:** Extend the existing `offs_client_t` with a transport-type discriminator and a union of transport-specific state. The recv/send paths branch on transport type. WS handles the HTTP upgrade handshake and WebSocket binary framing internally. WT uses MsQuic for QUIC transport with the same 4-byte-length-prefix CBOR protocol as Unix/TCP.

**Tech Stack:** C, libcbor, OpenSSL (SHA-1 for WS upgrade + optional TLS), MsQuic (for WT), existing ws_frame module

---

## Transport Discriminator

The transport type enum is internal to `offs_client.c` (not in the public header — callers just pass a URL string). `offs_client_t` gains a `transport_type` field and a union for transport-specific state:

```c
typedef enum {
  OFFS_TRANSPORT_UNIX,
  OFFS_TRANSPORT_TCP,
  OFFS_TRANSPORT_WS,
  OFFS_TRANSPORT_WT,
} offs_transport_type_e;

typedef struct {
  int fd;
  uint8_t connected;
  offs_transport_type_e transport_type;
  union {
    struct { /* unix/tcp — raw socket + stream framer */ } raw;
    struct { /* ws — socket + WS frame state */ } ws;
    struct { /* wt — MsQuic handles */ } wt;
  } transport;
  stream_framer_t* framer;     /* used by unix/tcp/wt, NULL for ws */
  buffer_t* write_buffer;
  pthread_mutex_t lock;
  pthread_t recv_thread;
  volatile uint8_t running;
  uint8_t* read_buf;
  size_t read_buf_size;
  /* callbacks */
  offs_put_response_cb_t put_cb;
  void* put_cb_ctx;
  offs_get_data_cb_t get_data_cb;
  void* get_data_cb_ctx;
  offs_get_end_cb_t get_end_cb;
  void* get_end_cb_ctx;
  offs_error_cb_t error_cb;
  void* error_cb_ctx;
} offs_client_t;
```

Unix and TCP share the same recv thread (raw socket + `stream_framer`). WS has its own recv thread (raw socket + WS frame parsing). WT uses MsQuic callbacks instead of a pthread.

## URL Parsing

`offs_client_connect()` already parses `unix://` and `tcp://`. Extend it to also handle:

- `ws://host:port/path` — WebSocket, plain TCP
- `wss://host:port/path` — WebSocket over TLS
- `wt://host:port` — WebTransport, QUIC without custom cert verification
- `wts://host:port` — WebTransport, QUIC with TLS cert verification

## WebSocket Client

### Connection Flow

1. TCP connect to `host:port` (same as existing `_connect_tcp`)
2. If `wss://`, perform TLS handshake using OpenSSL (`SSL_connect`)
3. Send HTTP upgrade request:
   ```
   GET /offs HTTP/1.1\r\n
   Host: <host>\r\n
   Upgrade: websocket\r\n
   Connection: Upgrade\r\n
   Sec-WebSocket-Key: <random 16 bytes base64>\r\n
   Sec-WebSocket-Version: 13\r\n
   \r\n
   ```
4. Read HTTP response, verify `101 Switching Protocols` status and `Sec-WebSocket-Accept` header
5. Transition to WebSocket binary frame mode

### WS Frame Handling

Client-to-server frames are **masked** (RFC 6455 requirement). Add `ws_frame_build_masked()`:

```c
uint8_t* ws_frame_build_masked(uint8_t opcode, const uint8_t* payload,
                                size_t payload_len, size_t* out_len);
```

Uses a random 4-byte mask key, XORs the payload, and sets the MASK bit. The existing `ws_frame_parse()` already handles both masked and unmasked frames, so it works for parsing server responses unchanged.

### Send Path (`_ws_send_frame`)

1. Serialize CBOR item
2. Wrap in a masked WebSocket binary frame via `ws_frame_build_masked()`
3. Send over socket (or `SSL_write` for WSS)

### Recv Path (`_ws_recv_thread`)

1. Read raw bytes from socket (or `SSL_read` for WSS)
2. Accumulate in a buffer
3. Call `ws_frame_parse()` to extract complete frames
4. For BINARY frames: parse CBOR, dispatch via `_handle_frame()`
5. For CLOSE frames: set `connected = 0`, exit thread
6. For PING frames: send PONG response
7. Discard TEXT frames with a close code 1003

### WSS (TLS) Support

When URL scheme is `wss://`:
- Create an `SSL*` from `SSL_CTX` with `TLS_client_method()`
- Set `SSL_set_tlsext_host_name()` for SNI
- Perform `SSL_connect()` after the TCP connect
- All subsequent I/O uses `SSL_read()` / `SSL_write()`
- `SSL_CTX` and `SSL*` stored in `transport.ws`

## WebTransport Client

### Connection Flow

1. Create MsQuic registration and configuration (ALPN `"offs"`)
2. Open a QUIC connection to `host:port`
3. Open a bidirectional QUIC stream
4. Use `stream_framer` for CBOR message framing (same as Unix/TCP)

### Send Path

1. Serialize CBOR item
2. Wrap in 4-byte-length-prefix frame via `stream_frame_encode()`
3. Send via MsQuic `StreamSend()` (asynchronous — buffer must remain valid until send complete callback)

### Recv Path

No pthread — MsQuic delivers `QUIC_STREAM_EVENT_RECEIVE` on its own thread. The callback:
1. Copies received bytes into a client-owned buffer
2. Posts a message to the client's actor mailbox (or uses a mutex-protected buffer + condition variable to signal the application thread)

Since `offs_client_t` uses synchronous callbacks (not an actor system), the simplest approach is: MsQuic's receive callback feeds bytes into the `stream_framer`, extracts complete frames, and dispatches them to `_handle_frame()`. Because `_handle_frame` calls user callbacks directly, we need to protect callback state with the existing `pthread_mutex_t lock`.

### WT Transport State

```c
struct {
  HQUIC registration;
  HQUIC configuration;
  HQUIC connection;
  HQUIC stream;
  uint8_t stream_open;     /* 1 after stream is ready */
  uint8_t* send_buf;       /* pending send buffer for async StreamSend */
  size_t send_buf_len;
} wt;
```

### Send Completion

MsQuic `StreamSend` is asynchronous. The client must keep the send buffer alive until `QUIC_STREAM_EVENT_SEND_COMPLETE`. Use a simple pattern: allocate a buffer, pass it to `StreamSend` with the send-complete callback freeing it. The send-path mutex ensures only one outstanding send at a time (matches the current `_send_frame` pattern which holds `client->lock`).

## Files to Create/Modify

### New files

| File | Purpose |
|---|---|
| `src/ClientAPI/WS/ws_frame.h` (modify) | Add `ws_frame_build_masked()` declaration |
| `src/ClientAPI/WS/ws_frame.c` (modify) | Add `ws_frame_build_masked()` implementation |

### Modified files

| File | Changes |
|---|---|
| `src/ClientLibs/c/offs_client.h` | No changes — transport type enum is internal |
| `src/ClientLibs/c/offs_client.c` | Major changes: transport union, WS upgrade, WS frame I/O, WSS TLS, WT MsQuic connection, URL parsing for `ws://`/`wss://`/`wt://` |
| `CMakeLists.txt` | Link OpenSSL and MsQuic for client library target; add `ws_frame.c` to client lib sources |

## Error Handling

- WS upgrade failure: set `connected = 0`, return NULL from `offs_client_connect()`
- WSS TLS failure: same as WS upgrade failure, plus `SSL_free()` / `SSL_CTX_free()`
- WT QUIC connection failure: clean up MsQuic handles, return NULL
- Frame parse errors: log and disconnect
- PING frames: auto-respond with PONG
- CLOSE frames: echo close payload, set `connected = 0`, exit recv thread
- MsQuic stream errors: set `connected = 0`, signal disconnect

## Test Plan

### `test/test_offs_client.cpp` — New tests

- **WsConnectAndDisconnect** — `ws://` connect and disconnect
- **WsPutBuffered** — PUT over WebSocket
- **WsGetAfterPut** — GET round-trip over WebSocket
- **WssConnectAndDisconnect** — `wss://` connect and disconnect (if TLS certs available; skip if not)
- **WtConnectAndDisconnect** — `wt://` connect and disconnect (if MsQuic available)

Each WS test creates a `ws_transport_t` server, connects the client via `ws://127.0.0.1:port/offs`, and verifies the same PUT/GET flow as the Unix tests. WT tests create a `wt_transport_t` server.

### Test infrastructure additions

- WS test fixture: same as existing `TestOffsClient` but with `ws_transport_t` instead of `unix_transport_t`
- WT test fixture: same but with `wt_transport_t` (gated behind `HAS_MSQUIC`)
- A `_next_ws_port` counter (starting at 40080) and `_next_wt_port` counter (starting at 41080) to avoid port conflicts

## Open Questions

- **MsQuic initialization**: The WT transport server already loads MsQuic via `offs_msquic_open()` singleton. The client can reuse this. Need to confirm the client can create its own registration/configuration for outbound connections.
- **TLS certificate for WSS tests**: Tests need a self-signed cert/key pair. Generate at test setup time, or ship a fixed test cert.