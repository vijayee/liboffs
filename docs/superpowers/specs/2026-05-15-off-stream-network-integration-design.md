# OFF Stream Network Integration Design

Date: 2026-05-15
Status: Draft

## Overview

This spec covers how OFF streams connect to the network layer for block fetch and block announce. Currently, all stream types operate purely against the local block_cache — a cache miss causes stream deactivation with an error. This design adds a network fetch path so streams can request blocks from peers, and a network announce path so new blocks propagate to the network.

**What this spec does NOT cover:** QUIC stream framing, connection lifecycle, peer_connection QUIC send/receive wiring. Those belong in a separate spec that wires up the transport layer. This spec defines the actor-message interface between streams and the network actor.

---

## 1. Design Decisions (from brainstorming)

### 1.1 Streams request from network directly (no bridge actor)

OFF streams hold a reference to the `network_t` actor. On a cache miss, the stream sends a `NETWORK_LOCAL_FIND_BLOCK` message to the network actor. When the network finds the block, two things happen concurrently:

1. The network stores the block in `block_cache` (so future cache hits succeed)
2. The network sends `NETWORK_FIND_BLOCK_RESULT` directly to the stream (with `found=true`)

The stream receives the result from the network — not from the block cache. Both actions happen on the network actor's dispatch thread, so there's no ordering dependency. The stream does not re-issue `block_cache_get`; it proceeds directly with the knowledge that the block is now in cache.

**Rationale:** A bridge actor adds latency and complexity with no benefit. The stream already operates via actor messages. Adding one more message hop (stream -> bridge -> network) is unnecessary indirection.

### 1.2 Duplex QUIC stream per RPC

Each RPC (FindBlock, StoreBlock, etc.) opens a bidirectional QUIC stream. Requester writes CBOR request, closes write side. Responder reads, processes, writes CBOR response, closes. 128KB blocks fit inline in CBOR — no separate data channel needed.

### 1.3 block_cache_put returns CACHE_PUT_NEW/EXISTS/ERROR

Already implemented. Callers use the result to decide whether to announce the block on the network.

### 1.4 FIB propagation with blocks

Blocks arriving from the network carry a `block_fib` (Fibonacci hit counter). `block_cache_put` accepts `incoming_fib` and stores it for new entries or updates to `max(local, incoming)` for existing entries. Already implemented.

### 1.5 Wanted list for FindBlock dedup and notification

A bloom filter plus a linked list of entries, each tracking which stream actors are waiting. Two separate concerns:

1. **In-flight dedup**: Only one `WIRE_FIND_BLOCK` is sent per hash. If a second stream requests the same hash while a FindBlock is in flight, its actor is added to the existing entry's requester list. No duplicate wire message.
2. **Notification on arrival**: When the block arrives (via FindBlock response, StoreBlock, RecallBlock, or any other mechanism), all waiting requesters are notified with `NETWORK_FIND_BLOCK_RESULT(found=true)`.
3. **Recall matching after failure**: When FindBlock fails, the waiting actors are notified with `found=false` and removed from the requester list. But the hash stays in the bloom filter. This enables two things:
   - If a new request comes for the same hash later, a fresh `WIRE_FIND_BLOCK` is sent (since no one was waiting).
   - When the block arrives via StoreBlock/RecallBlock, the EABF recall mechanism can match it and notify any future requesters.

**Key lifecycle:**
- **New request, not in bloom**: Create entry, add actor to requester list, send `WIRE_FIND_BLOCK`.
- **New request, in bloom, requesters waiting**: Add actor to existing entry's requester list. Don't resend `WIRE_FIND_BLOCK`.
- **New request, in bloom, no requesters** (previous attempt failed): Create a fresh entry, add actor, send a new `WIRE_FIND_BLOCK`.
- **FindBlock succeeds**: Store block in cache, notify all requesters, remove entry from list AND bloom.
- **FindBlock fails**: Notify all requesters with `found=false`, clear requester list, keep bloom entry for recall matching.

### 1.6 RecallBlock flow

When a node acquires a block, it scans EABFs for level-0 entries matching the block hash. For each match, send `RecallBlock` to that peer. The peer responds with `RecallAccept` (carrying block_data + block_fib) or `RecallDecline` (remove EABF entry).

---

## 2. Data Structures

### 2.1 wanted_list_t — FindBlock dedup, requester notification, and recall matching

```c
/* src/Network/wanted_list.h */

#define WANTED_BLOOM_SIZE       4096   /* bits */
#define WANTED_BLOOM_HASHES     3

typedef struct wanted_requester_t {
    actor_t*  actor;                       /* stream actor waiting for this block */
    struct wanted_requester_t* next;
} wanted_requester_t;

typedef struct wanted_entry_t {
    buffer_t* hash;                        /* block hash */
    wanted_requester_t* requesters;        /* streams currently waiting (NULL if prev attempt failed) */
    struct wanted_entry_t* next;
} wanted_entry_t;

typedef struct wanted_list_t {
    bloom_filter_t* bloom;                 /* "have we ever searched for this hash?" */
    wanted_entry_t* entries;               /* linked list of hashes with active requesters */
    size_t           entry_count;
} wanted_list_t;
```

