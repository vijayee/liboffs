# Meridian Gossip Protocol Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Meridian gossip protocol for ring maintenance and peer discovery, following the reference implementation in libMeridian. Separate PingCapacity from gossip into its own timer-driven protocol.

**Architecture:** Two protocols now share the network actor: (1) Meridian gossip, which selects one random node per ring, sends ring membership, and discovers new peers via AddNode probing with a cache; (2) PingCapacity, which exchanges capacity/phase information on a 15-minute interval plus immediately on new peer connection.

**Tech Stack:** C (CBOR wire encoding), libcbor, existing actor/timer infrastructure

---

## Wire Protocol

### WIRE_GOSSIP (type 21)

Push message from initiator to gossip target. Carries the sender's ring membership — one random node per non-empty ring (excluding the target itself) — so the receiver can discover and probe new peers.

```c
#define WIRE_GOSSIP 21

typedef struct {
  uint64_t  message_id;
  node_id_t sender_id;
  uint32_t  rendezvous_addr;  // sender's public address (NAT traversal)
  uint16_t  rendezvous_port;  // sender's public port (NAT traversal)
  node_id_t targets[RING_MAX_RINGS];  // 1 random node per non-empty ring
  uint8_t   target_count;    // number of valid targets
} wire_gossip_t;
```

CBOR encoding: `[21, message_id, sender_id, rendezvous_addr, rendezvous_port, target_count, [target1, target2, ...]]`

### WIRE_GOSSIP_PULL (type 22)

Push-pull response sent back to the initiator. Same format as WIRE_GOSSIP but carries the responder's ring membership. This is the PUSHPULL mechanism from the reference implementation — both parties learn about each other's peers.

```c
#define WIRE_GOSSIP_PULL 22

typedef struct {
  uint64_t  message_id;
  node_id_t sender_id;
  uint32_t  rendezvous_addr;
  uint16_t  rendezvous_port;
  node_id_t targets[RING_MAX_RINGS];
  uint8_t   target_count;
} wire_gossip_pull_t;
```

Same CBOR encoding as WIRE_GOSSIP with type byte 22.

### Probe Cache (Reuse latency_cache_t)

The existing `latency_cache_t` (1024 entries, 5s TTL) serves as the probe cache. When `addNodeToRing` is called for a gossip-discovered node:
- If the node is in `latency_cache`: use the cached latency for ring insertion, skip re-pinging
- If not cached: send a Ping, defer ring insertion until PingResponse arrives with measured latency

This matches the reference implementation's `LatencyCache` with `PROBE_CACHE_SIZE=1024` and `PROBE_CACHE_TIMEOUT_US=5,000,000` (5 seconds).

---

## Gossip Tick Handler

Replace `network_handle_gossip_tick` with the Meridian algorithm. The tick fires every 2 seconds during init (5 rounds) then every 30 seconds in steady state — same as current timing, but now handles only gossip, not PingCapacity.

### Gossip tick (every 2s init / 30s steady)

1. Tick the scheduler, check `should_gossip`
2. If true: call `ring_set_get_random_nodes()` — returns 1 random node per non-empty ring (up to RING_MAX_RINGS=10 targets)
3. For each target that is a connected peer, build a `wire_gossip_t` with:
   - `sender_id` = local node ID
   - `rendezvous_addr` / `rendezvous_port` = our public address (0 if none)
   - `targets` = 1 random node per non-empty ring, excluding the target itself
   - `target_count` = number of valid targets
4. Send via `conn_state_send`
5. Record the gossip exchange in `gossip_handle`'s active list and query_table for timeout tracking

### Gossip receipt (WIRE_GOSSIP)

1. Log the received gossip (if test logging enabled)
2. Add sender to ring table (probe or use cached latency via `addNodeToRing`)
3. For each target in the gossip packet: add to ring table via `addNodeToRing`
4. Send `WIRE_GOSSIP_PULL` back to the sender with our own ring membership (PUSHPULL mode)

### Gossip pull receipt (WIRE_GOSSIP_PULL)

1. Log the received gossip pull (if test logging enabled)
2. Add sender + targets to ring table via `addNodeToRing`
3. No response — this is the pull half, no further exchange

### addNodeToRing flow

```c
static void network_add_node_to_ring(network_t* network, const node_id_t* node_id,
                                       uint32_t addr, uint16_t port) {
  // Check probe cache first
  float cached_latency_ms;
  if (latency_cache_get(network->latency_cache, node_id, &cached_latency_ms) == 0) {
    // Recently probed — use cached latency for ring insertion
    uint32_t latency_us = (uint32_t)(cached_latency_ms * 1000);
    net_node_t* node = net_node_create(node_id, addr, port);
    if (node != NULL) {
      ring_set_insert(network->rings, node, latency_us);
    }
    return;
  }
  // Not in cache — send Ping to measure latency
  // Ring insertion deferred until PingResponse arrives with measured latency.
  //
  // IMPORTANT: Gossip-discovered nodes may not be connected peers yet.
  // If the node is already a connected peer (in conn_mgr), send Ping directly.
  // If not, we must establish a connection first (via relay or direct QUIC).
  // For now, only probe nodes that are already connected peers.
  // Unconnected nodes from gossip targets are added to the ring table with
  // latency=0 (outermost ring) and will be probed on the next gossip round
  // once a connection is established.
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, node_id);
  if (peer != NULL && peer->connected) {
    wire_ping_t ping;
    memset(&ping, 0, sizeof(ping));
    ping.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&ping.sender_id, &network->authority->local_id, sizeof(node_id_t));
    ping.timestamp = (uint64_t)time(NULL) * 1000;
    // ... send ping to node_id via peer connection
  } else {
    // Not connected — insert at latency=0 (outermost ring) for now.
    // Will be probed and repositioned once connected.
    net_node_t* node = net_node_create(node_id, addr, port);
    if (node != NULL) {
      ring_set_insert(network->rings, node, 0);
    }
  }
}
```

