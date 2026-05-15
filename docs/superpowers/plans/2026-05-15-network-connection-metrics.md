# Network Connection Management, EABF Routing, and Metrics Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement peer connection actors, connection manager, EABF gravity-well routing with timing-wheel decay, Hebbian weight lifecycle, and optional topology metrics server.

**Architecture:** Each QUIC connection becomes a `peer_connection_t` actor that owns its EABF, Hebbian weight, per-RPC counters, and RTT. A `connection_manager_t` data structure (not actor) inside `network_t` holds the flat array of peer actors and Hebbian config. The network actor drives lifecycle, decay ticks, and metrics collection. EABFs are purely local routing state populated by FindBlock failures and StoreBlock successes, decayed by a timing wheel per EABF. The topology metrics server is an optional `authority_t` component.

**Tech Stack:** C11, libcbor, xxHash, poll-dancer (event loop), msquic (QUIC), CMake

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/Network/peer_connection.h` | Create | Peer connection actor struct, message types, API |
| `src/Network/peer_connection.c` | Create | Peer connection lifecycle, dispatch, RPC send, EABF operations |
| `src/Network/connection_manager.h` | Create | Connection manager data structure, lookup/add/remove/decay/iterate APIs |
| `src/Network/connection_manager.c` | Create | Connection manager implementation |
| `src/Network/hebbian_config.h` | Create | Hebbian configuration struct with per-RPC multipliers |
| `src/Network/timing_wheel.h` | Create | Timing wheel for EABF TTL expiry |
| `src/Network/timing_wheel.c` | Create | Timing wheel implementation |
| `src/Network/topology_metrics.h` | Create | Topology metrics server actor, snapshots, push API |
| `src/Network/topology_metrics.c` | Create | Topology metrics server implementation |
| `src/Network/network.h` | Modify | Add connection_manager_t*, timer IDs, metrics server reference |
| `src/Network/network.c` | Modify | Integrate connection manager, Hebbian decay tick, EABF tick, metrics push, gravity-well routing |
| `src/Network/authority.h` | Modify | Add topology_metrics_t* field |
| `src/Network/find_block.c` | Modify | Use gravity-well search via connection_manager_get_peers_for_topic |
| `src/Network/store_block.c` | Modify | Use gravity-well search, recall on block acquisition |
| `src/Actor/message.h` | Modify | Add PEER_SEND_* and CM_* and TOPOLOGY_METRICS_UPDATE message types |
| `test/test_network.cpp` | Modify | Add tests for all new components |

---

### Task 1: Timing Wheel

**Files:**
- Create: `src/Network/timing_wheel.h`
- Create: `src/Network/timing_wheel.c`
- Test: `test/test_network.cpp`

The timing wheel manages EABF entry TTLs. Each EABF owns one timing wheel. When an entry is inserted into any EABF level, a corresponding slot is added to the wheel. When the wheel ticks, expired entries are deleted from their EABF level.

- [ ] **Step 1: Write failing tests for timing wheel**

Add to `test/test_network.cpp`:

```cpp
class TimingWheelTest : public ::testing::Test {
protected:
  void SetUp() override {
    timing_wheel_init(&wheel, 64, 60000);  // 64 slots, 60s per slot = 64 min span
  }
  void TearDown() override {
    timing_wheel_deinit(&wheel);
  }
  timing_wheel_t wheel;
};

TEST_F(TimingWheelTest, InitDeinit) {
  EXPECT_NE(wheel.slots, nullptr);
  EXPECT_EQ(wheel.slot_count, 64u);
  EXPECT_EQ(wheel.slot_duration_ms, 60000u);
}

TEST_F(TimingWheelTest, AddEntry) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  uint64_t id = timing_wheel_add(&wheel, &peer_id, 0, 42, 7, block_hash);
  EXPECT_NE(id, 0u);
  EXPECT_EQ(wheel.count, 1u);
}

TEST_F(TimingWheelTest, AddAndExpire) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  timing_wheel_add(&wheel, &peer_id, 0, 42, 7, block_hash);
  // Advance by 64 slots = full rotation = entry expires
  timing_wheel_advance(&wheel, 64);
  EXPECT_EQ(wheel.count, 0u);
}

TEST_F(TimingWheelTest, AddAndRefresh) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  uint64_t id1 = timing_wheel_add(&wheel, &peer_id, 0, 42, 7, block_hash);
  // Refresh the same entry — moves it forward
  uint64_t id2 = timing_wheel_refresh(&wheel, id1, &peer_id, 0, 42, 7, block_hash);
  EXPECT_NE(id2, 0u);
  // Advance 32 slots — entry should still be alive (refreshed)
  timing_wheel_advance(&wheel, 32);
  EXPECT_EQ(wheel.count, 1u);
}

TEST_F(TimingWheelTest, RemoveById) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  uint64_t id = timing_wheel_add(&wheel, &peer_id, 1, 99, 3, block_hash);
  int result = timing_wheel_remove(&wheel, id);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(wheel.count, 0u);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target testliboffs 2>&1 | head -30`
Expected: Compilation errors — timing_wheel types and functions not defined yet.

- [ ] **Step 3: Write timing_wheel.h**

Create `src/Network/timing_wheel.h`:

```c
#ifndef OFFS_TIMING_WHEEL_H
#define OFFS_TIMING_WHEEL_H

#include "node_id.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// A single entry in the timing wheel
typedef struct timing_wheel_entry_t {
  uint64_t id;                      // unique ID for removal/refresh
  node_id_t peer_id;                // which peer's EABF
  uint32_t level;                   // EABF level
  size_t bucket_index;              // bucket in the EBF at that level
  uint32_t fingerprint;             // fingerprint in that bucket
  uint8_t block_hash[32];           // the block hash that was inserted
  struct timing_wheel_entry_t* next; // linked list within slot
} timing_wheel_entry_t;

// A single slot in the wheel (linked list of entries)
typedef struct timing_wheel_slot_t {
  timing_wheel_entry_t* head;
} timing_wheel_slot_t;

// The timing wheel
typedef struct timing_wheel_t {
  timing_wheel_slot_t* slots;
  size_t slot_count;
  uint64_t slot_duration_ms;       // duration of each slot in ms
  size_t current_slot;              // current position in the wheel
  uint64_t next_id;                 // monotonically increasing ID counter
  size_t count;                     // total entries across all slots
} timing_wheel_t;

// Initialize a timing wheel with slot_count slots, each of slot_duration_ms
void timing_wheel_init(timing_wheel_t* wheel, size_t slot_count, uint64_t slot_duration_ms);

// Deinitialize the wheel, freeing all entries
void timing_wheel_deinit(timing_wheel_t* wheel);

// Add an entry to the wheel. Returns unique ID for removal/refresh.
// The entry will expire after (slot_count * slot_duration_ms) from now.
uint64_t timing_wheel_add(timing_wheel_t* wheel, const node_id_t* peer_id,
                          uint32_t level, size_t bucket_index, uint32_t fingerprint,
                          const uint8_t* block_hash);

// Refresh an existing entry — removes old entry and adds a new one.
// Returns new ID. Returns 0 if old_id not found.
uint64_t timing_wheel_refresh(timing_wheel_t* wheel, uint64_t old_id,
                              const node_id_t* peer_id,
                              uint32_t level, size_t bucket_index, uint32_t fingerprint,
                              const uint8_t* block_hash);

// Remove an entry by ID. Returns 0 on success, -1 if not found.
int timing_wheel_remove(timing_wheel_t* wheel, uint64_t id);

// Advance the wheel by num_slots positions.
// Returns an array of expired entries and sets out_count.
// Caller must free the returned array (but not the entries themselves — they are freed).
// The returned entries contain the information needed to delete from EABF levels.
timing_wheel_entry_t* timing_wheel_advance(timing_wheel_t* wheel, size_t num_slots,
                                           size_t* out_count);

