# Block Cache Client API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add GET/PUT/DELETE block cache operations to all Client API transports (HTTP, TCP, WS, Unix, WT) with authentication required.

**Architecture:** Six new CBOR wire types, a shared `block_handlers` module for CBOR transports, and HTTP route handlers using async actor state machines. Block routes only register when auth is enabled (`api_key_hash != NULL`).

**Tech Stack:** C, CBOR, BLAKE3, base58, OpenSSL/bcrypt, actor model, poll-dancer

---

### Task 1: Wire Protocol — Define new message types and structs

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h:21-22` (add defines after AUTH_REQUEST)

- [ ] **Step 1: Add message type defines and struct declarations**

Add after `#define CLIENT_API_AUTH_REQUEST 12` at line 22:

```c
#define CLIENT_API_BLOCK_PUT_REQUEST     13
#define CLIENT_API_BLOCK_PUT_RESPONSE    14
#define CLIENT_API_BLOCK_GET_REQUEST     15
#define CLIENT_API_BLOCK_GET_RESPONSE    16
#define CLIENT_API_BLOCK_DELETE_REQUEST  17
#define CLIENT_API_BLOCK_DELETE_RESPONSE 18

// --- Block PUT Request ---
// [type, data: bstr, encoding: uint]
// encoding: 0 = raw bytes, 1 = base58 text
typedef struct {
  uint8_t* data;
  size_t data_size;
  uint8_t encoding;
} client_api_block_put_request_t;

// --- Block PUT Response ---
// [type, status: uint, hash: bstr|tstr]
// hash is raw bytes when encoding=0, base58 text string when encoding=1
typedef struct {
  uint8_t status;
  uint8_t* hash_data;
  size_t hash_len;
  uint8_t hash_is_text;  // 0 = bstr, 1 = tstr
} client_api_block_put_response_t;

// --- Block GET Request ---
// [type, hash: bstr]
typedef struct {
  uint8_t* hash_data;
  size_t hash_len;
} client_api_block_get_request_t;

// --- Block GET Response ---
// [type, status: uint, data: bstr]
typedef struct {
  uint8_t status;
  uint8_t* data;
  size_t data_size;
} client_api_block_get_response_t;

// --- Block DELETE Request ---
// [type, hash: bstr]
typedef struct {
  uint8_t* hash_data;
  size_t hash_len;
} client_api_block_delete_request_t;

// --- Block DELETE Response ---
// [type, status: uint]
typedef struct {
  uint8_t status;
} client_api_block_delete_response_t;
```

- [ ] **Step 2: Add encode/decode/destroy declarations**

Add after the auth request declarations (after line 132):

```c
cbor_item_t* client_api_block_put_request_encode(const client_api_block_put_request_t* msg);
int client_api_block_put_request_decode(cbor_item_t* item, client_api_block_put_request_t* msg);
void client_api_block_put_request_destroy(client_api_block_put_request_t* msg);

cbor_item_t* client_api_block_put_response_encode(const client_api_block_put_response_t* msg);
int client_api_block_put_response_decode(cbor_item_t* item, client_api_block_put_response_t* msg);
void client_api_block_put_response_destroy(client_api_block_put_response_t* msg);

cbor_item_t* client_api_block_get_request_encode(const client_api_block_get_request_t* msg);
int client_api_block_get_request_decode(cbor_item_t* item, client_api_block_get_request_t* msg);
void client_api_block_get_request_destroy(client_api_block_get_request_t* msg);

cbor_item_t* client_api_block_get_response_encode(const client_api_block_get_response_t* msg);
int client_api_block_get_response_decode(cbor_item_t* item, client_api_block_get_response_t* msg);
void client_api_block_get_response_destroy(client_api_block_get_response_t* msg);

cbor_item_t* client_api_block_delete_request_encode(const client_api_block_delete_request_t* msg);
int client_api_block_delete_request_decode(cbor_item_t* item, client_api_block_delete_request_t* msg);
void client_api_block_delete_request_destroy(client_api_block_delete_request_t* msg);

cbor_item_t* client_api_block_delete_response_encode(const client_api_block_delete_response_t* msg);
int client_api_block_delete_response_decode(cbor_item_t* item, client_api_block_delete_response_t* msg);
void client_api_block_delete_response_destroy(client_api_block_delete_response_t* msg);
```

- [ ] **Step 3: Commit**

```bash
git add src/ClientAPI/client_api_wire.h
git commit -m "feat: add block cache wire protocol type definitions"
```

---

### Task 2: Wire Protocol — Implement encode/decode/destroy

**Files:**
- Modify: `src/ClientAPI/client_api_wire.c` (append after auth request implementation)

- [ ] **Step 1: Add include for base58**

Add after existing includes at line 7:
```c
#include "../Util/base58.h"
```

- [ ] **Step 2: Implement block_put_request encode/decode/destroy**

Append at end of file:

```c
// --- Block PUT Request ---
// [type, data: bstr, encoding: uint]

cbor_item_t* client_api_block_put_request_encode(const client_api_block_put_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_PUT_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->data, msg->data_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->encoding);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_put_request_decode(cbor_item_t* item, client_api_block_put_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* data_item = cbor_array_get(item, 1);
  if (!cbor_isa_bytestring(data_item)) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data_size = cbor_bytestring_length(data_item);
  if (msg->data_size > 0) {
    msg->data = get_memory(msg->data_size);
    memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
  }
  cbor_decref(&data_item);

  if (cbor_array_size(item) >= 3) {
    cbor_item_t* enc_item = cbor_array_get(item, 2);
    if (cbor_isa_uint(enc_item)) {
      msg->encoding = cbor_get_uint8(enc_item);
    }
    cbor_decref(&enc_item);
  }

  return 0;
}

void client_api_block_put_request_destroy(client_api_block_put_request_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}
```

- [ ] **Step 3: Implement block_put_response encode/decode/destroy**