**Operations:**

| Function | Description |
|----------|-------------|
| `wanted_list_create()` | Create bloom filter and empty list |
| `wanted_list_destroy(wl)` | Free bloom and all entries/requesters |
| `wanted_list_check(wl, hash)` | Returns true if hash is in the bloom (may be false positive) |
| `wanted_list_find(wl, hash)` | Find entry by hash, returns NULL if no active requesters |
| `wanted_list_add(wl, hash, actor)` | Add hash to bloom. If entry exists, append actor. If not, create entry with this actor. |
| `wanted_list_remove(wl, hash)` | Remove entry from list AND bloom, return requester list for notification |
| `wanted_list_clear_requesters(wl, hash)` | Clear all requesters from entry but keep bloom entry for recall matching |

**Three states for a hash:**

| State | Bloom | Entry | Requesters | Behavior |
|-------|-------|-------|------------|----------|
| Never searched | No | No | No | New FindBlock, add to bloom and create entry |
| In-flight | Yes | Yes | Yes | Add actor to requesters, don't resend FindBlock |
| Previously failed | Yes | No | No | New FindBlock, create fresh entry and send again |

**Why the bloom persists after failure:** When FindBlock fails, requesters are notified and cleared, and the entry is removed from the linked list. But the hash stays in the bloom. This serves two purposes:
1. **Recall matching**: When a block arrives via StoreBlock or RecallBlock, `wanted_list_check` returns true, and the network can notify any future requesters.
2. **Fresh retry**: When a new request comes in for a hash that's in the bloom but has no entry (previous attempt failed), a fresh `WIRE_FIND_BLOCK` is sent with the new actor as the sole requester.

**Why the bloom is removed on success:** When FindBlock succeeds, both the entry and the bloom entry are removed. The block is now in `block_cache` — future requests will be cache hits and never reach the wanted list.

### 2.2 network_t additions

```c
/* additions to network_t in src/Network/network.h */

typedef struct network_t {
    /* ... existing fields ... */
    wanted_list_t*   wanted_list;      /* FindBlock dedup, requester tracking, recall matching */
    block_cache_t*   block_cache;      /* already exists — now used for network fetches */
} network_t;
```

No new actor needed. The `network_t` is already an actor. `wanted_list` is a data structure owned by `network_t` and accessed within its dispatch loop.

---

## 3. Read Path: Streams Requesting Blocks from Network

### 3.1 Flow

When a stream (readable_off_stream, readable_descriptor, or recycler_recipe) calls `block_cache_get` and receives `CACHE_GET_RESULT` with `block == NULL`:

**Current behavior:** Stream deactivates with error.

**New behavior:**

```
Stream                          Block Cache               Network Actor
  |                                 |                          |
  |-- CACHE_GET(hash, reply_to) -->|                          |
  |                                 |-- (checks index)        |
  |                                 |-- (not found)           |
  |<-- CACHE_GET_RESULT(NULL) -----|                          |
  |                                                            |
  |-- NETWORK_LOCAL_FIND_BLOCK(hash, &stream->actor) ------------->|
  |                                 |                          |-- wanted_bloom_check(hash)
  |                                 |                          |-- (not already searching)
  |                                 |                          |-- wanted_bloom_insert(hash)
  |                                 |                          |-- find_block_execute()
  |                                 |                          |-- PEER_SEND_FIND_BLOCK to matches
  |                                                            |
  |   ... time passes ...                                       |
  |                                                            |
  |<-------------------------- NETWORK_FIND_BLOCK_RESULT(found=true)  |
  |                                 |                          |-- wanted_bloom_remove(hash)
  |                                 |                          |-- block_cache_put(block, fib, NULL)
  |                                 |                          |
  |   (stream now knows block is in cache)                      |
  |-- CACHE_GET(hash, reply_to) -->|                          |
  |<-- CACHE_GET_RESULT(block) ----|                          |
```

**Key point:** The stream receives `NETWORK_FIND_BLOCK_RESULT` directly from the network actor — not from the block cache. This happens concurrently with the network storing the block in `block_cache`. Both happen on the network actor's dispatch thread, so by the time the stream's `CACHE_GET` message reaches the block cache, the block is already there. The stream does not need to re-issue `CACHE_GET` — it proceeds knowing the block is now available and issues a fresh `CACHE_GET` in the same state it would have if the block had been in cache originally.

**If FindBlock fails:** The stream receives `NETWORK_FIND_BLOCK_RESULT(found=false)` and deactivates with an error. The block may arrive later via StoreBlock or RecallBlock and be stored in cache — future stream requests for the same hash will succeed.

### 3.2 New message types

**Note:** `NETWORK_FIND_BLOCK`, `NETWORK_FIND_BLOCK_RESPONSE`, `NETWORK_STORE_BLOCK`, and `NETWORK_STORE_BLOCK_RESPONSE` already exist in the `message_type_e` enum for wire-level protocol. We add separate message types for stream-to-network actor messages to avoid payload type conflicts.