// Compute TTL for a given EABF distance level.
// Level 0: base_ttl, Level 1: base_ttl / 1.5, Level 2: base_ttl / 2, Level 3: base_ttl / 2.5
uint64_t timing_wheel_ttl_for_level(uint32_t level, uint64_t base_ttl_ms);

#endif // OFFS_TIMING_WHEEL_H
```

- [ ] **Step 4: Write timing_wheel.c**

Create `src/Network/timing_wheel.c`:

```c
#include "timing_wheel.h"
#include "../Util/allocator.h"
#include <string.h>

void timing_wheel_init(timing_wheel_t* wheel, size_t slot_count, uint64_t slot_duration_ms) {
  if (slot_count == 0) slot_count = 64;
  if (slot_duration_ms == 0) slot_duration_ms = 60000;  // 60s default
  wheel->slots = get_clear_memory(slot_count * sizeof(timing_wheel_slot_t));
  wheel->slot_count = slot_count;
  wheel->slot_duration_ms = slot_duration_ms;
  wheel->current_slot = 0;
  wheel->next_id = 1;
  wheel->count = 0;
}

void timing_wheel_deinit(timing_wheel_t* wheel) {
  if (wheel == NULL || wheel->slots == NULL) return;
  for (size_t slot = 0; slot < wheel->slot_count; slot++) {
    timing_wheel_entry_t* entry = wheel->slots[slot].head;
    while (entry != NULL) {
      timing_wheel_entry_t* next = entry->next;
      free(entry);
      entry = next;
    }
  }
  free(wheel->slots);
  wheel->slots = NULL;
  wheel->slot_count = 0;
  wheel->count = 0;
}

uint64_t timing_wheel_add(timing_wheel_t* wheel, const node_id_t* peer_id,
                          uint32_t level, size_t bucket_index, uint32_t fingerprint,
                          const uint8_t* block_hash) {
  if (wheel == NULL || peer_id == NULL) return 0;

  timing_wheel_entry_t* entry = get_clear_memory(sizeof(timing_wheel_entry_t));
  if (entry == NULL) return 0;

  entry->id = wheel->next_id++;
  memcpy(&entry->peer_id, peer_id, sizeof(node_id_t));
  entry->level = level;
  entry->bucket_index = bucket_index;
  entry->fingerprint = fingerprint;
  if (block_hash != NULL) {
    memcpy(entry->block_hash, block_hash, 32);
  }

  // Place entry at the slot that corresponds to its TTL
  // For level-based TTL, we compute how many slots ahead
  uint64_t ttl_ms = timing_wheel_ttl_for_level(level, wheel->slot_count * wheel->slot_duration_ms);
  size_t slots_ahead = (size_t)(ttl_ms / wheel->slot_duration_ms);
  if (slots_ahead == 0) slots_ahead = 1;
  if (slots_ahead >= wheel->slot_count) slots_ahead = wheel->slot_count - 1;
  size_t target_slot = (wheel->current_slot + slots_ahead) % wheel->slot_count;

  entry->next = wheel->slots[target_slot].head;
  wheel->slots[target_slot].head = entry;
  wheel->count++;
  return entry->id;
}

uint64_t timing_wheel_refresh(timing_wheel_t* wheel, uint64_t old_id,
                              const node_id_t* peer_id,
                              uint32_t level, size_t bucket_index, uint32_t fingerprint,
                              const uint8_t* block_hash) {
  if (wheel == NULL || old_id == 0) return 0;
  // Remove old entry
  timing_wheel_remove(wheel, old_id);
  // Add new entry with same data
  return timing_wheel_add(wheel, peer_id, level, bucket_index, fingerprint, block_hash);
}

int timing_wheel_remove(timing_wheel_t* wheel, uint64_t id) {
  if (wheel == NULL || id == 0) return -1;
  for (size_t slot = 0; slot < wheel->slot_count; slot++) {
    timing_wheel_entry_t** prev = &wheel->slots[slot].head;
    timing_wheel_entry_t* entry = *prev;
    while (entry != NULL) {
      if (entry->id == id) {
        *prev = entry->next;
        free(entry);
        wheel->count--;
        return 0;
      }
      prev = &entry->next;
      entry = entry->next;
    }
  }
  return -1;
}

timing_wheel_entry_t* timing_wheel_advance(timing_wheel_t* wheel, size_t num_slots,
                                            size_t* out_count) {
  if (wheel == NULL || num_slots == 0) {
    if (out_count) *out_count = 0;
    return NULL;
  }

  // Collect all expired entries
  size_t total_expired = 0;
  size_t expired_capacity = 64;
  timing_wheel_entry_t* expired = get_clear_memory(expired_capacity * sizeof(timing_wheel_entry_t));

  for (size_t step = 0; step < num_slots; step++) {
    wheel->current_slot = (wheel->current_slot + 1) % wheel->slot_count;
    timing_wheel_entry_t* entry = wheel->slots[wheel->current_slot].head;
    while (entry != NULL) {
      timing_wheel_entry_t* next = entry->next;
      // Copy entry data to expired list
      if (total_expired >= expired_capacity) {
        expired_capacity *= 2;
        timing_wheel_entry_t* new_expired = get_clear_memory(expired_capacity * sizeof(timing_wheel_entry_t));
        memcpy(new_expired, expired, total_expired * sizeof(timing_wheel_entry_t));
        free(expired);
        expired = new_expired;
      }
      memcpy(&expired[total_expired], entry, sizeof(timing_wheel_entry_t));
      total_expired++;
      // Free the entry
      free(entry);
      wheel->count--;
      entry = next;
    }
    wheel->slots[wheel->current_slot].head = NULL;
  }

  if (out_count) *out_count = total_expired;
  return expired;
}

