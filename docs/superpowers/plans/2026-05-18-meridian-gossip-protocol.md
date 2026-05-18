# Meridian Gossip Protocol Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Meridian gossip for ring maintenance and peer discovery (following libMeridian reference), and separate PingCapacity into its own 15-minute timer plus immediate-on-connect.

**Architecture:** Two separate protocols on the network actor: (1) Meridian gossip selects 1 random node per ring, sends WIRE_GOSSIP with ring membership, receives WIRE_GOSSIP_PULL back, and discovers new peers via addNodeToRing with probe cache; (2) PingCapacity exchanges capacity/phase info on a 15-minute timer plus immediately on new peer connection.

**Tech Stack:** C, libcbor, existing actor/timer infrastructure, GTest for unit tests, integration tests with multi-node RPC

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/Network/wire.h` | WIRE_GOSSIP / WIRE_GOSSIP_PULL struct definitions, encode/decode declarations |
| `src/Network/wire.c` | WIRE_GOSSIP / WIRE_GOSSIP_PULL encode and decode implementations |
| `src/Network/ring_set.h` | `ring_set_get_random_nodes()` declaration |
| `src/Network/ring_set.c` | `ring_set_get_random_nodes()` implementation |
| `src/Network/network.h` | `ping_capacity_timer_id` and `PING_CAPACITY_INTERVAL_MS` constant |
| `src/Network/network.c` | gossip tick rewrite, PingCapacity tick handler, salutation change, dispatch cases, addNodeToRing, QUIC/relay dispatch |
| `src/Actor/message.h` | `NETWORK_GOSSIP_RECEIVED`, `NETWORK_GOSSIP_PULL_RECEIVED`, `NETWORK_PING_CAPACITY_TICK` enum values |
| `test/test_network.cpp` | WireGossipTest, WireGossipPullTest, RingSetGetRandomNodes unit tests |
| `test/test_control_protocol.h` | `CTRL_GOSSIP` command |
| `test/test_node_main.c` | `CTRL_GOSSIP` handler |
| `test/test_rpc_integration.cpp` | GossipDiscoveryDiamond integration test |

---

### Task 1: Wire structs and encode/decode for WIRE_GOSSIP and WIRE_GOSSIP_PULL

**Files:**
- Modify: `src/Network/wire.h` (add struct definitions, encode/decode declarations)
- Modify: `src/Network/wire.c` (add encode/decode implementations)
- Test: `test/test_network.cpp`

- [ ] **Step 1: Write the failing tests for WIRE_GOSSIP and WIRE_GOSSIP_PULL roundtrip**

In `test/test_network.cpp`, add a `WireGossipTest` fixture and tests:

```cpp
class WireGossipTest : public ::testing::Test {
 protected:
  void SetUp() override {
    memset(&msg, 0, sizeof(msg));
    msg.message_id = 0x123456789ABCDEF0ULL;
    memcpy(&msg.sender_id, &test_node_id, sizeof(node_id_t));
    msg.rendezvous_addr = 0x7F000001;  // 127.0.0.1
    msg.rendezvous_port = 3964;
    msg.targets[0] = test_node_id;
    msg.targets[1] = test_node_id_b;
    msg.target_count = 2;
  }
  wire_gossip_t msg;
};

TEST_F(WireGossipTest, Roundtrip) {
  cbor_item_t* encoded = wire_gossip_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  wire_gossip_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = wire_gossip_decode(encoded, &decoded);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(decoded.message_id, msg.message_id);
  EXPECT_TRUE(node_id_equals(&decoded.sender_id, &msg.sender_id));
  EXPECT_EQ(decoded.rendezvous_addr, msg.rendezvous_addr);
  EXPECT_EQ(decoded.rendezvous_port, msg.rendezvous_port);
  EXPECT_EQ(decoded.target_count, 2u);
  EXPECT_TRUE(node_id_equals(&decoded.targets[0], &msg.sender_id));
  EXPECT_TRUE(node_id_equals(&decoded.targets[1], &test_node_id_b));
  cbor_decref(&encoded);
}