```c
// --- Block PUT Response ---
// [type, status: uint, hash: bstr|tstr]

cbor_item_t* client_api_block_put_response_encode(const client_api_block_put_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_PUT_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->hash_is_text) {
    item = cbor_build_string((const char*)msg->hash_data);
  } else {
    item = cbor_build_bytestring(msg->hash_data, msg->hash_len);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_put_response_decode(cbor_item_t* item, client_api_block_put_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* status_item = cbor_array_get(item, 1);
  msg->status = (uint8_t)cbor_get_uint8(status_item);
  cbor_decref(&status_item);

  cbor_item_t* hash_item = cbor_array_get(item, 2);
  if (cbor_isa_string(hash_item)) {
    msg->hash_is_text = 1;
    msg->hash_len = cbor_string_length(hash_item);
    msg->hash_data = get_memory(msg->hash_len + 1);
    memcpy(msg->hash_data, cbor_string_handle(hash_item), msg->hash_len);
    msg->hash_data[msg->hash_len] = '\0';
  } else if (cbor_isa_bytestring(hash_item)) {
    msg->hash_is_text = 0;
    msg->hash_len = cbor_bytestring_length(hash_item);
    if (msg->hash_len > 0) {
      msg->hash_data = get_memory(msg->hash_len);
      memcpy(msg->hash_data, cbor_bytestring_handle(hash_item), msg->hash_len);
    }
  } else {
    cbor_decref(&hash_item);
    return -1;
  }
  cbor_decref(&hash_item);

  return 0;
}

void client_api_block_put_response_destroy(client_api_block_put_response_t* msg) {
  if (msg == NULL) return;
  free(msg->hash_data);
}
```

- [ ] **Step 4: Implement block_get_request encode/decode/destroy**

```c
// --- Block GET Request ---
// [type, hash: bstr]

cbor_item_t* client_api_block_get_request_encode(const client_api_block_get_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_GET_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->hash_data, msg->hash_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_get_request_decode(cbor_item_t* item, client_api_block_get_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* hash_item = cbor_array_get(item, 1);
  msg->hash_data = _decode_bytestring(hash_item, &msg->hash_len);
  cbor_decref(&hash_item);

  if (msg->hash_data == NULL || msg->hash_len != 32) {
    free(msg->hash_data);
    msg->hash_data = NULL;
    return -1;
  }
  return 0;
}

void client_api_block_get_request_destroy(client_api_block_get_request_t* msg) {
  if (msg == NULL) return;
  free(msg->hash_data);
}
```

- [ ] **Step 5: Implement block_get_response encode/decode/destroy**

```c
// --- Block GET Response ---
// [type, status: uint, data: bstr]

cbor_item_t* client_api_block_get_response_encode(const client_api_block_get_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_GET_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->data != NULL && msg->data_size > 0) {
    item = cbor_build_bytestring(msg->data, msg->data_size);
  } else {
    item = cbor_build_bytestring(NULL, 0);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_get_response_decode(cbor_item_t* item, client_api_block_get_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* status_item = cbor_array_get(item, 1);
  msg->status = (uint8_t)cbor_get_uint8(status_item);
  cbor_decref(&status_item);

  cbor_item_t* data_item = cbor_array_get(item, 2);
  if (!cbor_is_null(data_item) && cbor_isa_bytestring(data_item)) {
    msg->data_size = cbor_bytestring_length(data_item);
    if (msg->data_size > 0) {
      msg->data = get_memory(msg->data_size);
      memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
    }
  }
  cbor_decref(&data_item);

  return 0;
}

void client_api_block_get_response_destroy(client_api_block_get_response_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}
```

- [ ] **Step 6: Implement block_delete_request encode/decode/destroy**

```c
// --- Block DELETE Request ---
// [type, hash: bstr]

cbor_item_t* client_api_block_delete_request_encode(const client_api_block_delete_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_DELETE_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->hash_data, msg->hash_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_delete_request_decode(cbor_item_t* item, client_api_block_delete_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* hash_item = cbor_array_get(item, 1);
  msg->hash_data = _decode_bytestring(hash_item, &msg->hash_len);
  cbor_decref(&hash_item);

  if (msg->hash_data == NULL || msg->hash_len != 32) {
    free(msg->hash_data);
    msg->hash_data = NULL;
    return -1;
  }
  return 0;
}

void client_api_block_delete_request_destroy(client_api_block_delete_request_t* msg) {
  if (msg == NULL) return;
  free(msg->hash_data);
}
```

- [ ] **Step 7: Implement block_delete_response encode/decode/destroy**

```c
// --- Block DELETE Response ---
// [type, status: uint]

cbor_item_t* client_api_block_delete_response_encode(const client_api_block_delete_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_DELETE_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_delete_response_decode(cbor_item_t* item, client_api_block_delete_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* status_item = cbor_array_get(item, 1);
  msg->status = (uint8_t)cbor_get_uint8(status_item);
  cbor_decref(&status_item);

  return 0;
}

void client_api_block_delete_response_destroy(client_api_block_delete_response_t* msg) {
  (void)msg;
}
```

- [ ] **Step 8: Commit**

```bash
git add src/ClientAPI/client_api_wire.c
git commit -m "feat: implement block cache wire protocol encode/decode"
```

---

### Task 3: Shared Block Handlers — header

**Files:**
- Create: `src/ClientAPI/block_handlers.h`

- [ ] **Step 1: Create block_handlers.h**

```c
//
// Created by victor on 5/26/26.
//

#ifndef OFFS_BLOCK_HANDLERS_H
#define OFFS_BLOCK_HANDLERS_H

#include "../BlockCache/block_cache.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "client_api_wire.h"
#include <cbor.h>
#include <stdint.h>

/* Opaque connection pointer — each transport casts its connection type */
typedef void block_connection_t;

/* Function pointer types for transport-specific I/O */
typedef void (*block_send_frame_fn)(block_connection_t* conn, cbor_item_t* frame);
typedef void (*block_send_error_fn)(block_connection_t* conn, uint8_t status, const char* msg);

typedef enum {
  BLOCK_OP_NONE = 0,
  BLOCK_OP_PUT  = 1,
  BLOCK_OP_GET  = 2,
  BLOCK_OP_DELETE = 3
} block_op_t;

typedef struct {
  block_connection_t* conn;
  block_cache_t* bc;
  actor_t* actor;
  uint8_t is_authenticated;
  block_send_frame_fn send_frame;
  block_send_error_fn send_error;

  /* Pending async state */
  block_op_t pending_op;
  uint8_t put_encoding;  /* 0=raw, 1=base58, for PUT responses */
} block_handler_ctx_t;

/* Frame handlers — called from each transport's dispatch_frame switch */
void block_handle_put_request(block_handler_ctx_t* ctx, cbor_item_t* frame);
void block_handle_get_request(block_handler_ctx_t* ctx, cbor_item_t* frame);
void block_handle_delete_request(block_handler_ctx_t* ctx, cbor_item_t* frame);

/* Cache result handler — called from each transport's actor dispatch.
   Returns 1 if the message was handled, 0 if it should fall through. */
int block_handle_cache_result(block_handler_ctx_t* ctx, message_t* msg);

#endif // OFFS_BLOCK_HANDLERS_H
```