```c
/* New entries in message_type_e enum (after TOPOLOGY_METRICS_UPDATE) */

/* Stream-to-network: request block from peers (distinct from wire-level NETWORK_FIND_BLOCK) */
NETWORK_LOCAL_FIND_BLOCK,
/* Network-to-stream: result of FindBlock */
NETWORK_FIND_BLOCK_RESULT,
/* Stream-to-network: announce new block to peers (distinct from wire-level NETWORK_STORE_BLOCK) */
NETWORK_LOCAL_STORE_BLOCK,
/* Network-to-stream: result of StoreBlock */
NETWORK_STORE_BLOCK_RESULT,

typedef struct {
    buffer_t* hash;       /* block hash to find */
    actor_t*  reply_to;   /* stream actor to notify */
} network_local_find_block_payload_t;

typedef struct {
    buffer_t* hash;       /* same hash from the request */
    int       found;      /* 1 = found (block now in cache), 0 = not found */
} network_find_block_result_payload_t;
```

**Naming:** `NETWORK_LOCAL_FIND_BLOCK` is the stream-to-network request (distinct from `NETWORK_FIND_BLOCK` which handles wire-level peer requests). `NETWORK_FIND_BLOCK_RESULT` is the network-to-stream response.

### 3.3 Stream changes

Each stream type needs a new state: `AWAITING_NETWORK_FETCH`. When `CACHE_GET_RESULT` returns `block == NULL`:

1. Send `NETWORK_LOCAL_FIND_BLOCK` message to network actor with `reply_to = &stream->actor`
2. Set stream state to `AWAITING_NETWORK_FETCH`
3. When `NETWORK_FIND_BLOCK_RESULT` arrives:
   - If `found == 1`: re-issue `block_cache_get(hash, &stream->actor)`, resume normal flow
   - If `found == 0`: deactivate with error

**readable_off_stream_t additions:**

```c
typedef enum {
    OFF_STREAM_FETCHING_BLOCKS,      /* existing: waiting for block_cache_get results */
    OFF_STREAM_AWAITING_NETWORK,     /* new: waiting for NETWORK_FIND_BLOCK_RESULT */
    OFF_STREAM_AWAITING_TUPLE,       /* existing: waiting for next tuple */
} off_stream_state_e;
```

**readable_descriptor_t additions:**

```c
typedef enum {
    DESC_FETCHING_DESCRIPTOR,        /* existing */
    DESC_AWAITING_NETWORK,            /* new */
    DESC_FETCHING_DATA,              /* existing */
    DESC_DONE                        /* existing */
} desc_state_e;
```

**recycler_recipe_t additions:**

```c
typedef enum {
    RECIPE_LOADING_DESCRIPTOR,        /* existing */
    RECIPE_AWAITING_NETWORK,          /* new */
    RECIPE_FETCHING_BLOCKS,           /* existing */
    RECIPE_DONE                       /* existing */
} recipe_state_e;
```

### 3.4 Network handler: NETWORK_FIND_BLOCK

```c
/* In network_dispatch, case NETWORK_LOCAL_FIND_BLOCK: */
void network_handle_local_find_block(network_t* network, message_t* msg) {
    network_local_find_block_payload_t* p = (network_local_find_block_payload_t*)msg->payload;

    /* 1. Check local block_cache first (fast path) */
    index_entry_t* entry = index_peek(network->block_cache->index, p->hash);
    if (entry != NULL) {
        /* Block is local — resolve immediately, no network request needed */
        network_find_block_result_payload_t result;
        result.hash = p->hash;
        result.found = 1;
        message_t reply;
        reply.type = NETWORK_FIND_BLOCK_RESULT;
        reply.payload = &result;
        reply.payload_destroy = NULL;
        actor_send(p->reply_to, &reply);
        return;
    }

    /* 2. Check wanted list — three cases */
    wanted_entry_t* existing = wanted_list_find(network->wanted_list, p->hash);
    if (existing != NULL) {
        /* Case A: In-flight request with active requesters.
         * Add this actor to the existing entry. Don't resend FindBlock. */
        wanted_list_add(network->wanted_list, p->hash, p->reply_to);
        return;
    }

    if (wanted_list_check(network->wanted_list, p->hash) && existing == NULL) {
        /* Case B: In bloom but no entry (previous FindBlock failed).
         * This is a fresh request for a previously-failed hash.
         * Create a new entry and send a new FindBlock. */
        wanted_list_add(network->wanted_list, p->hash, p->reply_to);
        /* Fall through to execute FindBlock routing */
    } else {
        /* Case C: Never searched for this hash.
         * Add to bloom and create entry. */
        wanted_list_add(network->wanted_list, p->hash, p->reply_to);
        /* Fall through to execute FindBlock routing */
    }

    /* 3. Execute FindBlock routing */
    find_block_state_t state;
    /* ... populate state from p->hash, TTL, visited_bloom ... */
    find_block_result_e result = find_block_execute(&state, &network->eabf_table,
                                                     &network->conn_mgr, network->rings,
                                                     &network->authority->local_id);

    switch (result) {
        case FOUND:
            /* Block is in local index — shouldn't happen after index_peek above,
               but handle defensively */
            wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, p->hash);
            network_find_block_result_payload_t found_result;
            found_result.hash = p->hash;
            found_result.found = 1;
            message_t found_msg = {0};
            found_msg.type = NETWORK_FIND_BLOCK_RESULT;
            found_msg.payload = &found_result;
            found_msg.payload_destroy = NULL;
            wanted_requester_t* req = requesters;
            while (req != NULL) {
                actor_send(req->actor, &found_msg);
                wanted_requester_t* next = req->next;
                free(req);
                req = next;
            }
            break;

        case FORWARDING:
            /* Forwarding to peers — response will arrive asynchronously
             * via WIRE_FIND_BLOCK_RESPONSE. Entry stays in the wanted list
             * with active requesters until the response arrives. */
            for (size_t i = 0; i < state.next_hop_count; i++) {
                peer_connection_t* peer = state.next_hops[i];
                /* Send PEER_SEND_FIND_BLOCK to peer actor */
                /* ... wire up when QUIC is ready ... */
            }
            break;

        case NOT_FOUND:
        case TTL_EXPIRED:
            /* No route to block — notify all requesters with found=0,
             * then clear requesters but keep the bloom entry for recall matching. */
            network_find_block_result_payload_t not_found_result;
            not_found_result.hash = p->hash;
            not_found_result.found = 0;
            message_t not_found_msg = {0};
            not_found_msg.type = NETWORK_FIND_BLOCK_RESULT;
            not_found_msg.payload = &not_found_result;
            not_found_msg.payload_destroy = NULL;
            wanted_requester_t* req = wanted_list_clear_requesters(network->wanted_list, p->hash);
            while (req != NULL) {
                actor_send(req->actor, &not_found_msg);
                wanted_requester_t* next = req->next;
                free(req);
                req = next;
            }
            break;
    }
}
```