uint64_t timing_wheel_ttl_for_level(uint32_t level, uint64_t base_ttl_ms) {
  // Level 0: full TTL, Level 1: 2/3, Level 2: 1/2, Level 3: 2/5
  double divisor = 1.0 + (double)level * 0.5;
  return (uint64_t)(base_ttl_ms / divisor);
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake --build . --target testliboffs && ./testliboffs --gtest_filter="TimingWheel*" 2>&1`
Expected: All TimingWheel tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/Network/timing_wheel.h src/Network/timing_wheel.c test/test_network.cpp
git commit -m "feat: add timing wheel for EABF TTL expiry"
```

---

### Task 2: Hebbian Configuration

**Files:**
- Create: `src/Network/hebbian_config.h`
- Modify: `test/test_network.cpp`

Extract Hebbian configuration into a dedicated struct so it can be configured via authority and passed to connection_manager.

- [ ] **Step 1: Write hebbian_config.h**

Create `src/Network/hebbian_config.h`:

```c
#ifndef OFFS_HEBBIAN_CONFIG_H
#define OFFS_HEBBIAN_CONFIG_H

#include <stdint.h>
#include <stddef.h>

// Per-RPC Hebbian weight multipliers (indexed by WIRE_* type constants)
#define HEBBIAN_RPC_MULTIPLIER_COUNT 20  // covers all wire message types

typedef struct hebbian_config_t {
  float initial_weight;         // weight for new connections (default 0.1)
  float drop_threshold;        // close connection below this (default 0.01)
  float decay_rate;            // subtracted per tick (default 0.001)
  uint64_t decay_tick_ms;      // ms between decay ticks (default 60000)
  float base_reward;           // base positive delta per success (default 0.1)
  float failure_penalty;       // negative delta per failure (default 0.2)
  float rate_limit_penalty;    // negative delta per rate limit (default 0.1)
  float recall_reward;         // amplified reward for recall push (default 2.0)
  float rpc_multipliers[HEBBIAN_RPC_MULTIPLIER_COUNT]; // per-type multiplier
} hebbian_config_t;

// Initialize with sensible defaults
void hebbian_config_init(hebbian_config_t* config);

// Initialize with production values (stronger defaults)
void hebbian_config_init_production(hebbian_config_t* config);

#endif // OFFS_HEBBIAN_CONFIG_H
```

- [ ] **Step 2: Write hebbian_config.c**

Create `src/Network/hebbian_config.c`:

```c
#include "hebbian_config.h"
#include <string.h>

// Wire message type indices for multiplier lookup
#include "wire.h"

void hebbian_config_init(hebbian_config_t* config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(hebbian_config_t));
  config->initial_weight = 0.1f;
  config->drop_threshold = 0.01f;
  config->decay_rate = 0.001f;
  config->decay_tick_ms = 60000;   // 60s
  config->base_reward = 0.1f;
  config->failure_penalty = 0.2f;
  config->rate_limit_penalty = 0.1f;
  config->recall_reward = 2.0f;

  // Default per-RPC multipliers
  for (size_t index = 0; index < HEBBIAN_RPC_MULTIPLIER_COUNT; index++) {
    config->rpc_multipliers[index] = 0.3f;  // default low
  }
  // Override specific types with higher values
  config->rpc_multipliers[WIRE_FIND_BLOCK] = 1.0f;
  config->rpc_multipliers[WIRE_FIND_BLOCK_RESPONSE] = 1.0f;
  config->rpc_multipliers[WIRE_STORE_BLOCK] = 1.5f;
  config->rpc_multipliers[WIRE_STORE_BLOCK_RESPONSE] = 1.5f;
  config->rpc_multipliers[WIRE_PING_BLOCK] = 0.8f;
  config->rpc_multipliers[WIRE_PING_BLOCK_RESPONSE] = 0.8f;
  config->rpc_multipliers[WIRE_SEEKING_BLOCKS] = 0.5f;
  config->rpc_multipliers[WIRE_SEEKING_BLOCKS_RESPONSE] = 0.5f;
  config->rpc_multipliers[WIRE_PING_CAPACITY] = 0.3f;
  config->rpc_multipliers[WIRE_PING_CAPACITY_RESPONSE] = 0.3f;
}

void hebbian_config_init_production(hebbian_config_t* config) {
  hebbian_config_init(config);
  // Production has more aggressive decay and higher drop threshold
  config->decay_rate = 0.002f;
  config->drop_threshold = 0.05f;
}
```

- [ ] **Step 3: Write failing test for hebbian_config**

Add to `test/test_network.cpp`:

```cpp
class HebbianConfigTest : public ::testing::Test {
protected:
  hebbian_config_t config;
  void SetUp() override { hebbian_config_init(&config); }
};

TEST_F(HebbianConfigTest, Defaults) {
  EXPECT_FLOAT_EQ(config.initial_weight, 0.1f);
  EXPECT_FLOAT_EQ(config.drop_threshold, 0.01f);
  EXPECT_FLOAT_EQ(config.decay_rate, 0.001f);
  EXPECT_EQ(config.decay_tick_ms, 60000u);
  EXPECT_FLOAT_EQ(config.base_reward, 0.1f);
  EXPECT_FLOAT_EQ(config.failure_penalty, 0.2f);
  EXPECT_FLOAT_EQ(config.rate_limit_penalty, 0.1f);
  EXPECT_FLOAT_EQ(config.recall_reward, 2.0f);
  // Check specific multipliers
  EXPECT_FLOAT_EQ(config.rpc_multipliers[WIRE_FIND_BLOCK], 1.0f);
  EXPECT_FLOAT_EQ(config.rpc_multipliers[WIRE_STORE_BLOCK], 1.5f);
}

TEST_F(HebbianConfigTest, ProductionDefaults) {
  hebbian_config_init_production(&config);
  EXPECT_FLOAT_EQ(config.decay_rate, 0.002f);
  EXPECT_FLOAT_EQ(config.drop_threshold, 0.05f);
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build && cmake --build . --target testliboffs && ./testliboffs --gtest_filter="HebbianConfig*" 2>&1`
Expected: All HebbianConfig tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Network/hebbian_config.h src/Network/hebbian_config.c test/test_network.cpp
git commit -m "feat: add Hebbian configuration with per-RPC multipliers"
```

---

### Task 3: Peer Connection Actor

**Files:**
- Create: `src/Network/peer_connection.h`
- Create: `src/Network/peer_connection.c`
- Modify: `src/Actor/message.h`
- Modify: `test/test_network.cpp`

The peer connection actor owns its EABF, Hebbian weight, per-RPC counters, and QUIC stream reference. It handles RPC send messages and Hebbian updates.

- [ ] **Step 1: Add peer connection message types to message.h**

Add after `NETWORK_QUIC_DISCONNECTED` in `src/Actor/message.h`:

```c
  /* Peer connection messages */
  PEER_SEND_FIND_BLOCK,
  PEER_SEND_STORE_BLOCK,
  PEER_SEND_PING_CAPACITY,
  PEER_SEND_SEEKING_BLOCKS,
  PEER_SEND_PING_BLOCK,
  PEER_SEND_FIND_NODE,
  PEER_UPDATE_HEBBIAN,
  PEER_GET_METRICS,
  PEER_CLOSE,
  PEER_EABF_TICK,
  /* Connection manager messages */
  CM_PEER_CONNECTED,
  CM_PEER_DISCONNECTED,
  CM_LOOKUP_PEER,
  CM_LOOKUP_ALL_PEERS,
  CM_GET_PEERS_FOR_TOPIC,
  /* Topology metrics messages */
  TOPOLOGY_METRICS_UPDATE,
```

- [ ] **Step 2: Write peer_connection.h**

Create `src/Network/peer_connection.h`:

```c
#ifndef OFFS_PEER_CONNECTION_H
#define OFFS_PEER_CONNECTION_H

#include "../Actor/actor.h"
#include "eabf.h"
#include "hebbian_config.h"
#include "node_id.h"
#include "timing_wheel.h"
#include "wire.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Forward declarations
typedef struct network_t network_t;
typedef struct quic_data_payload_t quic_data_payload_t;

#define PEER_RPC_TYPE_COUNT 20

typedef struct peer_metrics_snapshot_t {
  node_id_t node_id;
  float hebbian_weight;
  double rtt_ewma_ms;
  uint64_t rpc_count[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_success[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_failure[PEER_RPC_TYPE_COUNT];
  bool connected;
  int64_t connected_at_ms;
} peer_metrics_snapshot_t;

typedef struct peer_connection_t {
  actor_t actor;
  node_id_t remote_node_id;
  struct sockaddr_storage peer_addr;

  // EABF_{self → peer}: what blocks are reachable via this peer
  eabf_t* eabf;
  timing_wheel_t eabf_wheel;

  // Hebbian weight
  float hebbian_weight;

  // Per-RPC counters
  uint64_t rpc_count[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_success[PEER_RPC_TYPE_COUNT];
  uint64_t rpc_failure[PEER_RPC_TYPE_COUNT];

  // Meridian latency
  double last_rtt_ms;
  double rtt_ewma;

  // Connection state
  bool connected;
  int64_t connected_at_ms;

  // Back-reference
  network_t* network;
} peer_connection_t;

// Lifecycle
peer_connection_t* peer_connection_create(network_t* network, const node_id_t* remote_id,
                                          const struct sockaddr_storage* peer_addr,
                                          float initial_weight);
void peer_connection_destroy(peer_connection_t* peer);
void peer_connection_dispatch(void* state, message_t* msg);

// EABF operations
bool peer_eabf_subscribe(peer_connection_t* peer, const uint8_t* topic, size_t topic_len);
bool peer_eabf_unsubscribe(peer_connection_t* peer, const uint8_t* topic, size_t topic_len);
bool peer_eabf_check(const peer_connection_t* peer, const uint8_t* topic, size_t topic_len,
                     uint32_t* out_hops);

// EABF timing wheel operations
void peer_eabf_add_with_ttl(peer_connection_t* peer, const uint8_t* block_hash,
                            uint32_t level, size_t bucket_index, uint32_t fingerprint);
void peer_eabf_tick(peer_connection_t* peer);

// Hebbian operations
void peer_hebbian_update(peer_connection_t* peer, float delta);
void peer_hebbian_decay(peer_connection_t* peer, float decay_rate);

// Metrics
void peer_get_metrics(const peer_connection_t* peer, peer_metrics_snapshot_t* snapshot);

// RTT
void peer_update_rtt(peer_connection_t* peer, double rtt_ms);

#endif // OFFS_PEER_CONNECTION_H
```

- [ ] **Step 3: Write peer_connection.c**

Create `src/Network/peer_connection.c`:

```c
#include "peer_connection.h"
#include "network.h"
#include "../Util/allocator.h"
#include <string.h>
#include <math.h>
#include <time.h>

#define RTT_EWMA_ALPHA 0.1

peer_connection_t* peer_connection_create(network_t* network, const node_id_t* remote_id,
                                           const struct sockaddr_storage* peer_addr,
                                           float initial_weight) {
  if (network == NULL || remote_id == NULL) return NULL;
  peer_connection_t* peer = get_clear_memory(sizeof(peer_connection_t));
  if (peer == NULL) return NULL;

  memcpy(&peer->remote_node_id, remote_id, sizeof(node_id_t));
  if (peer_addr != NULL) {
    memcpy(&peer->peer_addr, peer_addr, sizeof(struct sockaddr_storage));
  }
  peer->network = network;
  peer->hebbian_weight = initial_weight;
  peer->connected = true;
  peer->connected_at_ms = (int64_t)(time(NULL) * 1000);

  // Create EABF for this peer
  peer->eabf = eabf_create(remote_id);
  if (peer->eabf == NULL) {
    free(peer);
    return NULL;
  }

  // Initialize timing wheel (64 slots × 60s = 64 min span, matches EABF design)
  timing_wheel_init(&peer->eabf_wheel, 64, 60000);

  // Initialize actor
  actor_init(&peer->actor, peer, peer_connection_dispatch, network->pool);

  return peer;
}

void peer_connection_destroy(peer_connection_t* peer) {
  if (peer == NULL) return;
  if (peer->eabf != NULL) {
    eabf_destroy(peer->eabf);
  }
  timing_wheel_deinit(&peer->eabf_wheel);
  actor_destroy(&peer->actor);
  free(peer);
}

void peer_connection_dispatch(void* state, message_t* msg) {
  if (state == NULL || msg == NULL) return;
  peer_connection_t* peer = (peer_connection_t*)state;

  switch (msg->type) {
    case PEER_UPDATE_HEBBIAN: {
      float* delta = (float*)msg->payload;
      if (delta != NULL) {
        peer_hebbian_update(peer, *delta);
        free(delta);
      }
      break;
    }
    case PEER_EABF_TICK: {
      peer_eabf_tick(peer);
      break;
    }
    case PEER_GET_METRICS: {
      // Fill the snapshot provided in payload
      peer_metrics_snapshot_t* snapshot = (peer_metrics_snapshot_t*)msg->payload;
      if (snapshot != NULL) {
        peer_get_metrics(peer, snapshot);
      }
      break;
    }
    case PEER_CLOSE: {
      peer->connected = false;
      break;
    }
    default:
      // RPC send messages will be handled in a later task when QUIC send is wired up
      break;
  }
  if (msg->payload != NULL && msg->payload_destroy != NULL) {
    msg->payload_destroy(msg->payload);
  }
}

// --- EABF operations ---

bool peer_eabf_subscribe(peer_connection_t* peer, const uint8_t* topic, size_t topic_len) {
  if (peer == NULL || peer->eabf == NULL) return false;
  return eabf_subscribe(peer->eabf, topic, topic_len);
}

bool peer_eabf_unsubscribe(peer_connection_t* peer, const uint8_t* topic, size_t topic_len) {
  if (peer == NULL || peer->eabf == NULL) return false;
  return eabf_unsubscribe(peer->eabf, topic, topic_len);
}

bool peer_eabf_check(const peer_connection_t* peer, const uint8_t* topic, size_t topic_len,
                    uint32_t* out_hops) {
  if (peer == NULL || peer->eabf == NULL) return false;
  return eabf_check(peer->eabf, topic, topic_len, out_hops);
}

void peer_eabf_add_with_ttl(peer_connection_t* peer, const uint8_t* block_hash,
                             uint32_t level, size_t bucket_index, uint32_t fingerprint) {
  if (peer == NULL || block_hash == NULL) return;
  timing_wheel_add(&peer->eabf_wheel, &peer->remote_node_id,
                   level, bucket_index, fingerprint, block_hash);
}

void peer_eabf_tick(peer_connection_t* peer) {
  if (peer == NULL) return;
  // Advance timing wheel by 1 slot (60s)
  size_t expired_count = 0;
  timing_wheel_entry_t* expired = timing_wheel_advance(&peer->eabf_wheel, 1, &expired_count);
  // Delete expired entries from EABF levels
  for (size_t index = 0; index < expired_count; index++) {
    timing_wheel_entry_t* entry = &expired[index];
    elastic_bloom_filter_t* ebf_level = eabf_get_level(peer->eabf, entry->level);
    if (ebf_level != NULL) {
      elastic_bloom_filter_remove(ebf_level, entry->block_hash, 32);
    }
  }
  if (expired != NULL) {
    free(expired);
  }
}

// --- Hebbian operations ---

void peer_hebbian_update(peer_connection_t* peer, float delta) {
  if (peer == NULL) return;
  peer->hebbian_weight += delta;
  if (peer->hebbian_weight < 0.0f) peer->hebbian_weight = 0.0f;
}

void peer_hebbian_decay(peer_connection_t* peer, float decay_rate) {
  if (peer == NULL) return;
  peer->hebbian_weight -= decay_rate;
  if (peer->hebbian_weight < 0.0f) peer->hebbian_weight = 0.0f;
}

// --- Metrics ---

void peer_get_metrics(const peer_connection_t* peer, peer_metrics_snapshot_t* snapshot) {
  if (peer == NULL || snapshot == NULL) return;
  memcpy(&snapshot->node_id, &peer->remote_node_id, sizeof(node_id_t));
  snapshot->hebbian_weight = peer->hebbian_weight;
  snapshot->rtt_ewma_ms = peer->rtt_ewma;
  memcpy(snapshot->rpc_count, peer->rpc_count, sizeof(peer->rpc_count));
  memcpy(snapshot->rpc_success, peer->rpc_success, sizeof(peer->rpc_success));
  memcpy(snapshot->rpc_failure, peer->rpc_failure, sizeof(peer->rpc_failure));
  snapshot->connected = peer->connected;
  snapshot->connected_at_ms = peer->connected_at_ms;
}

// --- RTT ---

void peer_update_rtt(peer_connection_t* peer, double rtt_ms) {
  if (peer == NULL) return;
  peer->last_rtt_ms = rtt_ms;
  if (peer->rtt_ewma == 0.0) {
    peer->rtt_ewma = rtt_ms;
  } else {
    peer->rtt_ewma = RTT_EWMA_ALPHA * rtt_ms + (1.0 - RTT_EWMA_ALPHA) * peer->rtt_ewma;
  }
}
```

- [ ] **Step 4: Write failing tests for peer connection**

Add to `test/test_network.cpp`:

```cpp
class PeerConnectionTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Minimal network setup — we only need the pool for actor_init
    // In a real test we'd create a full network_t, but for unit tests
    // we test peer_connection functions directly
    node_id_t peer_id = {};
    memset(peer_id.hash, 0xCC, NODE_ID_HASH_SIZE);
    peer = peer_connection_create(NULL, &peer_id, NULL, 0.1f);
  }
  void TearDown() override {
    peer_connection_destroy(peer);
  }
  peer_connection_t* peer;
};

TEST_F(PeerConnectionTest, CreateDestroy) {
  ASSERT_NE(peer, nullptr);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.1f);
  EXPECT_TRUE(peer->connected);
}

TEST_F(PeerConnectionTest, HebbianUpdate) {
  peer_hebbian_update(peer, 0.5f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.6f);
  peer_hebbian_update(peer, -0.3f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.3f);
}

TEST_F(PeerConnectionTest, HebbianDecay) {
  peer_hebbian_update(peer, 0.5f);
  peer_hebbian_decay(peer, 0.1f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.5f);  // 0.1 + 0.5 - 0.1 = 0.5
}

TEST_F(PeerConnectionTest, HebbianClamp) {
  peer_hebbian_update(peer, -1.0f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.0f);
}

TEST_F(PeerConnectionTest, EABFSubscribeCheck) {
  uint8_t topic[] = "test-block-hash-1234567890ab";
  bool result = peer_eabf_subscribe(peer, topic, 32);
  EXPECT_TRUE(result);
  uint32_t hops = 0;
  bool found = peer_eabf_check(peer, topic, 32, &hops);
  EXPECT_TRUE(found);
  EXPECT_EQ(hops, 0u);  // level 0 = direct subscription
}

TEST_F(PeerConnectionTest, RTTUpdate) {
  peer_update_rtt(peer, 15.0);
  EXPECT_DOUBLE_EQ(peer->rtt_ewma, 15.0);
  peer_update_rtt(peer, 25.0);
  EXPECT_NEAR(peer->rtt_ewma, 16.0, 0.01);  // EWMA: 0.1 * 25 + 0.9 * 15 = 16.0
}

TEST_F(PeerConnectionTest, MetricsSnapshot) {
  peer_hebbian_update(peer, 0.5f);
  peer_update_rtt(peer, 20.0);
  peer_metrics_snapshot_t snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  peer_get_metrics(peer, &snapshot);
  EXPECT_FLOAT_EQ(snapshot.hebbian_weight, 0.6f);
  EXPECT_DOUBLE_EQ(snapshot.rtt_ewma_ms, 20.0);
  EXPECT_TRUE(snapshot.connected);
}
```

- [ ] **Step 5: Build and run peer connection tests**

Run: `cd build && cmake --build . --target testliboffs 2>&1 | tail -20 && ./testliboffs --gtest_filter="PeerConnection*" 2>&1`
Expected: All PeerConnection tests PASS. Note: `peer_connection_create` with NULL network will work for the basic struct setup, but `actor_init` requires a valid pool pointer. We'll need to either pass a pool or adjust the test setup. If tests fail due to NULL pool, create a minimal scheduler pool in the test fixture.

- [ ] **Step 6: Fix any test failures and commit**

```bash
git add src/Network/peer_connection.h src/Network/peer_connection.c src/Actor/message.h test/test_network.cpp
git commit -m "feat: add peer connection actor with EABF, Hebbian, metrics"
```

---

### Task 4: Connection Manager

**Files:**
- Create: `src/Network/connection_manager.h`
- Create: `src/Network/connection_manager.c`
- Modify: `test/test_network.cpp`

The connection manager is a plain data structure — no actor, no message queue. Network calls its functions directly.

- [ ] **Step 1: Write connection_manager.h**

Create `src/Network/connection_manager.h`:

```c
#ifndef OFFS_CONNECTION_MANAGER_H
#define OFFS_CONNECTION_MANAGER_H

#include "peer_connection.h"
#include "hebbian_config.h"
#include "node_id.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct connection_manager_t {
  peer_connection_t** peers;
  size_t peer_count;
  size_t peer_capacity;
  hebbian_config_t hebbian;
  size_t max_connections;
} connection_manager_t;

