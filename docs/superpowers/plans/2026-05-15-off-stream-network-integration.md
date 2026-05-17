# OFF Stream Network Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect OFF streams to the network layer so streams can fetch blocks from peers on cache miss and announce new blocks to the network.

**Architecture:** Streams send `NETWORK_LOCAL_FIND_BLOCK` messages to the network actor on cache miss (distinct from wire-level `NETWORK_FIND_BLOCK`). The network actor uses a `wanted_list_t` (bloom filter + linked list) to deduplicate FindBlock requests and track waiting streams. When blocks arrive, the network stores them in `block_cache` and notifies waiting streams with `NETWORK_FIND_BLOCK_RESULT`. Writeable streams check `CACHE_PUT_RESULT` and announce new blocks via `NETWORK_LOCAL_STORE_BLOCK`.

**Tech Stack:** C (C11), CBOR wire protocol, actor message system, bloom filter, existing block_cache and OFF stream infrastructure.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/Network/wanted_list.h` | Wanted list data structure (bloom + linked list) |
| `src/Network/wanted_list.c` | Wanted list implementation |
| `src/Network/network.h` | Add `wanted_list_t*` field to `network_t` |
| `src/Network/network.c` | Add `NETWORK_LOCAL_FIND_BLOCK`, `NETWORK_FIND_BLOCK_RESULT`, `NETWORK_LOCAL_STORE_BLOCK`, `NETWORK_STORE_BLOCK_RESULT` handlers; extend existing handlers for wanted_list |
| `src/Actor/message.h` | Add LOCAL_FIND_BLOCK, FIND_BLOCK_RESULT, LOCAL_STORE_BLOCK, STORE_BLOCK_RESULT message types and payload structs |
| `src/OFFStreams/readable_off_stream.h` | Add `network_t*` field and state enum |
| `src/OFFStreams/readable_off_stream.c` | Handle cache miss → `NETWORK_FIND_BLOCK`, handle `NETWORK_FIND_BLOCK_RESULT` |
| `src/OFFStreams/readable_descriptor.h` | Add `network_t*` field and state enum |
| `src/OFFStreams/readable_descriptor.c` | Handle cache miss → `NETWORK_FIND_BLOCK`, handle `NETWORK_FIND_BLOCK_RESULT` |
| `src/OFFStreams/block_recipe.h` | Add `network_t*` field to `block_recipe_t`, add state enum to `recycler_recipe_t` |
| `src/OFFStreams/block_recipe.c` | Handle cache miss → `NETWORK_FIND_BLOCK`, handle `NETWORK_FIND_BLOCK_RESULT` |
| `src/OFFStreams/writeable_off_stream.h` | Add `network_t*` field |
| `src/OFFStreams/writeable_off_stream.c` | Check `CACHE_PUT_RESULT`, announce new blocks via `NETWORK_STORE_BLOCK` |
| `src/OFFStreams/writeable_descriptor.h` | Add `network_t*` field |
| `src/OFFStreams/writeable_descriptor.c` | Check `CACHE_PUT_RESULT`, announce new blocks via `NETWORK_STORE_BLOCK` |
| `src/Network/wire.h` | Add `block_data`, `block_data_len`, `block_fib` to `wire_find_block_response_t` and `wire_recall_accept_t` |
| `src/Network/wire.c` | Update CBOR encode/decode for new wire fields |
| `test/test_wanted_list.cpp` | Unit tests for `wanted_list_t` |
| `test/test_network_integration.cpp` | Integration tests for stream → network → stream round-trip |
| `CMakeLists.txt` | Add new source files |

---

### Task 1: wanted_list_t Data Structure

**Files:**
- Create: `src/Network/wanted_list.h`
- Create: `src/Network/wanted_list.c`
- Test: `test/test_wanted_list.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// test/test_wanted_list.cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Network/wanted_list.h"
#include "../src/Buffer/buffer.h"
#include "../src/RefCounter/refcounter.h"
}

TEST(WantedListTest, CreateDestroy) {
  wanted_list_t* wl = wanted_list_create();
  ASSERT_NE(wl, nullptr);
  wanted_list_destroy(wl);
}