TEST_F(WireGossipTest, ZeroTargets) {
  msg.target_count = 0;
  cbor_item_t* encoded = wire_gossip_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  wire_gossip_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int rc = wire_gossip_decode(encoded, &decoded);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(decoded.target_count, 0u);
  cbor_decref(&encoded);
}
```

Add a similar `WireGossipPullTest` fixture (same struct layout, type byte 22 instead of 21).

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/test/testliboffs --gtest_filter="WireGossipTest*:WireGossipPullTest*"`
Expected: FAIL (undefined reference to `wire_gossip_encode`, `wire_gossip_decode`, etc.)

- [ ] **Step 3: Add struct definitions to wire.h**

Add after the `wire_addr_response_t` struct and before the encode function declarations:

```c
// --- Gossip (ring maintenance / peer discovery) ---

#define WIRE_GOSSIP            21
#define WIRE_GOSSIP_PULL       22

typedef struct {
  uint64_t  message_id;
  node_id_t sender_id;
  uint32_t  rendezvous_addr;
  uint16_t  rendezvous_port;
  node_id_t targets[RING_MAX_RINGS];
  uint8_t   target_count;
} wire_gossip_t;

typedef struct {
  uint64_t  message_id;
  node_id_t sender_id;
  uint32_t  rendezvous_addr;
  uint16_t  rendezvous_port;
  node_id_t targets[RING_MAX_RINGS];
  uint8_t   target_count;
} wire_gossip_pull_t;
```

Add encode/decode declarations alongside the existing ones:

```c
cbor_item_t* wire_gossip_encode(const wire_gossip_t* msg);
int wire_gossip_decode(cbor_item_t* item, wire_gossip_t* msg);
cbor_item_t* wire_gossip_pull_encode(const wire_gossip_pull_t* msg);
int wire_gossip_pull_decode(cbor_item_t* item, wire_gossip_pull_t* msg);
```

- [ ] **Step 4: Add encode/decode implementations to wire.c**

Add `wire_gossip_encode` following the pattern of `wire_ping_capacity_encode` (6-element base + variable-length targets array):

```c
cbor_item_t* wire_gossip_encode(const wire_gossip_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_GOSSIP);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* sender = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, sender);
  cbor_decref(&sender);

  item = cbor_build_uint32(msg->rendezvous_addr);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint16(msg->rendezvous_port);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  // targets array: [target_count, [node_id_1, node_id_2, ...]]
  item = cbor_build_uint8(msg->target_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* targets = cbor_new_definite_array(msg->target_count);
  for (uint8_t index = 0; index < msg->target_count; index++) {
    cbor_item_t* node = _node_id_encode(&msg->targets[index]);
    (void)cbor_array_push(targets, node);
    cbor_decref(&node);
  }
  (void)cbor_array_push(array, targets);
  cbor_decref(&targets);

  return array;
}
```

Add `wire_gossip_decode` that extracts the 8-element CBOR array (type, message_id_hi, message_id_lo, sender_id, rendezvous_addr, rendezvous_port, target_count, targets_array). Validate array size >= 8, type byte == WIRE_GOSSIP, target_count <= RING_MAX_RINGS, and targets array size matches target_count.

Add `wire_gossip_pull_encode` and `wire_gossip_pull_decode` — identical to gossip encode/decode except type byte is WIRE_GOSSIP_PULL (22) and struct type is `wire_gossip_pull_t`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/test/testliboffs --gtest_filter="WireGossipTest*:WireGossipPullTest*"`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/Network/wire.h src/Network/wire.c test/test_network.cpp
git commit -m "feat: add WIRE_GOSSIP and WIRE_GOSSIP_PULL wire protocol encode/decode"
```

---

### Task 2: ring_set_get_random_nodes helper

**Files:**
- Modify: `src/Network/ring_set.h` (add declaration)
- Modify: `src/Network/ring_set.c` (add implementation)
- Test: `test/test_network.cpp`

- [ ] **Step 1: Write the failing test for ring_set_get_random_nodes**

