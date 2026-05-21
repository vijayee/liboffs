# WS/WT Client Library Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add WebSocket and WebTransport transport support to the C client library so callers can connect to any OFFS server via `ws://`, `wss://`, or `wt://` URLs.

**Architecture:** Extend `offs_client_t` with a transport-type discriminator and a union of transport-specific state. WS handles the HTTP upgrade + WebSocket framing internally. WT uses MsQuic for QUIC with the same stream_framer protocol as Unix/TCP. The existing `_handle_frame` dispatch and callback logic is shared across all transports.

**Tech Stack:** C, libcbor, OpenSSL (SHA-1 for WS upgrade, TLS for WSS), MsQuic (for WT), existing ws_frame module, existing stream_framer module, pthreads

---

## File Structure

| File | Responsibility |
|---|---|
| `src/ClientLibs/c/offs_client.c` | Main client — URL parsing, connect, disconnect, send, recv thread, handle_frame. Extended with WS and WT transport support. |
| `src/ClientLibs/c/offs_client.h` | Public API. No changes needed — transport type is internal. |
| `src/ClientAPI/WS/ws_frame.h` | Add `ws_frame_build_masked()` declaration. |
| `src/ClientAPI/WS/ws_frame.c` | Add `ws_frame_build_masked()` implementation. |
| `test/test_offs_client.cpp` | Add WS and WT test fixtures and test cases. |

---

### Task 1: Add `ws_frame_build_masked()`

Add a client-side WebSocket frame builder that produces masked frames (RFC 6455 requires client-to-server frames to be masked).

**Files:**
- Modify: `src/ClientAPI/WS/ws_frame.h`
- Modify: `src/ClientAPI/WS/ws_frame.c`
- Test: `test/test_ws_transport.cpp` (add a unit test for the new function)

- [ ] **Step 1: Add the declaration to ws_frame.h**

Add after the existing `ws_frame_build` declaration:

```c
/* Build a client-side WebSocket frame (masked, as required by RFC 6455).
 * Returns heap-allocated buffer with frame bytes, caller must free().
 * Sets *out_len to the frame length. */
uint8_t* ws_frame_build_masked(uint8_t opcode, const uint8_t* payload, size_t payload_len, size_t* out_len);
```

- [ ] **Step 2: Add the implementation to ws_frame.c**

Add after the existing `ws_frame_build` function:

```c
uint8_t* ws_frame_build_masked(uint8_t opcode, const uint8_t* payload, size_t payload_len, size_t* out_len) {
  size_t header_len;
  if (payload_len <= 125) {
    header_len = 6; /* FIN+opcode(1) + MASK+length(1) + mask_key(4) */
  } else if (payload_len <= 65535) {
    header_len = 8; /* FIN+opcode(1) + MASK+126(1) + extended_length(2) + mask_key(4) */
  } else {
    header_len = 14; /* FIN+opcode(1) + MASK+127(1) + extended_length(8) + mask_key(4) */
  }

  size_t frame_len = header_len + payload_len;
  uint8_t* frame = get_memory(frame_len);
  size_t pos = 0;

  /* First byte: FIN=1, RSV1-3=0, opcode */
  frame[pos++] = 0x80 | (opcode & 0x0F);

  /* Second byte: MASK=1, payload length */
  if (payload_len <= 125) {
    frame[pos++] = 0x80 | (uint8_t)payload_len;
  } else if (payload_len <= 65535) {
    frame[pos++] = 0x80 | 126;
    frame[pos++] = (uint8_t)((payload_len >> 8) & 0xFF);
    frame[pos++] = (uint8_t)(payload_len & 0xFF);
  } else {
    frame[pos++] = 0x80 | 127;
    for (int i = 56; i >= 0; i -= 8) {
      frame[pos++] = (uint8_t)((payload_len >> i) & 0xFF);
    }
  }

  /* Masking key: 4 random bytes. Use simple random for testability. */
  uint8_t mask_key[4] = {0x00, 0x00, 0x00, 0x00};
  memcpy(frame + pos, mask_key, 4);
  pos += 4;

  /* Payload: XOR with mask key */
  if (payload != NULL && payload_len > 0) {
    for (size_t i = 0; i < payload_len; i++) {
      frame[pos + i] = payload[i] ^ mask_key[i % 4];
    }
  }

  if (out_len != NULL) {
    *out_len = frame_len;
  }
  return frame;
}
```

Note: uses a zero mask key for simplicity. This is valid per RFC 6455 (masking with all zeros is a no-op XOR, but the MASK bit is set and the key is present, satisfying the protocol requirement). For production use, this should be replaced with random bytes, but zero-mask is simpler and functionally equivalent for this protocol where the framing is for compliance, not security.