### 3.5 Network handler: FindBlock response arrives from peer

When a `WIRE_FIND_BLOCK_RESPONSE` arrives with `found=true`:

```c
void network_handle_find_block_response(network_t* network, wire_find_block_response_t* response) {
    /* ... existing Hebbian and EABF logic ... */

    if (response->found) {
        /* Two things happen (both on the network actor thread):
         * 1. Store the block in block_cache (so future cache hits succeed)
         * 2. Notify all waiting requesters via NETWORK_FIND_BLOCK_RESULT */

        /* Store the block locally */
        buffer_t* block_data = /* extract from response */;
        block_t* block = block_create_existing_data_hash(block_data, response->block_hash);
        block_cache_put(network->block_cache, block, response->block_fib, NULL);
        block_destroy(block);

        /* Remove from wanted list (both bloom and entry) and notify all requesters */
        wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, response->block_hash);
        network_find_block_result_payload_t result;
        result.hash = response->block_hash;
        result.found = 1;
        message_t msg = {0};
        msg.type = NETWORK_FIND_BLOCK_RESULT;
        msg.payload = &result;
        msg.payload_destroy = NULL;
        wanted_requester_t* req = requesters;
        while (req != NULL) {
            actor_send(req->actor, &msg);
            wanted_requester_t* next = req->next;
            free(req);
            req = next;
        }
    } else {
        /* Not found — notify all requesters with found=false,
         * then clear requesters but keep the bloom entry for recall matching.
         * Future requests for this hash will see the bloom hit, find no entry,
         * and send a fresh FindBlock. */
        wanted_requester_t* requesters = wanted_list_clear_requesters(network->wanted_list, response->block_hash);
        if (requesters != NULL) {
            network_find_block_result_payload_t result;
            result.hash = response->block_hash;
            result.found = 0;
            message_t msg = {0};
            msg.type = NETWORK_FIND_BLOCK_RESULT;
            msg.payload = &result;
            msg.payload_destroy = NULL;
            wanted_requester_t* req = requesters;
            while (req != NULL) {
                actor_send(req->actor, &msg);
                wanted_requester_t* next = req->next;
                free(req);
                req = next;
            }
        }
    }
}
```

### 3.6 Block arrival via other paths (StoreBlock, RecallBlock)

When a block arrives via StoreBlock or RecallBlock, the network handler checks the wanted list. If any requesters are waiting, they're notified and the entry (including bloom) is removed:

```c
/* After block_cache_put returns CACHE_PUT_NEW for any StoreBlock/RecallBlock */
wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, block_hash);
if (requesters != NULL) {
    network_find_block_result_payload_t result;
    result.hash = block_hash;
    result.found = 1;
    message_t msg = {0};
    msg.type = NETWORK_FIND_BLOCK_RESULT;
    msg.payload = &result;
    msg.payload_destroy = NULL;
    wanted_requester_t* req = requesters;
    while (req != NULL) {
        actor_send(req->actor, &msg);
        wanted_requester_t* next = req->next;
        free(req);
        req = next;
    }
}
```

This handles the case where a stream requested a block, the FindBlock failed (bloom entry remains, requesters cleared), and the block later arrives via StoreBlock. Since there are no active requesters, `wanted_list_remove` just removes the bloom entry — no one to notify. But if a new request had come in between the failure and the arrival, that new requester would be in the list and would get notified.

---

## 4. Write Path: Streams Announcing Blocks to Network

### 4.1 Flow

When a stream stores a block via `block_cache_put` and the result is `CACHE_PUT_NEW`:

