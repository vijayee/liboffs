Here's a design prompt you can hand directly to Claude Code:

---

# OFFS Network Layer — Implementation Specification

## Overview

Implement the P2P network layer for the Owner-Free File System. The network is a self-organizing overlay combining Meridian (multi-resolution latency rings), SoPPSoN (Hebbian weight learning), Quasar (negative information routing), and Elastic Attenuated Bloom Filters with timing wheels for per-connection memory.

Nodes have fixed storage capacity. Below 50% they actively seek blocks (inhale). Above 80% they shed coldest blocks to nodes with room (exhale). Between 50-80% they route only. Seek rate slows quadratically as capacity approaches 50%.

---

## 1. RPC Messages

Implement these nine message types as protobuf or flat structs with serialization:

### 1.1 Ping / PingResponse
```
Ping:
  - message_id: uint64
  - timestamp: uint64 (for RTT calculation)

PingResponse:
  - message_id: uint64 (echoes request)
  - echo_time: uint64 (echoes request timestamp)
  - capacity: float32 [0.0, 1.0] (cached, updated atomically on block add/remove)
  - phase: uint8 (0=INHALE, 1=NEUTRAL, 2=EXHALE)
```
Handler: read cached capacity/phase (no locks, no scans). Echo timestamp. Must be fast — Meridian uses this for RTT.

### 1.2 PingCapacity / PingCapacityResponse
```
PingCapacity:
  - message_id: uint64
  - source: node_id (20 bytes)
  - capacity: float32
  - phase: uint8
  - samples: []struct {
      node_id: 20 bytes
      ring:    uint8 (which ring the source placed this node in)
      weight:  float32
      latency: uint32 (ms)
      capacity: float32
    }

PingCapacityResponse:
  - message_id: uint64
  - capacity: float32
  - phase: uint8
  - samples: []struct { ... } (same structure)
```
This is the gossip/anti-entropy message. Handler: measure latency to source and all samples. Place source and samples into appropriate rings as secondary members. Trigger Symmetry rule for new connections.

### 1.3 PingBlock / PingBlockResponse
```
PingBlock:
  - message_id: uint64
  - block_hash: 32 bytes

PingBlockResponse:
  - message_id: uint64
  - exists: bool
  - fib: uint32 (0 if not found)
  - healthy: bool (false if not found)
```
Handler: check index for block_hash. If found, verify block integrity in sections. Reinforce Hebbian weight w_{self→source} on success.

### 1.4 FindBlock / FindBlockResponse
```
FindBlock:
  - message_id: uint64
  - block_hash: 32 bytes
  - ttl: uint8 (remaining hops, decremented each forward)
  - visited_bloom: []byte (256 bytes = 2048 bits, bloom filter of visited node IDs)
  - visited_count: uint16 (approximate, for false positive estimation)
  - path: []node_id (max 6, nodes this request has traversed)
  - path_len: uint8
  - start_time: uint64 (when original requester began search)
  - original_source: node_id (who originally asked, for EBF memory on failure)

FindBlockResponse:
  - message_id: uint64
  - found: bool
  - holder: node_id (if found)
  - fib: uint32 (block's current fib rank)
  - path: []node_id (complete path from requester to holder)
  - path_len: uint8
  - latency: uint64 (total search time ms, for Hebbian learning rate)
```
Handler algorithm:
1. Check local index. If found → respond, increment fib, return.
2. If TTL=0 → insert block_hash into EABF_{self→original_source}.level with timing wheel TTL, respond not found.
3. Add self to path and visited_bloom. Decrement TTL.
4. Check all EABFs level 0→K for gravity well match. If found and neighbor not in visited_bloom → directed walk forward to that neighbor.
5. No gravity well → gather candidates from rings [ring_for_distance-1, ring_for_distance, ring_for_distance+1], filter by visited_bloom and min_weight > 0.01.
6. Roulette-wheel select FORWARD_FANOUT (3) candidates weighted by w_{self→candidate}. Forward to each.
7. If no candidates → same as TTL=0 failure path.

Response handler: if found, apply Hebbian rules along path (Frequency: requester→holder, Feedback: each forwarder→forwardee, Symmetry: reverse of each). Also populate EABFs along path (see Section 4).

### 1.5 FindNode / FindNodeResponse
```
FindNode:
  - message_id: uint64
  - target_id: node_id

FindNodeResponse:
  - message_id: uint64
  - closest_nodes: []node_id (up to 8)
```
Handler: return k=8 closest nodes in routing table to target_id by XOR distance.