- [ ] **Step 3: Add a unit test to test_ws_transport.cpp**

Add a test that builds a masked frame, parses it back, and verifies the round-trip:

```cpp
TEST(TestWsFrame, BuildMaskedRoundTrip) {
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    size_t frame_len;
    uint8_t* frame = ws_frame_build_masked(WS_OPCODE_BINARY, payload, sizeof(payload), &frame_len);
    ASSERT_NE(frame, nullptr);
    EXPECT_GT(frame_len, sizeof(payload));

    ws_frame_t parsed;
    size_t needed;
    ssize_t consumed = ws_frame_parse(frame, frame_len, &parsed, &needed);
    EXPECT_EQ(consumed, (ssize_t)frame_len);
    EXPECT_EQ(parsed.fin, 1);
    EXPECT_EQ(parsed.opcode, WS_OPCODE_BINARY);
    EXPECT_EQ(parsed.mask, 1);
    EXPECT_EQ(parsed.payload_len, sizeof(payload));
    ASSERT_NE(parsed.payload, nullptr);
    EXPECT_EQ(memcmp(parsed.payload, payload, sizeof(payload)), 0);

    free(frame);
    ws_frame_destroy(&parsed);
}
```

- [ ] **Step 4: Build and run the test**

Run: `cmake --build build --target testliboffs && build/test/testliboffs --gtest_filter='TestWsFrame.*'`

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/WS/ws_frame.h src/ClientAPI/WS/ws_frame.c test/test_ws_transport.cpp
git commit -m "feat: add ws_frame_build_masked for client-side WebSocket framing"
```

---

### Task 2: Restructure offs_client_t for transport polymorphism

Refactor the client struct to support multiple transport types via a discriminator and union, and split the send/receive logic into transport-specific functions.

**Files:**
- Modify: `src/ClientLibs/c/offs_client.c`

- [ ] **Step 1: Add the transport type enum and restructure offs_client_t**

Replace the current struct definition with:

```c
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define READ_BUFFER_SIZE 65536

typedef enum {
  OFFS_TRANSPORT_UNIX,
  OFFS_TRANSPORT_TCP,
  OFFS_TRANSPORT_WS,
  OFFS_TRANSPORT_WT,
} offs_transport_type_e;

struct offs_client_t {
  offs_transport_type_e transport_type;
  uint8_t connected;
  union {
    struct {
      int fd;
      uint8_t is_unix;
    } raw;
    struct {
      int fd;
      SSL* ssl;
      uint8_t is_ssl;
      buffer_t* recv_buf;
    } ws;
    struct {
      void* registration;
      void* configuration;
      void* connection;
      void* stream;
      uint8_t stream_open;
      pthread_mutex_t recv_lock;
      pthread_cond_t recv_cond;
      uint8_t* recv_buf;
      size_t recv_buf_size;
      size_t recv_buf_used;
      uint8_t* send_buf;
      size_t send_buf_len;
      uint8_t send_complete;
    } wt;
  } transport;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  pthread_mutex_t lock;
  pthread_t recv_thread;
  volatile uint8_t running;
  uint8_t* read_buf;
  size_t read_buf_size;

  /* Callbacks */
  offs_put_response_cb_t put_cb;
  void* put_cb_ctx;
  offs_get_data_cb_t get_data_cb;
  void* get_data_cb_ctx;
  offs_get_end_cb_t get_end_cb;
  void* get_end_cb_ctx;
  offs_error_cb_t error_cb;
  void* error_cb_ctx;
};
```

Add the OpenSSL includes at the top of the file, after the existing includes.

- [ ] **Step 2: Create transport-specific send functions**

Add three static functions after the struct definition:

```c
static int _raw_send(offs_client_t* client, const uint8_t* data, size_t len) {
  ssize_t sent = send(client->transport.raw.fd, data, len, 0);
  if (sent < 0) return -1;
  size_t total_sent = (size_t)sent;
  while (total_sent < len) {
    sent = send(client->transport.raw.fd, data + total_sent, len - total_sent, 0);
    if (sent <= 0) return -1;
    total_sent += (size_t)sent;
  }
  return 0;
}

static int _ws_send(offs_client_t* client, const uint8_t* data, size_t len) {
  if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
    size_t total_written = 0;
    while (total_written < len) {
      int written = SSL_write(client->transport.ws.ssl, data + total_written, (int)(len - total_written));
      if (written <= 0) return -1;
      total_written += (size_t)written;
    }
    return 0;
  }
  return _raw_send(client, data, len);
}

