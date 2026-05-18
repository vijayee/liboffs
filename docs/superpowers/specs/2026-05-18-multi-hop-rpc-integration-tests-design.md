# Multi-Hop RPC Integration Tests Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Test every wire RPC call over a multi-node network with per-hop verification, prioritizing multi-hop RPCs (FindBlock, StoreBlock, RankBlock) first.

**Architecture:** Add an in-memory message event log to each node, RPC injection control commands, and topology construction helpers in the test framework. Tests build specific topologies (chain, ring, star), inject RPCs via control commands, and verify each hop's behavior by querying event logs.

**Tech Stack:** C (node-side event logging, control command handlers), C++/GTest (test framework, topology helpers, assertions)

---

## Components

### 1. Message Event Log (C, node-side)

A fixed-size circular buffer in `network_t` that records every wire message sent or received. Each event captures the message type, direction, peer, correlation ID, and a result field.

```c
#define MESSAGE_LOG_CAPACITY 256

typedef enum {
  MSG_DIRECTION_SENT = 0,
  MSG_DIRECTION_RECEIVED = 1,
  MSG_DIRECTION_FORWARDED = 2  // received then re-sent
} msg_direction_e;

typedef struct {
  uint8_t type;              // WIRE_FIND_BLOCK, etc.
  uint8_t direction;         // msg_direction_e
  uint64_t timestamp_ms;     // time(NULL) * 1000
  node_id_t peer_id;         // who we sent to / received from
  uint64_t message_id;       // correlation ID from wire message
  uint8_t block_hash[32];    // zeroed if not applicable
  uint8_t result;            // 0=success, 1=forwarded, 2=not_found, 3=declined, etc.
} message_event_t;

typedef struct {
  message_event_t events[MESSAGE_LOG_CAPACITY];
  size_t count;              // total events written (may exceed capacity)
  size_t cursor;             // index of oldest event (wraps)
  PLATFORMLOCKTYPE(lock);
} message_log_t;
```

**Log points** — insert `message_log_record()` calls in `network_dispatch` at every message type handler, and in `conn_state_send` / `relay_client` send paths:

- **Sent:** When `conn_state_send` or relay send dispatches a message
- **Received:** When `NETWORK_QUIC_DATA` or `NETWORK_RELAY_RECEIVED` decodes a message
- **Forwarded:** When a handler re-sends a message (e.g., FindBlock forwarding, StoreBlock forwarding)

The `result` field is set by the handler:
- `WIRE_FIND_BLOCK`: `0` = found locally, `1` = forwarded, `2` = not found / TTL expired
- `WIRE_STORE_BLOCK`: `0` = accepted, `1` = forwarded, `2` = declined
- `WIRE_PING` / `WIRE_PING_CAPACITY` / etc.: `0` = responded

### 2. Control Commands for Event Log and RPC Injection (C, node-side)

Add to `test/test_control_protocol.h`:

```c
#define CTRL_GET_EVENTS     "GET_EVENTS"
#define CTRL_CLEAR_EVENTS   "CLEAR_EVENTS"
#define CTRL_FIND_BLOCK     "FIND_BLOCK"
#define CTRL_PING_PEER      "PING_PEER"
```

**GET_EVENTS** — Returns all events since a given cursor:
- Request: `GET_EVENTS [cursor]` (cursor defaults to 0 if omitted)
- Response: `EVENTS <total_count>|<index>:<type>,<direction>,<peer_id>,<message_id>,<result>;...`
- Events are returned from cursor to current, pipe-delimited entries

**CLEAR_EVENTS** — Resets the event log:
- Request: `CLEAR_EVENTS`
- Response: `OK`

**FIND_BLOCK** — Injects a FindBlock RPC into the network:
- Request: `FIND_BLOCK <hex_hash>`
- Response: `OK <message_id>` (returns the message_id for correlation)

**PING_PEER** — Sends a ping to a specific connected peer:
- Request: `PING_PEER <node_id_hex>`
- Response: `OK <message_id>`

### 3. Topology Construction Helpers (C++, test-side)

Add to `test/test_file_transfer_integration.cpp` or a new test file:

```cpp
struct TopologyNode {
  Process proc;
  std::string node_id;
  uint32_t endpoint_id;
  int control_fd;
};

// Chain topology: Node[0] -> Node[1] -> ... -> Node[N-1]
// Each node has ADD_PEER entries for its successor (and predecessor for responses)
// All nodes connect to the same relay for message routing
std::vector<TopologyNode> make_chain(int count, uint16_t base_port, uint16_t relay_port);

// All-to-all topology: every node can reach every other node
// Each node has ADD_PEER entries for all other nodes
std::vector<TopologyNode> make_full_mesh(int count, uint16_t base_port, uint16_t relay_port);
```

Each helper:
1. Starts a relay server on `relay_port`
2. Starts `count` nodes with sequential control ports
3. Connects each node to the relay
4. Uses ADD_PEER to establish the desired topology
5. Waits for peers to connect

### 4. Event Log Query Helpers (C++, test-side)

```cpp
struct MessageEvent {
  uint8_t type;
  uint8_t direction;
  std::string peer_id;
  uint64_t message_id;
  std::string block_hash;  // hex
  uint8_t result;
};

std::vector<MessageEvent> get_events(int control_fd, size_t after_cursor = 0);

bool has_event(const std::vector<MessageEvent>& events,
               uint8_t direction, uint8_t type,
               const std::string& peer_id = "",
               uint8_t result = 255 /* 255 = any */);

size_t count_events(const std::vector<MessageEvent>& events,
                    uint8_t direction, uint8_t type);
```

---

## Test Cases

### Multi-Hop Tests (Priority 1)