```cpp
TEST(RingSetGetRandomNodes, EmptySetReturnsZero) {
  ring_set_t* set = ring_set_create(RING_K, RING_M, RING_ALPHA);
  net_node_t nodes[RING_MAX_RINGS];
  size_t count = ring_set_get_random_nodes(set, nodes, RING_MAX_RINGS, NULL);
  EXPECT_EQ(count, 0u);
  ring_set_destroy(set);
}

TEST(RingSetGetRandomNodes, ReturnsOnePerNonEmptyRing) {
  ring_set_t* set = ring_set_create(RING_K, RING_M, RING_ALPHA);
  // Insert nodes into rings at different latencies
  for (int index = 0; index < 5; index++) {
    net_node_t* node = net_node_create(&test_node_ids[index], 0, 0);
    if (node != NULL) {
      ring_set_insert(set, node, (uint32_t)(1000 + index * 2000));
    }
  }
  net_node_t nodes[RING_MAX_RINGS];
  size_t count = ring_set_get_random_nodes(set, nodes, RING_MAX_RINGS, NULL);
  EXPECT_GT(count, 0u);
  EXPECT_LE(count, (size_t)set->ring_count);
  ring_set_destroy(set);
}

TEST(RingSetGetRandomNodes, ExcludesNodeId) {
  ring_set_t* set = ring_set_create(RING_K, RING_M, RING_ALPHA);
  net_node_t* node = net_node_create(&test_node_id, 0, 0);
  if (node != NULL) {
    ring_set_insert(set, node, 1000);
  }
  net_node_t nodes[RING_MAX_RINGS];
  // Exclude the only node — should return 0
  size_t count = ring_set_get_random_nodes(set, nodes, RING_MAX_RINGS, &test_node_id);
  EXPECT_EQ(count, 0u);
  ring_set_destroy(set);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/test/testliboffs --gtest_filter="RingSetGetRandomNodes*"`
Expected: FAIL (undefined reference to `ring_set_get_random_nodes`)

- [ ] **Step 3: Add declaration to ring_set.h**

```c
size_t ring_set_get_random_nodes(const ring_set_t* set,
                                  net_node_t* nodes,
                                  size_t max_nodes,
                                  const node_id_t* exclude_id);
```

- [ ] **Step 4: Add implementation to ring_set.c**

```c
size_t ring_set_get_random_nodes(const ring_set_t* set,
                                  net_node_t* nodes,
                                  size_t max_nodes,
                                  const node_id_t* exclude_id) {
  if (set == NULL || nodes == NULL) return 0;
  size_t count = 0;
  for (size_t ring_index = 0;
       ring_index < set->ring_count && count < max_nodes;
       ring_index++) {
    ring_t* ring = &set->rings[ring_index];
    if (ring->primary.length == 0) continue;
    // Select a random node from this ring's primary list
    int node_index = rand() % ring->primary.length;
    net_node_t* candidate = ring->primary.data[node_index];
    if (candidate == NULL) continue;
    // Skip if excluded
    if (exclude_id != NULL &&
        node_id_equals(&candidate->id, exclude_id)) {
      continue;
    }
    nodes[count++] = *candidate;
  }
  return count;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/test/testliboffs --gtest_filter="RingSetGetRandomNodes*"`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/Network/ring_set.h src/Network/ring_set.c test/test_network.cpp
git commit -m "feat: add ring_set_get_random_nodes for Meridian gossip target selection"
```

---

### Task 3: Message types and PingCapacity timer separation

**Files:**
- Modify: `src/Actor/message.h` (add NETWORK_GOSSIP_RECEIVED, NETWORK_GOSSIP_PULL_RECEIVED, NETWORK_PING_CAPACITY_TICK)
- Modify: `src/Network/network.h` (add ping_capacity_timer_id, PING_CAPACITY_INTERVAL_MS)
- Modify: `src/Network/network.c` (add timer setup, handler, dispatch case, destroy cleanup)

- [ ] **Step 1: Add message types to message.h**

After `NETWORK_GOSSIP_EXPIRE` (line 113), add:

```c
  NETWORK_GOSSIP_RECEIVED,
  NETWORK_GOSSIP_PULL_RECEIVED,
  NETWORK_PING_CAPACITY_TICK,