// Lifecycle
void connection_manager_init(connection_manager_t* mgr, size_t initial_capacity,
                             const hebbian_config_t* hebbian_config);
void connection_manager_deinit(connection_manager_t* mgr);

// Peer management
peer_connection_t* connection_manager_add(connection_manager_t* mgr,
                                          network_t* network,
                                          const node_id_t* remote_id,
                                          const struct sockaddr_storage* peer_addr);
int connection_manager_remove(connection_manager_t* mgr, const node_id_t* remote_id);
peer_connection_t* connection_manager_lookup(const connection_manager_t* mgr,
                                             const node_id_t* remote_id);

// Gravity well search: find peers whose EABF matches topic at lowest level
// Returns peers sorted by gravity well strength (lowest level = strongest)
// out_count is set to the number of matching peers
// Caller must free the returned array
peer_connection_t** connection_manager_get_peers_for_topic(
    const connection_manager_t* mgr,
    const uint8_t* topic, size_t topic_len,
    size_t* out_count);

// Hebbian decay: subtract hebbian.decay_rate from all peers,
// remove peers below hebbian.drop_threshold
// Returns the number of peers removed
size_t connection_manager_decay_tick(connection_manager_t* mgr);

// Metrics: collect snapshots from all peers into caller-provided array
// Returns the number of peers written (min of peer_count and max_count)
size_t connection_manager_collect_metrics(const connection_manager_t* mgr,
                                          peer_metrics_snapshot_t* snapshots,
                                          size_t max_count);

