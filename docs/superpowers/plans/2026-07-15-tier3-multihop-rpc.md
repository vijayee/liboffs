# Tier-3 Multi-hop RPC Hangs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make multi-hop FindBlock and ClosestNodes reliable on a healthy network — a block-absent GET or any peer churn no longer hangs the caller forever. Four audit findings (#5 not-found never reaches origin; #6 colliding message IDs + no timeout; #9 no timeout on pending finds + ignored send returns; #10 first-response-wins aborts a still-live search) all stem from the same root: no per-pending-request timeout, plus path-relay and message-ID gaps.

**Architecture:** Five focused fixes, each landing as its own commit. Task 1 (monotonic message IDs) and Task 2 (timeout infrastructure: deadlines + a dedicated sweep tick) are the backbone — they unblock Tasks 3-5. Task 3 relays not-found upstream so the origin learns of failure. Task 4 tracks fanout so the origin only fails after all branches report (or a timeout). Task 5 checks `conn_state_send` so an unreachable next hop fails the request immediately instead of waiting for the timeout.

**Tech Stack:** C11, `ATOMIC(...)` / `__atomic_*`, platform timer (`timer_actor_set`), GoogleTest, CMake, valgrind (DWARF4 build at `cmake-build-vg/`).

**Scope (in):** `docs/liboffs-audit-report.md` findings #5, #6, #9, #10.

**Scope (out — deferred):** audit #8/#11 (identity/TLS), #15-17/#19/#20/#24 (CLI lies), #18 (NAT/relay), #21-22/#23 (MEDIUM), concurrency F8-F11. Those belong to separate tiers.

**Build / test commands:**
```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='<filter>'
test -d cmake-build-vg || cmake -S . -B cmake-build-vg -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-gdwarf-4 -O0" -DCMAKE_CXX_FLAGS="-gdwarf-4 -O0"
cmake --build cmake-build-vg -j$(nproc) --target testliboffs
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='<filter>'
```

**Style reminder (from `docs/STYLE_GUIDE.md` + CLAUDE.md):** `_t` suffix, `type_action()`, no single-letter names, no TODO/FIXME in completed work, no `Co-Authored-By` in commits, use de-wonk before marking done, check valgrind for leaks.

**Known valgrind noise (acceptable, pre-existing):** the 320-byte + 5120-byte msquic "possibly lost" TLS blocks; the 136-byte `block_handlers.c` definitely-lost (pre-existing at tier-2 base). `definitely lost` must not grow from tier-3.

---

## File Structure

| File | Responsibility | Touched by task |
| --- | --- | --- |
| `src/Network/network.h` | Add `ATOMIC(uint64_t) next_message_id`; add `NETWORK_REQUEST_TIMEOUT_TICK` message type; add `request_timeout_ms` config; add `request_timer_id` field. Add `deadline_ms`/`fanout_count`/`not_found_count` to `wanted_entry_t` (in `wanted_list.h`). Add `deadline_ms` to `closest_nodes_pending_t`. | Tasks 1, 2 |
| `src/Network/network.c` | Init `next_message_id`; replace `time(NULL) * 1000` message_id sites with `atomic_fetch_add`; add the request-timeout tick handler (sweeps wanted_list + closest_pending); start the timer in `network_create`, cancel in `network_destroy`. Path relay for not-found in `network_handle_find_block_response` found==0 branch. Fanout counting in the origin's not-found handler. `conn_state_send` return checks at forwarding sites. | Tasks 1, 2, 3, 4, 5 |
| `src/Network/wanted_list.h`, `wanted_list.c` | Add `deadline_ms`/`fanout_count`/`not_found_count` fields; add `wanted_list_sweep(wl, now_ms)` returning expired entries; add `wanted_list_get` to read an entry (for fanout increment). | Task 2, 4 |
| `test/test_network.cpp` (or `test_rpc_integration.cpp`) | Tests: message-ID uniqueness; wanted_list timeout; closest_pending timeout; not-found reaches origin; fanout-count fail; conn_state_send fail. | Tasks 1-5 |

---

## Task 1: Monotonic message ID counter

**Files:**
- Modify: `src/Network/network.h` (add `ATOMIC(uint64_t) next_message_id` to `network_t`), `src/Network/network.c` (init; replace `time(NULL) * 1000` message_id sites)
- Test: `test/test_network.cpp`

**Why:** `message_id = (uint64_t)time(NULL) * 1000` (at `network.c:1237`, `2787`, and other sites) is second-granularity — two queries in the same wall-clock second get identical IDs. `closest_pending` is keyed on `message_id`; the remove returns the first match → colliding queries cross-deliver and orphan an entry. Fix: a per-node monotonic atomic counter.