### 1.6 StoreBlock / StoreBlockResponse
```
StoreBlock:
  - message_id: uint64
  - block_hash: 32 bytes
  - block_size: uint32
  - block_fib: uint32
  - replicas_needed: uint8 (how many more nodes must accept)
  - max_hops: uint8
  - visited_bloom: []byte (256 bytes)
  - visited_count: uint16
  - path: []node_id (max 6)
  - path_len: uint8
  - start_time: uint64
  - carry_data: bool
  - block_data: []byte (only if carry_data=true, 128KB typical)

StoreBlockResponse:
  - message_id: uint64
  - accepted: bool
  - holder: node_id (if accepted)
  - replicas_remaining: uint8 (if not accepted)
  - path: []node_id
  - path_len: uint8
  - latency: uint64
```
Handler algorithm:
1. Check SHOULD-ACCEPT: not already stored, room available, not in exhale phase (capacity < 0.80). Accept with probability = 1.0 - capacity/0.80.
2. If accept → write to sections, create index entry, put in hot cache (eviction sets ejection_date), check all EABFs for recall obligations, respond accepted. If replicas_needed > 0 after decrement, continue forwarding from here.
3. If decline and max_hops=0 → respond not accepted.
4. If decline → add self to path and visited_bloom, decrement max_hops.
5. Gather candidates from rings , filter by visited_bloom, skip peers in exhale phase, skip peers with consecutive_fails ≥ MAX_FAILS, skip peers we're rate-limiting.
6. Compute composite storage score S(j) = w_{self→j} × (1 - capacity_j) × 1/(1 + latency/β) × availability_j.
7. Roulette-wheel select FORWARD_FANOUT candidates weighted by S(j). Forward to each.

Response handler: if accepted, apply Hebbian rules along path and populate EABFs along path (see Section 4).

### 1.7 SeekingBlocks / SeekingBlocksResponse
```
SeekingBlocks:
  - message_id: uint64
  - capacity: float32 (requester's capacity)
  - exclude_hashes: [][]byte (blocks requester already has)

SeekingBlocksResponse:
  - message_id: uint64
  - offers: []struct {
      hash: 32 bytes
      fib:  uint32
      size: uint32
    }
```
Handler: select up to MAX_OFFERS blocks from local index using PICK-BLOCK-FOR-REPRESENTATION (walk ranks from highest fib downward, probability ∝ fibonacci(fib)). Exclude hashes in exclude_hashes list. Return offers.

Response handler: evaluate each offer. If SHOULD-PULL (block not already stored, capacity < 0.50, fib exceeds local threshold), send FindBlock for that hash.

### 1.8 RankBlock (fire-and-forget, no response)
```
RankBlock:
  - block_hash: 32 bytes
  - fib: uint32
  - count: uint32
  - origin: node_id
  - hop_count: uint8
```
Handler:
1. If we have this block and msg.fib > local.fib → upgrade local entry's fib, count, threshold. Update rank_map. Do not re-gossip.
2. If we don't have this block and msg.fib > highest_seen_rank → set highest_seen_rank. If capacity < 0.50, initiate Seek for this block.
3. If hop_count < MAX_RANK_HOPS (6), increment hop_count and forward to random subset of peers.

### 1.9 RecallBlock / RecallAccept / RecallDecline
```
RecallBlock:
  - message_id: uint64
  - block_hash: 32 bytes

RecallAccept:
  - message_id: uint64

RecallDecline:
  - message_id: uint64
```
Handler for RecallBlock: if we don't have the block and capacity < 0.50 → send RecallAccept, else → send RecallDecline.
Handler for RecallAccept: load block from sections, send StoreBlock with reason=RECALL.
Handler for RecallDecline: delete block_hash from EABF_{self→source}.

### Additional: RateLimited (sent when any request exceeds token bucket)
```
RateLimited:
  - message_id: uint64 (echoes rejected request)
  - type: uint8 (which RPC type was rejected)
  - retry_after_ms: uint32
  - current_limit: float32 (tokens/second currently available)
```

---

## 2. Core Data Structures

### 2.1 Ring Table
```c
Ring i: inner = β × α^(i-1), outer = β × α^i  (i ≥ 1)
Ring 0: [0, β)

Constants: α=2, β=5ms, k=8 primary per ring, m=4 secondary per ring
Ring count: ceil(log_α(max_RTT / β)), typically 8-12

ring_member_t {
  node_id: 20 bytes
  latency: uint32 (ms)
  weight: float32 (Hebbian w_ij)
  capacity: float32 [0,1]
  phase: uint8 (INHALE/NEUTRAL/EXHALE)
  last_gossip_time: uint64
  availability: float32 [0,1] (EWMA)
  consecutive_fails: uint32
}
```