#endif // OFFS_CONNECTION_MANAGER_H
```

- [ ] **Step 2: Write connection_manager.c**

Create `src/Network/connection_manager.c`:

```c
#include "connection_manager.h"
#include "network.h"
#include "../Util/allocator.h"
#include <string.h>

void connection_manager_init(connection_manager_t* mgr, size_t initial_capacity,
                             const hebbian_config_t* hebbian_config) {
  if (mgr == NULL) return;
  if (initial_capacity == 0) initial_capacity = 16;
  mgr->peers = get_clear_memory(initial_capacity * sizeof(peer_connection_t*));
  mgr->peer_count = 0;
  mgr->peer_capacity = initial_capacity;
  if (hebbian_config != NULL) {
    memcpy(&mgr->hebbian, hebbian_config, sizeof(hebbian_config_t));
  } else {
    hebbian_config_init(&mgr->hebbian);
  }
  mgr->max_connections = 128;  // reasonable default
}

void connection_manager_deinit(connection_manager_t* mgr) {
  if (mgr == NULL) return;
  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_destroy(mgr->peers[index]);
  }
  free(mgr->peers);
  mgr->peers = NULL;
  mgr->peer_count = 0;
  mgr->peer_capacity = 0;
}

peer_connection_t* connection_manager_add(connection_manager_t* mgr,
                                           network_t* network,
                                           const node_id_t* remote_id,
                                           const struct sockaddr_storage* peer_addr) {
  if (mgr == NULL || remote_id == NULL) return NULL;
  // Check if peer already exists
  peer_connection_t* existing = connection_manager_lookup(mgr, remote_id);
  if (existing != NULL) return existing;
  // Check capacity
  if (mgr->peer_count >= mgr->max_connections) return NULL;
  // Grow array if needed
  if (mgr->peer_count >= mgr->peer_capacity) {
    size_t new_capacity = mgr->peer_capacity * 2;
    peer_connection_t** new_peers = get_clear_memory(new_capacity * sizeof(peer_connection_t*));
    if (new_peers == NULL) return NULL;
    memcpy(new_peers, mgr->peers, mgr->peer_count * sizeof(peer_connection_t*));
    free(mgr->peers);
    mgr->peers = new_peers;
    mgr->peer_capacity = new_capacity;
  }

  peer_connection_t* peer = peer_connection_create(network, remote_id, peer_addr,
                                                    mgr->hebbian.initial_weight);
  if (peer == NULL) return NULL;

  mgr->peers[mgr->peer_count] = peer;
  mgr->peer_count++;
  return peer;
}