### Step 1: Write the failing test

Add a unit test that generates many message IDs in a tight loop and asserts no duplicates. Since the counter is inside `network_t`, either expose a test-only helper `uint64_t network_next_message_id_for_test(network_t*)` (gated by `#ifndef NDEBUG`, like the `quic_listener` test accessors in tier-1) OR test via two `network_handle_local_closest_nodes` calls in the same second and assert distinct `message_id`s on the wire (heavier). Prefer the test-only helper if clean.

```cpp
TEST(NetworkMessageId, MonotonicCounterNoCollisions) {
  network_t* network = test_network_create_minimal();  // adapt to existing fixture
  ASSERT_NE(network, nullptr);
  std::set<uint64_t> ids;
  for (int i = 0; i < 10000; i++) {
    ids.insert(network_next_message_id_for_test(network));
  }
  EXPECT_EQ(ids.size(), (size_t)10000) << "message IDs must not collide";
  test_network_destroy(network);
}
```

### Step 2: Run to verify it fails

Pre-fix, `network_next_message_id_for_test` doesn't exist; and the underlying `time(NULL) * 1000` would produce duplicates for same-second calls (the 10000-loop would all get the same ID in the same second).

### Step 3: Add the counter + helper

In `src/Network/network.h`, add to `network_t`:

```c
  ATOMIC(uint64_t) next_message_id;  /* monotonic per-node counter; avoids the
                                         time(NULL)*1000 second-granularity
                                         collisions that cross-delivered
                                         closest_pending entries. See audit #6. */
```

In `src/Network/network.c`, `network_create` (after the struct is initialized), seed it:

```c
  atomic_store(&network->next_message_id, (uint64_t)time(NULL) * 1000);
```

(Seed with the wall-clock so the counter starts roughly time-aligned, then increment monotonically.)

Add a static helper (and the test-only wrapper):

```c
static uint64_t network_next_message_id(network_t* network) {
  return atomic_fetch_add(&network->next_message_id, 1, memory_order_relaxed);
}

#ifndef NDEBUG
uint64_t network_next_message_id_for_test(network_t* network) {
  return network_next_message_id(network);
}
#endif
```

(Declare `network_next_message_id_for_test` in `network.h` under `#ifndef NDEBUG`.)

### Step 4: Replace the `time(NULL) * 1000` message_id sites

Grep for `message_id = .*time\(NULL\)` and `wire_query.message_id = now_ts` / `state.message_id = now_ts` where `now_ts = (uint64_t)time(NULL) * 1000`. Replace the message_id assignment with `network_next_message_id(network)`. Keep the `now_ts` for `start_time` / `start_time_ms` (those are timestamps, not IDs). Sites to update (from the grep):
- `network.c:1237-1238` (`network_handle_local_closest_nodes`): `wire_query.message_id = now_ts;` → `wire_query.message_id = network_next_message_id(network);` (keep `wire_query.start_time = now_ts;` at 1249).
- `network.c:2786-2787` (`network_handle_local_find_block`): `state.message_id = now_ts;` → `state.message_id = network_next_message_id(network);` (keep `state.start_time_ms = now_ts;` at 2790).
- Any other `message_id = now_ts` or `message_id = (uint64_t)time(NULL) * 1000` sites — grep and replace. (Check StoreBlock, FindNode, etc. — grep `message_id = ` in network.c.)

### Step 5: Run the test to verify it passes

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='NetworkMessageId.*'
```
Expected: PASS (10000 unique IDs).

### Step 6: Run the existing network/rpc tests + valgrind

```bash
cmake-build-verify/test/testliboffs --gtest_filter='TestNetwork.*:TestRpcIntegration.*:TestRecyclerWire.*:NetworkFindBlockResultPayload.*'
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='NetworkMessageId.*:TestNetwork.*:TestRpcIntegration.*'
```
Expected: PASS, 0 leaks.

### Step 7: Commit

```bash
git add src/Network/network.h src/Network/network.c test/test_network.cpp
git commit -m "fix(network): monotonic per-node message ID counter

