# Network Connection Management, EABF Routing, and Metrics Server

Date: 2026-05-15
Status: Draft

## Overview

This design covers three interconnected features for the OFFS network layer:

1. **Peer connection actors** — each QUIC connection is its own actor with RPC send API, Hebbian weight, per-RPC counters, and EABF routing state
2. **Connection manager** — a data structure inside `network_t` holding peer actors, with network driving lifecycle, Hebbian decay, and metrics collection
3. **Topology metrics server** — optional component of `authority_t` that collects per-peer metrics and Meridian ring topology for observability and training

The EABF is the routing backbone. It is purely local state — never transmitted between nodes — populated as a side effect of FindBlock failures and StoreBlock successes, and decayed by a timing wheel.

---

## 1. Peer Connection Actor

Each active QUIC connection is represented by a `peer_connection_t` actor. The peer actor owns its connection state, Hebbian weight, per-RPC metrics, and its outgoing EABF.

### Structure

```c
typedef struct peer_connection_t {
    actor_t actor;                         // own actor for message dispatch
    node_id_t remote_node_id;              // who we're connected to
    HQUIC stream;                          // QUIC stream handle (or stub ref)

    // EABF_{self → peer}: what blocks are reachable via this peer
    attenuated_bloom_filter_t* eabf;
    timing_wheel_t* eabf_wheel;            // TTL expiry for EABF entries

    // Hebbian weight — drives keep-alive decisions
    float hebbian_weight;
    float hebbian_initial_weight;          // configurable starting weight

    // Per-RPC counters (for metrics and weight training)
    uint64_t rpc_count[WIRE_MSG_TYPE_COUNT];
    uint64_t rpc_success[WIRE_MSG_TYPE_COUNT];
    uint64_t rpc_failure[WIRE_MSG_TYPE_COUNT];

    // Meridian latency measurements
    double last_rtt_ms;
    double rtt_ewma;                        // exponential weighted moving average

    // Connection state
    bool connected;
    int64_t connected_at_ms;

    // Back-reference
    network_t* network;
} peer_connection_t;
```

### Messages Handled

| Message | Behavior |
|---------|----------|
| `PEER_SEND_FIND_BLOCK` | Encode wire message, send over QUIC stream, increment `rpc_count` |
| `PEER_SEND_STORE_BLOCK` | Encode wire message, send over QUIC stream, increment `rpc_count` |
| `PEER_SEND_PING_CAPACITY` | Encode wire message, send over QUIC stream, increment `rpc_count` |
| `PEER_SEND_SEEKING_BLOCKS` | Encode wire message, send over QUIC stream, increment `rpc_count` |
| `PEER_SEND_PING_BLOCK` | Encode wire message, send over QUIC stream, increment `rpc_count` |
| `PEER_UPDATE_HEBBIAN` | Apply weight delta (positive on success, negative on failure/decay) |
| `PEER_GET_METRICS` | Return snapshot of rpc_count, weight, rtt_ewma |
| `PEER_CLOSE` | Graceful shutdown of QUIC stream, destroy actor |
| `PEER_EABF_TICK` | Advance timing wheel, delete expired EABF entries |

On any `PEER_SEND_*` message, the peer actor:
1. Encodes the wire message via the appropriate `wire_*_encode` function
2. Sends the CBOR-encoded payload over the QUIC stream
3. Increments `rpc_count[msg_type]`

When the network receives a successful RPC response for a message sent through this peer, the network sends `PEER_UPDATE_HEBBIAN` with a positive delta. The connection manager's Hebbian decay tick (driven by network actor timer) sends negative deltas.

---

## 2. Connection Manager

The connection manager is a **data structure** inside `network_t` — not an actor. It holds the flat array of peer connection actors and configuration for Hebbian weights. The network actor operates on it directly.

### Structure

```c
typedef struct connection_manager_t {
    peer_connection_t** peers;              // flat array of peer actors
    size_t peer_count;
    size_t peer_capacity;
    hebbian_config_t hebbian;             // Hebbian weight configuration
    size_t max_connections;                // upper bound on peer count
} connection_manager_t;
```

### Operations (called by network actor)

| Function | Description |
|----------|-------------|
| `connection_manager_lookup` | Find peer by `node_id_t`, return `peer_connection_t*` |
| `connection_manager_add` | Create new peer actor, add to table, set initial Hebbian weight from `hebbian.initial_weight` |
| `connection_manager_remove` | Close peer, destroy actor, remove from table |
| `connection_manager_decay_tick` | Iterate all peers, apply `hebbian.decay_rate`, close peers below `hebbian.drop_threshold` |
| `connection_manager_collect_metrics` | Iterate all peers, collect per-peer metrics into snapshot |
| `connection_manager_get_peers_for_topic` | Iterate all peers, check EABFs for topic, return matching peers sorted by gravity well strength |

