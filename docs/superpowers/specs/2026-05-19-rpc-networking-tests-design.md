# RPC Networking Integration Tests Design

## Overview

Add integration tests for the remaining untested RPC wire messages, exercising full algorithm behavior including hop forwarding and Hebbian weight calculations. This covers PING_CAPACITY, PING_BLOCK, FIND_NODE, SEEKING_BLOCKS, RECALL_BLOCK, and adds a Hebbian-focused STORE_BLOCK test. RATE_LIMITED is tested indirectly via request flooding.

## Approach

Add control protocol commands to `test_node_main.c` for triggering each untested RPC, then write integration tests in `test_rpc_integration.cpp` using the existing multi-process fork/exec test harness with purpose-built topologies per RPC.

## Control Protocol Commands

Six new commands in `test_control_protocol.h` and handlers in `test_node_main.c`:

| Command | Format | Wire Message | Notes |
|---|---|---|---|
| `PING_CAPACITY` | `PING_CAPACITY <node_id>` | `WIRE_PING_CAPACITY` | Encodes local capacity/phase |
| `PING_BLOCK` | `PING_BLOCK <node_id> <hash_hex>` | `WIRE_PING_BLOCK` | Queries block existence |
| `FIND_NODE` | `FIND_NODE <target_id>` | `WIRE_FIND_NODE` | Local query dispatched to network actor |
| `SEEKING_BLOCKS` | `SEEKING_BLOCKS <capacity> [<exclude_hash1> ...]` | `WIRE_SEEKING_BLOCKS` | Capacity as float, exclude hashes as space-separated 64-char hex strings |
| `RECALL_BLOCK` | `RECALL_BLOCK <hash_hex>` | `WIRE_RECALL_BLOCK` | Requests block recall |
| `STORE_BLOCK` | `STORE_BLOCK <hash_hex> <carry_data>` | `NETWORK_LOCAL_STORE_BLOCK` | Triggers store via local path |

New response prefixes:
- `PING_CAPACITY_RESP` — returns capacity and phase values
- `PING_BLOCK_RESP` — returns exists/fib/healthy
- `FIND_NODE_RESP` — returns closest node IDs
- `SEEKING_BLOCKS_RESP` — returns offer count
- `RECALL_RESP` — returns accepted/declined

RATE_LIMITED has no direct trigger command. It is tested by flooding requests to exceed a peer's rate limit bucket, causing the remote node to send back `WIRE_RATE_LIMITED`.

## Handler Implementation Details

### PING_CAPACITY handler

```
handle_ping_capacity_cmd(client_fd, node_id_hex):
  Parse node_id from hex string
  Lookup peer in connection_manager
  Construct wire_ping_capacity_t:
    message_id = random
    source = local node_id
    capacity = network->local_capacity (or 1.0f default)
    phase = NODE_PHASE_NEUTRAL
  Encode to CBOR, send via conn_state_send
  Respond OK with message_id
```

### PING_BLOCK handler

```
handle_ping_block_cmd(client_fd, node_id_hex, hash_hex):
  Parse node_id and 32-byte block_hash
  Lookup peer in connection_manager
  Construct wire_ping_block_t:
    message_id = random
    sender_id = local node_id
    block_hash = parsed hash
  Encode to CBOR, send via conn_state_send
  Respond OK with message_id
```

### FIND_NODE handler

```
handle_find_node_cmd(client_fd, target_id_hex):
  Parse target node_id
  Construct network_local_find_node_payload_t:
    target_id = parsed target
    reply_to = &network->actor
  Send via actor_send with NETWORK_LOCAL_FIND_NODE
  Respond OK
```

Note: FIND_NODE uses a local payload type similar to FIND_BLOCK and CLOSEST_NODES. If `network_local_find_node_payload_t` does not exist yet, it must be created with `target_id`, `reply_to`, and a destroy function.

### SEEKING_BLOCKS handler