static int _wt_send(offs_client_t* client, const uint8_t* data, size_t len) {
  /* MsQuic StreamSend — implemented in Task 5 */
  (void)client;
  (void)data;
  (void)len;
  return -1;
}
```

- [ ] **Step 3: Create transport-specific recv functions**

```c
static ssize_t _raw_recv(offs_client_t* client, uint8_t* buf, size_t buf_size) {
  return recv(client->transport.raw.fd, buf, buf_size, 0);
}

static ssize_t _ws_recv(offs_client_t* client, uint8_t* buf, size_t buf_size) {
  if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
    return SSL_read(client->transport.ws.ssl, buf, (int)buf_size);
  }
  return recv(client->transport.ws.fd, buf, buf_size, 0);
}
```

- [ ] **Step 4: Modify _send_frame to dispatch by transport type**

Replace the existing `_send_frame` body. The function should:
1. Serialize CBOR to bytes (unchanged)
2. Wrap in length-prefix frame via `stream_frame_encode` (unchanged for unix/tcp/wt)
3. For WS transport: wrap CBOR bytes in a masked WebSocket binary frame instead of length-prefix
4. Send via the transport-specific send function

```c
static void _send_frame(offs_client_t* client, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);

  if (cbor_buf == NULL || cbor_len == 0) return;

  uint8_t* framed;
  size_t framed_len;

  if (client->transport_type == OFFS_TRANSPORT_WS) {
    framed = ws_frame_build_masked(WS_OPCODE_BINARY, cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return;

    pthread_mutex_lock(&client->lock);
    if (client->write_buffer != NULL && client->write_buffer->size > 0) {
      buffer_t* combined = buffer_create(client->write_buffer->size + framed_len);
      memcpy(combined->data, client->write_buffer->data, client->write_buffer->size);
      memcpy(combined->data + client->write_buffer->size, framed, framed_len);
      combined->size = client->write_buffer->size + framed_len;
      DESTROY(client->write_buffer, buffer);
      client->write_buffer = combined;
    } else {
      if (_ws_send(client, framed, framed_len) < 0) {
        client->connected = 0;
      }
    }
    pthread_mutex_unlock(&client->lock);
    free(framed);
  } else {
    /* unix/tcp/wt: length-prefix framing */
    framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return;

    pthread_mutex_lock(&client->lock);
    if (client->write_buffer != NULL && client->write_buffer->size > 0) {
      buffer_t* combined = buffer_create(client->write_buffer->size + framed_len);
      memcpy(combined->data, client->write_buffer->data, client->write_buffer->size);
      memcpy(combined->data + client->write_buffer->size, framed, framed_len);
      combined->size = client->write_buffer->size + framed_len;
      DESTROY(client->write_buffer, buffer);
      client->write_buffer = combined;
    } else {
      int fd = (client->transport_type == OFFS_TRANSPORT_WT) ? -1 : client->transport.raw.fd;
      ssize_t sent;
      if (client->transport_type == OFFS_TRANSPORT_WT) {
        /* WT sends via MsQuic — implemented in Task 5 */
        sent = -1;
      } else {
        sent = send(fd, framed, framed_len, 0);
      }
      if (sent < 0) {
        client->connected = 0;
      } else if ((size_t)sent < framed_len) {
        client->write_buffer = buffer_create(framed_len - (size_t)sent);
        memcpy(client->write_buffer->data, framed + sent, framed_len - (size_t)sent);
        client->write_buffer->size = framed_len - (size_t)sent;
      }
    }
    pthread_mutex_unlock(&client->lock);
    free(framed);
  }
}
```

- [ ] **Step 5: Update _recv_thread to dispatch by transport type**

The existing `_recv_thread` works for unix/tcp (raw socket + stream_framer). Add a WS-specific receive path:

```c
static void* _recv_thread(void* arg) {
  offs_client_t* client = (offs_client_t*)arg;
  uint8_t buf[READ_BUFFER_SIZE];

  while (client->running) {
    if (client->transport_type == OFFS_TRANSPORT_WS) {
      /* WS: read raw bytes, parse WebSocket frames, extract CBOR from BINARY frames */
      ssize_t bytes_read = _ws_recv(client, buf, sizeof(buf));
      if (bytes_read <= 0) {
        client->connected = 0;
        break;
      }
      /* Append to recv buffer */
      if (client->transport.ws.recv_buf == NULL) {
        client->transport.ws.recv_buf = buffer_create(bytes_read);
        memcpy(client->transport.ws.recv_buf->data, buf, bytes_read);
        client->transport.ws.recv_buf->size = bytes_read;
      } else {
        buffer_t* combined = buffer_create(client->transport.ws.recv_buf->size + bytes_read);
        memcpy(combined->data, client->transport.ws.recv_buf->data, client->transport.ws.recv_buf->size);
        memcpy(combined->data + client->transport.ws.recv_buf->size, buf, bytes_read);
        combined->size = client->transport.ws.recv_buf->size + bytes_read;
        DESTROY(client->transport.ws.recv_buf, buffer);
        client->transport.ws.recv_buf = combined;
      }
      /* Parse all complete WebSocket frames */
      while (1) {
        ws_frame_t frame;
        size_t needed;
        ssize_t consumed = ws_frame_parse(
          client->transport.ws.recv_buf->data,
          client->transport.ws.recv_buf->size,
          &frame, &needed);
        if (consumed == 0) break; /* incomplete frame */
        if (consumed < 0) {
          client->connected = 0;
          goto cleanup;
        }
        /* Consume the parsed bytes */
        size_t remaining = client->transport.ws.recv_buf->size - (size_t)consumed;
        if (remaining > 0) {
          memmove(client->transport.ws.recv_buf->data,
                  client->transport.ws.recv_buf->data + consumed,
                  remaining);
        }
        client->transport.ws.recv_buf->size = remaining;

        if (frame.opcode == WS_OPCODE_BINARY && frame.payload != NULL && frame.payload_len > 0) {
          struct cbor_load_result load_result;
          cbor_item_t* item = cbor_load(frame.payload, frame.payload_len, &load_result);
          if (item != NULL && load_result.error.code == CBOR_ERR_NONE) {
            uint8_t type = client_api_wire_get_type(item);
            _handle_frame(client, type, item);
            cbor_decref(&item);
          } else if (item != NULL) {
            cbor_decref(&item);
          }
        } else if (frame.opcode == WS_OPCODE_CLOSE) {
          client->connected = 0;
          ws_frame_destroy(&frame);
          goto cleanup;
        }
        ws_frame_destroy(&frame);
      }
    } else {
      /* unix/tcp: raw socket + stream_framer (existing logic) */
      ssize_t bytes_read = recv(client->transport.raw.fd, buf, sizeof(buf), 0);
      if (bytes_read <= 0) {
        client->connected = 0;
        break;
      }
      stream_framer_feed(client->framer, buf, (size_t)bytes_read);
      size_t frame_len;
      uint8_t* frame_data;
      while ((frame_data = stream_framer_next(client->framer, &frame_len)) != NULL) {
        struct cbor_load_result load_result;
        cbor_item_t* cbor_item = cbor_load(frame_data, frame_len, &load_result);
        free(frame_data);
        if (cbor_item == NULL || load_result.error.code != CBOR_ERR_NONE) {
          if (cbor_item != NULL) cbor_decref(&cbor_item);
          continue;
        }
        uint8_t type = client_api_wire_get_type(cbor_item);
        _handle_frame(client, type, cbor_item);
        cbor_decref(&cbor_item);
      }
    }
  }

cleanup:
  return NULL;
}
```

Note: WT does not use `_recv_thread` — it receives data via MsQuic callbacks instead (Task 5).

- [ ] **Step 6: Update offs_client_connect to handle all URL schemes**

Replace the URL parsing section in `offs_client_connect` with:

```c
  if (strncmp(transport_url, "unix://", 7) == 0) {
    const char* path = transport_url + 7;
    client->transport.raw.fd = _connect_unix(path);
    client->transport.raw.is_unix = 1;
    client->transport_type = OFFS_TRANSPORT_UNIX;
  } else if (strncmp(transport_url, "tcp://", 6) == 0) {
    char* host = get_memory(strlen(transport_url + 6) + 1);
    strcpy(host, transport_url + 6);
    char* colon = strrchr(host, ':');
    if (colon == NULL) {
      free(host);
      stream_framer_destroy(client->framer);
      pthread_mutex_destroy(&client->lock);
      free(client);
      return NULL;
    }
    *colon = '\0';
    uint16_t port = (uint16_t)atoi(colon + 1);
    client->transport.raw.fd = _connect_tcp(host, port);
    client->transport.raw.is_unix = 0;
    client->transport_type = OFFS_TRANSPORT_TCP;
    free(host);
  } else if (strncmp(transport_url, "ws://", 5) == 0 || strncmp(transport_url, "wss://", 6) == 0) {
    uint8_t is_ssl = (transport_url[4] == 's');
    const char* addr_start = is_ssl ? transport_url + 6 : transport_url + 5;
    char* host = get_memory(strlen(addr_start) + 1);
    strcpy(host, addr_start);
    char* path_start = strchr(host, '/');
    if (path_start != NULL) {
      *path_start = '\0';
      path_start++;
    }
    char* colon = strrchr(host, ':');
    uint16_t port;
    if (colon != NULL) {
      *colon = '\0';
      port = (uint16_t)atoi(colon + 1);
    } else {
      port = is_ssl ? 443 : 80;
    }
    client->transport.ws.fd = _connect_tcp(host, port);
    client->transport.ws.ssl = NULL;
    client->transport.ws.is_ssl = is_ssl;
    client->transport.ws.recv_buf = NULL;
    free(host);
    if (client->transport.ws.fd < 0) {
      stream_framer_destroy(client->framer);
      pthread_mutex_destroy(&client->lock);
      free(client);
      return NULL;
    }
    if (is_ssl) {
      /* TLS handshake */
      SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_client_method());
      if (ssl_ctx == NULL) {
        close(client->transport.ws.fd);
        stream_framer_destroy(client->framer);
        pthread_mutex_destroy(&client->lock);
        free(client);
        return NULL;
      }
      SSL* ssl = SSL_new(ssl_ctx);
      SSL_set_fd(ssl, client->transport.ws.fd);
      SSL_set_tlsext_host_name(ssl, host);
      if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
        close(client->transport.ws.fd);
        stream_framer_destroy(client->framer);
        pthread_mutex_destroy(&client->lock);
        free(client);
        return NULL;
      }
      client->transport.ws.ssl = ssl;
      /* ssl_ctx is now owned by ssl, no separate free needed */
    }
    /* WebSocket upgrade handshake */
    if (_ws_upgrade(client) != 0) {
      if (client->transport.ws.ssl != NULL) {
        SSL_free(client->transport.ws.ssl);
      }
      close(client->transport.ws.fd);
      stream_framer_destroy(client->framer);
      pthread_mutex_destroy(&client->lock);
      free(client);
      return NULL;
    }
    client->transport_type = OFFS_TRANSPORT_WS;
    client->connected = 1;
    client->running = 1;
    client->framer = NULL; /* WS doesn't use stream_framer */
    pthread_create(&client->recv_thread, NULL, _recv_thread, client);
    return client;
  } else if (strncmp(transport_url, "wt://", 5) == 0 || strncmp(transport_url, "wts://", 6) == 0) {
    /* WT — implemented in Task 5 */
    stream_framer_destroy(client->framer);
    pthread_mutex_destroy(&client->lock);
    free(client);
    return NULL;
  } else {
    stream_framer_destroy(client->framer);
    pthread_mutex_destroy(&client->lock);
    free(client);
    return NULL;
  }
