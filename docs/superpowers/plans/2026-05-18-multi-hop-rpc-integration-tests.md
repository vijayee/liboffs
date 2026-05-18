# Multi-Hop RPC Integration Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Test every wire RPC call over a multi-node network with per-hop verification, prioritizing multi-hop RPCs (FindBlock, StoreBlock, RankBlock) first.

**Architecture:** Add an in-memory message event log to each node, RPC injection control commands, and topology construction helpers in the test framework. Tests build specific topologies (chain, ring, star), inject RPCs via control commands, and verify each hop's behavior by querying event logs.

**Tech Stack:** C (node-side event logging, control command handlers), C++/GTest (test framework, topology helpers, assertions)

**Security:** All message logging and RPC injection is compiled out of release builds via `#ifdef OFFS_TEST`. The `message_log_t` in `network_t`, all `message_log_record()` calls, the control command handlers, and the `CTRL_*` protocol definitions are gated behind this flag. The `test_node_main.c` (which contains the TCP control socket server) is only linked into the test binary, never `off_server`. In release builds, there is zero memory overhead, zero processing overhead, and no attack surface from test infrastructure.

---

## File Structure

| File | Purpose |
|------|---------|
| `src/Network/message_log.h` | New: message_log_t definition, init/destroy/record/query/clear API — all behind `#ifdef OFFS_TEST` |
| `src/Network/message_log.c` | New: message_log implementation (circular buffer, thread-safe record/query) |
| `src/Network/network.h` | Modify: add `message_log_t log` field to `network_t` inside `#ifdef OFFS_TEST` |
| `src/Network/network.c` | Modify: init/destroy the log, insert `message_log_record()` calls at each message handler |
| `src/Network/conn_state.c` | Modify: insert `message_log_record()` in send paths |
| `src/Network/hebbian.h` | Modify: add `hebbian_table_query()` declaration |
| `src/Network/hebbian.c` | Modify: add `hebbian_table_query()` implementation |
| `test/test_control_protocol.h` | Modify: add GET_EVENTS, CLEAR_EVENTS, FIND_BLOCK, PING_PEER, HEBBIAN commands |
| `test/test_node_main.c` | Modify: add handlers for new control commands |
| `test/test_rpc_integration.cpp` | New: multi-hop RPC integration tests with Hebbian verification |
| `test/CMakeLists.txt` | Modify: add test_rpc_integration target with OFFS_TEST define |

---

## Task 1: Create message_log.h and message_log.c

**Files:**
- Create: `src/Network/message_log.h`
- Create: `src/Network/message_log.c`

- [ ] **Step 1: Create message_log.h with the struct definitions and API**

Create `src/Network/message_log.h`:

```c
#ifndef OFFS_MESSAGE_LOG_H
#define OFFS_MESSAGE_LOG_H

#include "node_id.h"
#include <stdint.h>
#include <stddef.h>

#ifdef OFFS_TEST

#define MESSAGE_LOG_CAPACITY 256

typedef enum {
  MSG_DIRECTION_SENT = 0,
  MSG_DIRECTION_RECEIVED = 1,
  MSG_DIRECTION_FORWARDED = 2
} msg_direction_e;

typedef struct {
  uint8_t type;              // WIRE_FIND_BLOCK, etc.
  uint8_t direction;         // msg_direction_e
  uint64_t timestamp_ms;     // monotonic timestamp
  node_id_t peer_id;         // who we sent to / received from
  uint64_t message_id;       // correlation ID from wire message
  uint8_t block_hash[32];    // zeroed if not applicable
  uint8_t result;            // 0=success, 1=forwarded, 2=not_found, 3=declined
  float hebbian_weight;      // peer's Hebbian weight AFTER this event
} message_event_t;

typedef struct {
  message_event_t events[MESSAGE_LOG_CAPACITY];
  size_t count;              // total events written (may exceed capacity)
  size_t cursor;             // index of oldest event (wraps)
} message_log_t;

void message_log_init(message_log_t* log);
void message_log_record(message_log_t* log, uint8_t type, uint8_t direction,
                         const node_id_t* peer, uint64_t message_id,
                         const uint8_t* block_hash, uint8_t result,
                         const hebbian_table_t* hebbian);
size_t message_log_query(const message_log_t* log, size_t after_cursor,
                          message_event_t* out, size_t out_cap);
void message_log_clear(message_log_t* log);

#else

// Release build: message logging is completely compiled out
typedef struct { int _unused; } message_log_t;
#define message_log_init(log) ((void)0)
#define message_log_record(log, type, dir, peer, msg_id, hash, result, hebbian) ((void)0)
#define message_log_query(log, after, out, out_cap) ((size_t)0)
#define message_log_clear(log) ((void)0)

#endif // OFFS_TEST

#endif // OFFS_MESSAGE_LOG_H
```