int connection_manager_remove(connection_manager_t* mgr, const node_id_t* remote_id) {
  if (mgr == NULL || remote_id == NULL) return -1;
  for (size_t index = 0; index < mgr->peer_count; index++) {
    if (node_id_equals(&mgr->peers[index]->remote_node_id, remote_id)) {
      peer_connection_destroy(mgr->peers[index]);
      // Compact
      for (size_t shift = index; shift < mgr->peer_count - 1; shift++) {
        mgr->peers[shift] = mgr->peers[shift + 1];
      }
      mgr->peer_count--;
      return 0;
    }
  }
  return -1;
}

peer_connection_t* connection_manager_lookup(const connection_manager_t* mgr,
                                              const node_id_t* remote_id) {
  if (mgr == NULL || remote_id == NULL) return NULL;
  for (size_t index = 0; index < mgr->peer_count; index++) {
    if (node_id_equals(&mgr->peers[index]->remote_node_id, remote_id)) {
      return mgr->peers[index];
    }
  }
  return NULL;
}

peer_connection_t** connection_manager_get_peers_for_topic(
    const connection_manager_t* mgr,
    const uint8_t* topic, size_t topic_len,
    size_t* out_count) {
  if (mgr == NULL || topic == NULL || out_count == NULL) {
    if (out_count) *out_count = 0;
    return NULL;
  }

  // Collect matching peers with their minimum hop level
  typedef struct {
    peer_connection_t* peer;
    uint32_t min_level;
  } peer_match_t;

  peer_match_t* matches = get_clear_memory(mgr->peer_count * sizeof(peer_match_t));
  size_t match_count = 0;

  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_t* peer = mgr->peers[index];
    if (!peer->connected) continue;
    uint32_t hops = 0;
    if (peer_eabf_check(peer, topic, topic_len, &hops)) {
      matches[match_count].peer = peer;
      matches[match_count].min_level = hops;
      match_count++;
    }
  }

  if (match_count == 0) {
    free(matches);
    *out_count = 0;
    return NULL;
  }

  // Sort by min_level (strongest gravity well first)
  for (size_t outer = 0; outer < match_count - 1; outer++) {
    for (size_t inner = outer + 1; inner < match_count; inner++) {
      if (matches[inner].min_level < matches[outer].min_level) {
        peer_match_t temp = matches[outer];
        matches[outer] = matches[inner];
        matches[inner] = temp;
      }
    }
  }

  // Extract peer pointers
  peer_connection_t** result = get_clear_memory(match_count * sizeof(peer_connection_t*));
  for (size_t index = 0; index < match_count; index++) {
    result[index] = matches[index].peer;
  }
  free(matches);
  *out_count = match_count;
  return result;
}

size_t connection_manager_decay_tick(connection_manager_t* mgr) {
  if (mgr == NULL) return 0;
  size_t removed = 0;
  size_t index = 0;
  while (index < mgr->peer_count) {
    peer_hebbian_decay(mgr->peers[index], mgr->hebbian.decay_rate);
    if (mgr->peers[index]->hebbian_weight < mgr->hebbian.drop_threshold) {
      // Remove peer below threshold
      peer_connection_destroy(mgr->peers[index]);
      for (size_t shift = index; shift < mgr->peer_count - 1; shift++) {
        mgr->peers[shift] = mgr->peers[shift + 1];
      }
      mgr->peer_count--;
      removed++;
      // Don't increment index — next element shifted into current position
    } else {
      index++;
    }
  }
  return removed;
}

size_t connection_manager_collect_metrics(const connection_manager_t* mgr,
                                          peer_metrics_snapshot_t* snapshots,
                                          size_t max_count) {
  if (mgr == NULL || snapshots == NULL) return 0;
  size_t count = mgr->peer_count < max_count ? mgr->peer_count : max_count;
  for (size_t index = 0; index < count; index++) {
    peer_get_metrics(mgr->peers[index], &snapshots[index]);
  }
  return count;
}
```

- [ ] **Step 3: Write failing tests for connection manager**

Add to `test/test_network.cpp`:

```cpp
class ConnectionManagerTest : public ::testing::Test {
protected:
  connection_manager_t mgr;
  hebbian_config_t config;
  void SetUp() override {
    hebbian_config_init(&config);
    connection_manager_init(&mgr, 4, &config);
  }
  void TearDown() override {
    connection_manager_deinit(&mgr);
  }
};

TEST_F(ConnectionManagerTest, InitDeinit) {
  EXPECT_NE(mgr.peers, nullptr);
  EXPECT_EQ(mgr.peer_count, 0u);
}

TEST_F(ConnectionManagerTest, AddLookupRemove) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  peer_connection_t* peer = connection_manager_add(&mgr, NULL, &id1, NULL);
  ASSERT_NE(peer, nullptr);
  EXPECT_EQ(mgr.peer_count, 1u);

  peer_connection_t* found = connection_manager_lookup(&mgr, &id1);
  EXPECT_EQ(found, peer);

  int result = connection_manager_remove(&mgr, &id1);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(mgr.peer_count, 0u);
}

TEST_F(ConnectionManagerTest, GravityWellSearch) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  node_id_t id2 = {};
  memset(id2.hash, 0xBB, NODE_ID_HASH_SIZE);

  peer_connection_t* peer1 = connection_manager_add(&mgr, NULL, &id1, NULL);
  peer_connection_t* peer2 = connection_manager_add(&mgr, NULL, &id2, NULL);

  // Subscribe peer1 to a topic at level 0
  uint8_t topic[32];
  memset(topic, 0xDD, 32);
  peer_eabf_subscribe(peer1, topic, 32);

  size_t match_count = 0;
  peer_connection_t** matches = connection_manager_get_peers_for_topic(&mgr, topic, 32, &match_count);
  ASSERT_NE(matches, nullptr);
  EXPECT_EQ(match_count, 1u);
  EXPECT_EQ(matches[0], peer1);
  free(matches);
}

TEST_F(ConnectionManagerTest, DecayTick) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  peer_connection_t* peer = connection_manager_add(&mgr, NULL, &id1, NULL);
  ASSERT_NE(peer, nullptr);
  // Initial weight is 0.1, decay_rate is 0.001, drop_threshold is 0.01
  // After many decay ticks, weight should drop below threshold
  for (int index = 0; index < 99; index++) {
    size_t removed = connection_manager_decay_tick(&mgr);
    // None removed until weight drops below 0.01
    if (removed > 0) break;
  }
  // After enough ticks, peer should be removed
  // With decay_rate=0.001 per tick, after 90 ticks weight = 0.1 - 90*0.001 = 0.01
  // After 91 ticks, weight = 0.009 < 0.01, removed
  EXPECT_EQ(mgr.peer_count, 0u);
}
```

- [ ] **Step 4: Build and run connection manager tests**

Run: `cd build && cmake --build . --target testliboffs 2>&1 | tail -20 && ./testliboffs --gtest_filter="ConnectionManager*" 2>&1`
Expected: All ConnectionManager tests PASS. Note: `peer_connection_create` with NULL network will need handling — if tests fail, we'll need a test pool or mock.

- [ ] **Step 5: Fix any test failures and commit**

```bash
git add src/Network/connection_manager.h src/Network/connection_manager.c test/test_network.cpp
git commit -m "feat: add connection manager data structure with gravity-well search"
```

---

### Task 5: Topology Metrics Server

**Files:**
- Create: `src/Network/topology_metrics.h`
- Create: `src/Network/topology_metrics.c`
- Modify: `src/Network/authority.h`
- Modify: `test/test_network.cpp`

The topology metrics server is an optional actor that receives metrics pushes from the network actor and stores aggregated snapshots.

- [ ] **Step 1: Write topology_metrics.h**

Create `src/Network/topology_metrics.h`:

```c
#ifndef OFFS_TOPOLOGY_METRICS_H
#define OFFS_TOPOLOGY_METRICS_H