message_id was time(NULL) * 1000 — second-granularity, so two queries in
the same wall-clock second collided. closest_pending is keyed on
message_id and the remove returns the first match, so colliding queries
cross-delivered and orphaned an entry. Add an ATOMIC(uint64_t) per-node
counter seeded from the wall clock and incremented via fetch_add; replace
the time(NULL)*1000 message_id sites (ClosestNodes, FindBlock, StoreBlock,
etc.) with the counter. start_time/start_time_ms still use the wall clock.
See audit #6."
```

---

## Task 2: Pending-request timeout infrastructure

**Files:**
- Modify: `src/Network/wanted_list.h`, `wanted_list.c` (add deadline + sweep), `src/Network/network.h` (add `NETWORK_REQUEST_TIMEOUT_TICK` + `request_timeout_ms` + `request_timer_id`), `src/Network/network.c` (init, start, cancel, sweep handler)
- Test: `test/test_network.cpp` or `test/test_wanted_list.cpp`

**Why:** There is no wanted_list expiry and no request timeout anywhere (#5, #9). A lost response or dead hop means the origin's wanted_list entry and the requesting stream actor persist for the process lifetime. `closest_pending` (for ClosestNodes, #6) has the same no-timeout issue. Fix: add a deadline to each pending entry and a dedicated sweep tick.

### Step 1: Write the failing test

A test that adds a wanted_list entry with a short deadline, runs the sweep, and asserts the entry is removed and the requester is notified. Adapt to the existing wanted_list test fixture (grep `test/` for `wanted_list`).

```cpp
TEST(WantedListTimeout, SweepExpiresEntriesAndReturnsRequesters) {
  wanted_list_t* wl = wanted_list_create();
  buffer_t* hash = buffer_create_from_pointer_copy(sample_hash_32, 32);
  actor_t discard; int count = 0;
  actor_init(&discard, &count, test_discard_dispatch, NULL);
  wanted_list_add_with_deadline(wl, hash, &discard, /*deadline_ms=*/ now + 100);  // 100ms
  // Wait past the deadline.
  platform_sleep_ms(150);
  uint64_t now_ms = (uint64_t)time(NULL) * 1000;
  wanted_requester_t* expired = wanted_list_sweep(wl, now_ms);
  ASSERT_NE(expired, nullptr);
  // The expired entry's requester is the discard actor.
  EXPECT_EQ(expired->actor, &discard);
  wanted_requester_list_destroy(expired);
  buffer_destroy(hash);
  wanted_list_destroy(wl);
}
```

(Adapt `wanted_list_add_with_deadline` — see Step 3; or pass the deadline to `wanted_list_add` via a new param. The existing `wanted_list_add(wl, hash, actor)` signature is used by `network.c:2781` — either add a deadline param to `wanted_list_add` (update the call site) or add a new `wanted_list_add_with_deadline`.)

### Step 2: Run to verify it fails

`wanted_list_sweep` and the deadline field don't exist — won't compile / fails.

### Step 3: Add deadline + sweep to wanted_list

In `src/Network/wanted_list.h`, add fields to `wanted_entry_t`:

```c
typedef struct wanted_entry_t {
  buffer_t* hash;
  wanted_requester_t* requesters;
  uint64_t deadline_ms;      /* 0 = no deadline (back-compat) */
  uint8_t  fanout_count;     /* # of next-hops the origin fanned out to */
  uint8_t  not_found_count;  /* # of not-found responses received */
  struct wanted_entry_t* next;
} wanted_entry_t;
```

Add the sweep + a getter (declare after the existing functions):

```c
/* Returns the linked list of requesters for entries whose deadline_ms has
   passed (deadline_ms != 0 && deadline_ms <= now_ms). Removes those entries.
   Entries with deadline_ms == 0 are never swept (back-compat for callers
   that don't set a deadline). See audit #5/#9. */
wanted_requester_t* wanted_list_sweep(wanted_list_t* wl, uint64_t now_ms);

/* Get (without removing) an entry, for fanout/not_found accounting. */
wanted_entry_t* wanted_list_get(wanted_list_t* wl, buffer_t* hash);
```

Change `wanted_list_add` to take a deadline:

```c
void wanted_list_add(wanted_list_t* wl, buffer_t* hash, actor_t* actor, uint64_t deadline_ms);
```

In `src/Network/wanted_list.c`, update `wanted_list_add` to store the deadline (init `fanout_count=0`, `not_found_count=0`). Add the sweep + getter:

```c
wanted_entry_t* wanted_list_get(wanted_list_t* wl, buffer_t* hash) {
  return wanted_list_find(wl, hash);  /* alias for clarity at call sites */
}