```

- [ ] **Step 2: Add timer constant and timer_id to network.h**

In network.h, add the constant near the other interval constants (after the `network_t` struct):

```c
#define PING_CAPACITY_INTERVAL_MS 900000  // 15 minutes
```

Add the timer_id field to `network_t` after `metrics_push_timer_id`:

```c
  uint64_t ping_capacity_timer_id;
```

- [ ] **Step 3: Add PING_CAPACITY_INTERVAL_MS constant in network.c**

Near the top of network.c where `TOPOLOGY_METRICS_PUSH_INTERVAL_MS` is defined (line 31), add:

```c
#define PING_CAPACITY_INTERVAL_MS 900000  // 15 minutes
```

- [ ] **Step 4: Add timer setup in network_create**

After the `metrics_push_timer_id` timer setup (around line 127), add:

```c
  // PingCapacity timer: periodic capacity exchange with all peers
  network->ping_capacity_timer_id = timer_actor_set(timer,
      PING_CAPACITY_INTERVAL_MS,
      PING_CAPACITY_INTERVAL_MS,
      &network->actor,
      NETWORK_PING_CAPACITY_TICK);
```

- [ ] **Step 5: Add timer cancellation in network_destroy**

After the `metrics_push_timer_id` cancellation (around line 137), add:

```c
  if (network->ping_capacity_timer_id != 0) {
    timer_actor_cancel(network->timer, network->ping_capacity_timer_id);
    network->ping_capacity_timer_id = 0;
  }
```

- [ ] **Step 6: Add dispatch cases in network_dispatch**

Add after `NETWORK_GOSSIP_EXPIRE`:

```c
    case NETWORK_PING_CAPACITY_TICK:
      network_handle_ping_capacity_tick(network, msg);
      break;
    case NETWORK_GOSSIP_RECEIVED:
      network_handle_gossip_received(network, msg);
      break;
    case NETWORK_GOSSIP_PULL_RECEIVED:
      network_handle_gossip_pull_received(network, msg);
      break;