TEST(WantedListTest, CheckReturnsFalseForUnknown) {
  wanted_list_t* wl = wanted_list_create();
  buffer_t* hash = buffer_create_from_hex("0123456789abcdef0123456789abcdef", 32);
  ASSERT_NE(hash, nullptr);
  EXPECT_FALSE(wanted_list_check(wl, hash));
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(WantedListTest, AddAndCheck) {
  wanted_list_t* wl = wanted_list_create();
  buffer_t* hash = buffer_create_from_hex("0123456789abcdef0123456789abcdef", 32);
  actor_t dummy_actor = {0};
  wanted_list_add(wl, hash, &dummy_actor);
  EXPECT_TRUE(wanted_list_check(wl, hash));
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(WantedListTest, AddMultipleRequesters) {
  wanted_list_t* wl = wanted_list_create();
  buffer_t* hash = buffer_create_from_hex("0123456789abcdef0123456789abcdef", 32);
  actor_t actor1 = {0};
  actor_t actor2 = {0};
  wanted_list_add(wl, hash, &actor1);
  wanted_list_add(wl, hash, &actor2);
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  ASSERT_NE(entry, nullptr);
  /* Should have 2 requesters */
  int count = 0;
  wanted_requester_t* req = entry->requesters;
  while (req != nullptr) { count++; req = req->next; }
  EXPECT_EQ(count, 2);
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(WantedListTest, RemoveReturnsRequesters) {
  wanted_list_t* wl = wanted_list_create();
  buffer_t* hash = buffer_create_from_hex("0123456789abcdef0123456789abcdef", 32);
  actor_t actor1 = {0};
  actor_t actor2 = {0};
  wanted_list_add(wl, hash, &actor1);
  wanted_list_add(wl, hash, &actor2);
  wanted_requester_t* requesters = wanted_list_remove(wl, hash);
  ASSERT_NE(requesters, nullptr);
  /* Bloom should no longer contain the hash */
  EXPECT_FALSE(wanted_list_check(wl, hash));
  /* Free requesters */
  wanted_requester_t* req = requesters;
  while (req != nullptr) {
    wanted_requester_t* next = req->next;
    free(req);
    req = next;
  }
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(WantedListTest, ClearRequestersKeepsBloom) {
  wanted_list_t* wl = wanted_list_create();
  buffer_t* hash = buffer_create_from_hex("0123456789abcdef0123456789abcdef", 32);
  actor_t actor1 = {0};
  wanted_list_add(wl, hash, &actor1);
  wanted_requester_t* requesters = wanted_list_clear_requesters(wl, hash);
  /* Bloom should still contain the hash */
  EXPECT_TRUE(wanted_list_check(wl, hash));
  /* But find should return NULL (no entry) */
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  EXPECT_EQ(entry, nullptr);
  /* Free requesters */
  wanted_requester_t* req = requesters;
  while (req != nullptr) {
    wanted_requester_t* next = req->next;
    free(req);
    req = next;
  }
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(WantedListTest, RetryAfterFailure) {
  wanted_list_t* wl = wanted_list_create();
  buffer_t* hash = buffer_create_from_hex("0123456789abcdef0123456789abcdef", 32);
  actor_t actor1 = {0};
  actor_t actor2 = {0};
  /* First request */
  wanted_list_add(wl, hash, &actor1);
  /* Fail: clear requesters but keep bloom */
  wanted_requester_t* reqs = wanted_list_clear_requesters(wl, hash);
  free(reqs);
  /* Bloom hit but no entry → fresh request */
  EXPECT_TRUE(wanted_list_check(wl, hash));
  EXPECT_EQ(wanted_list_find(wl, hash), nullptr);
  /* New request after failure */
  wanted_list_add(wl, hash, &actor2);
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  ASSERT_NE(entry, nullptr);
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . && ./test/test_wanted_list`
Expected: FAIL (file not found)

- [ ] **Step 3: Implement wanted_list.h**

```c
// src/Network/wanted_list.h
#ifndef OFFS_WANTED_LIST_H
#define OFFS_WANTED_LIST_H
#include "../Bloom/bloom_filter.h"
#include "../Actor/actor.h"
#include "../Buffer/buffer.h"
#include <stdint.h>
#include <stdbool.h>

#define WANTED_BLOOM_SIZE       4096
#define WANTED_BLOOM_HASHES     3

typedef struct wanted_requester_t {
    actor_t*  actor;
    struct wanted_requester_t* next;
} wanted_requester_t;

typedef struct wanted_entry_t {
    buffer_t* hash;
    wanted_requester_t* requesters;
    struct wanted_entry_t* next;
} wanted_entry_t;

typedef struct wanted_list_t {
    bloom_filter_t* bloom;
    wanted_entry_t* entries;
    size_t          entry_count;
} wanted_list_t;

wanted_list_t* wanted_list_create(void);
void wanted_list_destroy(wanted_list_t* wl);
bool wanted_list_check(wanted_list_t* wl, buffer_t* hash);
wanted_entry_t* wanted_list_find(wanted_list_t* wl, buffer_t* hash);
void wanted_list_add(wanted_list_t* wl, buffer_t* hash, actor_t* actor);
wanted_requester_t* wanted_list_remove(wanted_list_t* wl, buffer_t* hash);
wanted_requester_t* wanted_list_clear_requesters(wanted_list_t* wl, buffer_t* hash);

#endif
```

- [ ] **Step 4: Implement wanted_list.c**

```c
// src/Network/wanted_list.c
#include "wanted_list.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"

wanted_list_t* wanted_list_create(void) {
  wanted_list_t* wl = get_clear_memory(sizeof(wanted_list_t));
  size_t bloom_size;
  uint32_t hash_count;
  bloom_filter_optimal_size(10000, 0.01, &bloom_size, &hash_count);
  if (bloom_size < WANTED_BLOOM_SIZE) bloom_size = WANTED_BLOOM_SIZE;
  if (hash_count < WANTED_BLOOM_HASHES) hash_count = WANTED_BLOOM_HASHES;
  wl->bloom = bloom_filter_create(bloom_size, hash_count);
  wl->entries = NULL;
  wl->entry_count = 0;
  return wl;
}

void wanted_list_destroy(wanted_list_t* wl) {
  if (wl == NULL) return;
  wanted_entry_t* entry = wl->entries;
  while (entry != NULL) {
    wanted_entry_t* next = entry->next;
    if (entry->hash != NULL) buffer_destroy(entry->hash);
    wanted_requester_t* req = entry->requesters;
    while (req != NULL) {
      wanted_requester_t* next_req = req->next;
      free(req);
      req = next_req;
    }
    free(entry);
    entry = next;
  }
  bloom_filter_destroy(wl->bloom);
  free(wl);
}

bool wanted_list_check(wanted_list_t* wl, buffer_t* hash) {
  return bloom_filter_contains(wl->bloom, hash->data, hash->size);
}

wanted_entry_t* wanted_list_find(wanted_list_t* wl, buffer_t* hash) {
  wanted_entry_t* entry = wl->entries;
  while (entry != NULL) {
    if (buffer_compare(entry->hash, hash) == 0) return entry;
    entry = entry->next;
  }
  return NULL;
}

void wanted_list_add(wanted_list_t* wl, buffer_t* hash, actor_t* actor) {
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  if (entry != NULL) {
    /* Entry exists — append requester */
    wanted_requester_t* req = get_clear_memory(sizeof(wanted_requester_t));
    req->actor = actor;
    req->next = entry->requesters;
    entry->requesters = req;
  } else {
    /* New entry */
    bloom_filter_add(wl->bloom, hash->data, hash->size);
    entry = get_clear_memory(sizeof(wanted_entry_t));
    entry->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
    wanted_requester_t* req = get_clear_memory(sizeof(wanted_requester_t));
    req->actor = actor;
    req->next = NULL;
    entry->requesters = req;
    entry->next = wl->entries;
    wl->entries = entry;
    wl->entry_count++;
  }
}

wanted_requester_t* wanted_list_remove(wanted_list_t* wl, buffer_t* hash) {
  wanted_entry_t** current = &wl->entries;
  while (*current != NULL) {
    if (buffer_compare((*current)->hash, hash) == 0) {
      wanted_entry_t* entry = *current;
      *current = entry->next;
      wl->entry_count--;
      wanted_requester_t* requesters = entry->requesters;
      buffer_destroy(entry->hash);
      free(entry);
      /* Note: we cannot remove from bloom filter (bloom filters don't support deletion).
       * This is acceptable — false positives from stale bloom entries just mean
       * a redundant FindBlock that will be resolved quickly. The bloom will be
       * rebuilt periodically to keep false positive rate low. */
      return requesters;
    }
    current = &(*current)->next;
  }
  return NULL;
}

wanted_requester_t* wanted_list_clear_requesters(wanted_list_t* wl, buffer_t* hash) {
  wanted_entry_t** current = &wl->entries;
  while (*current != NULL) {
    if (buffer_compare((*current)->hash, hash) == 0) {
      wanted_entry_t* entry = *current;
      wanted_requester_t* requesters = entry->requesters;
      entry->requesters = NULL;
      /* Remove entry from list but keep bloom entry */
      *current = entry->next;
      wl->entry_count--;
      buffer_destroy(entry->hash);
      free(entry);
      return requesters;
    }
    current = &(*current)->next;
  }
  return NULL;
}
```

- [ ] **Step 5: Add wanted_list to CMakeLists.txt and test target**

Add `src/Network/wanted_list.c` to the library source list and `test/test_wanted_list.cpp` to the test target.

- [ ] **Step 6: Run tests and verify they pass**

Run: `cd build && cmake --build . && ./test/test_wanted_list`
Expected: All tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/Network/wanted_list.h src/Network/wanted_list.c test/test_wanted_list.cpp CMakeLists.txt
git commit -m "feat: add wanted_list data structure for FindBlock dedup and requester tracking"
```

---

### Task 2: Message Types and Network Handler Integration

**Important:** `NETWORK_FIND_BLOCK`, `NETWORK_FIND_BLOCK_RESPONSE`, `NETWORK_STORE_BLOCK`, and `NETWORK_STORE_BLOCK_RESPONSE` already exist in `message_type_e` for wire-level protocol. The handlers `network_handle_find_block`, `network_handle_find_block_response`, `network_handle_store_block`, and `network_handle_store_block_response` already exist in `network.c`. We must NOT reuse these — we need separate message types for stream-to-network actor messages.

**Files:**
- Modify: `src/Actor/message.h`
- Modify: `src/Network/network.h`
- Modify: `src/Network/network.c`

- [ ] **Step 1: Add new message types and payload structs to message.h**

Add four new entries to the `message_type_e` enum, after `TOPOLOGY_METRICS_UPDATE`:

```c
  /* Local stream-to-network messages (distinct from wire-level NETWORK_FIND_BLOCK etc.) */
  NETWORK_LOCAL_FIND_BLOCK,        /* Stream sends this to network actor on cache miss */
  NETWORK_FIND_BLOCK_RESULT,      /* Network actor sends this back to stream */
  NETWORK_LOCAL_STORE_BLOCK,      /* Stream sends this to network actor on CACHE_PUT_NEW */
  NETWORK_STORE_BLOCK_RESULT,     /* Network actor sends this back to stream */
```

Add payload structs after the enum (before the `message_t` struct):

```c
/* Stream-to-network: request block from peers */
typedef struct {
  buffer_t* hash;       /* block hash to find */
  actor_t*  reply_to;  /* stream actor to notify */
} network_local_find_block_payload_t;

/* Network-to-stream: result of FindBlock */
typedef struct {
  buffer_t* hash;       /* same hash from the request */
  int       found;      /* 1 = found (block now in cache), 0 = not found */
} network_find_block_result_payload_t;

/* Stream-to-network: announce new block to peers */
typedef struct {
  buffer_t* hash;       /* block hash */
  uint32_t  fib;        /* FIB counter for the block */
  actor_t*  reply_to;  /* stream actor to notify (NULL = fire-and-forget) */
} network_local_store_block_payload_t;

/* Network-to-stream: result of StoreBlock */
typedef struct {
  int       accepted;  /* 1 = accepted by network, 0 = declined */
  uint32_t  replicas;  /* number of replicas stored */
  actor_t*  reply_to;  /* NULL for fire-and-forget */
} network_store_block_result_payload_t;
```

- [ ] **Step 2: Add wanted_list field to network_t in network.h**

Add `#include "wanted_list.h"` to the includes. Add `wanted_list_t* wanted_list;` field to the `network_t` struct, after the `eabf_ttl_table_t eabf_ttl;` field.

- [ ] **Step 3: Add wanted_list creation/destruction in network.c**

Add `#include "wanted_list.h"` to the includes section.

In `network_create`, add:
```c
network->wanted_list = wanted_list_create();
```

In `network_destroy`, add:
```c
wanted_list_destroy(network->wanted_list);
```

- [ ] **Step 4: Add handler stubs to network_dispatch**

Add cases for the four new message types in the `network_dispatch` switch statement. For now, just log and break — the full implementation comes in later tasks.

- [ ] **Step 5: Build and verify**

Run: `cd build && cmake --build .`
Expected: Build succeeds with no errors

- [ ] **Step 6: Commit**

```bash
git add src/Actor/message.h src/Network/network.h src/Network/network.c
git commit -m "feat: add LOCAL_FIND/STORE_BLOCK message types, result types, and wanted_list to network_t"
```

---

### Task 3: Network FindBlock Handler (Read Path)

**Note:** The existing `network_handle_find_block` handles wire-level FindBlock requests from peers (payload: `wire_find_block_t*`). The new `network_handle_local_find_block` handles local stream-originated requests (payload: `network_local_find_block_payload_t*`). These are separate handlers for separate message types.

**Files:**
- Modify: `src/Network/network.c`

- [ ] **Step 1: Implement network_handle_local_find_block**

Add a new handler function in `network.c` that implements the stream-originated FindBlock logic from the spec (section 3.4):

```c
static void network_handle_local_find_block(network_t* network, message_t* msg) {
  network_local_find_block_payload_t* p = (network_local_find_block_payload_t*)msg->payload;
  if (p == NULL) return;

  /* 1. Check local block_cache first (fast path) */
  index_entry_t* entry = index_peek(network->block_cache->index, p->hash);
  if (entry != NULL) {
    /* Block is local — resolve immediately */
    network_find_block_result_payload_t result = {
      .hash = p->hash,
      .found = 1
    };
    message_t reply = {0};
    reply.type = NETWORK_FIND_BLOCK_RESULT;
    reply.payload = &result;
    reply.payload_destroy = NULL;
    actor_send(p->reply_to, &reply);
    return;
  }

  /* 2. Check wanted list — three cases */
  wanted_entry_t* existing = wanted_list_find(network->wanted_list, p->hash);
  if (existing != NULL) {
    /* Case A: In-flight with active requesters — just add this actor */
    wanted_list_add(network->wanted_list, p->hash, p->reply_to);
    return;
  }

  /* Case B or C: No active requesters (previously failed, or never searched) */
  wanted_list_add(network->wanted_list, p->hash, p->reply_to);

  /* 3. Execute FindBlock routing */
  find_block_state_t state;
  memset(&state, 0, sizeof(state));
  /* ... populate state from p->hash, default TTL, empty visited_bloom, path = [local_id] ... */
  memcpy(state.block_hash, p->hash->data, 32);
  state.ttl = FIND_BLOCK_DEFAULT_TTL;
  memcpy(&state.original_source, &network->authority->local_id, sizeof(node_id_t));
  memcpy(&state.path[0], &network->authority->local_id, sizeof(node_id_t));
  state.path_len = 1;

  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;
  find_block_result_e result = find_block_execute(
      &network->eabf_table, &network->eabf_ttl, &network->conn_mgr,
      network->rings, &network->authority->local_id, &state,
      next_hops, &next_hop_count);

  switch (result) {
    case FIND_BLOCK_FOUND: {
      /* Shouldn't happen after index_peek check, but handle defensively */
      wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, p->hash);
      network_find_block_result_payload_t found_result = {
        .hash = p->hash, .found = 1
      };
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
    }
    case FIND_BLOCK_FORWARDING: {
      /* Send WIRE_FIND_BLOCK to selected next-hops */
      for (size_t i = 0; i < next_hop_count; i++) {
        /* ... send to next_hops[i] via QUIC when wired up ... */
      }
      break;
    }
    case FIND_BLOCK_NOT_FOUND:
    case FIND_BLOCK_TTL_EXPIRED: {
      /* Notify all requesters with found=0, clear requesters but keep bloom */
      network_find_block_result_payload_t not_found_result = {
        .hash = p->hash, .found = 0
      };
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
}
```

- [ ] **Step 2: Extend network_handle_find_block_response for wanted_list**

The existing `network_handle_find_block_response` (handles `NETWORK_FIND_BLOCK_RESPONSE` from peers) needs to check the wanted list and notify waiting streams. Add at the end of the `if (response->found)` block:

```c
/* Remove from wanted list and notify all waiting requesters */
wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, /* block_hash from response */);
if (requesters != NULL) {
  network_find_block_result_payload_t result = { .hash = /* block_hash */, .found = 1 };
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

And in the `else` (not found) block, add:

```c
/* Clear requesters but keep bloom entry for recall matching */
wanted_requester_t* requesters = wanted_list_clear_requesters(network->wanted_list, /* block_hash */);
if (requesters != NULL) {
  network_find_block_result_payload_t result = { .hash = /* block_hash */, .found = 0 };
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

- [ ] **Step 3: Wire up LOCAL_FIND_BLOCK dispatch and add wanted_list check to StoreBlock/RecallAccept**

Add dispatch case for `NETWORK_LOCAL_FIND_BLOCK`:
```c
case NETWORK_LOCAL_FIND_BLOCK:
  network_handle_local_find_block(network, msg);
  break;
```

In `network_handle_store_block` (existing), after `store_block_execute` returns `STORE_BLOCK_ACCEPTED`, add wanted_list check:
```c
/* Check if any local streams are waiting for this block */
wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, store->block_hash);
if (requesters != NULL) {
  network_find_block_result_payload_t result = { .hash = /* wrap block_hash */, .found = 1 };
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

Same pattern in `network_handle_recall_accept` after storing the block.

- [ ] **Step 4: Build and verify**

Run: `cd build && cmake --build .`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add src/Network/network.c
git commit -m "feat: implement local FindBlock handler with wanted_list integration"
```

---

### Task 4: Readable Streams — Network Fetch on Cache Miss

**Files:**
- Modify: `src/OFFStreams/readable_off_stream.h`
- Modify: `src/OFFStreams/readable_off_stream.c`
- Modify: `src/OFFStreams/readable_descriptor.h`
- Modify: `src/OFFStreams/readable_descriptor.c`
- Modify: `src/OFFStreams/block_recipe.h`
- Modify: `src/OFFStreams/block_recipe.c`

- [ ] **Step 1: Add network_t* field and state enum to readable_off_stream**

In `readable_off_stream.h`, add:
```c
#include "../Network/network.h"

typedef enum {
    OFF_STREAM_FETCHING_BLOCKS,
    OFF_STREAM_AWAITING_NETWORK,
    OFF_STREAM_AWAITING_TUPLE,
} off_stream_state_e;

/* Add to readable_off_stream_t struct: */
    network_t* network;
    buffer_t* pending_fetch_hash;
    off_stream_state_e state;
```

Update `readable_off_stream_create` signature to accept `network_t* network` parameter (default NULL for local-only mode).

- [ ] **Step 2: Add NETWORK_LOCAL_FIND_BLOCK dispatch on cache miss**

In `readable_off_stream.c`, in the `CACHE_GET_RESULT` handler where `result->block == NULL`, replace the deactivation with:
```c
if (result->block == NULL) {
    if (stream->network != NULL) {
        /* Network-aware: send NETWORK_LOCAL_FIND_BLOCK */
        stream->state = OFF_STREAM_AWAITING_NETWORK;
        stream->pending_fetch_hash = (buffer_t*)refcounter_reference((refcounter_t*)result->hash);
        network_local_find_block_payload_t payload;
        payload.hash = stream->pending_fetch_hash;
        payload.reply_to = &stream->stream.actor;
        message_t msg;
        msg.type = NETWORK_LOCAL_FIND_BLOCK;
        msg.payload = &payload;
        msg.payload_destroy = NULL;
        actor_send(&stream->network->actor, &msg);
    } else {
        /* Local-only: deactivate as before */
        stream_deactivate((stream_t*)stream, ERROR("Block not found in cache"));
    }
}
```

- [ ] **Step 3: Add NETWORK_FIND_BLOCK_RESULT handler**

In `readable_off_stream_dispatch`, add:
```c
case NETWORK_FIND_BLOCK_RESULT: {
    network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
    if (result->found) {
        block_cache_get(stream->bc, stream->pending_fetch_hash, &stream->stream.actor);
        stream->state = OFF_STREAM_FETCHING_BLOCKS;
    } else {
        stream_deactivate((stream_t*)stream, ERROR("Block not found on network"));
    }
    if (stream->pending_fetch_hash != NULL) {
        DESTROY(stream->pending_fetch_hash, buffer);
        stream->pending_fetch_hash = NULL;
    }
    break;
}
```

- [ ] **Step 4: Repeat for readable_descriptor**

Add `network_t* network`, `buffer_t* pending_fetch_hash`, and state enum to `readable_descriptor_t`. Same pattern: on cache miss, send `NETWORK_LOCAL_FIND_BLOCK` if network is available, handle `NETWORK_FIND_BLOCK_RESULT`.

- [ ] **Step 5: Repeat for recycler_recipe**

Add `network_t* network`, `buffer_t* pending_fetch_hash`, and state enum to `recycler_recipe_t`. Same pattern. On descriptor miss, skip to next ORI. On data block miss, deactivate. Send `NETWORK_LOCAL_FIND_BLOCK` when network is available.

- [ ] **Step 6: Update constructor calls in tests**

Update all `readable_off_stream_create`, `readable_descriptor_create`, and `recycler_recipe_create` calls in test files to pass `NULL` for the new `network_t*` parameter. This preserves backward compatibility for local-only mode.

- [ ] **Step 7: Build and run existing tests**

Run: `cd build && cmake --build . && ./test/testliboffs --gtest_filter="*BlockCache*:*BlockRecipe*:*OffStream*"`
Expected: All existing tests pass (network=NULL means local-only behavior)

- [ ] **Step 8: Commit**

```bash
git add src/OFFStreams/readable_off_stream.h src/OFFStreams/readable_off_stream.c src/OFFStreams/readable_descriptor.h src/OFFStreams/readable_descriptor.c src/OFFStreams/block_recipe.h src/OFFStreams/block_recipe.c test/
git commit -m "feat: add network fetch path to readable streams on cache miss"
```

---

### Task 5: Writeable Streams — Network Announce on Cache Put

**Files:**
- Modify: `src/OFFStreams/writeable_off_stream.h`
- Modify: `src/OFFStreams/writeable_off_stream.c`
- Modify: `src/OFFStreams/writeable_descriptor.h`
- Modify: `src/OFFStreams/writeable_descriptor.c`

- [ ] **Step 1: Add network_t* field to writeable_off_stream**

In `writeable_off_stream.h`, add `#include "../Network/network.h"` and `network_t* network;` field to `writeable_off_stream_t`. Update `writeable_off_stream_create` signature to accept `network_t* network` parameter.

- [ ] **Step 2: Handle CACHE_PUT_RESULT in writeable_off_stream**

In `writeable_off_stream.c`, change the `block_cache_put` calls for the off block from fire-and-forget (`reply_to=NULL`) to async with `reply_to=&stream->stream.actor`. Add a `CACHE_PUT_RESULT` handler that checks `result->result == CACHE_PUT_NEW` and sends `NETWORK_LOCAL_STORE_BLOCK` if `stream->network != NULL`.

- [ ] **Step 3: Add network_t* field to writeable_descriptor**

Same pattern as step 1 for `writeable_descriptor_t`.

- [ ] **Step 4: Handle CACHE_PUT_RESULT in writeable_descriptor**

Change `block_cache_put(desc->bc, block, 0, NULL)` to `block_cache_put(desc->bc, block, 0, &desc->stream.actor)`. Add `CACHE_PUT_RESULT` handler that checks for `CACHE_PUT_NEW` and sends `NETWORK_LOCAL_STORE_BLOCK`.

- [ ] **Step 5: Update test constructor calls**

Pass `NULL` for `network_t*` in all existing tests.

- [ ] **Step 6: Build and run existing tests**

Run: `cd build && cmake --build . && ./test/testliboffs --gtest_filter="*BlockCache*:*BlockRecipe*:*OffStream*"`
Expected: All existing tests pass

- [ ] **Step 7: Commit**

```bash
git add src/OFFStreams/writeable_off_stream.h src/OFFStreams/writeable_off_stream.c src/OFFStreams/writeable_descriptor.h src/OFFStreams/writeable_descriptor.c test/
git commit -m "feat: add LOCAL_STORE_BLOCK announce path to writeable streams on CACHE_PUT_NEW"
```

---

### Task 6: Wire Protocol — Inline Block Data in Responses

**Files:**
- Modify: `src/Network/wire.h`
- Modify: `src/Network/wire.c`

- [ ] **Step 1: Add block_data fields to wire_find_block_response_t**

In `wire.h`, add three fields to `wire_find_block_response_t`:
```c
uint8_t* block_data;       /* block content (NULL if not found) */
size_t   block_data_len;   /* length of block data */
uint32_t block_fib;        /* FIB counter from the holder */
```

- [ ] **Step 2: Add block_data fields to wire_recall_accept_t**

In `wire.h`, add to `wire_recall_accept_t`:
```c
uint8_t* block_data;       /* the requested block's data */
size_t   block_data_len;  /* length of block data */
uint32_t block_fib;        /* FIB counter for the block */
```

- [ ] **Step 3: Update CBOR encode for wire_find_block_response**

In `wire.c`, update `wire_find_block_response_encode` to conditionally encode `block_data` (only when `found=true` and `block_data != NULL`). Use a CBOR array with the existing fields plus optional block fields.

- [ ] **Step 4: Update CBOR decode for wire_find_block_response**

In `wire.c`, update `wire_find_block_response_decode` to extract the optional `block_data`, `block_data_len`, and `block_fib` fields from the CBOR array. Set `block_data = NULL` and `block_data_len = 0` when not present.

- [ ] **Step 5: Update CBOR encode/decode for wire_recall_accept**

Same pattern: conditionally encode `block_data`, `block_data_len`, `block_fib`.

- [ ] **Step 6: Add destroy functions for updated structs**

Update `wire_find_block_response_destroy` and `wire_recall_accept_destroy` to free `block_data` if non-NULL.

- [ ] **Step 7: Write CBOR round-trip test**

Add tests in `test/test_network.cpp` that encode and decode `wire_find_block_response_t` with and without block data, and `wire_recall_accept_t` with block data.

- [ ] **Step 8: Build, run tests, commit**

```bash
git add src/Network/wire.h src/Network/wire.c test/test_network.cpp
git commit -m "feat: add inline block data to FindBlock response and RecallAccept wire protocol"
```

---

### Task 7: Integration Tests

**Files:**
- Create: `test/test_stream_network.cpp`

- [ ] **Step 1: Write integration test for readable_off_stream network fetch**

Test that when `block_cache_get` returns NULL and `network` is provided, the stream sends `NETWORK_LOCAL_FIND_BLOCK`, and when `NETWORK_FIND_BLOCK_RESULT(found=true)` arrives, the stream re-issues `block_cache_get` and continues.

- [ ] **Step 2: Write integration test for network fetch failure**

Test that when `NETWORK_FIND_BLOCK_RESULT(found=false)` arrives, the stream deactivates with an error.

- [ ] **Step 3: Write integration test for writeable_off_stream announce**

Test that when `block_cache_put` returns `CACHE_PUT_NEW` and `network` is provided, the stream sends `NETWORK_LOCAL_STORE_BLOCK`.

- [ ] **Step 4: Write integration test for writeable_off_stream no-announce on CACHE_PUT_EXISTS**

Test that when `block_cache_put` returns `CACHE_PUT_EXISTS`, the stream does NOT send `NETWORK_LOCAL_STORE_BLOCK`.

- [ ] **Step 5: Write test for wanted_list dedup**

Test that when two streams request the same hash, only one `WIRE_FIND_BLOCK` is sent and both streams receive the result.

- [ ] **Step 6: Run all tests**

Run: `cd build && cmake --build . && ./test/testliboffs`
Expected: All tests pass, no memory leaks

- [ ] **Step 7: Commit**

```bash
git add test/test_stream_network.cpp
git commit -m "test: add integration tests for stream-network block fetch and announce"
```

---

### Task 8: De-Wonk and Memory Leak Check

- [ ] **Step 1: Run full test suite with ASan**

Run: `cd build && cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -g" .. && cmake --build . && ./test/testliboffs`
Expected: All tests pass, no memory leaks

- [ ] **Step 2: Audit all new code for memory leaks**

Check that every `refcounter_reference` has a matching `refcounter_yield` or `DESTROY`, every `get_clear_memory` has a matching `free`, every `buffer_create` has a matching `buffer_destroy`, and all `wanted_list_add` entries are properly cleaned up on all code paths.

- [ ] **Step 3: Audit all new code for use-after-free**

Check that `wanted_list_remove` and `wanted_list_clear_requesters` callers free the returned requester lists. Check that `pending_fetch_hash` is properly cleaned up in all stream dispatch paths.

- [ ] **Step 4: Fix any issues found**

- [ ] **Step 5: Commit**

```bash
git commit -m "fix: address de-wonk findings in stream-network integration"
```