**Current behavior:** Block is stored locally. No network announcement.

**New behavior:**

```
Stream                         Block Cache              Network Actor
  |                                |                          |
  |-- CACHE_PUT(block, fib, &stream->actor) -------------->  |
  |                                |                          |
  |<-- CACHE_PUT_RESULT(NEW, fib) ---|                        |
  |                                                           |
  |-- NETWORK_LOCAL_STORE_BLOCK(hash, fib, reply_to) -------------->|
  |                                                           |-- store_block_should_accept()
  |                                                           |-- (determine if we should announce)
  |                                                           |-- PEER_SEND_STORE_BLOCK to peers
  |                                                           |-- (or RankBlock broadcast)
  |                                                           |
  |<-- NETWORK_STORE_BLOCK_RESULT(accepted, replicas) -------  |
  |                                                           |-- EABF subscriptions
```

### 4.2 New message types

```c
/* Sent by stream to network actor after CACHE_PUT_NEW (distinct from wire-level NETWORK_STORE_BLOCK) */
NETWORK_LOCAL_STORE_BLOCK,
/* Sent by network actor back to stream */
NETWORK_STORE_BLOCK_RESULT,

typedef struct {
    buffer_t* hash;       /* block hash */
    uint32_t  fib;        /* FIB counter for the block */
    actor_t*  reply_to;   /* stream actor to notify (NULL = fire-and-forget) */
} network_local_store_block_payload_t;

typedef struct {
    int       accepted;   /* 1 = accepted by network, 0 = declined */
    uint32_t  replicas;   /* number of replicas stored */
    actor_t*  reply_to;   /* NULL for fire-and-forget */
} network_store_block_result_payload_t;
```

### 4.3 Which blocks get announced

| Stream Type | Block Source | Announce to Network? | Why |
|-------------|-------------|---------------------|-----|
| `writeable_off_stream` | Random blocks from `new_blocks_recipe` | **No** | Recipe handles its own announcement |
| `writeable_off_stream` | Off block (XOR result) | **Yes** if `CACHE_PUT_NEW` | This is the novel content |
| `writeable_descriptor` | Descriptor blocks | **Yes** if `CACHE_PUT_NEW` | Descriptors are content-addressable references |
| `readable_off_stream` | Blocks fetched from network | **No** | Already on network |
| `readable_descriptor` | Blocks fetched from network | **No** | Already on network |
| `new_blocks_recipe` | Random blocks | **No** | Random blocks have no value on network |
| `recycler_recipe` | Blocks from descriptors | **No** | Already on network |

### 4.4 Stream changes for write path

**writeable_off_stream:** After storing the off block, check `CACHE_PUT_RESULT`:

```c
/* Current: block_cache_put(stream->bc, off_block, 0, NULL) */
/* New: */
cache_put_payload_t put_msg;
put_msg.block = off_block;
put_msg.incoming_fib = 0;
put_msg.reply_to = &stream->stream.actor;
block_cache_put(stream->bc, off_block, 0, &stream->stream.actor);

/* In dispatch, handle CACHE_PUT_RESULT: */
case CACHE_PUT_RESULT: {
    cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
    if (result->result == CACHE_PUT_NEW) {
        /* New block — announce to network */
        network_local_store_block_payload_t payload;
        payload.hash = off_block_hash; /* need to capture this */
        payload.fib = result->fib;
        payload.reply_to = NULL; /* fire-and-forget for now */
        message_t net_msg;
        net_msg.type = NETWORK_LOCAL_STORE_BLOCK;
        net_msg.payload = &payload;
        net_msg.payload_destroy = NULL;
        actor_send(&stream->network->actor, &net_msg);
    }
    /* Continue normal flow regardless */
    break;
}
```

**writeable_descriptor:** After storing each descriptor block, check `CACHE_PUT_RESULT`:

```c
/* Current: block_cache_put(desc->bc, block, 0, NULL) */
/* New: need async result to check if block was new */
block_cache_put(desc->bc, block, 0, &desc->stream.actor);

/* In dispatch, handle CACHE_PUT_RESULT: */
case CACHE_PUT_RESULT: {
    cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
    if (result->result == CACHE_PUT_NEW && stream->network != NULL) {
        network_local_store_block_payload_t payload;
        payload.hash = /* stored hash */;
        payload.fib = result->fib;
        payload.reply_to = NULL;
        message_t net_msg;
        net_msg.type = NETWORK_LOCAL_STORE_BLOCK;
        net_msg.payload = &payload;
        net_msg.payload_destroy = NULL;
        actor_send(&stream->network->actor, &net_msg);
    }
    break;
}
```

### 4.5 Network handler: NETWORK_LOCAL_STORE_BLOCK