```
handle_seeking_blocks_cmd(client_fd, args):
  Parse capacity (float) and optional exclude hashes (64-char hex each)
  Construct wire_seeking_blocks_t:
    message_id = random
    sender_id = local node_id
    capacity = parsed capacity
    exclude_hashes = parsed array of 32-byte hash pointers
    exclude_count = count of parsed hashes
  Encode to CBOR, send via conn_state_send to each connected peer
  (Or send to a specific peer if a target is provided)
  Destroy wire struct after sending
  Respond OK
```

### RECALL_BLOCK handler

```
handle_recall_block_cmd(client_fd, hash_hex):
  Parse 32-byte block_hash
  Construct wire_recall_block_t:
    message_id = random
    sender_id = local node_id
    block_hash = parsed hash
  Broadcast to all connected peers (recall is a request for any peer that has the block)
  Respond OK
```

### STORE_BLOCK handler (local injection)

```
handle_store_block_cmd(client_fd, hash_hex, carry_data_int):
  Parse block_hash and carry_data flag
  Lookup block in local block_cache via index_peek
  If not found, respond ERROR
  Construct wire_store_block_t:
    message_id = random
    block_hash = parsed hash
    block_size = entry size
    block_fib = entry fib counter
    replicas_needed = 2
    max_hops = 5
    visited_bloom = self added
    path = [local_id]
    path_len = 1
    start_time = current timestamp
    carry_data = parsed carry_data
    block_data = (if carry_data) load from cache, else NULL
    block_data_len = (if carry_data) data length, else 0
  Send via actor_send with NETWORK_STORE_BLOCK and payload_destroy = wire_store_block_destroy
  Respond OK
```

## Test Cases

### Test 1: PingCapacityRoundTrip

**Topology:** 2-node full mesh (make_full_mesh(2))

**Steps:**
1. Create 2-node mesh
2. Clear event logs
3. Node A sends PING_CAPACITY to Node B
4. Wait 2 seconds for propagation
5. Verify Node B received WIRE_PING_CAPACITY (has event with direction=RECEIVED, type=WIRE_PING_CAPACITY)
6. Verify Node A received WIRE_PING_CAPACITY_RESPONSE (has event with direction=RECEIVED, type=WIRE_PING_CAPACITY_RESPONSE)

**Verification:**
- Event log shows WIRE_PING_CAPACITY sent by A, received by B
- Event log shows WIRE_PING_CAPACITY_RESPONSE sent by B, received by A
- Capacity and phase values are exchanged (verified via event presence)

### Test 2: PingCapacityHebbianVerification

**Topology:** 2-node full mesh (make_full_mesh(2))

**Steps:**
1. Create 2-node mesh
2. Record initial Hebbian weights on Node A (get_hebbian)
3. Clear event logs
4. Node A sends 3 consecutive PING_CAPACITY to Node B, with 300ms delays
5. Wait 2 seconds after final ping
6. Record final Hebbian weights on Node A
7. Verify weight increased from before first ping
8. Verify WIRE_PING_CAPACITY sent events >= 3 on Node A
9. Verify WIRE_PING_CAPACITY_RESPONSE received events >= 3 on Node A

**Verification:**
- hebbian_increased(before, after, peer_id) returns true
- count_events for PING_CAPACITY_SENT >= 3
- count_events for PING_CAPACITY_RESPONSE_RECEIVED >= 3

### Test 3: PingBlockRoundTrip

**Topology:** 2-node full mesh (make_full_mesh(2))

**Steps:**
1. Create 2-node mesh
2. Store a block on Node B using STORE_FILE
3. Record initial Hebbian weights on Node A
4. Clear event logs
5. Node A sends PING_BLOCK to Node B with the block hash
6. Wait 2 seconds for round trip
7. Verify Node B received WIRE_PING_BLOCK
8. Verify Node A received WIRE_PING_BLOCK_RESPONSE
9. Record final Hebbian weights on Node A
10. Verify Hebbian weight toward Node B increased (successful block ping strengthens Hebbian)