Note: The `message_log_record` function takes a `hebbian_table_t*` pointer to snapshot the peer's weight. In the release build, this is a no-op macro that discards all arguments.

- [ ] **Step 2: Create message_log.c with circular buffer implementation**

Create `src/Network/message_log.c`:

```c
#ifdef OFFS_TEST

#include "message_log.h"
#include "hebbian.h"
#include <string.h>
#include <time.h>

void message_log_init(message_log_t* log) {
  if (log == NULL) return;
  memset(log, 0, sizeof(message_log_t));
}

void message_log_record(message_log_t* log, uint8_t type, uint8_t direction,
                         const node_id_t* peer, uint64_t message_id,
                         const uint8_t* block_hash, uint8_t result,
                         const hebbian_table_t* hebbian) {
  if (log == NULL) return;

  size_t index = log->count % MESSAGE_LOG_CAPACITY;
  message_event_t* event = &log->events[index];

  event->type = type;
  event->direction = direction;
  event->timestamp_ms = (uint64_t)(time(NULL) * 1000);

  if (peer != NULL) {
    memcpy(&event->peer_id, peer, sizeof(node_id_t));
  } else {
    memset(&event->peer_id, 0, sizeof(node_id_t));
  }

  event->message_id = message_id;

  if (block_hash != NULL) {
    memcpy(event->block_hash, block_hash, 32);
  } else {
    memset(event->block_hash, 0, 32);
  }

  event->result = result;

  // Snapshot the peer's Hebbian weight
  if (hebbian != NULL && peer != NULL) {
    event->hebbian_weight = hebbian_table_get(hebbian, peer);
  } else {
    event->hebbian_weight = 0.0f;
  }

  log->count++;
}

size_t message_log_query(const message_log_t* log, size_t after_cursor,
                          message_event_t* out, size_t out_cap) {
  if (log == NULL || out == NULL || out_cap == 0) return 0;

  // If count exceeds capacity, events wrap around. The oldest event is at
  // (count % CAPACITY). Events after_cursor are those with index > after_cursor.
  size_t start;
  if (log->count <= MESSAGE_LOG_CAPACITY) {
    // Buffer not full yet — start from after_cursor+1 or 0
    start = (after_cursor < log->count) ? after_cursor : log->count;
  } else {
    // Buffer is full — count > CAPACITY means some events were overwritten.
    // The effective range is [count - CAPACITY, count).
    // Map after_cursor to the actual buffer.
    size_t earliest = log->count - MESSAGE_LOG_CAPACITY;
    start = (after_cursor >= earliest) ? (after_cursor + 1) : earliest;
    if (start >= log->count) return 0;  // after_cursor is past all events
  }

  size_t copied = 0;
  for (size_t idx = start; idx < log->count && copied < out_cap; idx++) {
    size_t buf_idx = idx % MESSAGE_LOG_CAPACITY;
    out[copied] = log->events[buf_idx];
    // Overwrite the event's index in the timestamp field for cursor tracking
    // (the test side needs to know each event's logical index for pagination)
    // Actually, we set the index on the count field — but we can't modify
    // message_event_t to include an index field without changing the struct.
    // Instead, the caller tracks indices via the total count.
    copied++;
  }

  return copied;
}

void message_log_clear(message_log_t* log) {
  if (log == NULL) return;
  log->count = 0;
  log->cursor = 0;
}

#endif // OFFS_TEST
```

- [ ] **Step 3: Build and verify compilation**

Run: `cmake --build build --target offs 2>&1 | head -50`