### 2.2 EABF (Elastic Attenuated Bloom Filter) per connection
```
EABF_{self→peer}:
  levels: [0..K] of elastic_bloom_filter (K=3, i.e., 4 levels)
  wheel: timing_wheel (64 slots × 60s = 64 min span)

Level 0: blocks at distance 0 via this peer (peer wants/has this)
Level 1: blocks at distance 1 via this peer (peer's neighbor has this)
Level 2: blocks at distance 2
Level 3: blocks at distance 3

Each level: 512-byte bloom filter with elastic fingerprint bucket array in slow memory.
```

### 2.3 Timing Wheel
```
timing_wheel_t {
  slot_count: 64
  slot_duration_ms: 60000
  current_slot: uint32
  slots: [64]slot_t
  last_advance: uint64
}

slot_t {
  refs: []ebf_ref_t (references into EABF buckets)
}

ebf_ref_t {
  level: uint8
  bucket_index: uint32
  fp_index: uint32
}
```

### 2.4 Token Buckets (per peer, per RPC type)
```
token_bucket_t {
  base_rate: float32
  max_rate: float32
  burst_size: float32
  tokens: float32
  last_refill: uint64
  total_accepted: uint64
  total_rejected: uint64
}

peer_rate_limits_t {
  peer_id: node_id
  weight: float32
  buckets: map[rpc_type]token_bucket_t
}

Default rates:
  FindBlock:  base=5/s,  max=50/s,  burst=20, cost=1
  StoreBlock: base=0.5/s, max=5/s,   burst=3,  cost=1
  SeekingBlocks: base=1/s, max=10/s, burst=5,  cost=1
  PingCapacity: base=10/s, max=10/s, burst=10, cost=0.1
  Ping: base=10/s, max=10/s, burst=10, cost=0.1
```

### 2.5 Peer Availability
```
peer_availability_t {
  availability: float32 (EWMA [0,1])
  last_seen: uint64
  consecutive_fails: uint32
  first_seen: uint64
}
```
Updated on every ping success/timeout. EWMA with α=0.1. New peers start at 0.5.

---

## 3. Hebbian Learning Rules

Apply on successful FindBlockResponse and StoreBlockResponse:

```
τ = response.latency (total search time ms)
T = MAX_SEARCH_TIME_MS (6 hops × avg hop latency, e.g., 60000)
q = 1.0 (quality, validated on receipt)
γ = γ₀ × max(0, 1 - τ/T) × q
Δw = γ

γ₀ = 0.1
η_f = 0.25 (feedback constant)
η_s = 0.05 (symmetry constant)

FREQUENCY RULE: w_{requester→holder} += Δw
  (if no prior connection, create with initial strength = Δw)

FEEDBACK RULE: for i = 1 to path_len-2:
  w_{path[i]→path[i+1]} += η_f × Δw

SYMMETRY RULE: for i = 0 to path_len-2:
  w_{path[i+1]→path[i]} += η_s × Δw

Weight multipliers:
  Normal FindBlock success: 1.0×
  StoreBlock accepted (exhalation): 1.0×
  StoreBlock accepted (recall): 2.0×
  Push declined: -0.5×
```

---

## 4. EABF Population on Successful Store

When StoreBlock succeeds along path [A, B, C, D] where D is holder:

```
For each node i in path (except holder):
  forwardee = path[i+1]
  distance = (holder_index - i)
  
  Insert into EABF_{path[i]→forwardee}:
    level: distance - 1
    hash: block_hash
    ttl: BASE_TTL_MS / (1 + (distance-1) × 0.5)
      Level 0: 60 min, Level 1: 40 min, Level 2: 30 min, Level 3: 24 min

If entry already exists at that level → push timing wheel expiry forward (refresh).
```

---

## 5. Respiration Cycle

### 5.1 Seek Throttle
```
τ(c) = τ_min + (τ_max - τ_min) × (c/0.50)^α    for c ∈ [0, 0.50]
τ(c) = ∞                                          for c ≥ 0.50

τ_min = 5000ms, τ_max = 300000ms, α = 2.0
```

### 5.2 Inhale Cycle (runs when capacity < 50% and seek interval elapsed)
```
1. Select diverse neighbors from rings [0,1,2] with min_weight > 0.1
2. Send SeekingBlocks to selected peers
3. Process responses: for each offer, if SHOULD-PULL → send FindBlock
```