**Verification:**
- has_event(B, RECEIVED, WIRE_PING_BLOCK) is true
- has_event(A, RECEIVED, WIRE_PING_BLOCK_RESPONSE) is true
- hebbian_increased(before, after, node_B_id) is true

### Test 4: FindNodeDiamond

**Topology:** Diamond (A-B-D, A-C-D) via make_diamond()

**Steps:**
1. Create diamond topology
2. Let gossip establish ring membership on all nodes (send GOSSIP to each, wait 2s)
3. Clear event logs
4. Node A sends FIND_NODE with Node D's ID as target
5. Wait 2 seconds for query propagation
6. Verify WIRE_FIND_NODE was sent/forwarded by intermediate nodes (B and/or C)
7. Verify WIRE_FIND_NODE_RESPONSE was received by Node A
8. Verify total forwarded FIND_NODE messages are bounded (no loops)

**Verification:**
- has_event on B or C for WIRE_FIND_NODE (RECEIVED or FORWARDED) is true
- has_event on A for WIRE_FIND_NODE_RESPONSE (RECEIVED) is true
- Total forwarded WIRE_FIND_NODE across all nodes <= 4 (bounded by visited bloom)

### Test 5: SeekingBlocksChain

**Topology:** 3-node chain (A→B→C) via make_chain(3)

**Steps:**
1. Create 3-node chain
2. Store a block on Node C using STORE_FILE
3. Clear event logs
4. Node A sends SEEKING_BLOCKS with capacity=1.0 and no exclude hashes
5. Wait 3 seconds for propagation
6. Verify WIRE_SEEKING_BLOCKS was forwarded through the chain (A→B→C)
7. Verify Node C sent WIRE_SEEKING_BLOCKS_RESPONSE with offers
8. Verify total forwarded SEEKING_BLOCKS messages are bounded

**Verification:**
- has_event(B, FORWARDED, WIRE_SEEKING_BLOCKS) or has_event(B, RECEIVED, WIRE_SEEKING_BLOCKS) is true
- has_event(C, RECEIVED, WIRE_SEEKING_BLOCKS) is true
- has_event on any node for WIRE_SEEKING_BLOCKS_RESPONSE is true
- Total forwarded SEEKING_BLOCKS <= 2 (A→B, B→C)

### Test 6: RecallBlockRoundTrip

**Topology:** 2-node full mesh (make_full_mesh(2))

**Steps:**
1. Create 2-node mesh
2. Store a block on Node B using STORE_FILE
3. Clear event logs
4. Node A sends RECALL_BLOCK with the block hash
5. Wait 2 seconds for round trip
6. Verify Node B received WIRE_RECALL_BLOCK
7. Verify WIRE_RECALL_ACCEPT was sent by Node B
8. Verify Node A received WIRE_RECALL_ACCEPT
9. Verify the response was accepted (not declined)

**Verification:**
- has_event(B, RECEIVED, WIRE_RECALL_BLOCK) is true
- has_event(B, SENT, WIRE_RECALL_ACCEPT) or has_event(B, SENT, WIRE_RECALL_DECLINE) is true
- has_event(A, RECEIVED, WIRE_RECALL_ACCEPT) is true

### Test 7: StoreBlockHebbianDiamond

**Topology:** Diamond (A-B-D, A-C-D) via make_diamond()

**Steps:**
1. Create diamond topology
2. Record initial Hebbian weights on all nodes
3. Store a block on Node D using STORE_FILE
4. Clear event logs
5. Node A sends STORE_BLOCK with the block hash and carry_data=0
6. Wait 3 seconds for propagation
7. Verify WIRE_STORE_BLOCK was forwarded through diamond path (A→B→D or A→C→D)
8. Verify WIRE_STORE_BLOCK_RESPONSE with accepted=1 came back
9. Record final Hebbian weights on all nodes
10. Verify Hebbian weight increased along the successful store path
11. Verify total forwarded STORE_BLOCK messages are bounded by visited bloom (no loops)

