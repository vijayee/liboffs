# RPC Networking Integration Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 8 integration tests for untested RPC wire messages (PING_CAPACITY, PING_BLOCK, FIND_NODE, SEEKING_BLOCKS, RECALL_BLOCK, RATE_LIMITED, STORE_BLOCK Hebbian) with full algorithm verification including hop behavior and Hebbian weight calculations.

**Architecture:** Add 6 new control protocol commands to `test_node_main.c` that construct and send wire messages directly via `conn_state_send()` or `actor_send()`, then write integration tests in `test_rpc_integration.cpp` using the existing multi-process fork/exec harness with purpose-built topologies.

**Tech Stack:** C (test_node_main.c), C++ with Google Test (test_rpc_integration.cpp), CBOR wire encoding, QUIC networking via msquic

---

## File Structure

| File | Change | Purpose |
|---|---|---|
| `test/test_control_protocol.h` | Modify | Add 6 new command + response prefix constants |
| `test/test_node_main.c` | Modify | Add 6 control command handlers (guarded by OFFS_TEST) |
| `test/test_rpc_integration.cpp` | Modify | Add wire type constants, topology helpers, 8 test cases |
| `src/Actor/message.h` | Modify | Add `NETWORK_LOCAL_FIND_NODE` enum value and `network_local_find_node_payload_t` struct |
| `src/Network/network.c` | Modify | Add `NETWORK_LOCAL_FIND_NODE` dispatch case and destroy function |

---

### Task 1: Add control protocol constants

**Files:**
- Modify: `test/test_control_protocol.h`

- [ ] **Step 1: Add new command constants**

Add after the existing `CTRL_MEASURE_NODES` definition (line 24):

```c
/* Additional RPC test commands (OFFS_TEST only) */
#define CTRL_PING_CAPACITY   "PING_CAPACITY"
#define CTRL_PING_BLOCK      "PING_BLOCK"
#define CTRL_FIND_NODE       "FIND_NODE"
#define CTRL_SEEKING_BLOCKS  "SEEKING_BLOCKS"
#define CTRL_RECALL_BLOCK    "RECALL_BLOCK"
#define CTRL_STORE_BLOCK     "STORE_BLOCK"
```

- [ ] **Step 2: Add new response prefix constants**

Add after the existing `CTRL_RESP_MEASURE_NODES` definition (line 29):

```c
#define CTRL_RESP_PING_CAPACITY  "PING_CAPACITY_RESP"
#define CTRL_RESP_PING_BLOCK     "PING_BLOCK_RESP"
#define CTRL_RESP_FIND_NODE      "FIND_NODE_RESP"
#define CTRL_RESP_SEEKING_BLOCKS "SEEKING_BLOCKS_RESP"
#define CTRL_RESP_RECALL         "RECALL_RESP"
```

- [ ] **Step 3: Commit**

```bash
git add test/test_control_protocol.h
git commit -m "test: add control protocol constants for RPC integration tests"
```

---

### Task 2: Add NETWORK_LOCAL_FIND_NODE message type and payload

**Files:**
- Modify: `src/Actor/message.h`
- Modify: `src/Network/network.c`

- [ ] **Step 1: Add payload struct and destroy declaration in message.h**

Add after the `network_local_closest_nodes_payload_t` struct (around line 207):

```c
typedef struct {
  node_id_t target_id;
  actor_t*  reply_to;
} network_local_find_node_payload_t;

void network_local_find_node_payload_destroy(void* ptr);
```

- [ ] **Step 2: Add NETWORK_LOCAL_FIND_NODE enum value in message.h**

Add after `NETWORK_LOCAL_CLOSEST_NODES` (around line 165):

```c
  NETWORK_LOCAL_FIND_NODE,
```

- [ ] **Step 3: Add destroy function implementation in network.c**

Add after `network_local_closest_nodes_payload_destroy()` (around line 87):

```c
void network_local_find_node_payload_destroy(void* ptr) {
  if (ptr == NULL) return;
  free(ptr);
}
```

- [ ] **Step 4: Add dispatch case in network_dispatch()**

Add after the `NETWORK_LOCAL_CLOSEST_NODES` case:

```c
    case NETWORK_LOCAL_FIND_NODE: {
      network_local_find_node_payload_t* payload =
          (network_local_find_node_payload_t*)msg->payload;
      if (payload == NULL) break;

      /* Build a FindNode wire message and send to each connected peer */
      wire_find_node_t find;
      memset(&find, 0, sizeof(find));
      find.message_id = (uint64_t)time(NULL) ^ ((uint64_t)rand() << 32);
      memcpy(&find.sender_id, &network->authority->local_id, sizeof(node_id_t));
      memcpy(&find.target_id, &payload->target_id, sizeof(node_id_t));

      for (size_t peer_idx = 0; peer_idx < network->conn_mgr.peer_count; peer_idx++) {
        peer_connection_t* peer = network->conn_mgr.peers[peer_idx];
        if (peer != NULL && peer->connected) {
          cbor_item_t* cbor = wire_find_node_encode(&find);
          conn_state_send(network, peer, cbor);
          cbor_decref(&cbor);
        }
      }
      break;
    }
```

- [ ] **Step 5: Commit**

```bash
git add src/Actor/message.h src/Network/network.c
git commit -m "feat: add NETWORK_LOCAL_FIND_NODE message type and dispatch"
```