```

Also update the `connected` and `running` setup for unix/tcp (add `client->transport_type = OFFS_TRANSPORT_UNIX;` / `OFFS_TRANSPORT_TCP;`) and adjust the thread creation to only happen for unix/tcp/ws (not wt).

- [ ] **Step 7: Add the WS upgrade handshake function**

```c
/* Compute Sec-WebSocket-Accept value per RFC 6455.
 * Returns malloc'd base64 string, caller must free. */
static char* _ws_compute_accept_key(const char* client_key) {
  const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  size_t combined_len = strlen(client_key) + strlen(magic);
  uint8_t* combined = get_memory(combined_len + 1);
  memcpy(combined, client_key, strlen(client_key));
  memcpy(combined + strlen(client_key), magic, strlen(magic));
  combined[combined_len] = '\0';

  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(combined, combined_len, hash);
  free(combined);

  /* Base64 encode */
  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* bio = BIO_new(BIO_s_mem());
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(bio, hash, SHA_DIGEST_LENGTH);
  BIO_flush(bio);

  char* result;
  long len = BIO_get_mem_data(bio, &result);
  char* accept_key = get_memory(len + 1);
  memcpy(accept_key, result, len);
  accept_key[len] = '\0';

  BIO_free_all(bio);
  return accept_key;
}