```c
void network_handle_local_store_block(network_t* network, message_t* msg) {
    network_local_store_block_payload_t* p = (network_local_store_block_payload_t*)msg->payload;

    /* 1. Determine acceptance using respiration/capacity logic */
    store_block_result_e result = store_block_execute(
        /* state populated from p->hash, network's EABF, conn_mgr, rings, local_id */);

    switch (result) {
        case STORE_BLOCK_ACCEPTED:
            /* 2. Subscribe in EABFs along the acceptance path */
            /* ... existing EABF logic ... */

            /* 3. Recall: check if any peers wanted this block */
            /* ... existing recall logic ... */

            /* 4. Forward if replicas needed */
            /* ... */

            /* 5. Reply to caller */
            if (p->reply_to != NULL) {
                network_store_block_result_payload_t reply;
                reply.accepted = 1;
                reply.replicas = 0; /* or actual count */
                reply.reply_to = NULL;
                message_t reply_msg;
                reply_msg.type = NETWORK_STORE_BLOCK_RESULT;
                reply_msg.payload = &reply;
                reply_msg.payload_destroy = NULL;
                actor_send(p->reply_to, &reply_msg);
            }
            break;

        case STORE_BLOCK_DECLINED:
            if (p->reply_to != NULL) {
                /* ... send declined result ... */
            }
            break;

        case STORE_BLOCK_FORWARDING:
            /* Forward to next hops */
            /* ... */
            break;
    }
}
```

---

## 5. Wanted List Lifecycle

### 5.1 New request (hash not in bloom)

Stream sends `NETWORK_LOCAL_FIND_BLOCK`:

1. `wanted_list_check(hash)` — returns false (not in bloom)
2. `wanted_list_add(hash, actor)` — creates entry, adds to bloom
3. Execute FindBlock routing, send `WIRE_FIND_BLOCK`

### 5.2 Duplicate request (hash in bloom, entry exists with requesters)

Another stream requests the same hash while a FindBlock is in flight:

1. `wanted_list_find(hash)` — returns the existing entry
2. Append the new actor to the entry's requester list
3. Don't send another `WIRE_FIND_BLOCK`

### 5.3 Retry after failure (hash in bloom, no entry)

A new request for a hash that previously failed:

1. `wanted_list_check(hash)` — returns true (in bloom)
2. `wanted_list_find(hash)` — returns NULL (entry was removed after failure)
3. Create a fresh entry, add actor as sole requester
4. Send a new `WIRE_FIND_BLOCK`

### 5.4 Success (block found via FindBlock response)

When `WIRE_FIND_BLOCK_RESPONSE` arrives with `found=true`:

1. Store the block in `block_cache` with the FIB from the response
2. `wanted_list_remove(hash)` — removes entry from list AND bloom, returns requester list
3. Send `NETWORK_FIND_BLOCK_RESULT(found=true)` to every requester
4. Free all requester nodes

### 5.5 Failure (block not found)

When `WIRE_FIND_BLOCK_RESPONSE` arrives with `found=false`, or FindBlock routing returns NOT_FOUND/TTL_EXPIRED:

1. `wanted_list_clear_requesters(hash)` — clears requesters from entry, removes entry from linked list, returns requester list
2. Send `NETWORK_FIND_BLOCK_RESULT(found=false)` to every requester
3. **Bloom entry stays** — the hash remains in the bloom for recall matching and dedup
4. Future requests for this hash see bloom hit + no entry → fresh FindBlock (section 5.3)

### 5.6 Block arrival via other paths (StoreBlock, RecallBlock)

When a block arrives via any path other than FindBlock response:

1. `wanted_list_remove(hash)` — if any requesters are waiting, returns them; otherwise just removes the bloom entry
2. Send `NETWORK_FIND_BLOCK_RESULT(found=true)` to any returned requesters
3. This unblocks streams that were waiting after a previous FindBlock failure

### 5.7 Race condition: block arrives between cache miss and FindBlock

The `network_handle_find_block_request` handler checks `index_peek` first. If the block is locally present, it resolves immediately without touching the wanted list.

### 5.8 Bloom filter maintenance

Since failed entries stay in the bloom indefinitely, the bloom may accumulate false positives over time. Periodically rebuild the bloom by iterating the current entry list (which only contains hashes with active requesters) and re-inserting them. This is a background operation triggered when the bloom's estimated false positive rate exceeds a threshold.

---

## 6. RecallBlock Flow

### 6.1 Triggering Recall

When `block_cache_put` stores a new block (`CACHE_PUT_NEW`) or when a `WIRE_STORE_BLOCK` is accepted locally:

1. Scan all peers' EABFs at level 0 for the block hash
2. For each match, send `WIRE_RECALL_BLOCK` to that peer

### 6.2 Receiving RecallBlock

```c
void network_handle_recall_block(network_t* network, wire_recall_block_t* recall) {
    /* Look up the block in local cache */
    /* If found: */
    /*   Encode block data + FIB into wire_recall_accept_t */
    /*   Send WIRE_RECALL_ACCEPT to requesting peer */
    /* If not found: */
    /*   Send WIRE_RECALL_DECLINE to requesting peer */
}
```

### 6.3 Receiving RecallAccept

```c
void network_handle_recall_accept(network_t* network, wire_recall_accept_t* accept) {
    /* accept contains block_data and block_fib */
    /* Store in block_cache */
    block_t* block = block_create_existing_data_hash(accept->block_data, accept->block_hash);
    block_cache_put(network->block_cache, block, accept->block_fib, NULL);
    block_destroy(block);

    /* Remove from EABF level 0 (promissory note fulfilled) */
    /* ... */
}
```