Expected: Clean compilation with no errors. The `#ifdef OFFS_TEST` sections are not compiled by default (the library doesn't define OFFS_TEST).

- [ ] **Step 4: Commit**

```bash
git add src/Network/message_log.h src/Network/message_log.c
git commit -m "feat: add message event log for test-only RPC tracing"
```

---

## Task 2: Add message_log_t to network_t and hebbian_table_query

**Files:**
- Modify: `src/Network/network.h` — add `message_log_t log` field
- Modify: `src/Network/network.c` — init/clear the log
- Modify: `src/Network/hebbian.h` — add `hebbian_table_query` declaration
- Modify: `src/Network/hebbian.c` — add `hebbian_table_query` implementation

- [ ] **Step 1: Add message_log_t field to network_t**

In `src/Network/network.h`, add `#include "message_log.h"` and add the log field inside `#ifdef OFFS_TEST`:

```c
// Add after other #includes at top:
#include "message_log.h"

// Inside network_t struct, after rate_limit_table_t rate_limits:
#ifdef OFFS_TEST
  message_log_t log;
#endif
```

- [ ] **Step 2: Add hebbian_table_query function**

In `src/Network/hebbian.h`, add after the existing declarations:

```c
// Query the weight table — returns a heap-allocated array of (node_id, weight)
// pairs and sets *out_count. Caller must free() the returned array.
// Returns NULL on error.
hebbian_weight_t* hebbian_table_query(const hebbian_table_t* table, size_t* out_count);
```

In `src/Network/hebbian.c`, add the implementation:

```c
hebbian_weight_t* hebbian_table_query(const hebbian_table_t* table, size_t* out_count) {
  if (table == NULL || out_count == NULL) return NULL;

  hebbian_weight_t* result = malloc(table->count * sizeof(hebbian_weight_t));
  if (result == NULL) return NULL;

  memcpy(result, table->entries, table->count * sizeof(hebbian_weight_t));
  *out_count = table->count;
  return result;
}
```

- [ ] **Step 3: Initialize and clear message_log in network_create/destroy**

In `src/Network/network.c`, in `network_create`, add after the `rate_limits` initialization:

```c
#ifdef OFFS_TEST
  message_log_init(&network->log);
#endif
```

In `network_destroy`, add before the `free(network)` call:

```c
#ifdef OFFS_TEST
  message_log_clear(&network->log);
#endif
```

- [ ] **Step 4: Build and verify**

Run: `cmake --build build --target offs 2>&1 | tail -20`

Expected: Clean build. The OFFS_TEST sections compile but are inactive since OFFS_TEST is not defined for the library target.

- [ ] **Step 5: Commit**

```bash
git add src/Network/network.h src/Network/network.c src/Network/hebbian.h src/Network/hebbian.c
git commit -m "feat: add message_log to network_t and hebbian_table_query for test infrastructure"
```

---

## Task 3: Insert message_log_record calls in network.c and conn_state.c

**Files:**
- Modify: `src/Network/network.c` — add log calls at each message handler
- Modify: `src/Network/conn_state.c` — add log call in send paths

This is the most invasive task. Each wire message handler needs a `message_log_record()` call. All calls are wrapped in `#ifdef OFFS_TEST` so they compile out in release builds.

- [ ] **Step 1: Add log calls for received messages in network_dispatch**

In `src/Network/network.c`, at the top of each wire message handler inside the `NETWORK_QUIC_DATA` case, add a log call. The pattern for received messages is:

```c
#ifdef OFFS_TEST
  message_log_record(&network->log, WIRE_FIND_BLOCK, MSG_DIRECTION_RECEIVED,
                     &sender_id, find->message_id, find->block_hash,
                     0, &network->hebbian);
#endif
```

For each handler, add the log call AFTER the NULL check on the decoded struct, BEFORE the handler logic. The handlers and their result values:

| Handler | Type | Direction | Result |
|---------|------|-----------|--------|
| `network_handle_find_block` | `WIRE_FIND_BLOCK` | `MSG_DIRECTION_RECEIVED` | 0 (always logged as received; result tracks forwarding) |
| `network_handle_find_block_response` | `WIRE_FIND_BLOCK_RESPONSE` | `MSG_DIRECTION_RECEIVED` | 0=found, 2=not_found |
| `network_handle_store_block` | `WIRE_STORE_BLOCK` | `MSG_DIRECTION_RECEIVED` | 0 |
| `network_handle_store_block_response` | `WIRE_STORE_BLOCK_RESPONSE` | `MSG_DIRECTION_RECEIVED` | 0=accepted, 3=declined |
| `network_handle_rank_block` | `WIRE_RANK_BLOCK` | `MSG_DIRECTION_RECEIVED` | 0 |
| Ping handlers | `WIRE_PING` etc. | `MSG_DIRECTION_RECEIVED` | 0 |
| `network_handle_seeking_blocks` | `WIRE_SEEKING_BLOCKS` | `MSG_DIRECTION_RECEIVED` | 0 |
| `network_handle_recall_block` | `WIRE_RECALL_BLOCK` | `MSG_DIRECTION_RECEIVED` | 0 |

For each handler that sends a response, add a FORWARDED log call right before `conn_state_send`:

```c
#ifdef OFFS_TEST
  message_log_record(&network->log, WIRE_FIND_BLOCK_RESPONSE, MSG_DIRECTION_FORWARDED,
                     &reply_peer_id, resp.message_id, resp.block_hash,
                     found ? 0 : 2, &network->hebbian);
#endif
```

For FindBlock forwarding (when forwarding to next hops), add a FORWARDED log call:

```c
#ifdef OFFS_TEST
  message_log_record(&network->log, WIRE_FIND_BLOCK, MSG_DIRECTION_FORWARDED,
                     &next_hop_id, forward.message_id, forward.block_hash,
                     1, &network->hebbian);
#endif
```

Add these log calls for:
1. FindBlock received (in `network_handle_find_block`)
2. FindBlock forwarded to next hops (in the FIND_BLOCK_FORWARDING case)
3. FindBlock FOUND response sent back (in the FOUND case)
4. FindBlock NOT_FOUND response sent back (in the NOT_FOUND/TTL_EXPIRED case)
5. FindBlockResponse received (in `network_handle_find_block_response`)
6. StoreBlock received (in `network_handle_store_block`)
7. StoreBlock forwarded (in the STORE_BLOCK_FORWARDING case)
8. StoreBlock accepted response sent (in the STORE_BLOCK_ACCEPTED case)
9. StoreBlock declined response sent (in the STORE_BLOCK_DECLINED/MAX_HOPS_REACHED case)
10. StoreBlockResponse received (in `network_handle_store_block_response`)
11. RankBlock received and forwarded (in `network_handle_rank_block`)

- [ ] **Step 2: Add log calls for sent messages in conn_state_send**

In `src/Network/conn_state.c`, add logging at the beginning of `conn_state_send` for the sent direction. Since `conn_state_send` sends an already-encoded CBOR message, we need to extract the type from the CBOR item. The simplest approach: log in the relay send path and the QUIC send path.

However, logging in `conn_state_send` is complicated because we need the wire type and message_id, which requires decoding the CBOR. Instead, we log SENT at the call sites in network.c where `conn_state_send` is called, using the wire type and message_id we already have. This is more efficient and avoids re-decoding.

So the SENT log calls go in network.c, right before each `conn_state_send` call. These are the same callsites where we already have FORWARDED log calls — we should log SENT for the initial message from the originator, and FORWARDED for messages relayed from intermediate nodes.

For now, the FORWARDED calls cover the multi-hop case. The SENT direction is for messages initiated by the local node (not in response to a received message). Those are triggered by the local stream network path (`NETWORK_LOCAL_FIND_BLOCK`, etc.), which we'll add in a follow-up task.

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --target offs 2>&1 | tail -20`

Expected: Clean build. All OFFS_TEST sections compile but are inactive.

- [ ] **Step 4: Commit**

```bash
git add src/Network/network.c src/Network/conn_state.c
git commit -m "feat: add message_log_record calls at wire message handlers (test-only)"
```

---

## Task 4: Add control commands for event log and RPC injection

**Files:**
- Modify: `test/test_control_protocol.h` — add new command definitions
- Modify: `test/test_node_main.c` — add handlers for new commands

- [ ] **Step 1: Add control command definitions to test_control_protocol.h**

Add after the existing command definitions:

```c
/* Event log commands (OFFS_TEST only) */
#define CTRL_GET_EVENTS      "GET_EVENTS"
#define CTRL_CLEAR_EVENTS    "CLEAR_EVENTS"
#define CTRL_FIND_BLOCK      "FIND_BLOCK"
#define CTRL_PING_PEER       "PING_PEER"
#define CTRL_HEBBIAN         "HEBBIAN"

/* Event log response prefix */
#define CTRL_RESP_EVENTS     "EVENTS"
#define CTRL_RESP_HEBBIAN    "HEBBIAN_RESP"
```

- [ ] **Step 2: Add handler for CTRL_GET_EVENTS in test_node_main.c**

Add in the `handle_command` function, before the final `else` clause:

```c
} else if (strcmp(line, CTRL_GET_EVENTS) == 0) {
  handle_get_events(client_fd, 0);
} else if (strncmp(line, CTRL_GET_EVENTS " ",
                strlen(CTRL_GET_EVENTS) + 1) == 0) {
  size_t cursor = (size_t)atol(line + strlen(CTRL_GET_EVENTS) + 1);
  handle_get_events(client_fd, cursor);
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
}
```

- [ ] **Step 3: Implement the handler functions**

Add these handler functions before `handle_command` in `test/test_node_main.c`:

```c
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
    message_event_t* event = &events[idx];
    char peer_hex[NODE_ID_STRING_SIZE];
    node_id_to_string(&event->peer_id, peer_hex);

    // Format first 8 hex chars of block_hash for correlation
    char hash_prefix[17];
    for (int byte = 0; byte < 8 && byte < 32; byte++) {
      snprintf(hash_prefix + byte * 2, 3, "%02x", event->block_hash[byte]);
    }
    hash_prefix[16] = '\0';

    if (event->peer_id.hash[0] == 0 && event->peer_id.hash[1] == 0 &&
        event->peer_id.hash[2] == 0 && event->peer_id.hash[3] == 0) {
      // Null peer — use "0" instead of all-zero hex
      offset += snprintf(response + offset, sizeof(response) - offset,
                         "%zu:%u,%u,0,%llu,%s,%u,%.4f",
                         cursor + idx, event->type, event->direction,
                         (unsigned long long)event->message_id,
                         hash_prefix, event->result, event->hebbian_weight);
    } else {
      offset += snprintf(response + offset, sizeof(response) - offset,
                         "%zu:%u,%u,%s,%llu,%s,%u,%.4f",
                         cursor + idx, event->type, event->direction,
                         peer_hex, (unsigned long long)event->message_id,
                         hash_prefix, event->result, event->hebbian_weight);
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

  // Parse 64-char hex hash
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

  // Create a local FindBlock message and dispatch it
  wire_find_block_t find;
  memset(&find, 0, sizeof(find));
  find.message_id = (uint64_t)(time(NULL)) ^ ((uint64_t)rand() << 32);
  memcpy(find.block_hash, block_hash, 32);
  find.ttl = FIND_BLOCK_MAX_PATH;
  memcpy(&find.original_source, &g_node.network->authority->local_id, sizeof(node_id_t));
  find.start_time = (uint64_t)(time(NULL) * 1000);

  message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = NETWORK_LOCAL_FIND_BLOCK;
  msg.payload = malloc(sizeof(network_local_find_block_payload_t));
  if (msg.payload == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " allocation failed");
    return;
  }
  memcpy(msg.payload, &find, sizeof(find));
  msg.payload_destroy = free;

  actor_send(&g_node.network->actor, &msg);

  char response[128];
  snprintf(response, sizeof(response), "%s %llu", CTRL_RESP_OK,
           (unsigned long long)find.message_id);
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
    snprintf(response, sizeof(response), "%s %llu", CTRL_RESP_OK,
             (unsigned long long)ping.message_id);
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
    node_id_to_string(&table->entries[idx].peer_id, node_hex);
    offset += snprintf(response + offset, sizeof(response) - offset,
                      "%s:%.4f", node_hex, table->entries[idx].weight);
    if (idx + 1 < table->count) {
      offset += snprintf(response + offset, sizeof(response) - offset, ";");
    }
  }

  send_response(client_fd, response);
}