wanted_requester_t* wanted_list_sweep(wanted_list_t* wl, uint64_t now_ms) {
  if (wl == NULL) return NULL;
  wanted_requester_t* expired_head = NULL;
  wanted_requester_t** expired_tail = &expired_head;
  wanted_entry_t** current = &wl->entries;
  while (*current != NULL) {
    wanted_entry_t* entry = *current;
    if (entry->deadline_ms != 0 && entry->deadline_ms <= now_ms) {
      // Unlink the entry, append its requesters to the expired list.
      *current = entry->next;
      wl->entry_count--;
      wanted_requester_t* req = entry->requesters;
      // Append req list to expired_tail.
      while (*expired_tail != NULL) expired_tail = &(*expired_tail)->next;
      *expired_tail = req;
      buffer_destroy(entry->hash);
      free(entry);
    } else {
      current = &entry->next;
    }
  }
  return expired_head;
}
```

Update the `wanted_list_add` call site in `network.c:2781` to pass a deadline:

```c
  uint64_t deadline_ms = (uint64_t)time(NULL) * 1000 + network->request_timeout_ms;
  wanted_list_add(network->wanted_list, payload->hash, payload->reply_to, deadline_ms);
```

### Step 4: Add the request-timeout tick

In `src/Actor/message.h`, add `NETWORK_REQUEST_TIMEOUT_TICK` to the `message_type_e` enum (near `NETWORK_GOSSIP_TICK`).

In `src/Network/network.h`, add to `network_t`:

```c
  ATOMIC(uint64_t) request_timer_id;
  uint32_t request_timeout_ms;  /* default e.g. 30000 (30s) */
```

In `src/Network/network.c`, `network_create` (after the gossip timer setup ~line 180), start the request-timeout timer (1s sweep interval):

```c
  network->request_timeout_ms = 30000;  /* 30s default */
  network->request_timer_id = 0;
  timer_actor_set(timer,
      1000,  /* 1s sweep */
      1000,
      &network->actor,
      NETWORK_REQUEST_TIMEOUT_TICK,
      &network->request_timer_id);
```

In `network_destroy` (near the gossip timer cancel ~line 281), cancel it:

```c
  if (atomic_load(&network->request_timer_id) != 0) {
    timer_actor_cancel(network->timer, atomic_load(&network->request_timer_id));
  }
```

Add the handler + dispatch case:

```c
static void network_handle_request_timeout_tick(network_t* network, message_t* msg) {
  (void)msg;
  uint64_t now_ms = (uint64_t)time(NULL) * 1000;

  // Sweep wanted_list — notify expired requesters with a timeout (found=0).
  wanted_requester_t* expired = wanted_list_sweep(network->wanted_list, now_ms);
  while (expired != NULL) {
    wanted_requester_t* next = expired->next;
    // The requester's hash: we don't have it here. Either store the hash in
    // the requester, or pass it back from sweep. (See Step 5 — adjust the
    // sweep to return the hash with each requester.)
    // For now, send a found=0 result without the hash (the requester should
    // handle a NULL hash as a timeout).
    network_find_block_result_payload_t* result =
        get_clear_memory(sizeof(network_find_block_result_payload_t));
    result->hash = NULL;  // timeout — no hash
    result->found = 0;
    message_t result_msg = {0};
    result_msg.type = NETWORK_FIND_BLOCK_RESULT;
    result_msg.payload = result;
    result_msg.payload_destroy = network_find_block_result_destroy;
    actor_send(expired->actor, &result_msg);
    free(expired);
    expired = next;
  }

  // Sweep closest_pending — signal expired reply_to actors.
  for (size_t idx = 0; idx < network->closest_pending_count; ) {
    if (network->closest_pending[idx].deadline_ms != 0 &&
        network->closest_pending[idx].deadline_ms <= now_ms) {
      actor_t* reply_to = network->closest_pending[idx].reply_to;
      // Remove (swap with last).
      network->closest_pending[idx] = network->closest_pending[network->closest_pending_count - 1];
      network->closest_pending_count--;
      // Signal the reply_to with a timeout result (the closest_nodes_result
      // payload with an empty/NULL result). Adapt to the existing result
      // payload shape — see how network_closest_pending_remove signals
      // reply_to at the success path (~line 1081).
      // ... send timeout result to reply_to ...
    } else {
      idx++;
    }
  }
}
```

In the dispatch switch (`network.c:3031` area), add:

```c
    case NETWORK_REQUEST_TIMEOUT_TICK:
      network_handle_request_timeout_tick(network, msg);
      break;