---

## PingCapacity Separation

### Current behavior (removed)

PingCapacity is sent to all peers on the gossip timer (2s init / 30s steady).

### New behavior

1. **On new peer connection** (salutation complete): immediately send PingCapacity to that peer
2. **Periodic**: every 15 minutes, send PingCapacity to all connected peers

Implementation:
- Remove PingCapacity sending from `network_handle_gossip_tick`
- Add `NETWORK_PING_CAPACITY_TICK` message type to the dispatch enum
- Add a new timer in `network_create` with 15-minute interval, dispatching `NETWORK_PING_CAPACITY_TICK`
- New handler `network_handle_ping_capacity_tick` walks all connected peers and sends PingCapacity
- In `network_handle_salutation`, after adding the peer to the ring table, send a PingCapacity to the new peer immediately

### Wire changes

No wire changes for PingCapacity — `wire_ping_capacity_t` and `wire_ping_capacity_response_t` remain the same. Only the timing and triggering changes.

---

## Ring Selection Helper

Add `ring_set_get_random_nodes()` to `ring_set.h/c`:

```c
// Select one random node from each non-empty ring, excluding exclude_id if non-NULL.
// Returns the number of nodes selected (up to RING_MAX_RINGS).
size_t ring_set_get_random_nodes(const ring_set_t* set,
                                  net_node_t* nodes,
                                  size_t max_nodes,
                                  const node_id_t* exclude_id);
```

This mirrors the reference `RingSet::getRandomNodes()` which selects one random node per non-empty primary ring.

---

## Dispatch and Timer Changes

### New message types (in `message.h`)

```c
NETWORK_GOSSIP,           //  — handles outbound gossip tick
NETWORK_GOSSIP_RECEIVED,  //  — handles received WIRE_GOSSIP wire message
NETWORK_GOSSIP_PULL_RECEIVED, // — handles received WIRE_GOSSIP_PULL wire message
NETWORK_PING_CAPACITY_TICK,   // — periodic PingCapacity to all peers
```

### New timer (in `network_create`)

```c
network->ping_capacity_timer_id = timer_actor_set(timer,
    PING_CAPACITY_INTERVAL_MS,  // 15 min = 900,000 ms
    PING_CAPACITY_INTERVAL_MS,
    &network->actor,
    NETWORK_PING_CAPACITY_TICK);
```

### Gossip timer (unchanged interval, new behavior)

The existing gossip timer at 2s/30s stays. The handler changes from sending PingCapacity to selecting ring targets and sending WIRE_GOSSIP messages.

---

## Integration with Existing Code

### `network_handle_gossip_tick` changes

Remove the PingCapacity loop. Replace with Meridian gossip target selection and WIRE_GOSSIP sending.

### `network_handle_gossip_expire` changes

No structural changes — still expires timed-out queries from `gossip_handle.active` and `gossip_handle.query_table`.

### `network_handle_salutation` changes

After adding the new peer to the ring table, send an immediate PingCapacity to that peer:

```c
// Send immediate PingCapacity to newly connected peer
wire_ping_capacity_t ping_cap;
memset(&ping_cap, 0, sizeof(ping_cap));
ping_cap.message_id = gossip_handle_next_query_id(&network->gossip);
memcpy(&ping_cap.source, &network->authority->local_id, sizeof(node_id_t));
ping_cap.capacity = atomic_load(&network->authority->capacity);
ping_cap.phase = atomic_load(&network->authority->phase);
cbor_item_t* cbor = wire_ping_capacity_encode(&ping_cap);
conn_state_send(network, peer, cbor);
cbor_decref(&cbor);
```

### Wire dispatch (`network_dispatch`)

Add cases for `NETWORK_GOSSIP_RECEIVED`, `NETWORK_GOSSIP_PULL_RECEIVED`, and `NETWORK_PING_CAPACITY_TICK`.

### QUIC listener / relay dispatch

Add CBOR type dispatch for incoming WIRE_GOSSIP (21) and WIRE_GOSSIP_PULL (22) wire messages, converting them to `NETWORK_GOSSIP_RECEIVED` / `NETWORK_GOSSIP_PULL_RECEIVED` actor messages.

---

## Testing

### Unit tests

- `WireGossipTest.Roundtrip` — encode/decode WIRE_GOSSIP with targets
- `WireGossipPullTest.Roundtrip` — encode/decode WIRE_GOSSIP_PULL
- `RingSetGetRandomNodes` — verify one node per ring, exclusion works, empty rings skipped

### Integration tests

- `GossipDiscoveryDiamond` — 4-node diamond, verify gossip propagates ring membership and nodes discover each other via probing
- `PingCapacityInterval` — verify PingCapacity fires immediately on connect, then on 15-minute intervals
- `ProbeCacheDedup` — verify nodes in latency_cache skip re-ping on gossip discovery