- [ ] **Step 2: Commit**

```bash
git add src/ClientAPI/block_handlers.h
git commit -m "feat: add block_handlers shared module header"
```

---

### Task 4: Shared Block Handlers — implementation

**Files:**
- Create: `src/ClientAPI/block_handlers.c`

- [ ] **Step 1: Create block_handlers.c with PUT handler**

```c
//
// Created by victor on 5/26/26.
//

#include "block_handlers.h"
#include "../BlockCache/block.h"
#include "../Buffer/buffer.h"
#include "../Util/base58.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

void block_handle_put_request(block_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_block_put_request_t msg;
  if (client_api_block_put_request_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid block PUT request");
    return;
  }

  if (msg.data_size == 0 || msg.data_size > (size_t)standard) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Block data must be 1-128000 bytes");
    client_api_block_put_request_destroy(&msg);
    return;
  }

  buffer_t* data_buf = buffer_create_from_existing_memory(msg.data, msg.data_size);
  msg.data = NULL; /* ownership transferred to buffer */

  block_t* block = block_create_by_type(data_buf, standard);
  buffer_destroy(data_buf);

  if (block == NULL) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Failed to create block");
    client_api_block_put_request_destroy(&msg);
    return;
  }

  ctx->pending_op = BLOCK_OP_PUT;
  ctx->put_encoding = msg.encoding;
  client_api_block_put_request_destroy(&msg);

  block_cache_put(ctx->bc, block, 0, ctx->actor);
}
```

- [ ] **Step 2: Add GET handler**

```c
void block_handle_get_request(block_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_block_get_request_t msg;
  if (client_api_block_get_request_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid block GET request");
    return;
  }

  buffer_t* hash = buffer_create_from_existing_memory(msg.hash_data, msg.hash_len);
  msg.hash_data = NULL; /* ownership transferred */

  ctx->pending_op = BLOCK_OP_GET;
  client_api_block_get_request_destroy(&msg);

  block_cache_get(ctx->bc, hash, ctx->actor);
}
```

- [ ] **Step 3: Add DELETE handler**

```c
void block_handle_delete_request(block_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_block_delete_request_t msg;
  if (client_api_block_delete_request_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid block DELETE request");
    return;
  }

  buffer_t* hash = buffer_create_from_existing_memory(msg.hash_data, msg.hash_len);
  msg.hash_data = NULL; /* ownership transferred */

  ctx->pending_op = BLOCK_OP_DELETE;
  client_api_block_delete_request_destroy(&msg);

  block_cache_remove(ctx->bc, hash, ctx->actor);
}
```

- [ ] **Step 4: Add cache result handler**

```c
int block_handle_cache_result(block_handler_ctx_t* ctx, message_t* msg) {
  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      if (ctx->pending_op != BLOCK_OP_PUT) return 0;
      ctx->pending_op = BLOCK_OP_NONE;

      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
      if (result->result < 0 || result->hash == NULL) {
        ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Failed to store block");
        return 1;
      }

      client_api_block_put_response_t response;
      memset(&response, 0, sizeof(response));
      response.status = CLIENT_API_STATUS_OK;

      if (ctx->put_encoding == 1) {
        /* base58 encoding */
        size_t encoded_len = base58_encoded_length(result->hash->size);
        char* encoded = get_memory(encoded_len + 1);
        int written = base58_encode(result->hash->data, result->hash->size, encoded, encoded_len);
        if (written > 0) {
          encoded[written] = '\0';
          response.hash_data = (uint8_t*)encoded;
          response.hash_len = (size_t)written;
          response.hash_is_text = 1;
        } else {
          free(encoded);
          ctx->send_error(ctx->conn, CLIENT_API_STATUS_INTERNAL_ERROR, "Base58 encoding failed");
          return 1;
        }
      } else {
        /* raw bytes */
        response.hash_data = get_memory(result->hash->size);
        memcpy(response.hash_data, result->hash->data, result->hash->size);
        response.hash_len = result->hash->size;
        response.hash_is_text = 0;
      }

      cbor_item_t* frame = client_api_block_put_response_encode(&response);
      client_api_block_put_response_destroy(&response);
      ctx->send_frame(ctx->conn, frame);
      return 1;
    }

    case CACHE_GET_RESULT: {
      if (ctx->pending_op != BLOCK_OP_GET) return 0;
      ctx->pending_op = BLOCK_OP_NONE;

      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      client_api_block_get_response_t response;
      memset(&response, 0, sizeof(response));

      if (result->block == NULL) {
        response.status = CLIENT_API_STATUS_NOT_FOUND;
      } else {
        response.status = CLIENT_API_STATUS_OK;
        response.data_size = result->block->data->size;
        response.data = get_memory(response.data_size);
        memcpy(response.data, result->block->data->data, response.data_size);
      }

      cbor_item_t* frame = client_api_block_get_response_encode(&response);
      client_api_block_get_response_destroy(&response);
      ctx->send_frame(ctx->conn, frame);
      return 1;
    }

    case CACHE_REMOVE_RESULT: {
      if (ctx->pending_op != BLOCK_OP_DELETE) return 0;
      ctx->pending_op = BLOCK_OP_NONE;

      cache_remove_result_payload_t* result = (cache_remove_result_payload_t*)msg->payload;
      client_api_block_delete_response_t response;
      memset(&response, 0, sizeof(response));
      response.status = (result->result == 0) ? CLIENT_API_STATUS_OK : CLIENT_API_STATUS_NOT_FOUND;

      cbor_item_t* frame = client_api_block_delete_response_encode(&response);
      ctx->send_frame(ctx->conn, frame);
      return 1;
    }

    default:
      return 0;
  }
}
```

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/block_handlers.c
git commit -m "feat: implement shared block handler logic"
```

---

### Task 5: TCP Transport — integrate block handlers

**Files:**
- Modify: `src/ClientAPI/TCP/tcp_connection.h` (add block_handler_ctx_t to struct)
- Modify: `src/ClientAPI/TCP/tcp_connection.c` (add dispatch cases and frame handler cases)

- [ ] **Step 1: Add block_handler_ctx_t to tcp_connection_t**

In `tcp_connection.h`, add include and field. After line 25 (`#include "../../BlockCache/block_cache.h"`), add:
```c
#include "../block_handlers.h"
```