#endif // OFFS_TEST
```

Note: The `#ifdef OFFS_TEST` guard on the handler functions means they only exist in the test binary. The command dispatcher still needs the `#ifdef OFFS_TEST` guard around the `FIND_BLOCK`, `PING_PEER`, and `HEBBIAN` branches so the compiler doesn't try to call functions that don't exist in release builds. The `GET_EVENTS` and `CLEAR_EVENTS` handlers also need the guard since they reference `message_log_t`.

- [ ] **Step 4: Add the necessary includes**

At the top of `test/test_node_main.c`, add:

```c
#ifdef OFFS_TEST
#include "message_log.h"
#include "hebbian.h"
#include "wire.h"
#include "conn_state.h"
#include "find_block.h"
#endif
```

- [ ] **Step 5: Update CTRL_STATUS to include endpoint**

This was already done in a previous commit (the `FORCE_RELAY_ENDPOINT` feature). Verify the STATUS handler already outputs `endpoint=%u`.

- [ ] **Step 6: Build the test binary**

Run: `cmake --build build --target test_file_transfer_integration 2>&1 | tail -30`

Expected: Clean build. The `#ifdef OFFS_TEST` sections compile because the test target will need `-DOFFS_TEST` added.

Wait — the test binary doesn't define OFFS_TEST yet. We'll add that in Task 6. For now, verify the non-OFFS_TEST path still compiles.