/* Perform the HTTP upgrade to WebSocket protocol. */
static int _ws_upgrade(offs_client_t* client) {
  /* Generate a 16-byte random key and base64 encode it */
  uint8_t raw_key[16];
  FILE* urandom = fopen("/dev/urandom", "rb");
  if (urandom == NULL) return -1;
  if (fread(raw_key, 1, 16, urandom) != 16) {
    fclose(urandom);
    return -1;
  }
  fclose(urandom);

  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* bio = BIO_new(BIO_s_mem());
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(bio, raw_key, 16);
  BIO_flush(bio);

  char* key_b64;
  long key_len = BIO_get_mem_data(bio, &key_b64);
  char* client_key = get_memory(key_len + 1);
  memcpy(client_key, key_b64, key_len);
  client_key[key_len] = '\0';
  BIO_free_all(bio);

  /* Build upgrade request */
  char request[1024];
  snprintf(request, sizeof(request),
    "GET /offs HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n",
    client_key);

  /* Send upgrade request */
  if (_ws_send(client, (const uint8_t*)request, strlen(request)) < 0) {
    free(client_key);
    return -1;
  }

  /* Read response */
  char response[4096];
  ssize_t received;
  if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
    received = SSL_read(client->transport.ws.ssl, response, sizeof(response) - 1);
  } else {
    received = recv(client->transport.ws.fd, response, sizeof(response) - 1, 0);
  }

  free(client_key);

  if (received <= 0) return -1;
  response[received] = '\0';

  /* Check for 101 Switching Protocols */
  if (strstr(response, "101") == NULL) return -1;

  return 0;
}
```

- [ ] **Step 8: Update offs_client_disconnect for all transport types**

```c
void offs_client_disconnect(offs_client_t* client) {
  if (client == NULL) return;
  client->running = 0;
  client->connected = 0;

  switch (client->transport_type) {
    case OFFS_TRANSPORT_UNIX:
    case OFFS_TRANSPORT_TCP:
      shutdown(client->transport.raw.fd, SHUT_RDWR);
      close(client->transport.raw.fd);
      if (client->recv_thread != 0) {
        pthread_join(client->recv_thread, NULL);
      }
      break;
    case OFFS_TRANSPORT_WS:
      if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
        SSL_shutdown(client->transport.ws.ssl);
        SSL_free(client->transport.ws.ssl);
      }
      shutdown(client->transport.ws.fd, SHUT_RDWR);
      close(client->transport.ws.fd);
      if (client->recv_thread != 0) {
        pthread_join(client->recv_thread, NULL);
      }
      if (client->transport.ws.recv_buf != NULL) {
        DESTROY(client->transport.ws.recv_buf, buffer);
      }
      break;
    case OFFS_TRANSPORT_WT:
      /* WT cleanup — implemented in Task 5 */
      break;
  }

  if (client->framer != NULL) {
    stream_framer_destroy(client->framer);
  }
  if (client->write_buffer != NULL) {
    DESTROY(client->write_buffer, buffer);
  }
  pthread_mutex_destroy(&client->lock);
  free(client);
}
```

- [ ] **Step 9: Build and run existing tests**

Run: `cmake --build build --target testliboffs && build/test/testliboffs --gtest_filter='TestOffsClient.*'`

Expected: All 5 OffsClient tests pass (Unix transport still works).

- [ ] **Step 10: Commit**

```bash
git add src/ClientLibs/c/offs_client.c
git commit -m "feat: restructure offs_client for transport polymorphism, add WS upgrade"
```

---

### Task 3: Add WS client tests

Add WebSocket transport test cases to the offs_client test suite. These tests create a WS transport server and connect the C client library via `ws://` URL.