```

### Step 5: Pass the hash back from the sweep (for a proper timeout result)

The requester needs the hash to match the timeout to its pending request. Adjust the sweep to return the hash with each requester. Simplest: add a `buffer_t*` to `wanted_requester_t` (set only for swept entries) OR return a small struct `{buffer_t* hash; actor_t* actor;}`. OR: have the sweep call a callback per expired entry with `(hash, requesters)`. Pick the cleanest — a callback or a returned list of `{hash, requesters}` pairs. Update Step 3's `wanted_list_sweep` signature accordingly and Step 4's handler to use the hash (REFERENCE it into the result payload).

(Read how the existing `found=0` path at `network.c:1815-1834` builds the result — it uses `hash_buf` from the response. The timeout path should mirror that: `result->hash = REFERENCE(hash_buf, buffer_t)`. The sweep provides the hash.)

### Step 6: Add deadline to closest_pending

In `src/Network/network.h`, add to `closest_nodes_pending_t`:

```c
typedef struct closest_nodes_pending_t {
  uint64_t message_id;
  actor_t* reply_to;
  uint64_t deadline_ms;
} closest_nodes_pending_t;
```

In `network_closest_pending_add` (`network.c:1180`), set the deadline:

```c
  network->closest_pending[...].deadline_ms = (uint64_t)time(NULL) * 1000 + network->request_timeout_ms;
```

### Step 7: Run the tests to verify they pass

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='WantedListTimeout.*:NetworkMessageId.*:TestNetwork.*:TestRpcIntegration.*'
```
Expected: PASS. Run the timeout test 5× to confirm the 100ms deadline + 1s sweep reliably fires.

### Step 8: Run valgrind

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='WantedListTimeout.*:TestNetwork.*:TestRpcIntegration.*:TestShutdown.*'
```
Expected: `definitely lost: 0 bytes`, no new leaks (the timer + sweep must not leak the result payloads or the requesters).

### Step 9: Commit

```bash
git add src/Network/network.h src/Network/network.c src/Network/wanted_list.h src/Network/wanted_list.c src/Actor/message.h test/test_network.cpp
git commit -m "fix(network): per-pending-request timeout sweep for wanted_list + closest_pending

There was no wanted_list expiry and no request timeout anywhere: a lost
response or dead hop left the origin's wanted_list entry and the requesting
stream actor alive for the process lifetime (audit #5, #9). closest_pending
(ClosestNodes, #6) had the same no-timeout issue. Add a deadline_ms to
wanted_entry_t and closest_nodes_pending_t, a wanted_list_sweep that returns
expired entries' requesters, and a 1s NETWORK_REQUEST_TIMEOUT_TICK on the
network actor that sweeps both tables and notifies expired requesters with
a found=0 result. Default timeout 30s. See audit #5/#6/#9."
```

---

## Task 3: Relay not-found upstream so the origin learns of failure

**Files:**
- Modify: `src/Network/network.c` (`network_handle_find_block_response` found==0 branch, ~line 1807)
- Test: `test/test_network.cpp` or `test/test_rpc_integration.cpp`

**Why:** The `found==1` branch relays upstream by locating self in `response->path` (`:1722-1746`), but the `found==0` branch (`:1807`) only notifies local requesters via `wanted_list_clear_requesters` — and at an intermediate hop there's NO local wanted_list entry (only the origin registers one, `:2781`). So C's NOT_FOUND dies at B; A never learns. The `NOT_FOUND` response path (`:1595-1614`) sends it back one hop, but the receiving hop's `found==0` handler doesn't relay further. Fix: the `found==0` branch replicates the self-in-path upstream relay; only at the origin (self is `original_source` or path[0]) does it notify local requesters.

### Step 1: Write the failing test

A multi-hop test: A → B → C, block absent at C. C sends NOT_FOUND to B. Assert A receives the not-found (its wanted_list entry is cleared + the requester is notified). This requires a 3-node fixture OR a mocked 2-hop path. If the existing `test_rpc_integration.cpp` has a multi-hop fixture, use it; otherwise, this may need a valgrind-only / integration-test assertion. Document the approach.

### Step 2: Run to verify it fails (or the integration test hangs pre-fix)

Pre-fix, A's wanted_list entry persists (the GET hangs). The test would time out waiting for A's result.

### Step 3: Fix the found==0 branch

In `src/Network/network.c`, `network_handle_find_block_response`, the `else` (found==0) branch at `:1807`. Replace the block. The current code:

```c
  } else {
    // Block not found — subscribe block_hash in EABFs as negative info
    // This is handled by TTL_EXPIRED in the forwarding path

    // Check wanted_list — notify any local requesters that the block was not found
    {
      buffer_t* hash_buf = buffer_create_from_pointer_copy(response->block_hash, 32);
      if (hash_buf != NULL) {
        wanted_requester_t* requesters = wanted_list_clear_requesters(network->wanted_list, hash_buf);
        if (requesters != NULL) {
          // ... notify each requester with found=0 ...
        }
        buffer_destroy(hash_buf);
      }
    }
  }
```

Replace with: first relay upstream (same self-in-path logic as the found==1 branch at `:1722-1746`), then notify local requesters ONLY if self is the origin.

```c
  } else {
    // Block not found. Relay the not-found upstream so the origin learns
    // (the old code only notified local requesters, which exist only at the
    // origin — so an intermediate hop's not-found died there and the origin
    // hung forever). See audit #5.

    // Relay not-found upstream: find self in the path; if we're not the
    // first node, relay to the predecessor. This mirrors the found==1
    // upstream relay at line 1722-1746.
    int self_index = -1;
    for (int index = 0; index < (int)response->path_len; index++) {
      if (node_id_equals(&response->path[index], &network->authority->local_id)) {
        self_index = index;
        break;
      }
    }
    if (self_index > 0) {
      const node_id_t* predecessor = &response->path[self_index - 1];
      peer_connection_t* relay_peer = connection_manager_lookup(&network->conn_mgr, predecessor);
      if (relay_peer != NULL) {
        cbor_item_t* cbor = wire_find_block_response_encode(response);
        conn_state_send(network, relay_peer, cbor);
        cbor_decref(&cbor);
        if (network->log != NULL) {
          message_log_record(network->log, WIRE_FIND_BLOCK_RESPONSE, MSG_DIRECTION_FORWARDED,
                             predecessor, response->message_id, response->block_hash,
                             2, &network->hebbian);
        }
      }
    }

    // Only at the origin (self is the original_source, i.e. self_index == 0
    // or we're not in the path because we ARE the origin and the terminal
    // sent the not-found directly back to us) do we notify local requesters.
    bool is_origin = (self_index == 0) ||
                     node_id_equals(&response->original_source, &network->authority->local_id);
    if (is_origin) {
      // Fanout accounting (Task 4 will refine this to fail only after all
      // branches report). For now, notify requesters with found=0 — but
      // see Task 4: this becomes "increment not_found_count; fail only if
      // not_found_count >= fanout_count".
      buffer_t* hash_buf = buffer_create_from_pointer_copy(response->block_hash, 32);
      if (hash_buf != NULL) {
        wanted_entry_t* entry = wanted_list_get(network->wanted_list, hash_buf);
        if (entry != NULL) {
          entry->not_found_count++;
          if (entry->not_found_count >= entry->fanout_count) {
            wanted_requester_t* requesters = wanted_list_clear_requesters(network->wanted_list, hash_buf);
            // ... notify each requester with found=0 (existing code) ...
          }
        }
        buffer_destroy(hash_buf);
      }
    }
  }