After the `tc` field (around line 59), add:
```c
  block_handler_ctx_t block_ctx;
```

- [ ] **Step 2: Initialize block_ctx in tcp_connection_create**

In `tcp_connection.c`, find `tcp_connection_create` (line 989). After the existing field initializations, add before `actor_init`:

```c
  /* Initialize block handler context */
  connection->block_ctx.conn = (block_connection_t*)connection;
  connection->block_ctx.bc = transport->bc;
  connection->block_ctx.actor = &connection->actor;
  connection->block_ctx.is_authenticated = connection->is_authenticated;
  connection->block_ctx.send_frame = (block_send_frame_fn)_tcp_connection_send_frame;
  connection->block_ctx.send_error = (block_send_error_fn)_tcp_connection_send_error;
  connection->block_ctx.pending_op = BLOCK_OP_NONE;
```

- [ ] **Step 3: Update is_authenticated in block_ctx after auth**

In `_tcp_handle_auth` (line 549), after `conn->is_authenticated = 1;` at line 566, add:
```c
    conn->block_ctx.is_authenticated = 1;
```

Also on line 551 (the no-hash path):
```c
  if (conn->transport == NULL || conn->transport->api_key_hash == NULL) {
    conn->is_authenticated = 1;
    conn->block_ctx.is_authenticated = 1;
    return;
  }
```

- [ ] **Step 4: Add frame dispatch cases**

In `_tcp_dispatch_frame` (line 575), add cases before `default:`:

```c
    case CLIENT_API_BLOCK_PUT_REQUEST:
      block_handle_put_request(&conn->block_ctx, frame);
      break;
    case CLIENT_API_BLOCK_GET_REQUEST:
      block_handle_get_request(&conn->block_ctx, frame);
      break;
    case CLIENT_API_BLOCK_DELETE_REQUEST:
      block_handle_delete_request(&conn->block_ctx, frame);
      break;
```

- [ ] **Step 5: Add cache result dispatch cases**

In `tcp_connection_dispatch` (line 680), add at the top of the switch statement (before `TCP_CONNECTION_DATA`):

```c
    case CACHE_PUT_RESULT:
    case CACHE_GET_RESULT:
    case CACHE_REMOVE_RESULT:
      if (block_handle_cache_result(&connection->block_ctx, msg)) break;
      /* fall through to default if not handled */
      break;
```

Note: These message types come through the actor system. The `block_handle_cache_result` function checks `pending_op` to match the message to the right pending operation.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/TCP/tcp_connection.h src/ClientAPI/TCP/tcp_connection.c
git commit -m "feat: integrate block handlers into TCP transport"
```

---

### Task 6: WS Transport — integrate block handlers

**Files:**
- Modify: `src/ClientAPI/WS/ws_connection.h`
- Modify: `src/ClientAPI/WS/ws_connection.c`

Follow the identical pattern as Task 5:

- [ ] **Step 1: Add include and block_ctx field to ws_connection_t**

In `ws_connection.h`, add `#include "../block_handlers.h"` after line 24, and add `block_handler_ctx_t block_ctx;` after the `tc` field (after line 66).

- [ ] **Step 2: Initialize block_ctx in ws_connection_create**

Find `ws_connection_create`. Add initialization:
```c
  connection->block_ctx.conn = (block_connection_t*)connection;
  connection->block_ctx.bc = transport->bc;
  connection->block_ctx.actor = &connection->actor;
  connection->block_ctx.is_authenticated = connection->is_authenticated;
  connection->block_ctx.send_frame = (block_send_frame_fn)_ws_connection_send_frame;
  connection->block_ctx.send_error = (block_send_error_fn)_ws_connection_send_error;
  connection->block_ctx.pending_op = BLOCK_OP_NONE;
```

- [ ] **Step 3: Update is_authenticated in _ws_handle_auth**

Add `conn->block_ctx.is_authenticated = 1;` in both auth-success paths (no-hash early return and bcrypt success).

- [ ] **Step 4: Add frame dispatch cases in _ws_dispatch_frame**

Add `CLIENT_API_BLOCK_PUT_REQUEST`, `CLIENT_API_BLOCK_GET_REQUEST`, `CLIENT_API_BLOCK_DELETE_REQUEST` cases calling the shared handlers.

- [ ] **Step 5: Add cache result dispatch in ws_connection_dispatch**

Add `CACHE_PUT_RESULT`, `CACHE_GET_RESULT`, `CACHE_REMOVE_RESULT` cases calling `block_handle_cache_result`.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/WS/ws_connection.h src/ClientAPI/WS/ws_connection.c
git commit -m "feat: integrate block handlers into WS transport"
```

---

### Task 7: Unix Transport — integrate block handlers

**Files:**
- Modify: `src/ClientAPI/Unix/unix_connection.h`
- Modify: `src/ClientAPI/Unix/unix_connection.c`

- [ ] **Step 1: Add include and block_ctx field to unix_connection_t**

In `unix_connection.h`, add `#include "../block_handlers.h"` after line 24, add `block_handler_ctx_t block_ctx;` after the `tc` field.

- [ ] **Step 2: Initialize block_ctx in unix_connection_create**

```c
  connection->block_ctx.conn = (block_connection_t*)connection;
  connection->block_ctx.bc = transport->bc;
  connection->block_ctx.actor = &connection->actor;
  connection->block_ctx.is_authenticated = connection->is_authenticated;
  connection->block_ctx.send_frame = (block_send_frame_fn)_unix_connection_send_frame;
  connection->block_ctx.send_error = (block_send_error_fn)_unix_connection_send_error;
  connection->block_ctx.pending_op = BLOCK_OP_NONE;
```