- [ ] **Step 7: Commit**

```bash
git add test/test_control_protocol.h test/test_node_main.c
git commit -m "feat: add event log, Hebbian query, and RPC injection control commands"
```

---

## Task 5: Create test_rpc_integration.cpp with topology helpers and tests

**Files:**
- Create: `test/test_rpc_integration.cpp`

This is the largest task. It creates the test file with:
1. Topology construction helpers (make_chain, make_full_mesh)
2. Event log query helpers (get_events, has_event, count_events, get_hebbian, hebbian_increased)
3. Multi-hop test cases (FindBlock Chain, FindBlock TTL Expiry, StoreBlock Replication Chain, RankBlock Flood)
4. Single-hop test cases (Ping, PingCapacity, PingBlock, FindNode, SeekingBlocks, RecallBlock, RateLimited)

- [ ] **Step 1: Create the test file with fixture and topology helpers**

Create `test/test_rpc_integration.cpp` with the test fixture and topology helpers. The fixture reuses the existing `FileTransferIntegrationTest` patterns (fork/exec, control socket, etc.) but adds the event log and Hebbian query helpers.

The key C++ helper functions:

```cpp
#include <gtest/gtest.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <linux/limits.h>
#include <string.h>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <cmath>
#include "test_control_protocol.h"

extern "C" int node_main(int argc, char* argv[]);

static std::string get_self_path() { /* same as file transfer test */ }
static std::string get_relay_path() { /* same as file transfer test */ }
static std::atomic<uint16_t> next_base_port{25000};

struct TopologyNode {
  pid_t pid = -1;
  uint16_t control_port = 0;
  int control_fd = -1;
  std::string node_id;
  uint32_t endpoint_id = 0;
  std::string cache_dir;
};
```