**FindBlock Chain (A -> B -> C)**
- C has a block, A searches for it
- Verify: B receives FIND_BLOCK from A, forwards to C
- Verify: C responds FOUND to B, B forwards response to A
- Verify: A receives FOUND response

**FindBlock TTL Expiry (A -> B -> C -> D, TTL=2)**
- E has a block, TTL starts at 3
- Verify: A forwards to B (TTL=3), B forwards to C (TTL=2), C forwards to D (TTL=1)
- Verify: D's TTL expires, responds NOT_FOUND
- Verify: NOT_FOUND propagates back through C, B to A

**StoreBlock Replication Chain (A -> B -> C)**
- A stores a block, replicas_needed=3
- Verify: B receives STORE_BLOCK, accepts (result=accepted), forwards to C
- Verify: C receives STORE_BLOCK, accepts
- Verify: STORE_BLOCK_RESPONSE propagates back

**RankBlock Flood (A -> B, C)**
- A sends RankBlock with hop_count=0
- Verify: B receives RANK_BLOCK, increments hop_count to 1
- Verify: C receives RANK_BLOCK (flooded from B or directly from A)

### Single-Hop Tests (Priority 2)

**Ping Round-Trip**
- A pings B, verify B responds with PingResponse
- Verify RTT measurement is populated

**PingCapacity Exchange**
- A sends PingCapacity to B
- Verify B responds with capacity and phase

**PingBlock Existence Check**
- A sends PingBlock to B for a block B has
- Verify B responds with exists=true

**FindNode K-Closest**
- A sends FindNode to B
- Verify B responds with closest nodes from its ring table

**SeekingBlocks Exchange**
- A sends SeekingBlocks to B
- Verify B responds with block offers

**RecallBlock Accept/Decline**
- A sends RecallBlock to B
- Verify B accepts (low capacity) or declines (high capacity)

**RateLimited**
- A sends many requests exceeding B's rate limit
- Verify B responds with RateLimited

---

## Data Flow

```
Test Process                        Node Process
-----------                         ------------
send_command(fd, "FIND_BLOCK <hash>")
  --> TCP control socket
                                    --> network_dispatch(NETWORK_LOCAL_FIND_BLOCK)
                                    --> find_block_execute() routes to peers
                                    --> conn_state_send() forwards message
                                    --> message_log_record(SENT, WIRE_FIND_BLOCK, peer_id, ...)
                                    <-- message_log_record(RECEIVED, WIRE_FIND_BLOCK, ...)

send_command(fd, "GET_EVENTS")
  <-- TCP control socket
  <-- "EVENTS 5|0:7,0,<node_id>,<msg_id>,1;1:8,1,<node_id>,<msg_id>,0;..."

assert(has_event(events, SENT, WIRE_FIND_BLOCK, node_b))
assert(has_event(events, RECEIVED, WIRE_FIND_BLOCK_RESPONSE, node_b))
```

## Error Handling

- **Event log overflow:** When `count >= MESSAGE_LOG_CAPACITY`, oldest events are overwritten. The `cursor` field tracks the wrap point. `GET_EVENTS` with a cursor value returns only events after that cursor, or an empty result if the cursor has been surpassed.
- **Control socket timeout:** Same 10-second timeout as existing control commands.
- **Topology setup failure:** If `ADD_PEER` or relay connection fails, the test fixture TearDown kills all processes and cleans up.

## Wire Message Format for GET_EVENTS Response

Each event is encoded as comma-separated fields: `type,direction,peer_id_hex,message_id,hash_prefix,result`

- `type`: wire message type number (e.g., 7=FIND_BLOCK, 8=FIND_BLOCK_RESPONSE)
- `direction`: 0=SENT, 1=RECEIVED, 2=FORWARDED
- `peer_id_hex`: node_id as hex string (or `0` if not applicable)
- `message_id`: decimal message correlation ID
- `hash_prefix`: first 8 hex chars of block_hash for correlation, or `0` if not applicable
- `result`: 0=success, 1=forwarded, 2=not_found, 3=declined

```
EVENTS <total_count>|<index>:<type>,<dir>,<peer_id>,<msg_id>,<hash_prefix>,<result>;...
```

Example:
```
EVENTS 3|0:7,0,a1b2c3,1001,4a5b6c7d,1;1:7,2,d4e5f6,1002,4a5b6c7d,1;2:8,1,a1b2c3,1001,4a5b6c7d,0
```

Decoded:
- Event 0: FIND_BLOCK(7) SENT(0) to peer a1b2c3, msg_id=1001, hash=4a5b6c7d..., result=FORWARDED(1)
- Event 1: FIND_BLOCK(7) FORWARDED(2) to peer d4e5f6, msg_id=1002, hash=4a5b6c7d..., result=FORWARDED(1)
- Event 2: FIND_BLOCK_RESPONSE(8) RECEIVED(1) from peer a1b2c3, msg_id=1001, hash=4a5b6c7d..., result=SUCCESS(0)

## File Changes

| File | Change |
|------|--------|
| `src/Network/network.h` | Add `message_log_t` to `network_t` |
| `src/Network/message_log.h` | New: message_log_t definition, init/destroy/record/query API |
| `src/Network/message_log.c` | New: implementation |
| `src/Network/network.c` | Call `message_log_record` at each message handler |
| `src/Network/conn_state.c` | Call `message_log_record` in send paths |
| `test/test_control_protocol.h` | Add GET_EVENTS, CLEAR_EVENTS, FIND_BLOCK, PING_PEER |
| `test/test_node_main.c` | Add handlers for new control commands, message_log initialization |
| `test/test_rpc_integration.cpp` | New: multi-hop RPC integration tests |
| `test/CMakeLists.txt` | Add test_rpc_integration target |