---

## 7. Wire Protocol Additions

### 7.1 WIRE_FIND_BLOCK_RESPONSE changes

The `wire_find_block_response_t` already has a `found` boolean and `holder` node_id. We need to add optional inline block data for when `found=true`:

```c
/* Current: */
typedef struct {
    uint32_t message_id;
    uint8_t  found;           /* 1 if found, 0 if not */
    node_id_t holder;        /* node that holds the block */
    uint32_t fib;            /* FIB counter at the holder */
    node_id_t path[WIRE_MAX_PATH];
    uint8_t  path_len;
    double   latency_ms;
} wire_find_block_response_t;

/* New: */
typedef struct {
    uint32_t message_id;
    uint8_t  found;
    node_id_t holder;
    uint32_t fib;
    node_id_t path[WIRE_MAX_PATH];
    uint8_t  path_len;
    double   latency_ms;
    /* Block data — only present when found=true */
    uint8_t* block_data;         /* block content (NULL if not found) */
    size_t   block_data_len;     /* length of block data */
    uint32_t block_fib;          /* FIB counter from the holder */
} wire_find_block_response_t;
```

### 7.2 WIRE_STORE_BLOCK changes

The `wire_store_block_t` already has `block_data` and `block_data_len` fields. The `block_fib` field carries the FIB counter. No changes needed.

### 7.3 WIRE_RECALL_BLOCK — no changes needed

Already defined: `{message_id, block_hash[32]}`.

### 7.4 WIRE_RECALL_ACCEPT — add block data

```c
/* Current: */
typedef struct {
    uint32_t message_id;
} wire_recall_accept_t;

/* New: */
typedef struct {
    uint32_t  message_id;
    uint8_t*  block_data;      /* the requested block's data */
    size_t    block_data_len;   /* length of block data */
    uint32_t  block_fib;        /* FIB counter for the block */
} wire_recall_accept_t;
```

The peer responding to `WIRE_RECALL_BLOCK` includes the block data and FIB in the accept response. This is the "push" model — the peer that wanted the block gets it delivered.

### 7.5 WIRE_RECALL_DECLINE — no changes needed

Already defined: `{message_id}`.

---

## 8. Stream Struct Changes

### 8.1 readable_off_stream_t

```c
typedef struct readable_off_stream_t {
    /* ... existing fields ... */
    network_t* network;    /* NULL = local-only mode, non-NULL = network-aware */
    int        network_fetch_in_flight;  /* count of pending NETWORK_LOCAL_FIND_BLOCK requests */
} readable_off_stream_t;
```

**Constructor change:** `readable_off_stream_create` takes an optional `network_t*` parameter. When `NULL`, the stream operates in local-only mode (current behavior). When non-NULL, cache misses trigger `NETWORK_LOCAL_FIND_BLOCK` instead of deactivation.

### 8.2 readable_descriptor_t

```c
typedef struct readable_descriptor_t {
    /* ... existing fields ... */
    network_t* network;    /* NULL = local-only, non-NULL = network-aware */
} readable_descriptor_t;
```

Same pattern: optional network reference.

### 8.3 writeable_off_stream_t

```c
typedef struct writeable_off_stream_t {
    /* ... existing fields ... */
    network_t* network;    /* NULL = local-only, non-NULL = network-aware */
} writeable_off_stream_t;
```

After storing off blocks, announce new blocks to network if `network != NULL`.

### 8.4 writeable_descriptor_t

```c
typedef struct writeable_descriptor_t {
    /* ... existing fields ... */
    network_t* network;    /* NULL = local-only, non-NULL = network-aware */
} writeable_descriptor_t;
```

After storing descriptor blocks, announce new blocks to network if `network != NULL`.

### 8.5 Backward compatibility

All stream constructors gain a `network_t*` parameter. Passing `NULL` preserves the existing local-only behavior. Existing tests and local-only usage continue to work without changes.

---

## 9. Dispatch Changes Summary

### 9.1 readable_off_stream dispatch additions

```c
case NETWORK_FIND_BLOCK_RESULT: {
    network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
    if (result->found) {
        /* Block is now in cache — re-issue block_cache_get */
        block_cache_get(stream->bc, stream->pending_fetch_hash, &stream->stream.actor);
        stream->state = OFF_STREAM_FETCHING_BLOCKS;
    } else {
        stream_deactivate((stream_t*)stream, ERROR("Block not found on network"));
    }
    break;
}
```

### 9.2 readable_descriptor dispatch additions

```c
case NETWORK_FIND_BLOCK_RESULT: {
    network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
    if (result->found) {
        /* Block is now in cache — re-issue block_cache_get for the descriptor block */
        block_cache_get(desc->bc, desc->pending_fetch_hash, &desc->stream.actor);
        desc->state = DESC_FETCHING_DESCRIPTOR; /* or DESC_FETCHING_DATA */
    } else {
        stream_deactivate((stream_t*)desc, ERROR("Descriptor block not found on network"));
    }
    break;
}
```

### 9.3 recycler_recipe dispatch additions