**Verification:**
- count_events across all nodes for WIRE_STORE_BLOCK (FORWARDED) <= 4 (bounded by visited bloom)
- has_event(D, RECEIVED, WIRE_STORE_BLOCK) is true
- has_event(A, RECEIVED, WIRE_STORE_BLOCK_RESPONSE) is true
- hebbian_increased on nodes along the successful path

## RATE_LIMITED Testing

RATE_LIMITED is a response-only message sent when a node's rate limit bucket is exhausted. To test it:

### Test 8: RateLimitedBackoff

**Topology:** 2-node full mesh (make_full_mesh(2))

**Steps:**
1. Create 2-node mesh
2. Clear event logs
3. Node A sends a rapid burst of PING_PEER commands to Node B (e.g., 20 pings in quick succession)
4. Wait 3 seconds for all responses and rate limit signals
5. Verify that at some point WIRE_RATE_LIMITED was sent by Node B and received by Node A

**Verification:**
- has_event(A, RECEIVED, WIRE_RATE_LIMITED) is true — Node A received a rate limit signal

This test depends on the default rate limit bucket being small enough to overflow. If the bucket is too large for the test to reliably trigger, we may need to configure a smaller bucket for testing.

## New Topology Helper

### make_ring(count)

A ring topology where each node connects to its neighbors in a circular pattern (N0→N1→N2→...→N{count-1}→N0). Useful for testing multi-hop forwarding with bounded path lengths. Implementation follows the existing pattern in make_chain/make_full_mesh/make_diamond.

### make_star(count)

A star topology with a central hub node connected to all leaf nodes, where leaves are only connected to the hub. Useful for testing RECALL_BLOCK and SEEKING_BLOCKS where the hub has blocks and leaves request them.

## Wire Type Constants for test_rpc_integration.cpp

The following wire type constants need to be added to test_rpc_integration.cpp for event log verification:

```cpp
static constexpr uint8_t WIRE_PING_CAPACITY_RESPONSE_VAL = 4;
static constexpr uint8_t WIRE_PING_BLOCK_VAL           = 5;
static constexpr uint8_t WIRE_PING_BLOCK_RESPONSE_VAL  = 6;
static constexpr uint8_t WIRE_FIND_NODE_VAL             = 9;
static constexpr uint8_t WIRE_FIND_NODE_RESPONSE_VAL    = 10;
static constexpr uint8_t WIRE_STORE_BLOCK_VAL           = 11;
static constexpr uint8_t WIRE_STORE_BLOCK_RESPONSE_VAL  = 12;
static constexpr uint8_t WIRE_SEEKING_BLOCKS_VAL        = 13;
static constexpr uint8_t WIRE_SEEKING_BLOCKS_RESPONSE_VAL = 14;
static constexpr uint8_t WIRE_RECALL_BLOCK_VAL          = 16;
static constexpr uint8_t WIRE_RECALL_ACCEPT_VAL          = 17;
static constexpr uint8_t WIRE_RECALL_DECLINE_VAL        = 18;
static constexpr uint8_t WIRE_RATE_LIMITED_VAL           = 19;
```

Note: WIRE_PING_CAPACITY_RESPONSE_VAL (4) is already defined. The others need to be added. Some (WIRE_FIND_BLOCK, WIRE_RANK_BLOCK, WIRE_GOSSIP, etc.) are already present.

## Implementation Order

1. Add wire type constants to test_rpc_integration.cpp
2. Add control protocol commands to test_control_protocol.h
3. Implement control command handlers in test_node_main.c (guarded by #ifdef OFFS_TEST)
4. Add topology helpers (make_ring, make_star) if needed
5. Write PingCapacityRoundTrip test
6. Write PingCapacityHebbianVerification test
7. Write PingBlockRoundTrip test
8. Write FindNodeDiamond test
9. Write SeekingBlocksChain test
10. Write RecallBlockRoundTrip test
11. Write StoreBlockHebbianDiamond test
12. Write RateLimitedBackoff test
13. Run full test suite, check for memory leaks with valgrind