---

## 3. Network Integration

### New fields in `network_t`

```c
typedef struct network_t {
    actor_t actor;                         // existing dispatch actor
    connection_manager_t* conn_mgr;        // peer table + config
    pd_watcher_t* hebbian_timer;           // decay tick timer
    pd_watcher_t* metrics_timer;           // metrics push timer
    pd_watcher_t* eabf_tick_timer;         // EABF timing wheel tick
    // ... existing ring_table, authority, etc.
} network_t;
```

### Message Processing Flow

**Inbound FIND_BLOCK (we don't have the block):**

1. QUIC listener sends `NETWORK_QUIC_DATA` to network actor
2. Network dispatches to `network_handle_find_block`
3. Handler checks local block cache — miss
4. Handler calls `connection_manager_get_peers_for_topic(conn_mgr, block_hash)` — iterates all peers' EABFs, returns matches sorted by lowest matching level
5. For each match (strongest gravity well first), network sends `PEER_SEND_FIND_BLOCK` to that peer actor
6. On success response, network sends `PEER_UPDATE_HEBBIAN` with positive delta to the originating peer

**Inbound STORE_BLOCK (we accept it):**

1. Network dispatches to `network_handle_store_block`
2. Handler checks acceptance probability, stores locally
3. Handler calls `connection_manager_get_peers_for_topic` to find peers whose EABFs match this block hash
4. For each match at level 0 (peer wanted this block), send `PEER_SEND_STORE_BLOCK` with `reason=RECALL`

**Recall on block acquisition:**

When any mechanism causes this node to acquire a block (inhale, exhale push, local PUT):
1. Network scans all peers' EABFs at level 0 for the block hash
2. For each match: push the block to that peer via `PEER_SEND_STORE_BLOCK`
3. Delete the EABF level 0 entry (promissory note fulfilled)
4. Apply amplified Hebbian reinforcement: `w_{peer→self} += 2.0 * gamma`

### Timers on Network Actor

| Timer | Interval | Action |
|-------|----------|--------|
| Hebbian decay | `hebbian_decay_tick_ms` (default 60s) | Iterate all peers, subtract `hebbian_decay_rate` from weight, close peers below threshold |
| EABF timing wheel | `eabf_tick_ms` (default 60s) | Send `PEER_EABF_TICK` to all peers (advance timing wheel, expire entries) |
| Metrics push | `metrics_push_ms` (default 300s) | Collect metrics from all peers + ring table, push to `authority_t->metrics_server` |

---

## 4. EABF Semantics

The EABF is the routing backbone of OFFS. It is **purely local state** — never transmitted between nodes. It is populated as a side effect of message handling and decayed by a timing wheel.

### What It Is

The EABF combines an elastic bloom filter (supports deletion and expansion via fingerprints) with attenuation (level N holds information about items N hops away). One EABF exists per outgoing connection. It answers: "Is block X reachable at distance L via this peer?"

### Structure

Each EABF is an `attenuated_bloom_filter_t` with `level_count` levels, where each level is an `elastic_bloom_filter_t`. A shared `timing_wheel_t` manages TTL expiry across all levels.

### Two Paths Populate the EABF

#### Path 1: Failed FindBlock (Retroactive)

When a FindBlock request arrives and this node cannot satisfy it:

```
A → B: FindBlock(block_X)
B: "I don't have block_X"

B inserts into EABF_{B→A}:
  Level 0 ← hash(block_X)
  Timing wheel slot ← now() + level_0_TTL
```

Meaning: "Peer A wants block_X. If I ever acquire it, A is 0 hops away — I can push it directly."

Re-insertion for the same block refreshes the TTL. Memory persists as long as the peer keeps asking.

#### Path 2: Successful StoreBlock (Proactive)

When a StoreBlock succeeds and the response travels back along the path, every node records where the block landed:

```
StoreBlock path: A → B → C → D (D accepted the block)

D (holder, distance 0):
  EABF_{D→C}.level[0] ← hash(block_X)    "C knows I have this"

C (forwarded to D, distance 1):
  EABF_{C→D}.level[1] ← hash(block_X)     "D has it, 1 hop beyond D"
  EABF_{C→B}.level[0] ← hash(block_X)     "B knows I routed this"

B (forwarded to C, distance 2):
  EABF_{B→C}.level[2] ← hash(block_X)     "reachable 2 hops beyond C"
  EABF_{B→A}.level[0] ← hash(block_X)     "A knows I routed this"

A (originator, distance 3):
  EABF_{A→B}.level[3] ← hash(block_X)     "reachable 3 hops beyond B"
```

General rule for a node at position `i` in the path (0-indexed) with holder at position `h`:

```
distance = h - i
EABF_{path[i] → path[i+1]}.level[distance - 1] ← hash(block_X)
```

### Gravity Well Search (FindBlock Routing)

When a FindBlock arrives at a node that doesn't have the block:

1. Check local index — if found, respond immediately
2. TTL check, visited set update
3. **Gravity well search**: check all peers' EABFs, level 0 first (closest), then level 1, etc. First match at the lowest level across all peers is the strongest gravity well. Forward to that peer.
4. No gravity well matched — fall back to weighted random walk (roulette wheel)

The critical property: level 0 hits are preferred over level 1, level 1 over level 2. Each hop brings the query closer to the holder.

### Recall on Block Acquisition

When a node acquires a block:

1. Check all peers' EABFs at level 0 for the block hash
2. For each match: push the block to that peer via StoreBlock (reason=RECALL)
3. Delete the EABF level 0 entry (promissory note fulfilled)
4. Amplified Hebbian reinforcement: `weight += 2.0 * base_reward`

### Timing Wheel Decay

Every entry in every level of every EABF has a TTL managed by the timing wheel. Higher levels get shorter TTLs because they're more volatile:

| Level | TTL | Rationale |
|-------|-----|-----------|
| 0 | 60 min | Direct knowledge — stable |
| 1 | 40 min | One hop away — moderately stable |
| 2 | 30 min | Two hops away — more volatile |
| 3 | 24 min | Three hops away — most volatile |

The timing wheel advances on `PEER_EABF_TICK`. Expired entries are deleted from their EABF level. Re-insertion refreshes TTL.

### What Each Level Means

| Level | Populated By | Meaning | Used For |
|-------|-------------|---------|----------|
| 0 | Failed FindBlock | "Peer wants this block" | Recall push on acquisition |
| 0 | Successful StoreBlock (forwardee is holder) | "Peer has this block" | FindBlock: direct hit, 0 more hops |
| 1 | StoreBlock (forwardee 1 hop from holder) | "Block is 1 hop beyond this peer" | FindBlock: 1 more hop |
| 2 | StoreBlock (forwardee 2 hops from holder) | "Block is 2 hops beyond this peer" | FindBlock: 2 more hops |
| 3 | StoreBlock (forwardee 3 hops from holder) | "Block is 3 hops beyond this peer" | FindBlock: 3 more hops |

---

## 5. Hebbian Weight Semantics

Hebbian weights drive connection keep-alive decisions. A peer with high weight is kept connected; a peer below the drop threshold is closed.

### Configuration

```c
typedef struct hebbian_config_t {
    float initial_weight;                  // weight for new connections (default 1.0)
    float drop_threshold;                 // close connection below this (default 0.1)
    float decay_rate;                     // subtracted per tick (default 0.05)
    uint64_t decay_tick_ms;               // ms between decay ticks (default 60000)
    float base_reward;                    // base positive delta per success (default 0.1)
    float rpc_multipliers[WIRE_MSG_TYPE_COUNT]; // per-type multiplier
} hebbian_config_t;
```

### Per-RPC Multipliers

| RPC Type | Multiplier | Rationale |
|----------|-----------|-----------|
| FIND_BLOCK response | 1.0 | Core utility — finding blocks is primary |
| STORE_BLOCK response | 1.5 | Storing costs bandwidth/storage |
| PING_BLOCK response | 0.8 | Useful but lighter |
| SEEKING_BLOCKS response | 0.5 | Informational |
| PING_CAPACITY response | 0.3 | Maintenance overhead |

### Weight Operations

| Event | Delta | Formula |
|-------|-------|--------|
| Successful RPC response | Positive | `base_reward * rpc_multipliers[type]` |
| Recall push (block acquisition) | Amplified | `2.0 * base_reward` |
| RPC failure/timeout | Negative | `-0.2` |
| Rate-limited by peer | Negative | `-0.1` |
| Periodic decay tick | Negative | `-decay_rate` |

### Lifecycle

- New connection starts at `initial_weight`
- Successful interactions increase weight
- Decay ticks decrease weight
- When weight < `drop_threshold`, connection manager closes the connection
- Multipliers are configurable via authority, not hardcoded — metrics server training data can inform tuning

---

## 6. Topology Metrics Server

The metrics server is an **optional component** of `authority_t`. Nodes that don't configure it skip metrics collection entirely.

### Structure

```c
typedef struct topology_metrics_t {
    actor_t actor;                         // own actor for receiving metrics
    authority_t* authority;

    // Configuration
    uint64_t collect_interval_ms;         // how often to request metrics from network
    char* bind_address;                   // HTTP/API bind address for external queries
    uint16_t bind_port;

    // Per-peer snapshots (populated by network pushes)
    struct peer_metrics_snapshot_t {
        node_id_t node_id;
        float hebbian_weight;
        double rtt_ewma_ms;
        uint64_t rpc_count[WIRE_MSG_TYPE_COUNT];
        uint64_t rpc_success[WIRE_MSG_TYPE_COUNT];
        uint64_t rpc_failure[WIRE_MSG_TYPE_COUNT];
        bool connected;
        int64_t connected_at_ms;
    }* peer_snapshots;
    size_t peer_snapshot_count;

    // Meridian ring topology (from network's ring table)
    struct ring_topology_snapshot_t {
        node_id_t node_id;
        uint32_t ring_level;
        double rtt_ms;
        float capacity;
        bool is_active_connection;
    }* ring_entries;
    size_t ring_entry_count;

    // Network-level aggregates
    size_t total_connections;
    float avg_hebbian_weight;
    uint64_t total_rpc_calls[WIRE_MSG_TYPE_COUNT];
} topology_metrics_t;
```

### Data Flow

1. Network actor's metrics timer fires
2. Network iterates `connection_manager.peers`, collects each peer's state into a snapshot
3. Network reads its `ring_table` entries into a ring topology snapshot
4. Network sends `TOPOLOGY_METRICS_UPDATE` message to `authority_t->metrics_server`
5. Metrics server stores the snapshot, aggregates, optionally exposes via HTTP API

### What It Tracks

- **Per-peer**: Hebbian weight, RTT EWMA, per-RPC call count/success/failure, connection duration
- **Per-RPC type**: aggregate call volume across all peers
- **Ring topology**: every node this node knows about, grouped by ring level, with active connections highlighted
- **Network-level**: total connections, average Hebbian weight

### What It's Used For

- **Observability**: dashboards showing network health, peer quality, load distribution
- **Training data**: Hebbian weight multipliers can be tuned based on observed RPC success/failure ratios
- **Topology visualization**: concentric rings with active connections highlighted, Hebbian weights as edge thickness
- **Future**: automated topology optimization (adjusting decay rates, drop thresholds, initial weights based on network-wide patterns)

### Authority Integration

```c
typedef struct authority_t {
    // ... existing fields ...
    topology_metrics_t* metrics_server;   // NULL if not configured
} authority_t;
```

If `metrics_server` is NULL, the network actor skips metrics pushes. No overhead.

---

## 7. Wire Protocol Changes

Minimal changes — the EABF is never transmitted, so no new wire messages for it.

### Existing (No Changes)

- Hebbian fields `weight` and `delta` already exist in `wire_ping_block_response_t`
- All existing RPC message types remain unchanged

### New Wire Message Types

None required. EABFs are populated by side effects of existing messages. Metrics are pushed internally to the topology server, not exchanged between peers.

### Future Consideration

A `WIRE_MSG_TYPE_EABF_SUBSCRIBE` / `WIRE_MSG_TYPE_EABF_UNSUBSCRIBE` may be useful for explicit subscription signaling between peers (to populate level 0 of the remote EABF directly), but this is not required for the initial implementation. The failed-FindBlock and successful-StoreBlock paths are sufficient.

---

## 8. Architecture Diagram

```
network_t
  ├── actor_t (dispatch + timers)
  │     ├── hebbian_timer → decay tick
  │     ├── eabf_tick_timer → timing wheel advance
  │     └── metrics_timer → push to topology server
  ├── connection_manager_t (data structure)
  │     ├── peer_connection_t (actor) → peer A
  │     │     ├── EABF_{self→A} + timing wheel
  │     │     ├── Hebbian weight, per-RPC counters
  │     │     └── RTT, QUIC stream
  │     ├── peer_connection_t (actor) → peer B
  │     │     ├── EABF_{self→B} + timing wheel
  │     │     └── ...
  │     └── ...
  ├── ring_table_t (Meridian membership — more peers than connections)
  └── block_cache reference (local lookups)

authority_t
  └── topology_metrics_t (actor, optional)
        ├── peer snapshots (from network push)
        ├── ring topology snapshots
        └── aggregate counters
```

---

## 9. Data Ownership Summary

| Component | Owns | Accesses |
|-----------|------|----------|
| `peer_connection_t` | EABF_{self→peer}, Hebbian weight, per-RPC counters, RTT, QUIC stream | Network (via direct ref from connection_manager) |
| `connection_manager_t` | Peer array, Hebbian config, RPC multipliers | Network (direct calls) |
| `network_t` | Ring table, block cache ref, timers, local routing logic | Connection manager (direct), peers (via actor_send), metrics server (via actor_send) |
| `topology_metrics_t` | Aggregated snapshots, ring topology | Authority (configured at startup) |