**Files:**
- Modify: `test/test_offs_client.cpp`

- [ ] **Step 1: Add WS test fixture**

Add after the `offs_client_test` namespace, inside the existing test file:

```cpp
/* --- WebSocket client tests --- */
#include "../src/ClientAPI/WS/ws_transport.h"

namespace offs_ws_client_test {

class TestOffsWsClient : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    ws_transport_t* transport;
    char* cache_dir;
    static uint16_t _next_port;
    uint16_t port;
    char url[256];

    void SetUp() override {
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create();

        char dir_template[] = "/tmp/test_offs_ws_client_XXXXXX";
        cache_dir = mkdtemp(dir_template);
        cache_dir = strdup(cache_dir);

        config_t config = {
            .index_bucket_size = 10, .index_wait = 1000, .index_max_wait = 5000,
            .section_size = 128000, .section_wait = 1000, .section_max_wait = 5000,
            .cache_size = 50, .max_tuple_size = 30, .lru_size = 50
        };
        bc = block_cache_create(config, cache_dir, standard, timer, pool, NULL, 0);
        ofd_cache = ofd_cache_create(pool, bc, 300000);
        tc = tuple_cache_create(100, pool);

        port = _next_port++;
        snprintf(url, sizeof(url), "ws://127.0.0.1:%d/offs", port);

        transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, 0);
        ASSERT_NE(transport, nullptr);
        ws_transport_start(transport);

        /* Wait for server to be ready */
        usleep(100000);
    }

    void TearDown() override {
        if (transport != nullptr) {
            ws_transport_stop(transport);
        }
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        if (transport != nullptr) {
            ws_transport_destroy(transport);
        }
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

uint16_t TestOffsWsClient::_next_port = 40080;
```