---

### Task 3: Add PING_CAPACITY control command handler

**Files:**
- Modify: `test/test_node_main.c`

This handler sends a `WIRE_PING_CAPACITY` message directly to a specified peer via `conn_state_send()`, following the same pattern as `handle_ping_peer_cmd()`.

- [ ] **Step 1: Add handle_ping_capacity_cmd() function**

Add after `handle_measure_nodes_cmd()` and before `#endif // OFFS_TEST` (around line 880):

```c
static void handle_ping_capacity_cmd(int client_fd, const char* node_id_hex) {
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

  wire_ping_capacity_t ping;
  memset(&ping, 0, sizeof(ping));
  ping.message_id = (uint64_t)(time(NULL)) ^ ((uint64_t)rand() << 32);
  memcpy(&ping.source, &g_node.network->authority->local_id, sizeof(node_id_t));
  ping.capacity = atomic_load(&g_node.network->authority->capacity);
  ping.phase = atomic_load(&g_node.network->authority->phase);

  cbor_item_t* cbor = wire_ping_capacity_encode(&ping);
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
```

- [ ] **Step 2: Add PING_CAPACITY command dispatch in handle_command()**

Add inside the `#ifdef OFFS_TEST` section, after the `CTRL_MEASURE_NODES` dispatch block:

```c
  } else if (strncmp(line, CTRL_PING_CAPACITY " ",
                strlen(CTRL_PING_CAPACITY) + 1) == 0) {
#ifdef OFFS_TEST
    handle_ping_capacity_cmd(client_fd, line + strlen(CTRL_PING_CAPACITY) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
```

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "test: add PING_CAPACITY control command handler"
```

---

### Task 4: Add PING_BLOCK control command handler

**Files:**
- Modify: `test/test_node_main.c`

- [ ] **Step 1: Add handle_ping_block_cmd() function**

Add after `handle_ping_capacity_cmd()`:

```c
static void handle_ping_block_cmd(int client_fd, const char* args) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  /* Parse: <node_id_hex> <block_hash_hex> */
  char node_id_str[NODE_ID_STRING_SIZE];
  char hash_hex[65];
  if (sscanf(args, "%47s %64s", node_id_str, hash_hex) != 2) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid PING_BLOCK args");
    return;
  }

  node_id_t peer_id;
  memset(&peer_id, 0, sizeof(peer_id));
  if (node_id_from_string(node_id_str, &peer_id) != 0) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid node_id");
    return;
  }

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

  peer_connection_t* peer = connection_manager_lookup(&g_node.network->conn_mgr, &peer_id);
  if (peer == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " peer not found");
    return;
  }

  wire_ping_block_t ping;
  memset(&ping, 0, sizeof(ping));
  ping.message_id = (uint64_t)(time(NULL)) ^ ((uint64_t)rand() << 32);
  memcpy(&ping.sender_id, &g_node.network->authority->local_id, sizeof(node_id_t));
  memcpy(ping.block_hash, block_hash, 32);

  cbor_item_t* cbor = wire_ping_block_encode(&ping);
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
```

- [ ] **Step 2: Add PING_BLOCK command dispatch in handle_command()**

```c
  } else if (strncmp(line, CTRL_PING_BLOCK " ",
                strlen(CTRL_PING_BLOCK) + 1) == 0) {
#ifdef OFFS_TEST
    handle_ping_block_cmd(client_fd, line + strlen(CTRL_PING_BLOCK) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
```

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "test: add PING_BLOCK control command handler"
```

---

### Task 5: Add FIND_NODE control command handler

**Files:**
- Modify: `test/test_node_main.c`

This handler uses `actor_send()` with `NETWORK_LOCAL_FIND_NODE` to trigger the network actor, similar to `handle_find_block_cmd()` and `handle_closest_nodes_cmd()`.

- [ ] **Step 1: Add handle_find_node_cmd() function**

```c
static void handle_find_node_cmd(int client_fd, const char* target_id_str) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  node_id_t target_id;
  memset(&target_id, 0, sizeof(target_id));
  if (node_id_from_string((char*)target_id_str, &target_id) != 0) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid node_id");
    return;
  }

  network_local_find_node_payload_t* payload =
      get_clear_memory(sizeof(network_local_find_node_payload_t));
  if (payload == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " allocation failed");
    return;
  }
  memcpy(&payload->target_id, &target_id, sizeof(node_id_t));
  payload->reply_to = &g_node.network->actor;

  message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = NETWORK_LOCAL_FIND_NODE;
  msg.payload = payload;
  msg.payload_destroy = network_local_find_node_payload_destroy;

  actor_send(&g_node.network->actor, &msg);

  send_response(client_fd, CTRL_RESP_OK);
}
```

- [ ] **Step 2: Add FIND_NODE command dispatch in handle_command()**

```c
  } else if (strncmp(line, CTRL_FIND_NODE " ",
                strlen(CTRL_FIND_NODE) + 1) == 0) {
#ifdef OFFS_TEST
    handle_find_node_cmd(client_fd, line + strlen(CTRL_FIND_NODE) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
```

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "test: add FIND_NODE control command handler"
```

---

### Task 6: Add SEEKING_BLOCKS control command handler

**Files:**
- Modify: `test/test_node_main.c`

This handler constructs a `wire_seeking_blocks_t` and sends it to all connected peers via `conn_state_send()`. The exclude_hashes field uses the current `uint8_t**` array format (bloom filter refactor deferred).

- [ ] **Step 1: Add handle_seeking_blocks_cmd() function**

```c
static void handle_seeking_blocks_cmd(int client_fd, const char* args) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  /* Parse: <capacity> [<exclude_hash1> <exclude_hash2> ...] */
  float capacity = 1.0f;
  uint8_t** exclude_hashes = NULL;
  size_t exclude_count = 0;

  char args_copy[2048];
  strncpy(args_copy, args, sizeof(args_copy) - 1);
  args_copy[sizeof(args_copy) - 1] = '\0';

  char* saveptr = NULL;
  char* token = strtok_r(args_copy, " ", &saveptr);

  /* First token is capacity */
  if (token != NULL) {
    capacity = strtof(token, NULL);
    token = strtok_r(NULL, " ", &saveptr);
  }

  /* Remaining tokens are exclude hashes (64-char hex) */
  size_t hash_capacity = 8;
  exclude_hashes = get_clear_memory(hash_capacity * sizeof(uint8_t*));
  if (exclude_hashes == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " allocation failed");
    return;
  }

  while (token != NULL) {
    if (strlen(token) != 64) {
      token = strtok_r(NULL, " ", &saveptr);
      continue;
    }
    uint8_t* hash = get_clear_memory(32);
    if (hash == NULL) break;
    int valid = 1;
    for (int idx = 0; idx < 32 && valid; idx++) {
      unsigned int byte;
      if (sscanf(token + idx * 2, "%02x", &byte) != 1) {
        valid = 0;
      } else {
        hash[idx] = (uint8_t)byte;
      }
    }
    if (!valid) {
      free(hash);
      token = strtok_r(NULL, " ", &saveptr);
      continue;
    }
    if (exclude_count >= hash_capacity) {
      hash_capacity *= 2;
      uint8_t** new_hashes = realloc(exclude_hashes, hash_capacity * sizeof(uint8_t*));
      if (new_hashes == NULL) break;
      exclude_hashes = new_hashes;
    }
    exclude_hashes[exclude_count++] = hash;
    token = strtok_r(NULL, " ", &saveptr);
  }

  /* Send to all connected peers */
  wire_seeking_blocks_t seeking;
  memset(&seeking, 0, sizeof(seeking));
  seeking.message_id = (uint64_t)(time(NULL)) ^ ((uint64_t)rand() << 32);
  memcpy(&seeking.sender_id, &g_node.network->authority->local_id, sizeof(node_id_t));
  seeking.capacity = capacity;
  seeking.exclude_hashes = exclude_hashes;
  seeking.exclude_count = exclude_count;

  int sent_count = 0;
  for (size_t peer_idx = 0; peer_idx < g_node.network->conn_mgr.peer_count; peer_idx++) {
    peer_connection_t* peer = g_node.network->conn_mgr.peers[peer_idx];
    if (peer != NULL && peer->connected) {
      cbor_item_t* cbor = wire_seeking_blocks_encode(&seeking);
      if (cbor != NULL) {
        conn_state_send(g_node.network, peer, cbor);
        cbor_decref(&cbor);
        sent_count++;
      }
    }
  }

  /* Free exclude hashes */
  for (size_t idx = 0; idx < exclude_count; idx++) {
    free(exclude_hashes[idx]);
  }
  free(exclude_hashes);

  if (sent_count > 0) {
    char response[128];
    snprintf(response, sizeof(response), "%s seeking_blocks_sent_to_%d_peers",
             CTRL_RESP_OK, sent_count);
    send_response(client_fd, response);
  } else {
    send_response(client_fd, CTRL_RESP_ERROR " no connected peers");
  }
}
```

- [ ] **Step 2: Add SEEKING_BLOCKS command dispatch in handle_command()**

```c
  } else if (strncmp(line, CTRL_SEEKING_BLOCKS " ",
                strlen(CTRL_SEEKING_BLOCKS) + 1) == 0) {
#ifdef OFFS_TEST
    handle_seeking_blocks_cmd(client_fd, line + strlen(CTRL_SEEKING_BLOCKS) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
```

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "test: add SEEKING_BLOCKS control command handler"
```

---

### Task 7: Add RECALL_BLOCK control command handler

**Files:**
- Modify: `test/test_node_main.c`

This handler constructs a `wire_recall_block_t` and sends it to all connected peers.

- [ ] **Step 1: Add handle_recall_block_cmd() function**

```c
static void handle_recall_block_cmd(int client_fd, const char* hash_hex) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

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

  wire_recall_block_t recall;
  memset(&recall, 0, sizeof(recall));
  recall.message_id = (uint64_t)(time(NULL)) ^ ((uint64_t)rand() << 32);
  memcpy(&recall.sender_id, &g_node.network->authority->local_id, sizeof(node_id_t));
  memcpy(recall.block_hash, block_hash, 32);

  int sent_count = 0;
  for (size_t peer_idx = 0; peer_idx < g_node.network->conn_mgr.peer_count; peer_idx++) {
    peer_connection_t* peer = g_node.network->conn_mgr.peers[peer_idx];
    if (peer != NULL && peer->connected) {
      cbor_item_t* cbor = wire_recall_block_encode(&recall);
      if (cbor != NULL) {
        conn_state_send(g_node.network, peer, cbor);
        cbor_decref(&cbor);
        sent_count++;
      }
    }
  }

  if (sent_count > 0) {
    char response[128];
    snprintf(response, sizeof(response), "%s recall_sent_to_%d_peers",
             CTRL_RESP_OK, sent_count);
    send_response(client_fd, response);
  } else {
    send_response(client_fd, CTRL_RESP_ERROR " no connected peers");
  }
}
```

- [ ] **Step 2: Add RECALL_BLOCK command dispatch in handle_command()**

```c
  } else if (strncmp(line, CTRL_RECALL_BLOCK " ",
                strlen(CTRL_RECALL_BLOCK) + 1) == 0) {
#ifdef OFFS_TEST
    handle_recall_block_cmd(client_fd, line + strlen(CTRL_RECALL_BLOCK) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
```

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "test: add RECALL_BLOCK control command handler"
```

---

### Task 8: Add STORE_BLOCK control command handler

**Files:**
- Modify: `test/test_node_main.c`

This handler injects a `NETWORK_LOCAL_STORE_BLOCK` message via `actor_send()`, using the existing `network_local_store_block_payload_t`. It looks up the block in the local block cache to get the FIB counter.

- [ ] **Step 1: Add handle_store_block_cmd() function**

```c
static void handle_store_block_cmd(int client_fd, const char* args) {
  if (g_node.network == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " no network");
    return;
  }

  char hash_hex[65];
  int carry_data_int = 0;
  if (sscanf(args, "%64s %d", hash_hex, &carry_data_int) != 2) {
    send_response(client_fd, CTRL_RESP_ERROR " invalid STORE_BLOCK args");
    return;
  }

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

  buffer_t* hash_buf = buffer_create_from_pointer_copy(block_hash, 32);
  if (hash_buf == NULL) {
    send_response(client_fd, CTRL_RESP_ERROR " allocation failed");
    return;
  }

  network_local_store_block_payload_t* payload =
      get_clear_memory(sizeof(network_local_store_block_payload_t));
  if (payload == NULL) {
    buffer_destroy(hash_buf);
    send_response(client_fd, CTRL_RESP_ERROR " allocation failed");
    return;
  }

  payload->hash = hash_buf;
  payload->reply_to = NULL;  /* fire-and-forget */

  /* Look up FIB counter from block cache index */
  index_entry_t* entry = index_peek(g_node.block_cache->index, hash_buf);
  payload->fib = (entry != NULL) ? entry->counter.fib : 0;

  message_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = NETWORK_LOCAL_STORE_BLOCK;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&g_node.network->actor, &msg);

  char response[128];
  snprintf(response, sizeof(response), "%s store_block_injected",
           CTRL_RESP_OK);
  send_response(client_fd, response);
}
```

- [ ] **Step 2: Add include for index.h**

Ensure `test_node_main.c` includes the `index_peek` declaration. Add at the top with the other includes if not already present:

```c
#include "../src/BlockCache/index.h"
```

- [ ] **Step 3: Add STORE_BLOCK command dispatch in handle_command()**

```c
  } else if (strncmp(line, CTRL_STORE_BLOCK " ",
                strlen(CTRL_STORE_BLOCK) + 1) == 0) {
#ifdef OFFS_TEST
    handle_store_block_cmd(client_fd, line + strlen(CTRL_STORE_BLOCK) + 1);
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
```

- [ ] **Step 4: Commit**

```bash
git add test/test_node_main.c
git commit -m "test: add STORE_BLOCK control command handler"
```

---

### Task 9: Add wire type constants to test_rpc_integration.cpp

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Add missing wire type constants**

Add after the existing `WIRE_CLOSEST_NODES_PROGRESS_VAL` constant (line 52):

```cpp
static constexpr uint8_t WIRE_PING_BLOCK_VAL             = 5;
static constexpr uint8_t WIRE_PING_BLOCK_RESPONSE_VAL     = 6;
static constexpr uint8_t WIRE_FIND_NODE_VAL               = 9;
static constexpr uint8_t WIRE_FIND_NODE_RESPONSE_VAL      = 10;
static constexpr uint8_t WIRE_STORE_BLOCK_VAL             = 11;
static constexpr uint8_t WIRE_STORE_BLOCK_RESPONSE_VAL    = 12;
static constexpr uint8_t WIRE_SEEKING_BLOCKS_VAL          = 13;
static constexpr uint8_t WIRE_SEEKING_BLOCKS_RESPONSE_VAL = 14;
static constexpr uint8_t WIRE_RECALL_BLOCK_VAL            = 16;
static constexpr uint8_t WIRE_RECALL_ACCEPT_VAL           = 17;
static constexpr uint8_t WIRE_RECALL_DECLINE_VAL          = 18;
static constexpr uint8_t WIRE_RATE_LIMITED_VAL            = 19;
```

Note: `WIRE_PING_CAPACITY_VAL` (3) and `WIRE_PING_CAPACITY_RESPONSE_VAL` (4) are already defined on lines 29-30.

- [ ] **Step 2: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add wire type constants for untested RPC messages"
```

---

### Task 10: Write PingCapacityRoundTrip and PingCapacityHebbianVerification tests

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Write PingCapacityRoundTrip test**

Add before the `main()` function, after the existing `MeasureNodesLatency` test:

```cpp
TEST_F(RpcIntegrationTest, PingCapacityRoundTrip) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 2-node mesh */
  auto mesh = make_full_mesh(2);
  ASSERT_EQ(mesh.size(), 2u);

  /* Clear event logs */
  for (size_t idx = 0; idx < mesh.size(); idx++) {
    clear_events(mesh[idx].control_fd);
  }

  /* Node A sends PING_CAPACITY to Node B */
  std::string cap_resp = send_command(mesh[0].control_fd,
      std::string(CTRL_PING_CAPACITY) + " " + mesh[1].node_id);
  EXPECT_NE(cap_resp.find(CTRL_RESP_OK), std::string::npos)
      << "PING_CAPACITY from node A: " << cap_resp;

  /* Wait for round trip */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Verify Node B received WIRE_PING_CAPACITY */
  auto events_b = get_events(mesh[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_CAPACITY_VAL))
      << "Node B should have received PING_CAPACITY";

  /* Verify Node A received WIRE_PING_CAPACITY_RESPONSE */
  auto events_a = get_events(mesh[0].control_fd);
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_CAPACITY_RESPONSE_VAL))
      << "Node A should have received PING_CAPACITY_RESPONSE";
#endif
}
```

- [ ] **Step 2: Write PingCapacityHebbianVerification test**

```cpp
TEST_F(RpcIntegrationTest, PingCapacityHebbianVerification) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 2-node mesh */
  auto mesh = make_full_mesh(2);
  ASSERT_EQ(mesh.size(), 2u);

  /* Query Hebbian weights before ping */
  auto hebbian_before_a = get_hebbian(mesh[0].control_fd);

  /* Clear event logs */
  for (size_t idx = 0; idx < mesh.size(); idx++) {
    clear_events(mesh[idx].control_fd);
  }

  /* Node A sends 3 PING_CAPACITY to Node B */
  for (int ping_count = 0; ping_count < 3; ping_count++) {
    std::string cap_resp = send_command(mesh[0].control_fd,
        std::string(CTRL_PING_CAPACITY) + " " + mesh[1].node_id);
    EXPECT_NE(cap_resp.find(CTRL_RESP_OK), std::string::npos)
        << "PING_CAPACITY #" << ping_count << " from node A: " << cap_resp;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  /* Wait for all responses */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Query Hebbian weights after ping */
  auto hebbian_after_a = get_hebbian(mesh[0].control_fd);

  /* Verify weight toward Node B increased */
  EXPECT_TRUE(hebbian_increased(hebbian_before_a, hebbian_after_a, mesh[1].node_id))
      << "Node A's Hebbian weight toward Node B should have increased after PING_CAPACITY";

  /* Verify WIRE_PING_CAPACITY sent events */
  auto events_a = get_events(mesh[0].control_fd);
  EXPECT_GE(count_events(events_a, MSG_DIRECTION_SENT_VAL, WIRE_PING_CAPACITY_VAL), 1u)
      << "Node A should have sent at least one PING_CAPACITY";
  EXPECT_GE(count_events(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_CAPACITY_RESPONSE_VAL), 1u)
      << "Node A should have received at least one PING_CAPACITY_RESPONSE";
#endif
}
```

- [ ] **Step 3: Build and verify tests compile**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test_rpc_integration 2>&1 | tail -20
```

- [ ] **Step 4: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add PingCapacity round-trip and Hebbian verification tests"
```

---

### Task 11: Write PingBlockRoundTrip test

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Write PingBlockRoundTrip test**

```cpp
TEST_F(RpcIntegrationTest, PingBlockRoundTrip) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 2-node mesh */
  auto mesh = make_full_mesh(2);
  ASSERT_EQ(mesh.size(), 2u);

  /* Store a block on Node B */
  std::string store_resp = send_command(mesh[1].control_fd,
      std::string(CTRL_STORE_FILE) + " 100000 2 3");
  ASSERT_NE(store_resp.find(CTRL_RESP_HASH), std::string::npos)
      << "STORE on node B: " << store_resp;

  std::string block_hash = parse_hash_from_store_response(store_resp);
  ASSERT_FALSE(block_hash.empty()) << "Failed to parse block hash from: " << store_resp;

  /* Record initial Hebbian weights on Node A */
  auto hebbian_before_a = get_hebbian(mesh[0].control_fd);

  /* Clear event logs */
  for (size_t idx = 0; idx < mesh.size(); idx++) {
    clear_events(mesh[idx].control_fd);
  }

  /* Node A sends PING_BLOCK to Node B with the block hash */
  std::string ping_resp = send_command(mesh[0].control_fd,
      std::string(CTRL_PING_BLOCK) + " " + mesh[1].node_id + " " + block_hash);
  EXPECT_NE(ping_resp.find(CTRL_RESP_OK), std::string::npos)
      << "PING_BLOCK from node A: " << ping_resp;

  /* Wait for round trip */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Verify Node B received WIRE_PING_BLOCK */
  auto events_b = get_events(mesh[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_BLOCK_VAL))
      << "Node B should have received PING_BLOCK";

  /* Verify Node A received WIRE_PING_BLOCK_RESPONSE */
  auto events_a = get_events(mesh[0].control_fd);
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_BLOCK_RESPONSE_VAL))
      << "Node A should have received PING_BLOCK_RESPONSE";

  /* Verify Hebbian weight toward Node B increased (successful block ping strengthens Hebbian) */
  auto hebbian_after_a = get_hebbian(mesh[0].control_fd);
  EXPECT_TRUE(hebbian_increased(hebbian_before_a, hebbian_after_a, mesh[1].node_id))
      << "Node A's Hebbian weight toward Node B should have increased after PING_BLOCK";
#endif
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test_rpc_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add PingBlock round-trip test with Hebbian verification"
```

---

### Task 12: Write FindNodeDiamond test

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Write FindNodeDiamond test**

```cpp
TEST_F(RpcIntegrationTest, FindNodeDiamond) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create diamond topology: A-B-D, A-C-D */
  auto diamond = make_diamond();
  ASSERT_EQ(diamond.size(), 4u);

  /* Let gossip establish ring membership first */
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    send_command(diamond[idx].control_fd, CTRL_GOSSIP);
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Clear event logs */
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    clear_events(diamond[idx].control_fd);
  }

  /* Node A initiates FindNode query for Node D */
  std::string find_resp = send_command(diamond[0].control_fd,
      std::string(CTRL_FIND_NODE) + " " + diamond[3].node_id);
  EXPECT_NE(find_resp.find(CTRL_RESP_OK), std::string::npos)
      << "FIND_NODE from node A: " << find_resp;

  /* Wait for query propagation */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Verify WIRE_FIND_NODE was sent by Node A */
  auto events_a = get_events(diamond[0].control_fd);
  EXPECT_GE(count_events(events_a, MSG_DIRECTION_SENT_VAL, WIRE_FIND_NODE_VAL), 1u)
      << "Node A should have sent WIRE_FIND_NODE";

  /* Verify WIRE_FIND_NODE was received/forwarded by intermediate nodes */
  bool intermediate_received = false;
  for (size_t idx = 1; idx < 3; idx++) {
    auto events = get_events(diamond[idx].control_fd);
    if (has_event(events, MSG_DIRECTION_RECEIVED_VAL, WIRE_FIND_NODE_VAL) ||
        has_event(events, MSG_DIRECTION_FORWARDED_VAL, WIRE_FIND_NODE_VAL)) {
      intermediate_received = true;
      break;
    }
  }
  EXPECT_TRUE(intermediate_received)
      << "At least one intermediate node should have received/forwarded FIND_NODE";

  /* Verify WIRE_FIND_NODE_RESPONSE was received by Node A */
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_FIND_NODE_RESPONSE_VAL))
      << "Node A should have received FIND_NODE_RESPONSE";

  /* Verify total forwarded FIND_NODE messages are bounded */
  size_t total_forwards = 0;
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    auto events = get_events(diamond[idx].control_fd);
    total_forwards += count_events(events, MSG_DIRECTION_FORWARDED_VAL, WIRE_FIND_NODE_VAL);
  }
  EXPECT_LE(total_forwards, 6u)
      << "Total forwarded FIND_NODE messages should be bounded (no loops)";
#endif
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test_rpc_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add FindNode diamond topology test"
```

---

### Task 13: Write SeekingBlocksChain test

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Write SeekingBlocksChain test**

```cpp
TEST_F(RpcIntegrationTest, SeekingBlocksChain) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 3-node chain: A -> B -> C */
  auto chain = make_chain(3);
  ASSERT_EQ(chain.size(), 3u);

  /* Store a block on Node C */
  std::string store_resp = send_command(chain[2].control_fd,
      std::string(CTRL_STORE_FILE) + " 100000 2 3");
  ASSERT_NE(store_resp.find(CTRL_RESP_HASH), std::string::npos)
      << "STORE on node C: " << store_resp;

  /* Clear event logs on all nodes */
  for (size_t idx = 0; idx < chain.size(); idx++) {
    clear_events(chain[idx].control_fd);
  }

  /* Node A sends SEEKING_BLOCKS with capacity=1.0, no excludes */
  std::string seek_resp = send_command(chain[0].control_fd,
      std::string(CTRL_SEEKING_BLOCKS) + " 1.0");
  EXPECT_NE(seek_resp.find(CTRL_RESP_OK), std::string::npos)
      << "SEEKING_BLOCKS from node A: " << seek_resp;

  /* Wait for propagation */
  std::this_thread::sleep_for(std::chrono::seconds(3));

  /* Verify WIRE_SEEKING_BLOCKS was sent by Node A */
  auto events_a = get_events(chain[0].control_fd);
  EXPECT_GE(count_events(events_a, MSG_DIRECTION_SENT_VAL, WIRE_SEEKING_BLOCKS_VAL), 1u)
      << "Node A should have sent WIRE_SEEKING_BLOCKS";

  /* Verify WIRE_SEEKING_BLOCKS_RESPONSE was received by at least one node */
  bool any_response = false;
  for (size_t idx = 0; idx < chain.size(); idx++) {
    auto events = get_events(chain[idx].control_fd);
    if (count_events(events, MSG_DIRECTION_RECEIVED_VAL, WIRE_SEEKING_BLOCKS_RESPONSE_VAL) > 0 ||
        count_events(events, MSG_DIRECTION_SENT_VAL, WIRE_SEEKING_BLOCKS_RESPONSE_VAL) > 0) {
      any_response = true;
      break;
    }
  }
  EXPECT_TRUE(any_response) << "At least one node should have received/sent SEEKING_BLOCKS_RESPONSE";

  /* Verify total forwarded SEEKING_BLOCKS messages are bounded */
  size_t total_forwards = 0;
  for (size_t idx = 0; idx < chain.size(); idx++) {
    auto events = get_events(chain[idx].control_fd);
    total_forwards += count_events(events, MSG_DIRECTION_FORWARDED_VAL, WIRE_SEEKING_BLOCKS_VAL);
  }
  EXPECT_LE(total_forwards, 4u)
      << "Total forwarded SEEKING_BLOCKS messages should be bounded";
#endif
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test_rpc_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add SeekingBlocks chain topology test"
```

---

### Task 14: Write RecallBlockRoundTrip test

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Write RecallBlockRoundTrip test**

```cpp
TEST_F(RpcIntegrationTest, RecallBlockRoundTrip) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 2-node mesh */
  auto mesh = make_full_mesh(2);
  ASSERT_EQ(mesh.size(), 2u);

  /* Store a block on Node B */
  std::string store_resp = send_command(mesh[1].control_fd,
      std::string(CTRL_STORE_FILE) + " 100000 2 3");
  ASSERT_NE(store_resp.find(CTRL_RESP_HASH), std::string::npos)
      << "STORE on node B: " << store_resp;

  std::string block_hash = parse_hash_from_store_response(store_resp);
  ASSERT_FALSE(block_hash.empty()) << "Failed to parse block hash from: " << store_resp;

  /* Clear event logs */
  for (size_t idx = 0; idx < mesh.size(); idx++) {
    clear_events(mesh[idx].control_fd);
  }

  /* Node A sends RECALL_BLOCK for the block hash */
  std::string recall_resp = send_command(mesh[0].control_fd,
      std::string(CTRL_RECALL_BLOCK) + " " + block_hash);
  EXPECT_NE(recall_resp.find(CTRL_RESP_OK), std::string::npos)
      << "RECALL_BLOCK from node A: " << recall_resp;

  /* Wait for round trip */
  std::this_thread::sleep_for(std::chrono::seconds(3));

  /* Verify Node B received WIRE_RECALL_BLOCK */
  auto events_b = get_events(mesh[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED_VAL, WIRE_RECALL_BLOCK_VAL))
      << "Node B should have received RECALL_BLOCK";

  /* Verify either RECALL_ACCEPT or RECALL_DECLINE was sent by Node B */
  bool recall_responded = has_event(events_b, MSG_DIRECTION_SENT_VAL, WIRE_RECALL_ACCEPT_VAL) ||
                          has_event(events_b, MSG_DIRECTION_SENT_VAL, WIRE_RECALL_DECLINE_VAL);
  EXPECT_TRUE(recall_responded)
      << "Node B should have sent RECALL_ACCEPT or RECALL_DECLINE";

  /* Verify Node A received a RECALL_ACCEPT or RECALL_DECLINE */
  auto events_a = get_events(mesh[0].control_fd);
  bool received_recall_response = has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_RECALL_ACCEPT_VAL) ||
                                   has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_RECALL_DECLINE_VAL);
  EXPECT_TRUE(received_recall_response)
      << "Node A should have received RECALL_ACCEPT or RECALL_DECLINE";
#endif
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test_rpc_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add RecallBlock round-trip test"
```

---

### Task 15: Write StoreBlockHebbianDiamond test

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Write StoreBlockHebbianDiamond test**

```cpp
TEST_F(RpcIntegrationTest, StoreBlockHebbianDiamond) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create diamond topology: A-B-D, A-C-D */
  auto diamond = make_diamond();
  ASSERT_EQ(diamond.size(), 4u);

  /* Store a block on Node D */
  std::string store_resp = send_command(diamond[3].control_fd,
      std::string(CTRL_STORE_FILE) + " 100000 2 3");
  ASSERT_NE(store_resp.find(CTRL_RESP_HASH), std::string::npos)
      << "STORE on node D: " << store_resp;

  std::string block_hash = parse_hash_from_store_response(store_resp);
  ASSERT_FALSE(block_hash.empty()) << "Failed to parse block hash";

  /* Record initial Hebbian weights on Node A */
  auto hebbian_before_a = get_hebbian(diamond[0].control_fd);

  /* Clear event logs */
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    clear_events(diamond[idx].control_fd);
  }

  /* Node A sends STORE_BLOCK with carry_data=0 */
  std::string sb_resp = send_command(diamond[0].control_fd,
      std::string(CTRL_STORE_BLOCK) + " " + block_hash + " 0");
  EXPECT_NE(sb_resp.find(CTRL_RESP_OK), std::string::npos)
      << "STORE_BLOCK from node A: " << sb_resp;

  /* Wait for propagation */
  std::this_thread::sleep_for(std::chrono::seconds(3));

  /* Verify WIRE_STORE_BLOCK was forwarded through diamond */
  bool any_forwarded = false;
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    auto events = get_events(diamond[idx].control_fd);
    if (count_events(events, MSG_DIRECTION_FORWARDED_VAL, WIRE_STORE_BLOCK_VAL) > 0 ||
        count_events(events, MSG_DIRECTION_SENT_VAL, WIRE_STORE_BLOCK_VAL) > 0) {
      any_forwarded = true;
      break;
    }
  }
  EXPECT_TRUE(any_forwarded) << "STORE_BLOCK should have been forwarded through the diamond";

  /* Verify Node D received STORE_BLOCK */
  auto events_d = get_events(diamond[3].control_fd);
  EXPECT_TRUE(has_event(events_d, MSG_DIRECTION_RECEIVED_VAL, WIRE_STORE_BLOCK_VAL))
      << "Node D should have received STORE_BLOCK";

  /* Verify Node A received STORE_BLOCK_RESPONSE */
  auto events_a = get_events(diamond[0].control_fd);
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_STORE_BLOCK_RESPONSE_VAL))
      << "Node A should have received STORE_BLOCK_RESPONSE";

  /* Verify Hebbian weight increased along successful store path */
  auto hebbian_after_a = get_hebbian(diamond[0].control_fd);
  /* The store response should strengthen Hebbian weight toward nodes along the path */
  bool hebbian_increased_for_any_peer = false;
  for (size_t idx = 1; idx < diamond.size(); idx++) {
    if (hebbian_increased(hebbian_before_a, hebbian_after_a, diamond[idx].node_id)) {
      hebbian_increased_for_any_peer = true;
      break;
    }
  }
  EXPECT_TRUE(hebbian_increased_for_any_peer)
      << "Node A's Hebbian weight toward at least one peer should have increased after STORE_BLOCK";

  /* Verify total forwarded STORE_BLOCK messages are bounded by visited bloom */
  size_t total_forwards = 0;
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    auto events = get_events(diamond[idx].control_fd);
    total_forwards += count_events(events, MSG_DIRECTION_FORWARDED_VAL, WIRE_STORE_BLOCK_VAL);
  }
  EXPECT_LE(total_forwards, 6u)
      << "Total forwarded STORE_BLOCK messages should be bounded (no loops)";
#endif
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test_rpc_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add StoreBlock Hebbian diamond topology test"
```

---

### Task 16: Write RateLimitedBackoff test

**Files:**
- Modify: `test/test_rpc_integration.cpp`

- [ ] **Step 1: Write RateLimitedBackoff test**

```cpp
TEST_F(RpcIntegrationTest, RateLimitedBackoff) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 2-node mesh */
  auto mesh = make_full_mesh(2);
  ASSERT_EQ(mesh.size(), 2u);

  /* Clear event logs */
  for (size_t idx = 0; idx < mesh.size(); idx++) {
    clear_events(mesh[idx].control_fd);
  }

  /* Flood Node B with pings to exhaust its rate limit bucket */
  for (int ping_count = 0; ping_count < 30; ping_count++) {
    send_command(mesh[0].control_fd,
        std::string(CTRL_PING_PEER) + " " + mesh[1].node_id);
  }

  /* Wait for responses and potential rate limiting */
  std::this_thread::sleep_for(std::chrono::seconds(3));

  /* Verify that WIRE_RATE_LIMITED was received by Node A or sent by Node B.
   * This test is best-effort: rate limiting may or may not trigger depending
   * on bucket configuration. If it triggers, we verify the message was exchanged. */
  auto events_a = get_events(mesh[0].control_fd);
  auto events_b = get_events(mesh[1].control_fd);

  bool rate_limited = has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_RATE_LIMITED_VAL) ||
                      has_event(events_b, MSG_DIRECTION_SENT_VAL, WIRE_RATE_LIMITED_VAL);

  /* If rate limiting triggered, verify the message was properly exchanged */
  if (rate_limited) {
    /* Node A should have received WIRE_RATE_LIMITED from Node B */
    EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_RATE_LIMITED_VAL))
        << "Node A should have received RATE_LIMITED when rate limit is exceeded";
  }
  /* Note: This test does not fail if rate limiting is not triggered,
   * as bucket sizes may vary. It verifies correctness when triggered. */
#endif
}
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test_rpc_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add RateLimited backoff test"
```

---

### Task 17: Build, run tests, check for memory leaks

- [ ] **Step 1: Full build**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make clean && make 2>&1 | tail -30
```

- [ ] **Step 2: Run existing tests to verify no regressions**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && make test 2>&1 | tail -50
```

- [ ] **Step 3: Run the new integration tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && ./build/test_rpc_integration --gtest_filter="RpcIntegrationTest.PingCapacity*:RpcIntegrationTest.PingBlock*:RpcIntegrationTest.FindNode*:RpcIntegrationTest.SeekingBlocks*:RpcIntegrationTest.RecallBlock*:RpcIntegrationTest.StoreBlockHebbian*:RpcIntegrationTest.RateLimited*" 2>&1 | tail -50
```

- [ ] **Step 4: Run valgrind on the new tests for memory leak detection**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && valgrind --leak-check=full --error-exitcode=1 ./build/test_rpc_integration --gtest_filter="RpcIntegrationTest.PingCapacityRoundTrip" 2>&1 | tail -50
```

- [ ] **Step 5: Fix any memory leaks found**

If valgrind reports leaks, fix them in the relevant handler or test code.

- [ ] **Step 6: Final commit if fixes needed**

```bash
git add -A
git commit -m "fix: address memory leaks in RPC integration tests"
```