```

- [ ] **Step 7: Add the PingCapacity tick handler**

Add a new handler function before `network_handle_gossip_tick`:

```c
static void network_handle_ping_capacity_tick(network_t* network, message_t* msg) {
  (void)msg;
  if (network->conn_mgr.peer_count == 0) return;

  for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
    peer_connection_t* peer = network->conn_mgr.peers[index];
    if (peer == NULL || !peer->connected) continue;

    wire_ping_capacity_t ping_cap;
    memset(&ping_cap, 0, sizeof(ping_cap));
    ping_cap.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&ping_cap.source, &network->authority->local_id, sizeof(node_id_t));
    ping_cap.capacity = atomic_load(&network->authority->capacity);
    ping_cap.phase = atomic_load(&network->authority->phase);

    cbor_item_t* cbor = wire_ping_capacity_encode(&ping_cap);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
}
```

- [ ] **Step 8: Build and verify compilation**

Run: `cmake --build build 2>&1 | grep -E "error|warning: implicit" | head -20`
Expected: No errors (handler function bodies exist but won't be called until the next task wires up the gossip handler)

- [ ] **Step 9: Commit**

```bash
git add src/Actor/message.h src/Network/network.h src/Network/network.c
git commit -m "feat: add PING_CAPACITY_TICK timer, GOSSIP_RECEIVED message types, and dispatch cases"
```

---

### Task 4: Rewrite gossip tick and add gossip handlers

**Files:**
- Modify: `src/Network/network.c` (rewrite `network_handle_gossip_tick`, add `network_handle_gossip_received`, `network_handle_gossip_pull_received`, `network_add_node_to_ring`)

- [ ] **Step 1: Rewrite network_handle_gossip_tick**

Replace the existing `network_handle_gossip_tick` (currently sends PingCapacity to all peers) with the Meridian gossip algorithm:

```c
static void network_handle_gossip_tick(network_t* network, message_t* msg) {
  (void)msg;
  uint64_t now_ms = (uint64_t)time(NULL) * 1000;

  // Expire stale gossip exchanges and timed-out queries
  gossip_handle_expire_queries(&network->gossip, now_ms);

  // Apply Hebbian decay on each gossip tick
  hebbian_decay(&network->hebbian);

  // Tick the scheduler to determine if we should gossip
  gossip_scheduler_tick(&network->gossip.scheduler, now_ms);
  if (!network->gossip.scheduler.should_gossip) return;

  // Select 1 random node per ring as gossip targets
  net_node_t targets[RING_MAX_RINGS];
  size_t target_count = ring_set_get_random_nodes(
      network->rings, targets, RING_MAX_RINGS,
      &network->authority->local_id);

  for (size_t index = 0; index < target_count; index++) {
    peer_connection_t* peer = connection_manager_lookup(
        &network->conn_mgr, &targets[index].id);
    if (peer == NULL || !peer->connected) continue;

    // Build gossip message with ring membership (excluding target)
    wire_gossip_t gossip;
    memset(&gossip, 0, sizeof(gossip));
    gossip.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&gossip.sender_id, &network->authority->local_id, sizeof(node_id_t));
    gossip.rendezvous_addr = 0;  // 0 until NAT detection fills this
    gossip.rendezvous_port = 0;

    // Fill targets: 1 random node per ring, excluding the target
    net_node_t ring_targets[RING_MAX_RINGS];
    gossip.target_count = (uint8_t)ring_set_get_random_nodes(
        network->rings, ring_targets, RING_MAX_RINGS, &targets[index].id);
    for (uint8_t target_index = 0;
         target_index < gossip.target_count && target_index < RING_MAX_RINGS;
         target_index++) {
      memcpy(&gossip.targets[target_index], &ring_targets[target_index],
             sizeof(node_id_t));
    }

    cbor_item_t* cbor = wire_gossip_encode(&gossip);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);

    if (network->log != NULL) {
      message_log_record(network->log, WIRE_GOSSIP, MSG_DIRECTION_SENT,
                         &targets[index].id, gossip.message_id, NULL,
                         0, &network->hebbian);
    }
  }
}
```

- [ ] **Step 2: Add network_add_node_to_ring helper**

Add before `network_handle_gossip_received`:

```c
static void network_add_node_to_ring(network_t* network,
                                       const node_id_t* node_id,
                                       uint32_t addr,
                                       uint16_t port) {
  if (network == NULL || node_id == NULL) return;

  // Check if already in ring table — skip duplicate insertion
  net_node_t* existing = ring_set_find_by_id(network->rings, node_id);
  if (existing != NULL) {
    // Update last_gossip_time and capacity from fresh gossip
    existing->last_gossip_time = (uint64_t)time(NULL) * 1000;
    net_node_record_success(existing);
    return;
  }

  // Check probe cache — use cached latency if available
  float cached_latency_ms;
  if (latency_cache_get(network->latency_cache, node_id, &cached_latency_ms) == 0) {
    uint32_t latency_us = (uint32_t)(cached_latency_ms * 1000);
    net_node_t* node = net_node_create(node_id, addr, port);
    if (node != NULL) {
      node->weight = FIND_BLOCK_MIN_WEIGHT;
      node->last_gossip_time = (uint64_t)time(NULL) * 1000;
      net_node_record_success(node);
      ring_set_insert(network->rings, node, latency_us);
    }
    return;
  }

  // Not in cache — only probe if already connected
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, node_id);
  if (peer != NULL && peer->connected) {
    // Send Ping to measure latency — ring insertion deferred until PingResponse
    wire_ping_t ping;
    memset(&ping, 0, sizeof(ping));
    ping.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&ping.sender_id, &network->authority->local_id, sizeof(node_id_t));
    ping.timestamp = (uint64_t)time(NULL) * 1000;

    cbor_item_t* cbor = wire_ping_encode(&ping);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  } else {
    // Not connected — insert at latency=0 (outermost ring) for now
    net_node_t* node = net_node_create(node_id, addr, port);
    if (node != NULL) {
      node->weight = FIND_BLOCK_MIN_WEIGHT;
      node->last_gossip_time = (uint64_t)time(NULL) * 1000;
      net_node_record_success(node);
      ring_set_insert(network->rings, node, 0);
    }
  }
}
```

- [ ] **Step 3: Add network_handle_gossip_received**

```c
static void network_handle_gossip_received(network_t* network, message_t* msg) {
  wire_gossip_t* gossip = (wire_gossip_t*)msg->payload;
  if (gossip == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_GOSSIP, MSG_DIRECTION_RECEIVED,
                       &gossip->sender_id, gossip->message_id, NULL,
                       0, &network->hebbian);
  }

  // Add sender to ring table
  network_add_node_to_ring(network, &gossip->sender_id,
                            gossip->rendezvous_addr, gossip->rendezvous_port);

  // Add all targets from the gossip packet
  for (uint8_t index = 0; index < gossip->target_count && index < RING_MAX_RINGS; index++) {
    network_add_node_to_ring(network, &gossip->targets[index], 0, 0);
  }

  // PUSHPULL: send our ring membership back to the sender
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &gossip->sender_id);
  if (peer != NULL && peer->connected) {
    wire_gossip_pull_t pull;
    memset(&pull, 0, sizeof(pull));
    pull.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&pull.sender_id, &network->authority->local_id, sizeof(node_id_t));
    pull.rendezvous_addr = 0;
    pull.rendezvous_port = 0;

    net_node_t ring_targets[RING_MAX_RINGS];
    pull.target_count = (uint8_t)ring_set_get_random_nodes(
        network->rings, ring_targets, RING_MAX_RINGS, &gossip->sender_id);
    for (uint8_t target_index = 0;
         target_index < pull.target_count && target_index < RING_MAX_RINGS;
         target_index++) {
      memcpy(&pull.targets[target_index], &ring_targets[target_index],
             sizeof(node_id_t));
    }

    cbor_item_t* cbor = wire_gossip_pull_encode(&pull);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
}
```

- [ ] **Step 4: Add network_handle_gossip_pull_received**

```c
static void network_handle_gossip_pull_received(network_t* network, message_t* msg) {
  wire_gossip_pull_t* pull = (wire_gossip_pull_t*)msg->payload;
  if (pull == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_GOSSIP_PULL, MSG_DIRECTION_RECEIVED,
                       &pull->sender_id, pull->message_id, NULL,
                       0, &network->hebbian);
  }

  // Add sender to ring table
  network_add_node_to_ring(network, &pull->sender_id,
                            pull->rendezvous_addr, pull->rendezvous_port);

  // Add all targets from the gossip pull packet
  for (uint8_t index = 0; index < pull->target_count && index < RING_MAX_RINGS; index++) {
    network_add_node_to_ring(network, &pull->targets[index], 0, 0);
  }

  // No response — this is the pull half of PUSHPULL
}
```

- [ ] **Step 5: Add forward declarations for new handlers**

At the top of network.c where other forward declarations are (around line 35), add:

```c
static void network_handle_gossip_received(network_t* network, message_t* msg);
static void network_handle_gossip_pull_received(network_t* network, message_t* msg);
static void network_handle_ping_capacity_tick(network_t* network, message_t* msg);
static void network_add_node_to_ring(network_t* network, const node_id_t* node_id,
                                       uint32_t addr, uint16_t port);