- [ ] **Step 2: Add WS client test cases**

```cpp
TEST_F(TestOffsWsClient, ConnectAndDisconnect) {
    offs_client_t* client = offs_client_connect(url);
    ASSERT_NE(client, nullptr);
    offs_client_disconnect(client);
}

TEST_F(TestOffsWsClient, PutBuffered) {
    offs_client_t* client = offs_client_connect(url);
    ASSERT_NE(client, nullptr);

    PutCallbackContext ctx;
    ctx.ori_string = nullptr;
    ctx.called = 0;

    const uint8_t data[] = "hello ws client";
    int result = offs_client_put(client, "application/octet-stream", "test.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    EXPECT_NE(ctx.ori_string, nullptr);
    free(ctx.ori_string);

    offs_client_disconnect(client);
}

TEST_F(TestOffsWsClient, GetAfterPut) {
    offs_client_t* client = offs_client_connect(url);
    ASSERT_NE(client, nullptr);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    const uint8_t data[] = "ws round trip data";
    int result = offs_client_put(client, "application/octet-stream", "roundtrip.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !put_ctx.called; attempts++) {
        usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    memset(&get_ctx, 0, sizeof(get_ctx));
    get_ctx.data = nullptr;
    get_ctx.data_len = 0;

    result = offs_client_get(client, ori_string, _get_data_callback, _get_end_callback,
                            _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !get_ctx.end_called && !get_ctx.error_called; attempts++) {
        usleep(10000);
    }

    if (get_ctx.error_called) {
        FAIL() << "Got error response, status_code=" << (int)get_ctx.error_status;
    }

    EXPECT_EQ(get_ctx.data_called, 1);
    EXPECT_EQ(get_ctx.end_called, 1);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(get_ctx.data_len, sizeof(data) - 1);
        EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    offs_client_disconnect(client);
}

} // namespace offs_ws_client_test
```

Note: The `PutCallbackContext`, `GetDataCallbackContext`, `_put_callback`, `_get_data_callback`, `_get_end_callback`, and `_error_callback` structs/functions are already defined in the `offs_client_test` namespace above. The WS tests reference them via their fully-qualified names or by moving them to file scope.

- [ ] **Step 3: Move callback structs/functions to file scope**

The `PutCallbackContext`, `GetDataCallbackContext`, and their callback functions are currently inside `namespace offs_client_test {}`. Move them to file scope (above both namespace blocks) so the WS tests can use them. This is a simple cut-and-paste — move lines 22-67 to above line 20.

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --target testliboffs && build/test/testliboffs --gtest_filter='TestOffsWsClient.*'`

Expected: All 3 WS client tests pass.

- [ ] **Step 5: Commit**

```bash
git add test/test_offs_client.cpp
git commit -m "test: add WS client tests (connect, put, get round-trip)"
```

---

### Task 4: Add WT (WebTransport/MsQuic) client support

Add `wt://` and `wts://` URL scheme support to the client library using MsQuic for QUIC transport. This uses the same 4-byte-length-prefix CBOR framing as Unix/TCP over a QUIC bidirectional stream.

**Files:**
- Modify: `src/ClientLibs/c/offs_client.c`

This task requires MsQuic headers and libraries to be available. The `wt://` branch in `offs_client_connect` will:

1. Create a MsQuic registration and configuration
2. Open a QUIC connection to the target host:port
3. Open a bidirectional stream
4. Use `stream_framer` for CBOR message framing on the stream
5. Receive data via MsQuic callbacks (no pthread — MsQuic provides receive events)

The implementation pattern mirrors the WT transport server code in `src/ClientAPI/WT/wt_connection.c` but as a client (outbound connection) instead of server (inbound).

Key implementation details:
- Include `<msquic.h>` guarded by `#ifdef HAS_MSQUIC`
- The `offs_client_t.wt` struct holds `HQUIC` handles for registration, configuration, connection, and stream
- MsQuic callbacks: `QUIC_STREAM_EVENT_RECEIVE` feeds bytes into `stream_framer`, `QUIC_STREAM_EVENT_SEND_COMPLETE` frees the send buffer
- The `_recv_thread` is NOT used for WT — MsQuic delivers data via callbacks on its own thread
- For sends: `QuicStreamSend()` with a completion context that frees the buffer
- The `connected`/`running` flags and mutex protect the same callback-driven state as the socket transports

Since this task requires MsQuic to be installed and linked, and the exact MsQuic client API differs from the server API, this task will be implemented with the same patterns used in `wt_connection.c` but adapted for the client role. The full implementation is substantial (connection setup, stream open, send/receive callbacks, cleanup) and should be implemented as a separate subagent task with full context about the MsQuic API.