Then the `RpcIntegrationTest` fixture with SetUp/TearDown (same pattern as `FileTransferIntegrationTest`), plus the topology helpers.

- [ ] **Step 2: Implement event log query helpers**

```cpp
struct MessageEvent {
  uint8_t type;
  uint8_t direction;
  std::string peer_id;
  uint64_t message_id;
  std::string hash_prefix;
  uint8_t result;
  float hebbian_weight;
  size_t index;  // logical index for pagination
};

struct HebbianEntry {
  std::string node_id;
  float weight;
};

std::vector<MessageEvent> get_events(int control_fd, size_t after_cursor = 0);
std::vector<HebbianEntry> get_hebbian(int control_fd);
bool has_event(const std::vector<MessageEvent>& events,
               uint8_t direction, uint8_t type,
               const std::string& peer_id = "",
               uint8_t result = 255);
size_t count_events(const std::vector<MessageEvent>& events,
                    uint8_t direction, uint8_t type);
bool hebbian_increased(const std::vector<HebbianEntry>& before,
                       const std::vector<HebbianEntry>& after,
                       const std::string& peer_id,
                       float min_delta = 0.001f);
bool hebbian_approx(const std::vector<HebbianEntry>& entries,
                    const std::string& peer_id,
                    float expected,
                    float tolerance = 0.01f);
```