```

(The fanout accounting in this block is Task 4's fix — include it here to avoid touching the same code twice, OR leave it as "notify immediately" and do the fanout accounting in Task 4. Pick one; the plan merges #5 and #10 handling into this branch since they touch the same code. If you prefer to keep tasks separate, leave the immediate-notify here and refine to fanout-counting in Task 4.)

### Step 4: Set fanout_count when the origin registers

In `network_handle_local_find_block` (`network.c:2781`), after `wanted_list_add`, set the fanout count:

```c
  wanted_list_add(network->wanted_list, payload->hash, payload->reply_to, deadline_ms);
  wanted_entry_t* entry = wanted_list_get(network->wanted_list, payload->hash);
  if (entry != NULL) {
    entry->fanout_count = (uint8_t)next_hop_count;  // the # of next-hops find_block_execute returned
    entry->not_found_count = 0;
  }
```

(If `next_hop_count` is 0 — no next hops — immediately fail the request with found=0, don't register.)

### Step 5: Run the tests to verify they pass

The multi-hop not-found test: A receives the not-found within the timeout (no hang). The first-response-wins test (Task 4): if a live branch's found=1 arrives after a dead branch's not-found, the origin succeeds (because the not-found didn't clear the entry — not_found_count < fanout_count).

### Step 6: Run valgrind

```bash
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestRpcIntegration.*:TestNetwork.*:TestShutdown.*'
```

### Step 7: Commit

```bash
git add src/Network/network.c test/test_network.cpp
git commit -m "fix(network): relay FindBlock not-found upstream so the origin learns