- [ ] **Step 3: Update is_authenticated in _unix_handle_auth**

- [ ] **Step 4: Add frame dispatch cases in _unix_dispatch_frame**

- [ ] **Step 5: Add cache result dispatch in unix_connection_dispatch**

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/Unix/unix_connection.h src/ClientAPI/Unix/unix_connection.c
git commit -m "feat: integrate block handlers into Unix transport"
```

---

### Task 8: WT Transport — integrate block handlers

**Files:**
- Modify: `src/ClientAPI/WT/wt_connection.h`
- Modify: `src/ClientAPI/WT/wt_connection.c`

- [ ] **Step 1: Add include and block_ctx field to wt_connection_t**

In `wt_connection.h`, add `#include "../block_handlers.h"` after line 25, add `block_handler_ctx_t block_ctx;` after the `tc` field.

- [ ] **Step 2: Initialize block_ctx in wt_connection_create**

```c
  conn->block_ctx.conn = (block_connection_t*)conn;
  conn->block_ctx.bc = transport->bc;
  conn->block_ctx.actor = &conn->actor;
  conn->block_ctx.is_authenticated = conn->is_authenticated;
  conn->block_ctx.send_frame = (block_send_frame_fn)_wt_connection_send_frame;
  conn->block_ctx.send_error = (block_send_error_fn)_wt_connection_send_error;
  conn->block_ctx.pending_op = BLOCK_OP_NONE;
```

- [ ] **Step 3: Update is_authenticated in _wt_handle_auth**

- [ ] **Step 4: Add frame dispatch cases in _wt_dispatch_frame**

- [ ] **Step 5: Add cache result dispatch in wt_connection_dispatch**

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/WT/wt_connection.h src/ClientAPI/WT/wt_connection.c
git commit -m "feat: integrate block handlers into WT transport"
```

---

### Task 9: HTTP Block Routes — header

**Files:**
- Create: `src/ClientAPI/HTTP/block_routes.h`

- [ ] **Step 1: Create block_routes.h**

```c
//
// Created by victor on 5/26/26.
//

#ifndef OFFS_BLOCK_ROUTES_H
#define OFFS_BLOCK_ROUTES_H

#include "http_server.h"
#include "../../BlockCache/block_cache.h"
#include "../../Configuration/config.h"
#include "../../Scheduler/scheduler.h"

/* Register block cache routes on the HTTP server.
   Only registers routes if config->api_key_hash is non-NULL (auth enabled).
   When api_key is NULL (auth disabled), no routes are registered. */
void block_routes_register(http_server_t* server, scheduler_pool_t* pool,
                           block_cache_t* bc, const config_t* config,
                           const char* api_key);

#endif // OFFS_BLOCK_ROUTES_H
```

- [ ] **Step 2: Commit**

```bash
git add src/ClientAPI/HTTP/block_routes.h
git commit -m "feat: add HTTP block routes header"
```

---

### Task 10: HTTP Block Routes — implementation (handlers)

**Files:**
- Create: `src/ClientAPI/HTTP/block_routes.c`

- [ ] **Step 1: Create file with includes, context struct, and block_put handler**

```c
//
// Created by victor on 5/26/26.
//

#include "block_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "http_connection.h"
#include "../../BlockCache/block.h"
#include "../../Buffer/buffer.h"
#include "../../Util/base58.h"
#include "../../Util/allocator.h"
#include "../../Actor/actor.h"
#include "../../Actor/message.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
  actor_t actor;
  http_response_t* response;
  http_connection_t* connection;
  block_cache_t* bc;
  scheduler_pool_t* pool;
  uint8_t put_encoding;  /* 0=raw, 1=base58 */
} block_http_state_t;

static void _block_http_state_destroy(block_http_state_t* state) {
  http_connection_t* conn = state->connection;
  http_response_destroy(state->response);
  if (conn) http_connection_destroy(conn);
  atomic_fetch_or(&state->actor.flags, ACTOR_FLAG_DESTROY);
  actor_destroy(&state->actor);
  scheduler_pool_defer_cleanup(state->pool, state, free);
}

static void _block_http_dispatch(void* state, message_t* msg);

static void _block_put_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  block_cache_t* bc = (block_cache_t*)user_data;

  if (request->body == NULL || request->body->size == 0
      || request->body->size > (size_t)standard) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  uint8_t encoding = 0;
  if (request->query_string && strstr(request->query_string, "encoding=base58") != NULL) {
    encoding = 1;
  }

  buffer_t* data_buf = buffer_copy(request->body);
  block_t* block = block_create_by_type(data_buf, standard);
  buffer_destroy(data_buf);

  if (block == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  block_http_state_t* state_data = get_clear_memory(sizeof(block_http_state_t));
  state_data->bc = bc;
  state_data->response = response;
  state_data->connection = response->connection;
  state_data->put_encoding = encoding;
  state_data->pool = ((off_routes_context_t*)NULL)->pool; /* Will be fixed below */
  refcounter_reference((refcounter_t*)state_data->connection);
  refcounter_reference((refcounter_t*)state_data->response);

  scheduler_pool_t* pool = NULL;
  /* We need the pool — it comes from the server context.
     The actual registration will pass it through user_data. */
  state_data->pool = pool;
  actor_init(&state_data->actor, state_data, _block_http_dispatch, pool);
  block_cache_put(bc, block, 0, &state_data->actor);
}
```

Wait — the above approach embeds `pool` in user_data, but `http_server_get_with_data` only takes one user_data pointer. The cleanest approach is to create a context struct that holds both `bc` and `pool`. Let me redesign.

Actually, the simplest approach: create a `block_routes_context_t` that holds `bc` and `pool`, pass it as user_data.

Let me rewrite this task properly.

- [ ] **Step 1: Create block_routes.c with context and all handlers**

```c
//
// Created by victor on 5/26/26.
//

#include "block_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "http_connection.h"
#include "../../BlockCache/block.h"
#include "../../Buffer/buffer.h"
#include "../../Util/base58.h"
#include "../../Util/allocator.h"
#include "../../Actor/actor.h"
#include "../../Actor/message.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
  block_cache_t* bc;
  scheduler_pool_t* pool;
} block_routes_context_t;