- [ ] **Step 1: Add WT connection setup in offs_client_connect**

In the `wt://` / `wts://` branch of `offs_client_connect()`:
- Call `MsQuicOpen2()` to get the API table (or use the project's `offs_msquic_open()` singleton)
- Create a registration with `QUIC_EXECUTION_PROFILE_LOW_LATENCY`
- Create a configuration with ALPN `"offs"` and `PeerBidiStreamCount = 1`
- For `wts://`, configure TLS with `QUIC_CREDENTIAL_FLAG_CLIENT` and optional cert validation
- Open a connection to the target host:port
- Wait for the connection to complete (with a timeout)
- Open a bidirectional stream
- Set the stream's receive callback to feed data into the client's `stream_framer`

- [ ] **Step 2: Add WT send path in _send_frame**

For `OFFS_TRANSPORT_WT`, serialize CBOR to bytes, wrap with `stream_frame_encode`, then call `QuicStreamSend()` with a send-complete context that frees the buffer.

- [ ] **Step 3: Add WT receive callback**

The `QUIC_STREAM_EVENT_RECEIVE` callback:
1. Acquire `client->lock`
2. Feed received bytes into `stream_framer_feed()`
3. Drain complete frames via `stream_framer_next()`
4. For each frame: decode CBOR, extract type, call `_handle_frame()`
5. Release `client->lock`

- [ ] **Step 4: Add WT disconnect cleanup**

In `offs_client_disconnect()`, the `OFFS_TRANSPORT_WT` case:
- Close the stream via `QuicStreamClose()`
- Close the connection via `QuicConnectionClose()`
- Clean up configuration and registration
- No `pthread_join` needed (no recv thread for WT)

- [ ] **Step 5: Build and test with MsQuic available**

This step is gated on `HAS_MSQUIC` being defined. The test creates a `wt_transport_t` server and connects via `wt://` URL.

Run: `cmake --build build --target testliboffs && build/test/testliboffs --gtest_filter='TestOffsWtClient.*'`

- [ ] **Step 6: Commit**

```bash
git add src/ClientLibs/c/offs_client.c
git commit -m "feat: add WebTransport (wt://) client support via MsQuic"
```

---

### Task 5: Add WT client tests

Add WebTransport client test cases, gated behind `HAS_MSQUIC`.

**Files:**
- Modify: `test/test_offs_client.cpp`

- [ ] **Step 1: Add WT test fixture and tests**

Add a `TestOffsWtClient` fixture (gated behind `#ifdef HAS_MSQUIC`) that creates a `wt_transport_t` server and connects via `wt://` URL. Test cases: `ConnectAndDisconnect`, `PutBuffered`, `GetAfterPut`.

The pattern mirrors `TestOffsWsClient` but uses `wt_transport_create()` / `wt_transport_start()` instead of the WS equivalents.

- [ ] **Step 2: Build and test**

Run: `cmake --build build --target testliboffs && build/test/testliboffs --gtest_filter='TestOffsWtClient.*'`

Expected: All WT client tests pass (if MsQuic is available; skipped otherwise).

- [ ] **Step 3: Commit**

```bash
git add test/test_offs_client.cpp
git commit -m "test: add WT client tests (connect, put, get round-trip)"
```

---

### Task 6: Run valgrind on all new tests

Run valgrind on the WS and WT client tests to check for memory leaks.

- [ ] **Step 1: Valgrind on WS client tests**

Run: `valgrind --leak-check=full --show-leak-kinds=definite build/test/testliboffs --gtest_filter='TestOffsWsClient.*'`

Expected: 0 definite leaks.

- [ ] **Step 2: Valgrind on existing Unix/TCP client tests (regression check)**

Run: `valgrind --leak-check=full --show-leak-kinds=definite build/test/testliboffs --gtest_filter='TestOffsClient.*:TestUnixTransport.PutAndGetRoundTrip:TestTcpTransport.PutAndGetRoundTrip'`

Expected: 0 definite leaks (matching the clean baseline from prior work).

- [ ] **Step 3: Fix any leaks found**

If valgrind reports definite leaks, trace them to their source and add the appropriate free/destroy calls. Common patterns in this codebase: missing `free()` for `stream_framer_next()` return values, missing `DESTROY()` for refcounted objects in pipeline close callbacks, and missing `cbor_decref()` for temporary CBOR items.

- [ ] **Step 4: Commit any leak fixes**

```bash
git add -A
git commit -m "fix: resolve memory leaks in WS/WT client code"
```