The found==0 branch only notified local requesters, which exist only at
the origin — so an intermediate hop's not-found died there and the origin
hung forever (its wanted_list entry and requesting stream persisted for
the process lifetime). Relay the not-found upstream using the same
self-in-path logic as the found==1 branch; only at the origin (self is
original_source) notify local requesters. Set fanout_count on the origin's
wanted_list entry and fail only after not_found_count >= fanout_count, so
a dead-end neighbor's not-found no longer aborts a still-live search before
a live branch's found=1 arrives. See audit #5/#10."
```

---

## Task 4: First-response-wins → fail only after all branches report or timeout

**Files:**
- Modify: `src/Network/network.c` (the origin's not-found handling — already partially done in Task 3)
- Test: `test/test_network.cpp` or `test/test_rpc_integration.cpp`

**Why:** FindBlock fans out to up to 3 next hops but the origin acts on the first response. A dead-end neighbor's NOT_FOUND removes the wanted entry and fails the stream before a live branch's found=1 arrives. Fix: the origin tracks the fanout count and only fails when all branches report not-found (or a timeout). This is the `not_found_count >= fanout_count` logic from Task 3 — if you already included it there, this task is the test + verification. If you kept Task 3 as immediate-notify, this task refines it to fanout-counting.

### Step 1: Write the failing test

A test where the origin fans out to 3 hops: hop1 returns not-found quickly, hop2 returns found=1 slowly. Pre-fix, the origin fails on hop1's not-found. Post-fix, the origin waits for hop2's found=1 and succeeds.

### Step 2-6: If Task 3 already included the fanout-counting, this task is the test + verification (run the test, confirm it passes, valgrind). If not, apply the `not_found_count >= fanout_count` refinement here (see Task 3 Step 3 for the code).

### Step 7: Commit

```bash
git add src/Network/network.c test/test_network.cpp
git commit -m "fix(network): fail FindBlock only after all branches report or timeout

FindBlock fans out to up to 3 next hops but the origin acted on the first
response — a dead-end neighbor's not-found removed the wanted entry and
failed the stream before a live branch's found=1 arrived (which then found
no entry -> block cached but GET already failed). Track fanout_count on
the origin's wanted_list entry and only clear+fail when not_found_count
>= fanout_count (or the timeout sweep from Task 2 expires). See audit #10."
```

---

## Task 5: Check `conn_state_send` return at forwarding sites

**Files:**
- Modify: `src/Network/network.c` (the forwarding sites in `network_handle_local_find_block`, `network_handle_find_block` forward path, `network_handle_closest_nodes` forward path)
- Test: `test/test_network.cpp`

**Why:** `conn_state_send` returns −1 when a peer's stream is gone, but every relay call site ignores it. A mid-request hop death or dropped datagram silently evaporates the response; the origin's entry and stream never resolve (until the Task 2 timeout fires, which is slow). Fix: check the return; if the next hop is unreachable, fail the request immediately (at the origin) or send a not-found back (at an intermediate hop, so the origin learns sooner).

### Step 1: Write the failing test

A test where the origin's only next hop has a dead stream (`conn_state_send` returns −1). Assert the origin's request fails immediately (not after the 30s timeout).

### Step 2: Run to verify it fails (pre-fix, the request hangs for 30s)

### Step 3: Check the return at forwarding sites

Grep for `conn_state_send(network,` in `network.c`. At each forwarding site:
- **Origin's `network_handle_local_find_block`** (where `find_block_execute` returns next_hops and the code forwards): after forwarding to each next hop, if ALL sends failed (no reachable next hop), immediately fail the request (notify the requester with found=0, remove the wanted_list entry). If some sends succeeded, keep the wanted_list entry (the timeout catches the failed hops).
- **Intermediate `network_handle_find_block` forwarding** (where the FindBlock is relayed onward): if the forward send fails, send a not-found back along the path (so the origin learns sooner, not via the 30s timeout).
- **`network_handle_closest_nodes` forwarding**: same — if the forward fails, send a closest_nodes_response with an empty result back to the reply_to.

Read each forwarding site and add the return check. Example for the origin:

```c
  size_t reachable_hops = 0;
  for (size_t hop = 0; hop < next_hop_count; hop++) {
    peer_connection_t* next_peer = connection_manager_lookup(&network->conn_mgr, &next_hops[hop]->id);
    if (next_peer != NULL) {
      cbor_item_t* cbor = wire_find_block_encode(&forward);
      if (conn_state_send(network, next_peer, cbor) == 0) {
        reachable_hops++;
      }
      cbor_decref(&cbor);
    }
  }
  if (reachable_hops == 0) {
    // No reachable next hop — fail the request immediately.
    wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, payload->hash);
    // notify each requester with found=0 ...
    wanted_requester_list_destroy(requesters);
    return;
  }
  // Update fanout_count to reachable_hops (so not_found_count >= fanout_count
  // is achievable; the unreachable hops won't send not-founds).
  wanted_entry_t* entry = wanted_list_get(network->wanted_list, payload->hash);
  if (entry != NULL) entry->fanout_count = (uint8_t)reachable_hops;