The `get_events` function sends `GET_EVENTS <cursor>` and parses the `EVENTS <total>|<index>:<type>,<dir>,<peer_id>,<msg_id>,<hash_prefix>,<result>,<hebbian_weight>;...` response.

The `get_hebbian` function sends `HEBBIAN` and parses the `HEBBIAN_RESP <count>|<node_id>:<weight>;...` response.

- [ ] **Step 3: Implement make_chain topology helper**

```cpp
std::vector<TopologyNode> make_chain(int count, uint16_t base_port,
                                       uint16_t relay_port) {
  // Start relay server
  // Start count nodes with sequential control ports
  // Connect all nodes to the relay
  // Wait for relay connections
  // For each pair of adjacent nodes (i->i+1), use ADD_PEER to set up
  //   bidirectional relay-only connections
  // Wait for peer connections
  // Return the node vector
}
```

- [ ] **Step 4: Implement make_full_mesh topology helper**

```cpp
std::vector<TopologyNode> make_full_mesh(int count, uint16_t base_port,
                                          uint16_t relay_port) {
  // Start relay server
  // Start count nodes
  // Connect all nodes to the relay
  // Wait for relay connections
  // For every pair (i, j) where i != j, use ADD_PEER to set up
  //   bidirectional relay-only connections
  // Wait for peer connections
  // Return the node vector
}
```

- [ ] **Step 5: Write the FindBlock Chain test**

```cpp
TEST_F(RpcIntegrationTest, FindBlockChain) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  // Create 3-node chain: A -> B -> C
  auto nodes = make_chain(3, base_port, base_port + 90);
  ASSERT_EQ(nodes.size(), 3u);

  // Store a block on node C
  std::string store_resp = send_command(nodes[2].control_fd,
      std::string(CTRL_STORE_FILE) + " 1000 2 3");
  ASSERT_NE(store_resp.find(CTRL_RESP_HASH), std::string::npos);

  // Parse the block hash from the store response
  // ...

  // Clear event logs on all nodes
  for (auto& node : nodes) {
    send_command(node.control_fd, CTRL_CLEAR_EVENTS);
  }

  // Get Hebbian state before FindBlock
  auto hebbian_a_before = get_hebbian(nodes[0].control_fd);

  // Node A searches for the block
  std::string find_resp = send_command(nodes[0].control_fd,
      std::string(CTRL_FIND_BLOCK) + " " + block_hash_hex);
  ASSERT_NE(find_resp.find(CTRL_RESP_OK), std::string::npos);

  // Wait for the FindBlock to propagate
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Check events on node B: should have RECEIVED FIND_BLOCK from A, FORWARDED to C
  auto events_b = get_events(nodes[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED, WIRE_FIND_BLOCK));
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_FORWARDED, WIRE_FIND_BLOCK));

  // Check events on node C: should have RECEIVED FIND_BLOCK from B
  auto events_c = get_events(nodes[2].control_fd);
  EXPECT_TRUE(has_event(events_c, MSG_DIRECTION_RECEIVED, WIRE_FIND_BLOCK));

  // Check events on node A: should have RECEIVED FIND_BLOCK_RESPONSE from B
  auto events_a = get_events(nodes[0].control_fd);
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED, WIRE_FIND_BLOCK_RESPONSE));

  // Verify Hebbian weight toward C increased
  auto hebbian_a_after = get_hebbian(nodes[0].control_fd);
  EXPECT_TRUE(hebbian_increased(hebbian_a_before, hebbian_a_after, nodes[2].node_id, 0.01f));
#endif
}
```

- [ ] **Step 6: Write the FindBlock TTL Expiry test**

Similar pattern to FindBlock Chain but with a 4-node chain (A->B->C->D) and TTL=2, verifying that D's TTL expires and NOT_FOUND propagates back.

- [ ] **Step 7: Write the StoreBlock Replication Chain test**

3-node chain: A stores a block with replicas_needed=3, verifying that B accepts and forwards to C, and STORE_BLOCK_RESPONSE propagates back.

