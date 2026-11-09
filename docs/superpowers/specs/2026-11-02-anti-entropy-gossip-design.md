# Anti-Entropy Divergence Repair over Ring Gossip

Date: 2026-11-02
Status: Approved (approach A; payload = ids only; cadence = slow tick, 1 peer/tick; chaff cap = 10/exchange)

## Problem

Ring-set membership propagates today through one vector: the 30s push-pull gossip
tick, which delivers at most one random member per ring (≤10 ids) to one random
recipient ring, capped at `GOSSIP_PER_SOURCE_CAP = 5` novel insertions per packet.
Rings hold only 8 primary + 4 secondary members per latency band
(`RING_K = 8`, `RING_M = 4`). Two consequences:

1. **Divergence**: two nodes' views of ring membership drift apart over time, and
   repair-by-random-walk is slow and chaff-capped. A node recovering from a partition
   or a long outage can stay divergent for many gossip rounds.
2. **One-hop addresses**: third parties learn a peer's *id* via gossip targets, but the
   id is inserted address-less (outermost ring). Reachability depends on routing
   (FIND_NODE / closest-nodes), not on the gossip payload.

Anti-entropy exchange is added to repair (1). Repairing (2) was explicitly
considered and rejected for now — rendezvous port churn (SRFLX) makes address
advertisement stale-prone, and routing already provides reachability. The digest
covers **known ids across all rings**; ids only, no addresses, no latencies.

## Design

Anti-entropy is a periodic digest-plus-difference exchange between one random
connected pair per tick. It supplements — does not replace — the existing
push-pull gossip. All legs run on the existing network protocol actor (no locks,
no new threads).

### Module: `src/Network/anti_entropy.c/.h`

Stateless helpers only, consistent with the request-scoped actor pattern:

- `anti_entropy_bloom_build(const node_id_t* ids, size_t count, uint8_t* out)` —
  fills a `WIRE_ANTI_ENTROPY_BLOOM_BYTES` (512) byte filter, k = 3, hashed over
  `node_id.hash` (32 bytes) with the same double-hash scheme as `visited_bloom`.
- `anti_entropy_bloom_check(const uint8_t* bloom, const node_id_t* id)` —
  membership test.
- Difference enumeration over a `ring_set_t`: ids whose membership test fails.
  `ring_set_collect_topology()` (ring_set.c:195) is the enumeration source.

No new module-level state; ~100 lines.

### Wire format

Message numbers 28–30 (next free in wire.h):

```c
#define WIRE_ANTI_ENTROPY          28
#define WIRE_ANTI_ENTROPY_OFFER    29
#define WIRE_ANTI_ENTROPY_RESPONSE 30

#define WIRE_ANTI_ENTROPY_BLOOM_BYTES 512
#define ANTI_ENTROPY_MAX_IDS RING_MAX_RINGS  // 10

typedef struct {  // leg 1
  uint64_t    message_id;
  node_id_t   sender_id;
  uint8_t     bloom[WIRE_ANTI_ENTROPY_BLOOM_BYTES];
  uint8_t     member_count;      // informational: filter load
} wire_anti_entropy_t;

typedef struct {  // leg 2 (OFFER) and leg 3 (RESPONSE)
  uint64_t    message_id;
  node_id_t   sender_id;
  uint8_t     bloom[WIRE_ANTI_ENTROPY_BLOOM_BYTES]; // OFFER only; RESPONSE omits
  node_id_t   ids[ANTI_ENTROPY_MAX_IDS];
  uint8_t     id_count;
} wire_anti_entropy_offer_t;
```

### Data flow

1. **Tick**: new timer `NETWORK_ANTI_ENTROPY_TICK` armed in `network_create` at
   `config.anti_entropy_interval_s`. Handler picks one uniformly random
   **connected** peer by scanning `connection_manager.peers` (directly iterable);
   skips if none. Sends `WIRE_ANTI_ENTROPY` with a bloom over its full ring set.
2. **Receiver (B)** on `WIRE_ANTI_ENTROPY`: responds with `WIRE_ANTI_ENTROPY_OFFER`
   containing B's own bloom (enabling A's leg) + ≤10 ids from B's set that test
   absent in A's bloom. Inserts nothing yet.
3. **Initiator (A)** on `WIRE_ANTI_ENTROPY_OFFER`: inserts offered ids (below),
   computes its own ids absent in B's bloom, sends ≤10 in
   `WIRE_ANTI_ENTROPY_RESPONSE`.
4. **Receiver (B)** on `WIRE_ANTI_ENTROPY_RESPONSE`: inserts offered ids. Done —
   both directions reconciled in one pass, three messages total.

Insertions go through the existing
`network_add_node_to_ring(network, id, 0, 0, NULL)` — address-less, outermost
ring — identical to how gossip-received targets are inserted. Updated
`last_gossip_time` and `net_node_record_success` come for free.

Because legs 2/3 fire synchronously in the receive handler, nothing waits on a
reply: no new timeout state and no query-table registration — the message ids
exist only for the message log.

### Caps and chaff bound

- `ANTI_ENTROPY_MAX_IDS = 10` novel ids per offer/response direction.
- Only *novel* insertions count toward the cap; ids already present are skipped
  without consuming budget (same pattern as `GOSSIP_PER_SOURCE_CAP`).
- No cross-exchange counter. Consecutive ticks pair with different peers;
  per-source chaff stays bounded at 10 ids per 300s — an order of magnitude below
  the gossip push path's insert rate.

### Error handling

- **Bloom false positives** cause a skipped candidate; self-healing on a later
  tick. No retry logic.
- **Empty ring set** (fresh node): all-zero filter; peer offers its id list up to
  the cap. Harmless.
- **Malformed packets**: decode failure, `id_count > ANTI_ENTROPY_MAX_IDS`, or
  oversized bloom — reject and free, logged like other decode failures in the
  dispatch. `message_log_record` on send/receive of all three legs.
- **Peer disconnects mid-exchange**: legs 2/3 are conn_state sends; a dead
  connection makes the send a no-op. No state to unwind.

### Config

- New field `anti_entropy_interval_s`: default 300, minimum 10 (`config_validate`).
  No zero-disable — repair is part of correctness, not an optimization.
- Timer lifecycle mirrors `gossip_timer_id`: atomic id, cancel in `network_destroy`.

## Testing

gtest following `test_network.cpp` patterns; harness mirrors the two-node pattern
used by existing network integration tests.

- **Unit**: filter build/check round-trip; FP-rate sanity at 120 members;
  encode/decode round-trip for all three messages; decode rejection of malformed
  offer/response (bad id_count, truncated bloom).
- **Integration**: two in-process nodes with disjoint ring sets converge
  bidirectionally after one tick; already-known ids do not consume the cap; a node
  holding 11+ novel ids offers exactly 10.
- **Memory**: valgrind suite on the two-node test with `-gdwarf-4`, per the
  standard post-implementation gate.

## Rejected alternatives

- **Gossip to everyone (full fanout)**: O(N²) per-round traffic; churns the
  8+4-slot latency rings with address-less ids; degrades Meridian routing.
- **Bloom to avoid cycles in the push path**: push-pull random exchange does not
  flood, and `ring_set_find_by_id` already dedupes repeats cheaply.
- **Exact set exchange (no bloom)**: 4KB per pairing to learn nothing in the
  common near-identical case; grows with N where a fixed 512B filter does not.
- **Ids + addresses in the digest**: repairs the one-hop address gap but
  advertises stale SRFLX ports; reachability already flows through routing.
- **Change-triggered immediate reconciliation**: extra mechanism and state;
  revisit only if the 300s tick is measurably too slow to converge in practice.