```c
case NETWORK_FIND_BLOCK_RESULT: {
    network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
    if (result->found) {
        /* Block is now in cache — re-issue block_cache_get for the data block */
        block_cache_get(recipe->recipe.bc, recipe->pending_fetch_hash, &recipe->recipe.stream.actor);
        recipe->state = RECIPE_FETCHING_BLOCKS;
    } else {
        /* For data blocks: deactivate. For descriptor blocks: skip to next ORI. */
        if (recipe->loading_descriptor) {
            recipe->ori_index++;
            recipe->loading_descriptor = 0;
            _start_descriptor_load(recipe);
        } else {
            stream_deactivate((stream_t*)recipe, ERROR("Block not found on network"));
        }
    }
    break;
}
```

### 9.4 writeable_off_stream dispatch additions

```c
case CACHE_PUT_RESULT: {
    cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
    if (result->result == CACHE_PUT_NEW && stream->network != NULL) {
        /* Announce new block to network */
        network_local_store_block_payload_t payload;
        payload.hash = /* the hash of the stored block */;
        payload.fib = result->fib;
        payload.reply_to = NULL;
        message_t net_msg;
        net_msg.type = NETWORK_LOCAL_STORE_BLOCK;
        net_msg.payload = &payload;
        net_msg.payload_destroy = NULL;
        actor_send(&stream->network->actor, &net_msg);
    }
    /* Continue normal stream flow */
    break;
}
```

### 9.5 writeable_descriptor dispatch additions

Same pattern as writeable_off_stream: handle `CACHE_PUT_RESULT`, announce new blocks to network.

---

## 10. Implementation Order

### Phase 1: Wanted List + Network FindBlock (Read Path)

1. Implement `wanted_list_t` data structure
2. Add `wanted_list` field to `network_t`
3. Add `NETWORK_LOCAL_FIND_BLOCK` and `NETWORK_FIND_BLOCK_RESULT` message types and payloads
4. Add `network_handle_find_block_request` to `network_dispatch`
5. Add `AWAITING_NETWORK` state to `readable_off_stream_t`
6. Change `readable_off_stream_dispatch` to send `NETWORK_LOCAL_FIND_BLOCK` on cache miss instead of deactivating
7. Add `NETWORK_FIND_BLOCK_RESULT` handler to `readable_off_stream_dispatch`
8. Same for `readable_descriptor_t` and `recycler_recipe_t`
9. Test: unit tests for `wanted_list_t`, integration test for stream -> network -> stream round-trip

### Phase 2: StoreBlock Announce (Write Path)

1. Add `NETWORK_LOCAL_STORE_BLOCK` and `NETWORK_STORE_BLOCK_RESULT` message types and payloads
2. Change `writeable_off_stream` and `writeable_descriptor` to use async `block_cache_put` with `reply_to`
3. Add `CACHE_PUT_RESULT` handler to both streams
4. Add `NETWORK_LOCAL_STORE_BLOCK` handler to `network_dispatch`
5. Test: integration test for write path announcing blocks to network

### Phase 3: Wire Protocol for Inline Block Data

1. Update `wire_find_block_response_t` encode/decode to include optional `block_data`, `block_data_len`, `block_fib`
2. Update `wire_recall_accept_t` encode/decode to include `block_data`, `block_data_len`, `block_fib`
3. Update `network_handle_find_block_response` to store received blocks in `block_cache` with FIB
4. Update `network_handle_recall_accept` to store received blocks in `block_cache` with FIB
5. Test: CBOR encode/decode round-trip tests for new fields

### Phase 4: RecallBlock Flow

1. After `block_cache_put` returns `CACHE_PUT_NEW`, scan EABFs for level-0 entries
2. Send `WIRE_RECALL_BLOCK` to matching peers
3. Handle `WIRE_RECALL_BLOCK` by looking up block in local cache and responding with accept or decline
4. Handle `WIRE_RECALL_ACCEPT` by storing block locally and removing EABF entry
5. Test: integration test for recall flow

---

## 11. Error Handling

### Network unavailable

When `network` is `NULL` (local-only mode), streams fall back to existing behavior: deactivate on cache miss, no announcement on cache put.

### FindBlock timeout

If a FindBlock request never receives a response (network partition, peer disconnect), the stream stays in `AWAITING_NETWORK_FETCH` state. The stream should have its own timeout (using `timer_actor_set` with a reasonable duration like 30 seconds). On timeout, the stream sends `NETWORK_FIND_BLOCK_RESULT(found=0)` to itself and deactivates. The wanted list entry's requesters are cleared (they've been notified), but the bloom entry stays for recall matching.

### Multiple concurrent requests for the same block

The wanted list deduplicates: only one `WIRE_FIND_BLOCK` is sent per hash while a request is in flight. If a second stream requests the same hash, its actor is added to the existing entry's requester list. When the block arrives (by any path), all requesters are notified. If a previous FindBlock failed for that hash (bloom hit, no entry), a fresh `WIRE_FIND_BLOCK` is sent with the new requester as the sole entry. This handles the case where multiple streams need the same block simultaneously.

### StoreBlock declined

If the network declines a StoreBlock, the block is still stored locally. The stream continues normally. The block just won't be announced to peers — it can still be found via inbound FindBlock requests.