- [ ] **Step 8: Write the RankBlock Flood test**

A sends RankBlock, verifying that B and C receive it with incremented hop_count.

- [ ] **Step 9: Write single-hop tests (Ping, PingCapacity, PingBlock)****

2-node direct connection tests verifying round-trip and Hebbian weight changes.

- [ ] **Step 10: Build the test binary**

Run: `cmake --build build --target test_rpc_integration 2>&1 | tail -30`

Expected: Clean compilation.

- [ ] **Step 11: Run the tests**

Run: `./build/test/test_rpc_integration --gtest_filter="*FindBlockChain*" 2>&1 | tail -30`

Expected: Test runs (may need debugging of control command handlers).

- [ ] **Step 12: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add multi-hop RPC integration tests with Hebbian verification"
```

---

## Task 6: Add test_rpc_integration target to CMakeLists.txt

**Files:**
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Add test_rpc_integration target**

Add after the `test_file_transfer_integration` target definition:

```cmake
add_executable(test_rpc_integration test_rpc_integration.cpp test_node_main.c)
add_dependencies(test_rpc_integration cbor)
add_dependencies(test_rpc_integration offs)
add_dependencies(test_rpc_integration blake3)
add_dependencies(test_rpc_integration http-parser)
target_compile_definitions(test_rpc_integration PRIVATE OFFS_TEST)
target_link_libraries(test_rpc_integration PRIVATE -Wl,--whole-archive offs -Wl,--no-whwhole-archive)
target_link_libraries(test_rpc_integration PRIVATE ssl crypto)
target_link_libraries(test_rpc_integration PRIVATE blake3)
target_link_libraries(test_rpc_integration PRIVATE hashmap)
target_link_libraries(test_rpc_integration PRIVATE http-parser)
target_link_libraries(test_rpc_integration PUBLIC cbor)
target_link_libraries(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../poll-dancer/build/libpoll_dancer.a)
target_link_libraries(test_rpc_integration PRIVATE GTest::gtest_main)
target_link_libraries(test_rpc_integration PRIVATE GTest::gmock)
target_link_libraries(test_rpc_integration PRIVATE pthread)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
  target_compile_definitions(test_rpc_integration PRIVATE HAS_MSQUIC)
  target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/src/inc)
  target_link_libraries(test_rpc_integration PRIVATE msquic::msquic msquic::platform)
endif()
target_include_directories(test_rpc_integration PUBLIC ${C_INC})
target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../poll-dancer/include)
target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)

include(GoogleTest)
gtest_add_tests(TARGET test_rpc_integration TEST_LIST test_list)
```

Note the key difference from `test_file_transfer_integration`: `target_compile_definitions(test_rpc_integration PRIVATE OFFS_TEST)` enables the event logging, Hebbian query, and RPC injection infrastructure.

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --target test_rpc_integration 2>&1 | tail -30`

Expected: Clean build with OFFS_TEST enabled.

- [ ] **Step 3: Commit**

```bash
git add test/CMakeLists.txt
git commit -m "build: add test_rpc_integration target with OFFS_TEST flag"
```

---

## Task 7: Integration testing and debugging

**Files:**
- Potentially all files modified in Tasks 1-6

- [ ] **Step 1: Run the full test suite**

Run: `ctest --test-dir build 2>&1 | tail -40`

Expected: All existing tests pass. New test_rpc_integration tests pass.

- [ ] **Step 2: Fix any failures**

Debug and fix any issues found during integration testing. Common issues:
- Control command parsing errors (format string mismatch)
- Event log format errors (test-side parsing issues)
- Timing issues (not waiting long enough for message propagation)
- Missing includes or linking issues

- [ ] **Step 3: Run valgrind for memory leaks**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/test/test_rpc_integration --gtest_filter="*FindBlock*" 2>&1 | tail -40`

Expected: No memory leaks in the new code paths.

- [ ] **Step 4: Run de-wonk audit**

Use the de-wonk skill to audit the new files for:
- Unimplemented/stubbed functions
- Disabled code paths
- Memory leaks
- Inconsistent patterns

- [ ] **Step 5: Final commit if any fixes are needed**

```bash
git add -A
git commit -m "fix: integration test fixes from de-wonk audit"
```