#include "../Actor/actor.h"
#include "peer_connection.h"
#include "ring_set.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct ring_topology_entry_t {
  node_id_t node_id;
  uint32_t ring_level;
  double rtt_ms;
  float capacity;
  bool is_active_connection;
} ring_topology_entry_t;

typedef struct topology_metrics_t {
  actor_t actor;

  // Per-peer snapshots
  peer_metrics_snapshot_t* peer_snapshots;
  size_t peer_snapshot_count;
  size_t peer_snapshot_capacity;

  // Ring topology snapshots
  ring_topology_entry_t* ring_entries;
  size_t ring_entry_count;
  size_t ring_entry_capacity;

  // Network-level aggregates
  size_t total_connections;
  float avg_hebbian_weight;
  uint64_t total_rpc_calls[PEER_RPC_TYPE_COUNT];

  // Configuration
  uint64_t collect_interval_ms;
} topology_metrics_t;

// Lifecycle
topology_metrics_t* topology_metrics_create(scheduler_pool_t* pool);
void topology_metrics_destroy(topology_metrics_t* metrics);
void topology_metrics_dispatch(void* state, message_t* msg);

// Push metrics from network actor
void topology_metrics_update_peers(topology_metrics_t* metrics,
                                    const peer_metrics_snapshot_t* snapshots,
                                    size_t count);
void topology_metrics_update_rings(topology_metrics_t* metrics,
                                    const ring_topology_entry_t* entries,
                                    size_t count);

#endif // OFFS_TOPOLOGY_METRICS_H
```

- [ ] **Step 2: Write topology_metrics.c**

Create `src/Network/topology_metrics.c`:

```c
#include "topology_metrics.h"
#include "../Util/allocator.h"
#include <string.h>

topology_metrics_t* topology_metrics_create(scheduler_pool_t* pool) {
  topology_metrics_t* metrics = get_clear_memory(sizeof(topology_metrics_t));
  if (metrics == NULL) return NULL;

  metrics->peer_snapshot_capacity = 16;
  metrics->peer_snapshots = get_clear_memory(metrics->peer_snapshot_capacity * sizeof(peer_metrics_snapshot_t));
  metrics->peer_snapshot_count = 0;

  metrics->ring_entry_capacity = 64;
  metrics->ring_entries = get_clear_memory(metrics->ring_entry_capacity * sizeof(ring_topology_entry_t));
  metrics->ring_entry_count = 0;

  metrics->collect_interval_ms = 300000;  // 5 minutes
  metrics->total_connections = 0;
  metrics->avg_hebbian_weight = 0.0f;
  memset(metrics->total_rpc_calls, 0, sizeof(metrics->total_rpc_calls));

  actor_init(&metrics->actor, metrics, topology_metrics_dispatch, pool);
  return metrics;
}

void topology_metrics_destroy(topology_metrics_t* metrics) {
  if (metrics == NULL) return;
  free(metrics->peer_snapshots);
  free(metrics->ring_entries);
  actor_destroy(&metrics->actor);
  free(metrics);
}

void topology_metrics_dispatch(void* state, message_t* msg) {
  if (state == NULL || msg == NULL) return;
  topology_metrics_t* metrics = (topology_metrics_t*)state;

  switch (msg->type) {
    case TOPOLOGY_METRICS_UPDATE: {
      // Metrics update is handled via the direct function calls below
      // This message type is for future async updates
      break;
    }
    default:
      break;
  }
  if (msg->payload != NULL && msg->payload_destroy != NULL) {
    msg->payload_destroy(msg->payload);
  }
}

void topology_metrics_update_peers(topology_metrics_t* metrics,
                                    const peer_metrics_snapshot_t* snapshots,
                                    size_t count) {
  if (metrics == NULL || snapshots == NULL) return;
  // Grow if needed
  if (count > metrics->peer_snapshot_capacity) {
    size_t new_capacity = count * 2;
    peer_metrics_snapshot_t* new_snapshots = get_clear_memory(new_capacity * sizeof(peer_metrics_snapshot_t));
    if (new_snapshots == NULL) return;
    free(metrics->peer_snapshots);
    metrics->peer_snapshots = new_snapshots;
    metrics->peer_snapshot_capacity = new_capacity;
  }
  memcpy(metrics->peer_snapshots, snapshots, count * sizeof(peer_metrics_snapshot_t));
  metrics->peer_snapshot_count = count;

  // Compute aggregates
  metrics->total_connections = 0;
  float total_weight = 0.0f;
  memset(metrics->total_rpc_calls, 0, sizeof(metrics->total_rpc_calls));
  for (size_t index = 0; index < count; index++) {
    if (snapshots[index].connected) {
      metrics->total_connections++;
    }
    total_weight += snapshots[index].hebbian_weight;
    for (size_t rpc = 0; rpc < PEER_RPC_TYPE_COUNT; rpc++) {
      metrics->total_rpc_calls[rpc] += snapshots[index].rpc_count[rpc];
    }
  }
  metrics->avg_hebbian_weight = count > 0 ? total_weight / count : 0.0f;
}

void topology_metrics_update_rings(topology_metrics_t* metrics,
                                    const ring_topology_entry_t* entries,
                                    size_t count) {
  if (metrics == NULL || entries == NULL) return;
  // Grow if needed
  if (count > metrics->ring_entry_capacity) {
    size_t new_capacity = count * 2;
    ring_topology_entry_t* new_entries = get_clear_memory(new_capacity * sizeof(ring_topology_entry_t));
    if (new_entries == NULL) return;
    free(metrics->ring_entries);
    metrics->ring_entries = new_entries;
    metrics->ring_entry_capacity = new_capacity;
  }
  memcpy(metrics->ring_entries, entries, count * sizeof(ring_topology_entry_t));
  metrics->ring_entry_count = count;
}
```

- [ ] **Step 3: Add `topology_metrics_t*` field to `authority_t`**

In `src/Network/authority.h`, add after the `max_inflight` field:

```c
  struct topology_metrics_t* metrics_server;  // NULL if not configured
```

- [ ] **Step 4: Write failing tests for topology metrics**

Add to `test/test_network.cpp`:

```cpp
class TopologyMetricsTest : public ::testing::Test {
protected:
  topology_metrics_t* metrics;
  void SetUp() override {
    metrics = topology_metrics_create(NULL);
  }
  void TearDown() override {
    topology_metrics_destroy(metrics);
  }
};

TEST_F(TopologyMetricsTest, CreateDestroy) {
  ASSERT_NE(metrics, nullptr);
  EXPECT_EQ(metrics->peer_snapshot_count, 0u);
  EXPECT_EQ(metrics->ring_entry_count, 0u);
}