/* --- Async state machine for block operations --- */

typedef struct {
  actor_t actor;
  http_response_t* response;
  http_connection_t* connection;
  block_routes_context_t* ctx;
  uint8_t put_encoding;
  enum { BLOCK_HTTP_PUT, BLOCK_HTTP_GET, BLOCK_HTTP_DELETE } op;
} block_http_state_t;

static void _block_http_state_destroy(block_http_state_t* state) {
  http_connection_t* conn = state->connection;
  http_response_destroy(state->response);
  if (conn) http_connection_destroy(conn);
  atomic_fetch_or(&state->actor.flags, ACTOR_FLAG_DESTROY);
  actor_destroy(&state->actor);
  scheduler_pool_defer_cleanup(state->ctx->pool, state, free);
}

static void _block_http_dispatch(void* vstate, message_t* msg) {
  block_http_state_t* state = (block_http_state_t*)vstate;

  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      if (state->op != BLOCK_HTTP_PUT) break;
      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;

      if (result->result < 0 || result->hash == NULL) {
        http_response_set_status(state->response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
        http_response_end(state->response);
        _block_http_state_destroy(state);
        return;
      }

      http_response_set_status(state->response, HTTP_STATUS_CREATED);

      if (state->put_encoding == 1) {
        http_response_set_header(state->response, "Content-Type", "text/plain");
        size_t encoded_len = base58_encoded_length(result->hash->size);
        char* encoded = get_memory(encoded_len + 1);
        int written = base58_encode(result->hash->data, result->hash->size, encoded, encoded_len);
        if (written > 0) {
          http_response_write(state->response, encoded, (size_t)written);
        }
        free(encoded);
      } else {
        http_response_set_header(state->response, "Content-Type", "application/octet-stream");
        http_response_write(state->response, (const char*)result->hash->data, result->hash->size);
      }

      http_response_end(state->response);
      _block_http_state_destroy(state);
      return;
    }

    case CACHE_GET_RESULT: {
      if (state->op != BLOCK_HTTP_GET) break;
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;

      if (result->block == NULL) {
        http_response_set_status(state->response, HTTP_STATUS_NOT_FOUND);
        http_response_end(state->response);
        _block_http_state_destroy(state);
        return;
      }

      http_response_set_status(state->response, HTTP_STATUS_OK);
      http_response_set_header(state->response, "Content-Type", "application/octet-stream");
      http_response_write(state->response, (const char*)result->block->data->data,
                          result->block->data->size);
      http_response_end(state->response);
      _block_http_state_destroy(state);
      return;
    }

    case CACHE_REMOVE_RESULT: {
      if (state->op != BLOCK_HTTP_DELETE) break;
      cache_remove_result_payload_t* result = (cache_remove_result_payload_t*)msg->payload;

      if (result->result != 0) {
        http_response_set_status(state->response, HTTP_STATUS_NOT_FOUND);
      } else {
        http_response_set_status(state->response, HTTP_STATUS_NO_CONTENT);
      }
      http_response_end(state->response);
      _block_http_state_destroy(state);
      return;
    }

    default:
      break;
  }
}
```

- [ ] **Step 2: Add PUT handler**

```c
static void _block_put_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  block_routes_context_t* ctx = (block_routes_context_t*)user_data;

  if (request->body == NULL || request->body->size == 0
      || request->body->size > (size_t)standard) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  uint8_t encoding = 0;
  if (request->query_string && strstr(request->query_string, "encoding=base58") != NULL) {
    encoding = 1;
  }

  buffer_t* data_buf = buffer_create_from_existing_memory(
      get_memory(request->body->size), request->body->size);
  memcpy(data_buf->data, request->body->data, request->body->size);

  block_t* block = block_create_by_type(data_buf, standard);
  buffer_destroy(data_buf);

  if (block == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  block_http_state_t* state = get_clear_memory(sizeof(block_http_state_t));
  state->ctx = ctx;
  state->response = response;
  state->connection = response->connection;
  state->put_encoding = encoding;
  state->op = BLOCK_HTTP_PUT;
  refcounter_reference((refcounter_t*)state->connection);
  refcounter_reference((refcounter_t*)state->response);

  actor_init(&state->actor, state, _block_http_dispatch, ctx->pool);
  block_cache_put(ctx->bc, block, 0, &state->actor);
}
```

- [ ] **Step 3: Add GET handler**

```c
/* Regex pattern for GET and DELETE: /blocks/<base58-hash> */
#define BLOCK_HASH_PATTERN "/blocks/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]+)"