```

### Step 4: Run the test to verify it passes (immediate fail, no 30s wait)

### Step 5: Run valgrind

### Step 6: Commit

```bash
git add src/Network/network.c test/test_network.cpp
git commit -m "fix(network): check conn_state_send return at forwarding sites

conn_state_send returns -1 when a peer's stream is gone, but every relay
call site ignored it — a mid-request hop death silently evaporated the
response and the origin hung until the 30s timeout (audit #9). Check the
return: at the origin, if no next hop is reachable, fail the request
immediately; at an intermediate hop, if the forward fails, send a not-found
back along the path so the origin learns sooner. Update fanout_count to
the reachable hop count so the not_found_count >= fanout_count check is
achievable. See audit #9."
```

---

## Task 6: Whole-tier verification

**Files:** none (verification only)

- [ ] Step 1: Build. `cmake --build cmake-build-verify -j$(nproc) --target testliboffs` — expect clean, no warnings on tier-3 files.
- [ ] Step 2: Full test suite. `cmake-build-verify/test/testliboffs` — expect all pass (modulo pre-existing SSL cert failures). No NEW failures.
- [ ] Step 3: Valgrind sweep. `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='NetworkMessageId.*:WantedListTimeout.*:TestNetwork.*:TestRpcIntegration.*:TestRecyclerWire.*:NetworkFindBlockResultPayload.*:TestShutdown.*:TestBlockCache.*'` — expect `definitely lost: 0 bytes` (no growth from the pre-existing 136-byte `block_handlers.c` leak), 0 invalid reads. The new timer + sweep must not leak.
- [ ] Step 4: De-wonk audit on tier-3 files.
- [ ] Step 5: TODO check. `git log master..HEAD --pickaxe-regex -S'TODO\|FIXME\|HACK\|XXX' -- <tier-3 files>` — expect empty.
- [ ] Step 6: Leak check (covered by step 3).
- [ ] Step 7: Final commit if de-wonk/leak found fixes.

---

## Self-Review

**1. Spec coverage.** Tier-3 scope = audit #5, #6, #9, #10.
- #6 (colliding message IDs) → Task 1 ✓
- #5 (not-found never reaches origin) → Task 3 ✓
- #9 (no timeout on pending finds) → Task 2 (timeout sweep) + Task 5 (conn_state_send check) ✓
- #10 (first-response-wins) → Task 4 (fanout counting) ✓
- Timeout sweep (shared #5/#6/#9) → Task 2 ✓
- Whole-tier verification → Task 6 ✓

**2. Placeholder scan.** Every code step shows the actual code or the exact change. Where a test helper is referenced (`test_network_create_minimal`, `test_discard_dispatch`), the step says to adapt to existing infrastructure. The multi-hop test (Task 3) may require a 3-node fixture — the step allows a valgrind-only / integration fallback if the fixture is too heavy.

**3. Type / signature consistency.**
- `wanted_list_add` signature changes to `(wl, hash, actor, deadline_ms)` — update the call site at `network.c:2781`. `wanted_list_sweep` and `wanted_list_get` are new.
- `closest_nodes_pending_t` gains `deadline_ms`. `network_closest_pending_add` sets it.
- `NETWORK_REQUEST_TIMEOUT_TICK` is added to the same enum as `NETWORK_GOSSIP_TICK`. The dispatch case follows the existing pattern.
- `wanted_entry_t` gains `deadline_ms`/`fanout_count`/`not_found_count`. The sweep + the not-found handler use them consistently.

**4. Interaction check.** Task 2's sweep and Task 3's fanout accounting both touch `wanted_entry_t`. Task 3's not-found handler increments `not_found_count`; Task 2's sweep expires the entry on deadline. These are compatible (the sweep removes the entry; the not-found handler clears it when `not_found_count >= fanout_count`). Task 5's `reachable_hops` adjusts `fanout_count` — set it AFTER `wanted_list_add` and AFTER the sends, so it reflects the actual reachable count. The order in `network_handle_local_find_block`: add to wanted_list → forward (count reachable) → set fanout_count = reachable. If reachable == 0, remove + fail immediately.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-15-tier3-multihop-rpc.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, two-stage review between tasks. Best here because the 5 tasks span the network routing core + wanted_list + closest_pending and a fresh context per task keeps each fix tight.
2. **Inline Execution** — execute in this session with checkpoints.

Which approach?