TEST_F(TopologyMetricsTest, UpdatePeers) {
  peer_metrics_snapshot_t snapshots[2];
  memset(snapshots, 0, sizeof(snapshots));
  memset(snapshots[0].node_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  snapshots[0].hebbian_weight = 0.5f;
  snapshots[0].rtt_ewma_ms = 20.0;
  snapshots[0].connected = true;
  memset(snapshots[1].node_id.hash, 0xBB, NODE_ID_HASH_SIZE);
  snapshots[1].hebbian_weight = 0.8f;
  snapshots[1].rtt_ewma_ms = 30.0;
  snapshots[1].connected = true;

  topology_metrics_update_peers(metrics, snapshots, 2);
  EXPECT_EQ(metrics->peer_snapshot_count, 2u);
  EXPECT_EQ(metrics->total_connections, 2u);
  EXPECT_FLOAT_EQ(metrics->avg_hebbian_weight, 0.65f);
}
```

- [ ] **Step 5: Build and run topology metrics tests**

Run: `cd build && cmake --build . --target testliboffs 2>&1 | tail -20 && ./testliboffs --gtest_filter="TopologyMetrics*" 2>&1`
Expected: All TopologyMetrics tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/Network/topology_metrics.h src/Network/topology_metrics.c src/Network/authority.h test/test_network.cpp
git commit -m "feat: add topology metrics server for per-peer and ring topology snapshots"
```

---

### Task 6: Integrate Connection Manager and Timers into Network

**Files:**
- Modify: `src/Network/network.h`
- Modify: `src/Network/network.c`

Add the connection_manager, Hebbian decay timer, EABF tick timer, and metrics push timer to network_t. Wire up the timer callbacks.

- [ ] **Step 1: Update network.h**

Add to `network_t` struct after `rate_limit_table_t rate_limits;`:

```c
  connection_manager_t conn_mgr;       // peer connection table + config
  uint64_t hebbian_decay_timer_id;    // timer ID for Hebbian decay ticks
  uint64_t eabf_tick_timer_id;        // timer ID for EABF timing wheel advance
  uint64_t metrics_push_timer_id;     // timer ID for metrics push to topology server
```

Add includes for new headers:

```c
#include "connection_manager.h"
#include "timing_wheel.h"
#include "topology_metrics.h"
```

- [ ] **Step 2: Update network.c**

In `network_create`, after initializing `rate_limits`:
- Call `connection_manager_init(&network->conn_mgr, 16, NULL);` (uses default Hebbian config)
- Set timer IDs to 0

In `network_destroy`, before freeing:
- Call `connection_manager_deinit(&network->conn_mgr);`
- Cancel all three timers if non-zero

In `network_dispatch`, add cases for:
- `NETWORK_QUIC_CONNECTED` — call `connection_manager_add` to create a new peer_connection
- `NETWORK_QUIC_DISCONNECTED` — call `connection_manager_remove` to destroy the peer
- Timer callback messages for Hebbian decay, EABF tick, and metrics push

- [ ] **Step 3: Build and verify no regressions**

Run: `cd build && cmake --build . --target testliboffs 2>&1 | tail -20 && ./testliboffs 2>&1 | tail -20`
Expected: All existing tests pass, no new failures.

- [ ] **Step 4: Commit**

```bash
git add src/Network/network.h src/Network/network.c
git commit -m "feat: integrate connection manager, Hebbian decay, EABF tick, and metrics push timers into network_t"
```

---

### Task 7: Wire Up EABF Gravity-Well Routing in FindBlock

**Files:**
- Modify: `src/Network/find_block.c`

Replace the existing EABF gravity-well search with `connection_manager_get_peers_for_topic`. When a FindBlock arrives and we don't have the block locally, use the gravity-well search to find the best peer to forward to.

- [ ] **Step 1: Update find_block.c to use connection_manager**

Add `#include "connection_manager.h"` and `#include "peer_connection.h"`.

In `find_block_execute`, after the local cache check misses:
1. Call `connection_manager_get_peers_for_topic(&network->conn_mgr, msg->block_hash, 32, &match_count)` to get peers sorted by gravity well strength
2. Forward FindBlock to the first matching peer that isn't in the visited bloom
3. If no gravity well match, fall back to the existing roulette wheel selection
4. Free the matches array when done

- [ ] **Step 2: Build and run tests**

Run: `cd build && cmake --build . --target testliboffs && ./testliboffs --gtest_filter="*Find*" 2>&1`
Expected: Existing FindBlock tests still pass.

- [ ] **Step 3: Commit**

```bash
git add src/Network/find_block.c
git commit -m "feat: use gravity-well EABF search in FindBlock routing"
```

---

### Task 8: Wire Up StoreBlock Recall and Gravity-Well Forwarding

**Files:**
- Modify: `src/Network/store_block.c`

When a StoreBlock is accepted, use the gravity-well search to find peers whose EABFs match the block hash. Forward to peers at level 0 (recall) and at higher levels (directed walk).

- [ ] **Step 1: Update store_block.c to use connection_manager**

Add `#include "connection_manager.h"` and `#include "peer_connection.h"`.

In `store_block_execute`:
1. After local storage succeeds, call `connection_manager_get_peers_for_topic(&network->conn_mgr, msg->block_hash, 32, &match_count)`
2. For peers with EABF match at level 0 (peer wanted this block): send StoreBlock with `reason=RECALL`
3. For peers with EABF match at level 1+ (directed walk): forward StoreBlock normally
4. Add the block hash to the peer's EABF at the appropriate level via `peer_eabf_subscribe` or direct EBF insert with TTL

Also implement the recall-on-acquisition logic:
- Add a function `network_recall_block` that checks all peers' EABFs at level 0 for a given block hash and pushes the block to matching peers.

- [ ] **Step 2: Build and run tests**

Run: `cd build && cmake --build . --target testliboffs && ./testliboffs --gtest_filter="*Store*" 2>&1`
Expected: Existing StoreBlock tests still pass.

- [ ] **Step 3: Commit**

```bash
git add src/Network/store_block.c
git commit -m "feat: use gravity-well EABF search in StoreBlock forwarding and recall"
```

---

### Task 9: EABF Population on Failed FindBlock and Successful StoreBlock

**Files:**
- Modify: `src/Network/network.c`

Implement the two EABF population paths from the design spec:

1. **Failed FindBlock**: When a FindBlock arrives and we don't have the block, insert the block hash into the originating peer's EABF at level 0, with TTL.
2. **Successful StoreBlock**: As the response travels back along the path, each node records the block hash at the appropriate level in the EABF for the next-hop peer.

- [ ] **Step 1: Add EABF population to network_handle_find_block**

In the FindBlock handler, when we forward to another peer (or can't satisfy locally):
1. Look up the originating peer via `connection_manager_lookup`
2. Call `peer_eabf_subscribe` or direct `elastic_bloom_filter_add` with TTL
3. Record the block hash in the peer's EABF at level 0

- [ ] **Step 2: Add EABF population to network_handle_store_block_response**

In the StoreBlock response handler:
1. Parse the path from the response
2. For each node in the path (starting from the holder), insert the block hash into the appropriate EABF at the appropriate level based on distance from holder
3. Add TTL entries to the timing wheel

- [ ] **Step 3: Build and run tests**

Run: `cd build && cmake --build . --target testliboffs 2>&1 | tail -20 && ./testliboffs 2>&1 | tail -20`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/Network/network.c
git commit -m "feat: populate EABF on failed FindBlock and successful StoreBlock"
```

---

### Task 10: De-Wonk and Integration Test

**Files:**
- All files modified in Tasks 1-9
- `test/test_network.cpp`

Run the de-wonk audit on all new and modified files. Check for memory leaks, uninitialized fields, missing NULL checks, and incorrect EABF level calculations. Then run the full test suite under valgrind (with `-gdwarf-4` flag if needed per valgrind compatibility notes).

- [ ] **Step 1: De-wonk audit — read all new files**

Read all files created/modified in Tasks 1-9. Check for:
- Unimplemented/stubbed functions
- Memory leaks (malloc without free, get_clear_memory without free)
- NULL pointer dereferences
- Off-by-one errors in EABF level calculations
- Missing error handling on fallible operations

- [ ] **Step 2: Fix all CRITICAL and HIGH issues found**

- [ ] **Step 3: Run full test suite**

Run: `cd build && cmake --build . --target testliboffs && ./testliboffs 2>&1`
Expected: All tests pass.

- [ ] **Step 4: Run valgrind leak check**

Run: `cd build && cmake -DCMAKE_C_FLAGS="-gdwarf-4" -DCMAKE_CXX_FLAGS="-gdwarf-4" .. && cmake --build . --target testliboffs && valgrind --leak-check=full --error-exitcode=1 ./testliboffs --gtest_filter="*TimingWheel*:*HebbianConfig*:*PeerConnection*:*ConnectionManager*:*TopologyMetrics*" 2>&1 | tail -40`
Expected: No leaks are possible, no invalid reads/writes.

- [ ] **Step 5: Commit fixes**

```bash
git add -A
git commit -m "fix: de-wonk audit — memory safety, NULL checks, and EABF level corrections"
```