### 5.3 Exhale Cycle (runs when capacity ≥ 80%)
```
1. to_free = blocks needed to reach 50% capacity
2. Collect all cold blocks: entries with ejection_date > 0
3. Sort by ejection_date ASC (oldest eviction first)
4. Find recipients: peers with capacity < 0.50, sort by capacity ASC
5. For each block in cold[0..to_free]:
   - Load from sections
   - Send StoreBlock(reason=EXHALATION) to best recipient
   - On acceptance: index_remove locally, delete from sections
   - On rejection: try next recipient
```

### 5.4 Ejection Date Lifecycle
```
Block enters hot cache: ejection_date = 0
LRU evicts block: ejection_date = now()
Block re-enters hot cache via access: ejection_date = 0
```

---

## 6. Ring Membership Management

Run periodically in background:
1. For each ring, compute local coordinates for all primary+secondary members:
   coords(peer_j) = [latency(self, j₁), latency(self, j₂), ..., latency(self, jₖ), w_{self→j}]
2. Find subset of k peers with maximal hypervolume convex hull → new primaries
3. Remaining m peers → secondaries (FIFO pool)
4. If a peer is unreachable during computation, drop and replace from secondaries

---

## 7. Gossip Cycle

Run periodically (short period for new nodes, lengthens to steady state):
1. For each ring, pick random primary member
2. Send PingCapacity with source=self, capacity, phase, and one random sample from each ring
3. On receiving PingCapacity: measure latency to source and all samples, place into rings as secondaries, trigger Symmetry rule

---

## 8. Timing Wheel Maintenance Tick

Run every 60 seconds:
1. Advance hand by elapsed slots
2. For each expired slot across all EABFs:
    - Delete each ref's fingerprint from its EABF bucket
    - If bucket becomes empty, clear corresponding bloom filter bit

---

## 9. Rate Limiting

On every incoming message:
1. Get or create peer_rate_limits for source
2. Apply capacity_multiplier: if type is StoreBlock and capacity ≥ 0.80 → multiplier = 0.05; if 0.50-0.80 → linear taper; otherwise 1.0
3. Try consume token from appropriate bucket
4. If rejected → send RateLimited response with retry_after_ms and current_limit
5. Sender-side: before forwarding, check local copy of remote limits. Skip peers in backoff or with insufficient burst.

---

## 10. Block Selection for Representations

```
PICK-BLOCK-FOR-REPRESENTATION(index):
  max_fib = find_max_fib(index.ranks)
  for fib = max_fib down to 1:
    bucket = index.ranks[fib]
    if bucket not empty:
      if random() < fibonacci(fib) / fibonacci(max_fib + 1):
        return random_element(bucket)
  return random_element(index.ranks[0])
```

---

## 11. Key Constants

Constant	Value	Purpose
BLOCK_SIZE	131072 (128KB)	Standard block size
REPLICATION_FACTOR	3	Additional nodes for new blocks
FORWARD_FANOUT	3	Peers to forward to per hop
MAX_PATH	6	Max hops in path recording
VISITED_BLOOM_BYTES	256	Negative information filter
EABF_LEVELS	4	K=3, levels 0-3
EABF_LEVEL_BYTES	512	Per-level bloom filter size
WHEEL_SLOTS	64	Timing wheel granularity
WHEEL_SLOT_MS	60000	1 minute per slot
BASE_TTL_MS	3600000	1 hour default memory
MAX_RANK_HOPS	6	Rank gossip TTL
INHALE_MAX	0.50	Stop seeking here
EXHALE_MIN	0.80	Start shedding here
EXHALE_TARGET	0.50	Shed until here
γ₀	0.1	Base learning rate
η_f	0.25	Feedback constant
η_s	0.05	Symmetry constant
α_ring	2	Ring radius multiplier
β_ring	5ms	Innermost ring radius
k_ring	8	Primary ring members
m_ring	4	Secondary ring members
LATENCY_NORM_MS	50	β for storage score
MAX_OFFERS	20	Max blocks in SeekingBlocksResponse

---

## 12. Implementation Notes

- All message handlers must be non-blocking. Use async I/O.
- EABF slow memory (fingerprint buckets) can live on disk; fast memory (bloom bits) in RAM.
- Timing wheel ticks should not block message processing.
- Capacity and phase are cached atomics updated on block add/remove and threshold crossing.
- Hebbian weight updates and EABF insertions happen in the response path, not the request path.
- Token bucket refill happens on consume attempt, not on a timer.
- Ring hypervolume computation is background, best-effort; stale results are acceptable.
- Path arrays in messages are bounded by MAX_PATH; overflow is a protocol error.

---

Implement the message types first, then the ring table and routing, then the EABF + timing wheel, then the Hebbian learning, then the respiration cycle, then rate limiting. Each layer can be tested independently.