static void _block_get_handler(http_request_t* request, http_response_t* response,
                                void* user_data) {
  block_routes_context_t* ctx = (block_routes_context_t*)user_data;

  /* Extract base58 hash from path: /blocks/<hash> */
  const char* path = request->path;
  const char* hash_start = strrchr(path, '/');
  if (hash_start == NULL || strlen(hash_start + 1) == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  hash_start++; /* skip '/' */

  uint8_t hash_bytes[32];
  size_t bytes_written = 0;
  if (base58_decode(hash_start, hash_bytes, sizeof(hash_bytes), &bytes_written) != 0
      || bytes_written != 32) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  uint8_t* hash_copy = get_memory(32);
  memcpy(hash_copy, hash_bytes, 32);
  buffer_t* hash = buffer_create_from_existing_memory(hash_copy, 32);

  block_http_state_t* state = get_clear_memory(sizeof(block_http_state_t));
  state->ctx = ctx;
  state->response = response;
  state->connection = response->connection;
  state->op = BLOCK_HTTP_GET;
  refcounter_reference((refcounter_t*)state->connection);
  refcounter_reference((refcounter_t*)state->response);

  actor_init(&state->actor, state, _block_http_dispatch, ctx->pool);
  block_cache_get(ctx->bc, hash, &state->actor);
}
```

- [ ] **Step 4: Add DELETE handler**

```c
static void _block_delete_handler(http_request_t* request, http_response_t* response,
                                   void* user_data) {
  block_routes_context_t* ctx = (block_routes_context_t*)user_data;

  const char* path = request->path;
  const char* hash_start = strrchr(path, '/');
  if (hash_start == NULL || strlen(hash_start + 1) == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  hash_start++;

  uint8_t hash_bytes[32];
  size_t bytes_written = 0;
  if (base58_decode(hash_start, hash_bytes, sizeof(hash_bytes), &bytes_written) != 0
      || bytes_written != 32) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  uint8_t* hash_copy = get_memory(32);
  memcpy(hash_copy, hash_bytes, 32);
  buffer_t* hash = buffer_create_from_existing_memory(hash_copy, 32);

  block_http_state_t* state = get_clear_memory(sizeof(block_http_state_t));
  state->ctx = ctx;
  state->response = response;
  state->connection = response->connection;
  state->op = BLOCK_HTTP_DELETE;
  refcounter_reference((refcounter_t*)state->connection);
  refcounter_reference((refcounter_t*)state->response);

  actor_init(&state->actor, state, _block_http_dispatch, ctx->pool);
  block_cache_remove(ctx->bc, hash, &state->actor);
}
```

- [ ] **Step 5: Add registration function**

```c
void block_routes_register(http_server_t* server, scheduler_pool_t* pool,
                           block_cache_t* bc, const config_t* config,
                           const char* api_key) {
  /* Only register block routes if auth is enabled */
  if (config == NULL || config->api_key_hash == NULL || api_key == NULL) {
    return;
  }

  block_routes_context_t* ctx = get_clear_memory(sizeof(block_routes_context_t));
  ctx->bc = bc;
  ctx->pool = pool;

  http_server_put_with_data(server, "/blocks",
                             _block_put_handler, ctx, free);
  http_server_get_with_data(server, BLOCK_HASH_PATTERN,
                             _block_get_handler, ctx, NULL);
  http_server_delete_with_data(server, BLOCK_HASH_PATTERN,
                                _block_delete_handler, ctx, NULL);
}
```

Note: The GET and DELETE routes share the same user_data (ctx), but `http_server_get_with_data` takes a `user_data_destroy` callback. Since we register GET first (with `free`), then DELETE (with `NULL`), the ctx will be freed when the GET route is destroyed. DELETE gets `NULL` for destroy to avoid double-free. Actually this is fragile. Let me fix: only the first registration uses `free`, the others use `NULL`.

Actually, looking at `http_server.h`, each route gets its own user_data. But we only have one `ctx`. So we need to be careful. Let me register all three routes and only free ctx with the first one:

```c
  http_server_put_with_data(server, "/blocks",
                             _block_put_handler, ctx, free);
  http_server_get_with_data(server, BLOCK_HASH_PATTERN,
                             _block_get_handler, ctx, NULL);
  http_server_delete_with_data(server, BLOCK_HASH_PATTERN,
                                _block_delete_handler, ctx, NULL);
```

Since the server destroys routes in order, PUT (first registered) will free ctx when the server is destroyed. GET and DELETE won't double-free.

Wait, actually each route stores its own copy of user_data. So when the server destroys routes, each route's `user_data_destroy` is called. If we set `free` on PUT (first), GET and DELETE need to not free. This is correct.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/HTTP/block_routes.h src/ClientAPI/HTTP/block_routes.c
git commit -m "feat: add HTTP block cache routes with async actor bridging"
```

---

### Task 11: Wire up HTTP block routes in server startup

**Files:**
- Modify: `examples/off_server/main.c` (add block_routes_register call)

- [ ] **Step 1: Add include and registration call**

In `main.c`, add include:
```c
#include "ClientAPI/HTTP/block_routes.h"
```

After `off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL);` (line 104), add:
```c
  block_routes_register(server, pool, bc, NULL, NULL);
```

Note: `config` and `api_key` are NULL because the example server doesn't use auth, so block routes won't be registered (as designed).

- [ ] **Step 2: Commit**

```bash
git add examples/off_server/main.c
git commit -m "feat: wire block routes into example server"
```

---

### Task 12: Build verification

**Files:**
- None (build check only)

- [ ] **Step 1: Build the project**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1
```

Expected: Build succeeds with no errors or warnings.

- [ ] **Step 2: Fix any compilation errors**

If the build fails, fix errors and recommit before proceeding.

---

### Task 13: Write wire protocol unit tests

**Files:**
- Create: `test/test_block_cache_api.cpp`

- [ ] **Step 1: Create test file with encode/decode round-trip tests**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "ClientAPI/client_api_wire.h"
#include "Util/base58.h"
}

/* --- Block PUT Request --- */

TEST(BlockCacheAPIWire, PutRequestEncodeDecode) {
  uint8_t data[] = {'h', 'e', 'l', 'l', 'o'};
  client_api_block_put_request_t msg;
  msg.data = data;
  msg.data_size = 5;
  msg.encoding = 0;

  cbor_item_t* encoded = client_api_block_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_BLOCK_PUT_REQUEST);

  client_api_block_put_request_t decoded;
  int ret = client_api_block_put_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.data_size, 5u);
  EXPECT_EQ(memcmp(decoded.data, "hello", 5), 0);
  EXPECT_EQ(decoded.encoding, 0);

  client_api_block_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, PutRequestEncodeDecodeBase58) {
  uint8_t data[] = {'w', 'o', 'r', 'l', 'd'};
  client_api_block_put_request_t msg;
  msg.data = data;
  msg.data_size = 5;
  msg.encoding = 1;

  cbor_item_t* encoded = client_api_block_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_put_request_t decoded;
  int ret = client_api_block_put_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.encoding, 1);

  client_api_block_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, PutRequestDecodeMinimal) {
  /* Only type and data, no encoding field */
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_BLOCK_PUT_REQUEST);
  cbor_array_push(array, type_item); cbor_decref(&type_item);
  cbor_item_t* data_item = cbor_build_bytestring((const uint8_t*)"test", 4);
  cbor_array_push(array, data_item); cbor_decref(&data_item);

  client_api_block_put_request_t decoded;
  int ret = client_api_block_put_request_decode(array, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.encoding, 0); /* defaults to 0 */
  EXPECT_EQ(decoded.data_size, 4u);

  client_api_block_put_request_destroy(&decoded);
  cbor_decref(&array);
}

/* --- Block PUT Response --- */

TEST(BlockCacheAPIWire, PutResponseEncodeDecodeRaw) {
  uint8_t hash[32];
  memset(hash, 0xAB, 32);

  client_api_block_put_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;
  msg.hash_data = hash;
  msg.hash_len = 32;
  msg.hash_is_text = 0;

  cbor_item_t* encoded = client_api_block_put_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_put_response_t decoded;
  int ret = client_api_block_put_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);
  EXPECT_EQ(decoded.hash_is_text, 0);
  EXPECT_EQ(decoded.hash_len, 32u);
  EXPECT_EQ(memcmp(decoded.hash_data, hash, 32), 0);

  client_api_block_put_response_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, PutResponseEncodeDecodeBase58) {
  const char* hash_str = "2gPvUwFKjMqX5N7rR3tZ8h1B4vC6xW9yA0bD";
  size_t str_len = strlen(hash_str);

  client_api_block_put_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;
  msg.hash_data = (uint8_t*)hash_str;
  msg.hash_len = str_len;
  msg.hash_is_text = 1;

  cbor_item_t* encoded = client_api_block_put_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_put_response_t decoded;
  int ret = client_api_block_put_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);
  EXPECT_EQ(decoded.hash_is_text, 1);
  EXPECT_EQ(decoded.hash_len, str_len);
  EXPECT_EQ(memcmp(decoded.hash_data, hash_str, str_len), 0);

  client_api_block_put_response_destroy(&decoded);
  cbor_decref(&encoded);
}

/* --- Block GET Request --- */

TEST(BlockCacheAPIWire, GetRequestEncodeDecode) {
  uint8_t hash[32];
  memset(hash, 0x42, 32);

  client_api_block_get_request_t msg;
  msg.hash_data = hash;
  msg.hash_len = 32;

  cbor_item_t* encoded = client_api_block_get_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_BLOCK_GET_REQUEST);

  client_api_block_get_request_t decoded;
  int ret = client_api_block_get_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.hash_len, 32u);
  EXPECT_EQ(memcmp(decoded.hash_data, hash, 32), 0);

  client_api_block_get_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, GetRequestDecodeInvalidHashSize) {
  uint8_t short_hash[16];
  memset(short_hash, 0x42, 16);

  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_BLOCK_GET_REQUEST);
  cbor_array_push(array, type_item); cbor_decref(&type_item);
  cbor_item_t* hash_item = cbor_build_bytestring(short_hash, 16);
  cbor_array_push(array, hash_item); cbor_decref(&hash_item);

  client_api_block_get_request_t decoded;
  int ret = client_api_block_get_request_decode(array, &decoded);
  EXPECT_EQ(ret, -1);

  cbor_decref(&array);
}

/* --- Block GET Response --- */

TEST(BlockCacheAPIWire, GetResponseEncodeDecode) {
  uint8_t block_data[128000];
  memset(block_data, 0xCC, sizeof(block_data));

  client_api_block_get_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;
  msg.data = block_data;
  msg.data_size = sizeof(block_data);

  cbor_item_t* encoded = client_api_block_get_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_get_response_t decoded;
  int ret = client_api_block_get_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);
  EXPECT_EQ(decoded.data_size, 128000u);

  client_api_block_get_response_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(BlockCacheAPIWire, GetResponseNotFound) {
  client_api_block_get_response_t msg;
  msg.status = CLIENT_API_STATUS_NOT_FOUND;
  msg.data = NULL;
  msg.data_size = 0;

  cbor_item_t* encoded = client_api_block_get_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_get_response_t decoded;
  int ret = client_api_block_get_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_NOT_FOUND);
  EXPECT_EQ(decoded.data_size, 0u);

  client_api_block_get_response_destroy(&decoded);
  cbor_decref(&encoded);
}

/* --- Block DELETE Request --- */

TEST(BlockCacheAPIWire, DeleteRequestEncodeDecode) {
  uint8_t hash[32];
  memset(hash, 0xFF, 32);

  client_api_block_delete_request_t msg;
  msg.hash_data = hash;
  msg.hash_len = 32;

  cbor_item_t* encoded = client_api_block_delete_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(encoded), CLIENT_API_BLOCK_DELETE_REQUEST);

  client_api_block_delete_request_t decoded;
  int ret = client_api_block_delete_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.hash_len, 32u);

  client_api_block_delete_request_destroy(&decoded);
  cbor_decref(&encoded);
}

/* --- Block DELETE Response --- */

TEST(BlockCacheAPIWire, DeleteResponseEncodeDecode) {
  client_api_block_delete_response_t msg;
  msg.status = CLIENT_API_STATUS_OK;

  cbor_item_t* encoded = client_api_block_delete_response_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_block_delete_response_t decoded;
  int ret = client_api_block_delete_response_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.status, CLIENT_API_STATUS_OK);

  cbor_decref(&encoded);
}
```

- [ ] **Step 2: Run tests**

```bash
cd build && cmake .. && make -j$(nproc) && ctest -V -R BlockCacheAPIWire
```

Expected: All wire protocol tests pass.

- [ ] **Step 3: Commit**

```bash
git add test/test_block_cache_api.cpp test/CMakeLists.txt
git commit -m "test: add wire protocol round-trip tests for block cache API"
```

---

### Task 14: De-Wonk Audit + Memory Leak Check

- [ ] **Step 1: Run de-wonk skill**

Invoke the de-wonk skill to check for unimplemented, stubbed, or disabled code.

- [ ] **Step 2: Run tests under Valgrind**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-gdwarf-4" && make -j$(nproc)
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
  --suppressions=../valgrind.supp \
  ./test/test_block_cache_api 2>&1 | tee valgrind_block_api.log
```

Expected: No new leaks beyond pre-existing ones tracked in `reference_valgrind_leaks.md`.

- [ ] **Step 3: Run full test suite under Valgrind**

```bash
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
  --suppressions=../valgrind.supp \
  ctest --test-dir . --output-on-failure 2>&1 | tee valgrind_full.log
```

Expected: All tests pass, no new leaks introduced.

- [ ] **Step 4: Address any issues found**

Fix leaks, incomplete implementations, or disabled code found by de-wonk or Valgrind.

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "chore: de-wonk audit and memory leak fixes for block cache API"
```