```

- [ ] **Step 6: Build and verify compilation**

Run: `cmake --build build 2>&1 | grep -E "error" | head -20`
Expected: No errors

- [ ] **Step 7: Commit**

```bash
git add src/Network/network.c
git commit -m "feat: rewrite gossip tick for Meridian protocol, add gossip/pull handlers and addNodeToRing"
```

---

### Task 5: Wire dispatch for incoming WIRE_GOSSIP and WIRE_GOSSIP_PULL

**Files:**
- Modify: `src/Network/network.c` (add QUIC listener and relay dispatch cases for wire types 21 and 22)

- [ ] **Step 1: Add WIRE_GOSSIP and WIRE_GOSSIP_PULL dispatch in QUIC listener section**

In the QUIC data dispatch switch (around line 2079-2270), after `case WIRE_RATE_LIMITED:`, add:

```c
        case WIRE_GOSSIP: {
          wire_gossip_t* payload = get_clear_memory(sizeof(wire_gossip_t));
          if (payload != NULL) {
            if (wire_gossip_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_gossip_received(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_GOSSIP_PULL: {
          wire_gossip_pull_t* payload = get_clear_memory(sizeof(wire_gossip_pull_t));
          if (payload != NULL) {
            if (wire_gossip_pull_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_gossip_pull_received(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
```

- [ ] **Step 2: Add WIRE_GOSSIP and WIRE_GOSSIP_PULL dispatch in relay section**

Find the relay dispatch switch (around line 2496-2700) and add the same `case WIRE_GOSSIP:` and `case WIRE_GOSSIP_PULL:` blocks after `case WIRE_RATE_LIMITED:`.

- [ ] **Step 3: Build and verify compilation**

Run: `cmake --build build 2>&1 | grep -E "error" | head -20`
Expected: No errors

- [ ] **Step 4: Commit**

```bash
git add src/Network/network.c
git commit -m "feat: add QUIC and relay dispatch for WIRE_GOSSIP and WIRE_GOSSIP_PULL"
```

---

### Task 6: Immediate PingCapacity on new peer connection

**Files:**
- Modify: `src/Network/network.c` (add PingCapacity send in `network_handle_salutation`)

- [ ] **Step 1: Add immediate PingCapacity send after salutation**

In `network_handle_salutation`, after the existing code that inserts the authenticated peer into the ring table (around line 349), add the PingCapacity send:

```c
    // Send immediate PingCapacity to newly connected peer
    {
      wire_ping_capacity_t ping_cap;
      memset(&ping_cap, 0, sizeof(ping_cap));
      ping_cap.message_id = gossip_handle_next_query_id(&network->gossip);
      memcpy(&ping_cap.source, &network->authority->local_id, sizeof(node_id_t));
      ping_cap.capacity = atomic_load(&network->authority->capacity);
      ping_cap.phase = atomic_load(&network->authority->phase);
      cbor_item_t* ping_cap_cbor = wire_ping_capacity_encode(&ping_cap);
      conn_state_send(network, peer, ping_cap_cbor);
      cbor_decref(&ping_cap_cbor);
    }
```

- [ ] **Step 2: Build and verify compilation**

Run: `cmake --build build 2>&1 | grep -E "error" | head -20`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add src/Network/network.c
git commit -m "feat: send immediate PingCapacity on new peer connection"
```

---

### Task 7: CTRL_GOSSIP integration test command

**Files:**
- Modify: `test/test_control_protocol.h` (add CTRL_GOSSIP command)
- Modify: `test/test_node_main.c` (add CTRL_GOSSIP handler)

- [ ] **Step 1: Add CTRL_GOSSIP to test_control_protocol.h**

After `CTRL_RANK_BLOCK "RANK_BLOCK"`, add:

```c
#define CTRL_GOSSIP          "GOSSIP"
```

After `CTRL_RESP_HEBBIAN "HEBBIAN_RESP"`, add:

```c
#define CTRL_RESP_GOSSIP     "GOSSIP_RESP"
```

- [ ] **Step 1b: Add wire type constants to test_rpc_integration.cpp**

After the `WIRE_RATE_LIMITED_VAL` constant (line 45), add:

```cpp
static constexpr uint8_t WIRE_GOSSIP_VAL               = 21;
static constexpr uint8_t WIRE_GOSSIP_PULL_VAL           = 22;
```

- [ ] **Step 2: Add CTRL_GOSSIP handler in test_node_main.c**

In the `handle_command` function, before the final `else` block, add a handler for CTRL_GOSSIP that triggers a gossip round:

```c
  } else if (strncmp(line, CTRL_GOSSIP, strlen(CTRL_GOSSIP)) == 0) {
#ifdef OFFS_TEST
    if (g_node.network != NULL) {
      message_t msg;
      memset(&msg, 0, sizeof(msg));
      msg.type = NETWORK_GOSSIP_TICK;
      actor_send(&g_node.network->actor, &msg);
      send_response(client_fd, CTRL_RESP_OK);
    } else {
      send_response(client_fd, CTRL_RESP_ERROR " no network");
    }
#else
    send_response(client_fd, CTRL_RESP_ERROR " not available");
#endif
```

- [ ] **Step 3: Build and verify compilation**

Run: `cmake --build build 2>&1 | grep -E "error" | head -20`
Expected: No errors

- [ ] **Step 4: Commit**

```bash
git add test/test_control_protocol.h test/test_node_main.c
git commit -m "test: add CTRL_GOSSIP command for triggering gossip rounds in integration tests"
```

---

### Task 8: Integration test — GossipDiscoveryDiamond

**Files:**
- Modify: `test/test_rpc_integration.cpp` (add GossipDiscoveryDiamond test)

- [ ] **Step 1: Write the GossipDiscoveryDiamond test**

Add after the `RankBlockVisitedBloomDiamond` test:

```cpp
TEST_F(RpcIntegrationTest, GossipDiscoveryDiamond) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create diamond topology: A-B-D, A-C-D */
  auto diamond = make_diamond();
  ASSERT_EQ(diamond.size(), 4u);

  /* Clear event logs on all nodes */
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    clear_events(diamond[idx].control_fd);
  }

  /* Trigger gossip on all nodes */
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    std::string resp = send_command(diamond[idx].control_fd, CTRL_GOSSIP);
    EXPECT_NE(resp.find(CTRL_RESP_OK), std::string::npos)
        << "GOSSIP on node " << idx << ": " << resp;
  }

  /* Wait for gossip propagation and GOSSIP_PULL responses */
  std::this_thread::sleep_for(std::chrono::seconds(5));

  /* Verify that WIRE_GOSSIP messages were sent and received */
  bool gossip_sent = false;
  bool gossip_received = false;
  bool gossip_pull_received = false;
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    auto events = get_events(diamond[idx].control_fd);
    if (has_event(events, MSG_DIRECTION_SENT_VAL, WIRE_GOSSIP_VAL)) {
      gossip_sent = true;
    }
    if (has_event(events, MSG_DIRECTION_RECEIVED_VAL, WIRE_GOSSIP_VAL)) {
      gossip_received = true;
    }
    if (has_event(events, MSG_DIRECTION_RECEIVED_VAL, WIRE_GOSSIP_PULL_VAL)) {
      gossip_pull_received = true;
    }
  }
  EXPECT_TRUE(gossip_sent) << "At least one node should send GOSSIP";
  EXPECT_TRUE(gossip_received) << "At least one node should receive GOSSIP";
  EXPECT_TRUE(gossip_pull_received) << "At least one node should receive GOSSIP_PULL";

  /* Verify gossip traffic is bounded (no flooding) */
  size_t total_gossip_sent = 0;
  for (size_t idx = 0; idx < diamond.size(); idx++) {
    auto events = get_events(diamond[idx].control_fd);
    total_gossip_sent += count_events(events, MSG_DIRECTION_SENT_VAL, WIRE_GOSSIP_VAL);
  }
  /* With 4 nodes and up to 10 rings each, max 40 gossip messages per round.
   * With bounded gossip, total should be well under that. */
  EXPECT_LE(total_gossip_sent, 40u) << "Gossip traffic should be bounded";
#endif
}
```

- [ ] **Step 2: Build and verify compilation**

Run: `cmake --build build 2>&1 | grep -E "error" | head -20`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "test: add GossipDiscoveryDiamond integration test"
```

---

### Task 9: Full build, test, and de-wonk

- [ ] **Step 1: Build the full project**

Run: `cmake --build build`
Expected: Clean build with no errors

- [ ] **Step 2: Run all unit tests**

Run: `./build/test/testliboffs`
Expected: All tests pass (including new WireGossipTest, WireGossipPullTest, RingSetGetRandomNodes)

- [ ] **Step 3: Run integration tests**

Run: `./build/test/test_rpc_integration`
Expected: All integration tests pass (including GossipDiscoveryDiamond if QUIC is available)

- [ ] **Step 4: Run de-wonk audit**

Use the de-wonk skill to check all modified files for unimplemented, stubbed, disabled, broken, or weird code. Fix any issues found.

- [ ] **Step 5: Final commit if fixes needed**

```bash
git add -A
git commit -m "fix: de-wonk fixes for Meridian gossip implementation"
```