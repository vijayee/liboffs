# Tier-1 Memory-Safety Criticals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the confirmed Critical and High memory-safety defects on the network/relay receive path and in the actor/refcounter core, so untrusted input can no longer trigger heap over-reads, per-message payload leaks, unbounded allocations, use-after-free, or use-after-return.

**Architecture:** Eight focused, independently-testable fixes, each landing as its own commit. Five are surgical (drop a bad length override, free a payload, cap a frame size, idempotent conn-track, heap-allocate a stack payload). Three are structural (refcounter escrow becomes a single atomic transaction; mailbox becomes teardown-aware via a send-side mutex; scheduler tracks actor queue-state so destroy waits for the worker to release the actor). Each fix is TDD: a failing test first, then the minimal code, then valgrind on the touched suites.

**Tech Stack:** C11 (GCC/Clang/MSVC), `__atomic_*` / C11 `_Atomic`, libcbor, MsQuic, GoogleTest for C++ tests, CMake, valgrind (built with `-gdwarf-4` per `reference_valgrind_dwarf5.md`).

**Scope (in):** `docs/liboffs-audit-report.md` findings #1, #2, #3, #4, #7 and `docs/concurrency-pass.md` findings F1, F4, F5.

**Scope (out — deferred to follow-on plans):** Audit #5/#6/#9/#10 (multi-hop RPC hangs), #8/#11 (identity/TLS), #15-17/#19/#20/#24 (CLI lies), #18 (NAT/relay feature), and concurrency F2 (sections_dispatch double-thread), F3 (conn_mgr shutdown UAF), F6/F7 (relay/WT teardown). These are real but not tier-1; a separate plan should cover F2/F3/F6/F7 as a "concurrency teardown" tier, and the others per the audit's suggested fix order.

**Build / test commands (used throughout):**
```bash
# Configure once (from repo root). -gdwarf-4 is required for valgrind (see reference_valgrind_dwarf5.md).
cmake -S . -B cmake-build-verify -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-gdwarf-4 -O0" -DCMAKE_CXX_FLAGS="-gdwarf-4 -O0"

# Build the library + tests
cmake --build cmake-build-verify -j$(nproc) --target testliboffs

# Run one test binary filtered by name
cmake-build-verify/test/testliboffs --gtest_filter='StreamFramer.*'

# Valgrind on a specific binary (per reference_valgrind_leaks.md, 0 leaks is the bar)
valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='StreamFramer.*'
```

**Style reminder (from `docs/STYLE_GUIDE.md` + CLAUDE.md):** `_t` suffix on types, `type_action()` functions, `refcounter_t refcounter` as the first member of refcounted structs, `get_clear_memory` + `refcounter_init` last in create, no single-letter variable names (use `payload`/`left`/`right`/`stream`/`index`), no TODO/FIXME in completed work, no `Co-Authored-By` lines in commits.

---

## File Structure

| File | Responsibility | Touched by task |
| --- | --- | --- |
| `src/Network/wire.c` | CBOR encode/decode of wire messages. Drop the bad `block_data_len` override in `wire_find_block_response_decode` and `wire_recall_accept_decode`. | Task 1 |
| `test/test_wire_validation.cpp` | New unit tests for the two fixed decoders. | Task 1 |
| `src/Network/network.c` | Synchronous dispatch in `NETWORK_QUIC_DATA` and `RELAY_RECEIVED`. Free the payload after each synchronous handler; set `wire_find_block_response_destroy` on the FIND_BLOCK_RESPONSE case. | Task 2 |
| `test/test_network.cpp` | New unit test asserting payloads are freed on the synchronous dispatch path. | Task 2 |
| `src/Network/stream_framer.c`, `stream_framer.h` | Frame-length cap + overflow-checked size math. | Task 3 |
| `test/test_stream_framer.cpp` | New unit tests for the cap and the overflow path. | Task 3 |
| `src/Network/quic_listener.c` | Make `_conn_track_add` idempotent. | Task 4 |
| `test/test_quic_integration.cpp` | New unit test for the idempotent add. | Task 4 |
| `src/Network/nat_detect.c` | Heap-allocate the two `wire_addr_request_t` payloads. | Task 5 |
| `test/test_nat_detect.cpp` | New unit test asserting payloads survive the actor send. | Task 5 |
| `src/RefCounter/refcounter.h` | Pack `{count,yield,pending_deref}` into one `_Atomic uint32_t`; remove the three separate atomic fields; keep `is_actor` (now unused for branching, kept for layout/debug). | Task 6 |
| `src/RefCounter/refcounter.c` | Reimplement all six operations as CAS loops over the packed word. | Task 6 |
| `test/test_refcounter.cpp` | New tests for the escrow race and the underflow guard. | Task 6 |
| `src/Actor/message_queue.h` | Add `platform_mutex_t* send_lock` and `_Atomic bool destroyed` to `message_queue_t`. | Task 7 |
| `src/Actor/message_queue.c` | Lock-couple push with destroy; change `message_queue_push` to return success/failure (not `was_empty`); free the message on the destroyed path. | Task 7 |
| `src/Actor/actor.c` | Adapt `actor_send` to the new push signature (free payload on failure); `actor_destroy` now sets `destroyed` before draining. | Task 7 |
| `test/test_message_queue.cpp` | New tests for the push-vs-destroy race. | Task 7 |
| `src/Actor/actor.h` | Add `_Atomic uint8_t queue_state` (IDLE/QUEUED/RUNNING) to `actor_t`; add `ACTOR_FLAG_*` aliases if needed. | Task 8 |
| `src/Actor/actor.c` | Set `queue_state` on init. | Task 8 |
| `src/Scheduler/scheduler.c` | Worker CASes `queue_state` on pick/re-queue; `actor_destroy` waits for `IDLE` before `message_queue_destroy`. | Task 8 |
| `test/test_scheduler.cpp` | New test for the re-queue-vs-destroy race. | Task 8 |

---

## Task 1: Drop the attacker-declared `block_data_len` override

**Files:**
- Modify: `src/Network/wire.c:1168-1184` (`wire_find_block_response_decode`) and `src/Network/wire.c:648-664` (`wire_recall_accept_decode`)
- Test: `test/test_wire_validation.cpp`

**Why:** `wire_find_block_response_decode` sizes the block buffer to the CBOR bytestring length (line 1171-1174), then **overwrites** `block_data_len` with an independent, unvalidated integer from array index 10 (line 1179). The sink `buffer_create_from_pointer_copy` then reads past the short heap buffer → heap over-read / disclosure / crash. `wire_recall_accept_decode:659` has the same bug. `wire_store_block_decode:1312` already does this correctly (uses the bytestring length only) and is the reference.

- [ ] **Step 1: Write the failing test.** Append to `test/test_wire_validation.cpp`:

```cpp
extern "C" {
#include "../src/Network/wire.h"
}

// Build a FindBlockResponse where array[9] (data bytestring) is 16 bytes,
// but array[10] (data_len) claims 4096. The decoder must use the bytestring
// length (16), not the declared 4096, so block_data_len == 16 and the
// nested buffer is exactly 16 bytes.
static cbor_item_t* _build_find_block_response_mismatched_len() {
  cbor_item_t* array = cbor_new_definite_array(12);
  cbor_item_t* item;
  item = cbor_build_uint8(WIRE_FIND_BLOCK_RESPONSE); cbor_array_push(array, item); cbor_decref(&item);
  // sender_id (node_id: 32 bytes)
  uint8_t sender[32] = {0}; item = cbor_build_bytestring(sender, 32); cbor_array_push(array, item); cbor_decref(&item);
  // message_id hi/lo
  item = cbor_build_uint64(0); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(1); cbor_array_push(array, item); cbor_decref(&item);
  // block_hash (32 bytes)
  uint8_t hash[32] = {0}; item = cbor_build_bytestring(hash, 32); cbor_array_push(array, item); cbor_decref(&item);
  // found
  item = cbor_build_uint8(1); cbor_array_push(array, item); cbor_decref(&item);
  // holder (node_id)
  item = cbor_build_bytestring(sender, 32); cbor_array_push(array, item); cbor_decref(&item);
  // fib, path (empty array)
  item = cbor_build_uint32(0); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_new_definite_array(0); cbor_array_push(array, item); cbor_decref(&item);
  // latency
  item = cbor_build_uint64(0); cbor_array_push(array, item); cbor_decref(&item);
  // data bytestring — 16 bytes of 0xAA
  uint8_t data[16]; memset(data, 0xAA, 16);
  item = cbor_build_bytestring(data, 16); cbor_array_push(array, item); cbor_decref(&item);
  // data_len — the LIE: claims 4096
  item = cbor_build_uint64(4096); cbor_array_push(array, item); cbor_decref(&item);
  // bfib
  item = cbor_build_uint32(0); cbor_array_push(array, item); cbor_decref(&item);
  return array;
}

TEST(TestWireValidation, FindBlockResponseUsesBytestringLengthNotDeclared) {
  cbor_item_t* cbor = _build_find_block_response_mismatched_len();
  wire_find_block_response_t msg;
  memset(&msg, 0, sizeof(msg));
  int rc = wire_find_block_response_decode(cbor, &msg);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(msg.block_data_len, (size_t)16) << "decoder must take length from the bytestring, not array[10]";
  ASSERT_NE(msg.block_data, nullptr);
  EXPECT_EQ(msg.block_data[0], 0xAA);
  EXPECT_EQ(msg.block_data[15], 0xAA);
  wire_find_block_response_destroy(&msg);
  cbor_decref(&cbor);
}
```

And a parallel test for `wire_recall_accept_decode`:

```cpp
static cbor_item_t* _build_recall_accept_mismatched_len() {
  cbor_item_t* array = cbor_new_definite_array(8);
  cbor_item_t* item;
  item = cbor_build_uint8(WIRE_RECALL_ACCEPT); cbor_array_push(array, item); cbor_decref(&item);
  uint8_t sender[32] = {0}; item = cbor_build_bytestring(sender, 32); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(0); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(1); cbor_array_push(array, item); cbor_decref(&item);
  uint8_t hash[32] = {0}; item = cbor_build_bytestring(hash, 32); cbor_array_push(array, item); cbor_decref(&item);
  uint8_t data[16]; memset(data, 0xBB, 16);
  item = cbor_build_bytestring(data, 16); cbor_array_push(array, item); cbor_decref(&item);
  item = cbor_build_uint64(8192); cbor_array_push(array, item); cbor_decref(&item);  // LIE
  item = cbor_build_uint32(0); cbor_array_push(array, item); cbor_decref(&item);
  return array;
}

TEST(TestWireValidation, RecallAcceptUsesBytestringLengthNotDeclared) {
  cbor_item_t* cbor = _build_recall_accept_mismatched_len();
  wire_recall_accept_t msg;
  memset(&msg, 0, sizeof(msg));
  int rc = wire_recall_accept_decode(cbor, &msg);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(msg.block_data_len, (size_t)16);
  ASSERT_NE(msg.block_data, nullptr);
  EXPECT_EQ(msg.block_data[0], 0xBB);
  wire_recall_accept_destroy(&msg);
  cbor_decref(&cbor);
}
```

- [ ] **Step 2: Run to verify it fails.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='TestWireValidation.FindBlockResponse*:TestWireValidation.RecallAccept*'`
Expected: FAIL — `msg.block_data_len` is 4096 / 8192 (the declared lie), not 16.

- [ ] **Step 3: Implement the fix in `wire_find_block_response_decode`.** Replace lines 1168-1184:

```c
  if (array_size >= 12) {
    cbor_item_t* data = cbor_array_get(item, 9);
    if (cbor_isa_bytestring(data) && cbor_bytestring_length(data) > 0) {
      msg->block_data_len = cbor_bytestring_length(data);
      msg->block_data = get_clear_memory(msg->block_data_len);
      if (msg->block_data != NULL) {
        memcpy(msg->block_data, cbor_bytestring_handle(data), msg->block_data_len);
      }
    }
    cbor_decref(&data);
    /* The wire format also carries an explicit block_data_len at array index 10
       for symmetry with the encoder, but it is attacker-controlled and must NOT
       override the bytestring length we already used to size the buffer (a
       mismatch caused a heap over-read; see docs/liboffs-audit-report.md #1).
       Consume the item to release the refcount, then discard the value. */
    cbor_item_t* data_len = cbor_array_get(item, 10);
    cbor_decref(&data_len);
    cbor_item_t* bfib = cbor_array_get(item, 11);
    msg->block_fib = cbor_get_uint32(bfib);
    cbor_decref(&bfib);
  }
```

- [ ] **Step 4: Implement the parallel fix in `wire_recall_accept_decode`.** Replace lines 648-664:

```c
  if (array_size >= 8) {
    cbor_item_t* data = cbor_array_get(item, 5);
    if (cbor_isa_bytestring(data) && cbor_bytestring_length(data) > 0) {
      msg->block_data_len = cbor_bytestring_length(data);
      msg->block_data = get_clear_memory(msg->block_data_len);
      if (msg->block_data != NULL) {
        memcpy(msg->block_data, cbor_bytestring_handle(data), msg->block_data_len);
      }
    }
    cbor_decref(&data);
    /* Discard the attacker-controlled declared length at index 6; the
       bytestring length is the single source of truth. See audit #1. */
    cbor_item_t* data_len = cbor_array_get(item, 6);
    cbor_decref(&data_len);
    cbor_item_t* bfib = cbor_array_get(item, 7);
    msg->block_fib = cbor_get_uint32(bfib);
    cbor_decref(&bfib);
  }
```

- [ ] **Step 5: Run the tests to verify they pass.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='TestWireValidation.FindBlockResponse*:TestWireValidation.RecallAccept*'`
Expected: PASS.

- [ ] **Step 6: Run the full wire validation + network suites under valgrind.** Run: `valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='TestWireValidation.*'`
Expected: 0 leaks, exit 0 (per `reference_valgrind_leaks.md`).

- [ ] **Step 7: Commit.**

```bash
git add src/Network/wire.c test/test_wire_validation.cpp
git commit -m "fix(wire): use bytestring length, not attacker-declared block_data_len

wire_find_block_response_decode and wire_recall_accept_decode sized the
nested block buffer to the CBOR bytestring length, then overwrote
block_data_len with an independent unvalidated integer from the wire.
The sink then read past the short heap buffer -> heap over-read / crash
remotely triggerable by any peer answering a FindBlock. Drop the override;
the bytestring length is the single source of truth, matching
wire_store_block_decode. See docs/liboffs-audit-report.md #1."
```

---

## Task 2: Free the payload on the synchronous network dispatch path

**Files:**
- Modify: `src/Network/network.c` — the `NETWORK_QUIC_DATA` switch (lines ~3094-3398) and the `RELAY_RECEIVED` switch (lines ~3625-3900+)
- Test: `test/test_network.cpp`

**Why:** The synchronous dispatch builds a stack-local `dispatch_msg`, runs a handler, and only `cbor_decref`s the CBOR — it never calls `dispatch_msg.payload_destroy`. The actor path frees the payload via `actor.c:91`, but this synchronous path bypasses the actor. Because `get_clear_memory` is `calloc` (`Util/allocator.c:18`), this is a true heap leak on every inbound message, and block-carrying types additionally leak their 128 KB–2 MB nested block buffer. Compounding it, the `WIRE_FIND_BLOCK_RESPONSE` case uses the default `free` destroyer (line 3097) instead of `wire_find_block_response_destroy`, so even if the payload were freed the nested block would leak.

- [ ] **Step 1: Write the failing test.** Append to `test/test_network.cpp`. The test crafts a CBOR FindBlockResponse with a 128 KB block, drives the synchronous dispatch once via the network actor's dispatch function, and asserts the payload (and the nested block buffer) is freed. Use a custom allocator counter if available; otherwise use valgrind at step 6 as the leak assertion.

```cpp
extern "C" {
#include "../src/Network/network.h"
#include "../src/Network/wire.h"
#include "../src/Util/allocator.h"
#include <cbor.h>
}

// Count allocations so the test can assert the payload + nested buffer are
// freed exactly once by the synchronous dispatch tail. We wrap get_clear_memory
// by intercepting calloc via a global counter (the library's get_clear_memory
// is calloc per Util/allocator.c:18). This counter is incremented from a
// LD_PRELOAD-style shim is not feasible here; instead we assert via valgrind
// in step 6. The unit-level assertion here is that dispatch does not double-
// free (the process would crash). The leak is caught by valgrind.
```

Because the leak is most directly asserted under valgrind, the unit test here focuses on the **double-free guard**: after the fix, the synchronous dispatch tail frees the payload exactly once. A crash here means double-free. Add a test that dispatches a FindBlockResponse through the network actor and asserts the process survives:

```cpp
TEST(TestNetworkSynchronousDispatch, FindBlockResponsePayloadFreedExactlyOnce) {
  // Build a minimal network with a scheduler pool of 1 worker.
  // (Reuse the existing helper in test_network.cpp if one exists; if not,
  //  construct network_t* net = network_create(...) with a temp pool.)
  network_t* net = test_network_create_minimal();  // existing helper, or add one
  ASSERT_NE(net, nullptr);

  // Build a FindBlockResponse CBOR with a 128 KB block.
  cbor_item_t* wire_msg = test_build_find_block_response_cbor(128 * 1024);
  ASSERT_NE(wire_msg, nullptr);

  // Simulate one inbound QUIC_DATA message: the synchronous dispatch path.
  message_t in_msg;
  memset(&in_msg, 0, sizeof(in_msg));
  in_msg.type = NETWORK_QUIC_DATA;
  quic_data_payload_t quic_data;
  memset(&quic_data, 0, sizeof(quic_data));
  // Serialize wire_msg into quic_data.buffer for the handler to cbor_load.
  size_t cbor_len = 0;
  unsigned char* cbor_buf = cbor_serialize_alloc(wire_msg, &cbor_len, NULL);
  quic_data.buffer = cbor_buf;
  quic_data.buffer_len = cbor_len;
  quic_data.quic_connection = NULL;  // salutation-free path
  in_msg.payload = &quic_data;
  in_msg.payload_destroy = NULL;

  // Drive the network actor's dispatch synchronously.
  net->actor.dispatch(net->actor.state, &in_msg);

  free(cbor_buf);
  cbor_decref(&wire_msg);
  test_network_destroy(net);
  SUCCEED();  // Reach here = no double-free crash.
}
```

If `test_network_create_minimal` / `test_build_find_block_response_cbor` helpers do not already exist in `test/test_network.cpp`, add them as small static helpers at the top of the test file (create a `network_t` with a 1-worker pool and a minimal `quic_listener_t` stub; build a 12-element FindBlockResponse CBOR array with a `data` bytestring of the given length). If constructing a full `network_t` in the test proves too heavy, fall back to a valgrind-only assertion on an existing integration test (step 6) and skip the unit test — note this in the commit message.

- [ ] **Step 2: Run to verify it fails (or crashes under valgrind with leaks).** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='TestNetworkSynchronousDispatch.*'`
Expected: Either a crash (double-free once the fix is partially applied) or, pre-fix, valgrind reports `definitely lost` bytes matching one `wire_find_block_response_t` plus the nested 128 KB block per message.

- [ ] **Step 3: Set the correct destroyer on the FIND_BLOCK_RESPONSE case.** In `src/Network/network.c`, in the `NETWORK_QUIC_DATA` switch, locate the `WIRE_FIND_BLOCK_RESPONSE` case (around line 3235-3236) and add the destroyer before the handler call:

```c
        case WIRE_FIND_BLOCK_RESPONSE: {
          wire_find_block_response_t* payload = get_clear_memory(sizeof(wire_find_block_response_t));
          if (payload != NULL) {
            if (wire_find_block_response_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              dispatch_msg.payload_destroy = (void (*)(void*))wire_find_block_response_destroy;
              network_handle_find_block_response(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
```

Do the same in the `RELAY_RECEIVED` switch's `WIRE_FIND_BLOCK_RESPONSE` case (search the file for the second occurrence; it has the same shape). Audit the other cases for nested block buffers: any case whose `wire_*_destroy` does more than `free()` (i.e., frees a nested `block_data`) must set `dispatch_msg.payload_destroy` to that destroyer. Currently `WIRE_STORE_BLOCK` (line 3164), `WIRE_RECALL_ACCEPT` (line 3279), and `WIRE_SEEKING_BLOCKS` (line 3175) already do. `WIRE_FIND_BLOCK_RESPONSE` was the only gap.

- [ ] **Step 4: Free the payload after the synchronous switch.** In `src/Network/network.c`, immediately before `cbor_decref(&wire_msg);` at the end of the `NETWORK_QUIC_DATA` case (line 3398), add:

```c
      /* The synchronous dispatch bypasses actor_run, so the framework does
         not free the payload. Handlers that CONSUME the payload set
         dispatch_msg.payload = NULL; respect that. See audit #2. */
      if (dispatch_msg.payload != NULL && dispatch_msg.payload_destroy != NULL) {
        dispatch_msg.payload_destroy(dispatch_msg.payload);
        dispatch_msg.payload = NULL;
      }
      cbor_decref(&wire_msg);
      break;
```

Add the same block at the end of the `RELAY_RECEIVED` switch (just before its `cbor_decref(&wire_msg)`).

- [ ] **Step 5: Run the test to verify it passes.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='TestNetworkSynchronousDispatch.*'`
Expected: PASS (no crash).

- [ ] **Step 6: Run valgrind on the network + recycler + health_wire suites.** Run: `valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='TestNetworkSynchronousDispatch.*:TestRecyclerWire.*:TestHealthWire.*'`
Expected: 0 leaks, exit 0. If leaks remain, a handler is transferring ownership of `dispatch_msg.payload` without nulling it — audit each `network_handle_*` called from the synchronous switch and either null `dispatch_msg.payload` in the handler when it takes ownership, or document why the transfer is safe.

- [ ] **Step 7: Commit.**

```bash
git add src/Network/network.c test/test_network.cpp
git commit -m "fix(network): free payload on synchronous QUIC_DATA/RELAY_RECEIVED dispatch

The synchronous dispatch built a stack-local dispatch_msg, ran a handler,
and only cbor_decref'd the CBOR — it never freed dispatch_msg.payload.
Because get_clear_memory is calloc, every inbound message leaked its
wire_*_t, and block-carrying types additionally leaked their 128 KB-2 MB
nested block buffer. The actor path frees payloads via actor.c:91, but
this path bypasses the actor. Free the payload at the tail of the switch
(respecting CONSUME by checking for NULL), and set wire_find_block_response_destroy
on the FIND_BLOCK_RESPONSE case so the nested block is freed too. See audit #2."
```

---

## Task 3: Cap stream_framer frame size and check size overflow

**Files:**
- Modify: `src/Network/stream_framer.c` (add the cap, overflow-checked growth), `src/Network/stream_framer.h` (export the cap constant)
- Test: `test/test_stream_framer.cpp`

**Why:** `stream_framer_next` reads a big-endian `uint32_t` length (up to 4 GiB) with no maximum and buffers until it arrives (lines 67-73); a peer can advertise a huge length and slow-drip bytes to pin arbitrarily large allocations, and combined with `get_clear_memory` can drive an `abort()` on OOM = remote crash. On 32-bit, `total_message_size = 4 + length` wraps near `UINT32_MAX`, bypassing the completeness guard. The `new_capacity *= 2` loop (line 48) can overflow to 0. Blocks are ≤128 KB; a 2 MB cap is safe. This framer sits on the entire network receive surface.

- [ ] **Step 1: Write the failing tests.** Append to `test/test_stream_framer.cpp`:

```cpp
TEST(StreamFramer, RejectsOversizeDeclaredLength) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);
  // 4-byte prefix claiming 16 MB (above the 2 MB cap).
  uint8_t prefix[4] = {0x00, 0xFF, 0xFF, 0xFF};  // ~16 MB
  ASSERT_EQ(stream_framer_feed(framer, prefix, 4), 0);
  size_t out_len = 999;
  uint8_t* payload = stream_framer_next(framer, &out_len);
  EXPECT_EQ(payload, nullptr) << "oversize frame must be rejected, not buffered";
  EXPECT_EQ(out_len, (size_t)0);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, RejectsFeedThatExceedsCap) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);
  // Feed a 4 MB chunk with a length prefix claiming even more. feed must
  // refuse to grow the buffer past STREAM_FRAMER_MAX_FRAME_SIZE.
  uint8_t prefix[4] = {0x00, 0x20, 0x00, 0x00};  // 2 MB exactly (at the cap)
  ASSERT_EQ(stream_framer_feed(framer, prefix, 4), 0);
  // Now feed one byte more than the cap allows — feed must return -1.
  std::vector<uint8_t> big(STREAM_FRAMER_MAX_FRAME_SIZE + 1, 0x41);
  EXPECT_EQ(stream_framer_feed(framer, big.data(), big.size()), -1);
  stream_framer_destroy(framer);
}

TEST(StreamFramer, AcceptsFrameAtExactlyTheCap) {
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);
  size_t cap = STREAM_FRAMER_MAX_FRAME_SIZE;
  uint8_t prefix[4] = {
    (uint8_t)((cap >> 24) & 0xFF),
    (uint8_t)((cap >> 16) & 0xFF),
    (uint8_t)((cap >> 8) & 0xFF),
    (uint8_t)(cap & 0xFF)
  };
  ASSERT_EQ(stream_framer_feed(framer, prefix, 4), 0);
  std::vector<uint8_t> body(cap, 0x42);
  ASSERT_EQ(stream_framer_feed(framer, body.data(), cap), 0);
  size_t out_len = 0;
  uint8_t* payload = stream_framer_next(framer, &out_len);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(out_len, cap);
  free(payload);
  stream_framer_destroy(framer);
}
```

- [ ] **Step 2: Run to verify they fail.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='StreamFramer.Rejects*:StreamFramer.AcceptsFrameAtExactlyTheCap'`
Expected: FAIL — `STREAM_FRAMER_MAX_FRAME_SIZE` is undefined, and the current code happily buffers 16 MB.

- [ ] **Step 3: Add the cap constant to the header.** In `src/Network/stream_framer.h`, after the includes, add:

```c
/* Hard cap on a single framed message. Blocks are <= 128 KB; 2 MB gives
   headroom for future larger payloads and blocks any peer from pinning
   arbitrarily large allocations by advertising a huge length and slow-dripping
   bytes. See docs/liboffs-audit-report.md #3. */
#define STREAM_FRAMER_MAX_FRAME_SIZE ((size_t)(2 * 1024 * 1024))
```

- [ ] **Step 4: Cap and overflow-check `stream_framer_feed`.** Replace the growth block in `src/Network/stream_framer.c:39-59`:

```c
int stream_framer_feed(stream_framer_t* framer, const uint8_t* data, size_t len) {
  if (framer == NULL) return -1;
  if (len == 0) return 0;
  if (data == NULL) return -1;

  /* Reject any feed that would push the buffered bytes past the cap + prefix.
     This also bounds the slow-drip attack: a peer advertising a huge length
     cannot make us allocate beyond STREAM_FRAMER_MAX_FRAME_SIZE. */
  if (len > STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE) return -1;
  if (framer->used > STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE - len) {
    return -1;
  }

  size_t needed = framer->used + len;
  if (needed > framer->capacity) {
    size_t new_capacity = framer->capacity;
    while (new_capacity < needed) {
      /* Guard against doubling-overflow: never grow past the cap + prefix. */
      if (new_capacity > STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE) {
        new_capacity = STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE;
        break;
      }
      size_t next = new_capacity * 2;
      if (next <= new_capacity) {
        /* Multiplication overflow — clamp to the cap. */
        new_capacity = STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE;
        break;
      }
      new_capacity = next;
    }
    if (new_capacity < needed) {
      /* The cap prevented us from reaching `needed`; the caller is asking for
         more than the framer will hold. */
      return -1;
    }
    uint8_t* new_buffer = realloc(framer->buffer, new_capacity);
    if (new_buffer == NULL) return -1;
    framer->buffer = new_buffer;
    framer->capacity = new_capacity;
  }

  memcpy(framer->buffer + framer->used, data, len);
  framer->used += len;
  return 0;
}
```

- [ ] **Step 5: Cap and overflow-check `stream_framer_next`.** Replace the body of `stream_framer_next` (lines 61-89):

```c
uint8_t* stream_framer_next(stream_framer_t* framer, size_t* out_len) {
  if (out_len != NULL) *out_len = 0;
  if (framer == NULL) return NULL;

  if (framer->used < STREAM_FRAMER_LENGTH_PREFIX_SIZE) return NULL;

  uint32_t length = ((uint32_t)framer->buffer[0] << 24) |
                    ((uint32_t)framer->buffer[1] << 16) |
                    ((uint32_t)framer->buffer[2] << 8) |
                    (uint32_t)framer->buffer[3];

  /* Reject any declared length above the cap. Without this, a peer can pin
     arbitrarily large allocations by advertising 4 GiB and slow-dripping. */
  if ((size_t)length > STREAM_FRAMER_MAX_FRAME_SIZE) {
    return NULL;
  }

  /* Overflow-checked total size. On 32-bit, 4 + length can wrap near UINT32_MAX
     and bypass the completeness guard. */
  size_t total_message_size;
  if (length > SIZE_MAX - STREAM_FRAMER_LENGTH_PREFIX_SIZE) {
    return NULL;
  }
  total_message_size = STREAM_FRAMER_LENGTH_PREFIX_SIZE + (size_t)length;

  if (framer->used < total_message_size) return NULL;

  uint8_t* payload = get_clear_memory((size_t)length);
  if (payload == NULL) return NULL;
  if (length > 0) {
    memcpy(payload, framer->buffer + STREAM_FRAMER_LENGTH_PREFIX_SIZE, (size_t)length);
  }

  size_t remaining = framer->used - total_message_size;
  if (remaining > 0) {
    memmove(framer->buffer, framer->buffer + total_message_size, remaining);
  }
  framer->used = remaining;

  if (out_len != NULL) *out_len = (size_t)length;
  return payload;
}
```

- [ ] **Step 6: Run the tests to verify they pass.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='StreamFramer.*'`
Expected: PASS (including the existing tests — the cap is above all legitimate frame sizes).

- [ ] **Step 7: Run valgrind on the framer suite.** Run: `valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='StreamFramer.*'`
Expected: 0 leaks, exit 0.

- [ ] **Step 8: Commit.**

```bash
git add src/Network/stream_framer.c src/Network/stream_framer.h test/test_stream_framer.cpp
git commit -m "fix(stream_framer): cap frame size at 2 MB and check size overflow

stream_framer_next read a 4-byte big-endian length with no maximum and
buffered until it arrived; a peer could advertise 4 GiB and slow-drip
bytes to pin arbitrarily large allocations and drive abort() on OOM = a
remote crash. On 32-bit, 4 + length wrapped near UINT32_MAX, bypassing
the completeness guard, and the doubling growth loop could overflow to 0.
Blocks are <= 128 KB; 2 MB is a safe cap. Reject oversize declarations,
refuse to grow past the cap, and compute total_message_size with an
overflow check. See audit #3."
```

---

## Task 4: Make `_conn_track_add` idempotent

**Files:**
- Modify: `src/Network/quic_listener.c:91-108` (`_conn_track_add`)
- Test: `test/test_quic_integration.cpp`

**Why:** Outbound connections are added to the tracking array twice — once in `quic_listener_connect` (line 712) and once on the CONNECTED event (line 354). `_conn_track_remove` (line 111-121) deletes only the first match; the DISCONNECTED handler frees the HQUIC; the stale second slot then gets `ConnectionShutdown` on a freed handle at destroy. Reachable via the offsd config-reload cycle. Making `_conn_track_add` scan for an existing entry before appending prevents the double-add entirely, without changing the lifecycle call sites.

- [ ] **Step 1: Write the failing test.** Append to `test/test_quic_integration.cpp`:

```cpp
extern "C" {
#include "../src/Network/quic_listener.h"
}

// _conn_track_add is static, so we test it indirectly through the public
// surface: call the CONNECTED-equivalent path twice with the same HQUIC and
// assert the connection_count only goes up by 1. If quic_listener exposes
// no public accessor for connection_count, expose a test-only getter
// `size_t quic_listener__conn_count_for_test(quic_listener_t*)` guarded by
// `#ifdef OFFS_TEST_DEBUG` (or just `#ifndef NDEBUG`).
TEST(QuicListenerConnTrack, AddIsIdempotent) {
  // Construct a listener without msquic (the stub path if HAS_MSQUIC is off,
  // or a real listener with a mocked msquic vtable if on). Reuse the existing
  // test fixture in test_quic_integration.cpp.
  quic_listener_t* listener = test_quic_listener_create();
  ASSERT_NE(listener, nullptr);

  HQUIC fake_handle = (HQUIC)0x1234;
  // Call the equivalent of _conn_track_add twice. If the static function
  // cannot be reached directly, drive it through the CONNECTED event callback
  // (quic_connection_callback) with a QUIC_CONNECTION_EVENT_CONNECTED event
  // whose Connection is fake_handle, twice.
  test_quic_listener_emit_connected(listener, fake_handle);
  test_quic_listener_emit_connected(listener, fake_handle);

  EXPECT_EQ(quic_listener__conn_count_for_test(listener), (size_t)1)
      << "_conn_track_add must be idempotent on duplicate HQUIC";

  test_quic_listener_destroy(listener);
}
```

If `test_quic_listener_create` / `test_quic_listener_emit_connected` / `quic_listener__conn_count_for_test` do not exist, add them as test-only helpers (gated by a test build flag). If adding test hooks to the production header is undesirable, instead test via the integration path: trigger a connect + a CONNECTED event for the same connection and assert via `valgrind --tool=memcheck` that no invalid read occurs on the shutdown path (step 6).

- [ ] **Step 2: Run to verify it fails.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='QuicListenerConnTrack.*'`
Expected: FAIL — `connection_count == 2` (the double-add).

- [ ] **Step 3: Implement the idempotent add.** Replace `_conn_track_add` in `src/Network/quic_listener.c:91-108`:

```c
static void _conn_track_add(quic_listener_t* listener, HQUIC connection) {
  platform_mutex_lock(listener->conn_lock);
  /* Idempotent: CONNECTED can fire after quic_listener_connect already added
     the same HQUIC, and a duplicate would leave a stale slot after
     _conn_track_remove (which only removes the first match) -> the
     DISCONNECTED handler frees the HQUIC and the stale slot later gets
     ConnectionShutdown on a freed handle. Scan for an existing entry first.
     See docs/liboffs-audit-report.md #4. */
  for (size_t index = 0; index < listener->connection_count; index++) {
    if (listener->connections[index] == connection) {
      platform_mutex_unlock(listener->conn_lock);
      return;
    }
  }
  if (listener->connection_count >= listener->connection_capacity) {
    size_t new_cap = listener->connection_capacity == 0 ? 8 : listener->connection_capacity * 2;
    HQUIC* new_arr = get_clear_memory(new_cap * sizeof(HQUIC));
    if (new_arr == NULL) {
      platform_mutex_unlock(listener->conn_lock);
      return;
    }
    if (listener->connections != NULL) {
      memcpy(new_arr, listener->connections, listener->connection_count * sizeof(HQUIC));
      free(listener->connections);
    }
    listener->connections = new_arr;
    listener->connection_capacity = new_cap;
  }
  listener->connections[listener->connection_count++] = connection;
  platform_mutex_unlock(listener->conn_lock);
}
```

- [ ] **Step 4: Run the test to verify it passes.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='QuicListenerConnTrack.*'`
Expected: PASS.

- [ ] **Step 5: Run valgrind on the quic integration + shutdown suites.** Run: `valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='QuicListenerConnTrack.*:TestShutdown.*'`
Expected: 0 leaks, 0 invalid reads, exit 0. (The pre-existing `reference_scheduler_valgrind_error.md` "Invalid read of size 1" at `scheduler.c:119` during shutdown is unrelated and acceptable; it is not in the conn_track path.)

- [ ] **Step 6: Commit.**

```bash
git add src/Network/quic_listener.c test/test_quic_integration.cpp
git commit -m "fix(quic_listener): make _conn_track_add idempotent

Outbound connections were added to the tracking array twice: once in
quic_listener_connect and once on the CONNECTED event. _conn_track_remove
only deletes the first match, so the DISCONNECTED handler freed the HQUIC
and the stale second slot later got ConnectionShutdown on a freed handle at
destroy -> UAF reachable via the offsd config-reload cycle. Scan for an
existing entry before appending. See audit #4."
```

---

## Task 5: Heap-allocate the NAT-detect ADDR_REQUEST payloads

**Files:**
- Modify: `src/Network/nat_detect.c:251-271` (`nat_detect_start`)
- Test: `test/test_nat_detect.cpp`

**Why:** Two `wire_addr_request_t` live on `nat_detect_start`'s stack (lines 251, 262); `msg.payload` points at them with `payload_destroy = NULL`. `actor_send` shallow-copies the struct (keeping the pointer) and enqueues for another thread; by the time `relay_client.c:707` dereferences it, the stack frame is gone → UB read of a reclaimed frame. It usually "works" because the page is still mapped, which is why testing misses it. Heap-allocate each payload with a freeing `payload_destroy`.

- [ ] **Step 1: Write the failing test.** Append to `test/test_nat_detect.cpp`:

```cpp
extern "C" {
#include "../src/Network/nat_detect.h"
#include "../src/Network/Relay/relay_client.h"
}

// The payload handed to actor_send must outlive nat_detect_start's stack
// frame. We assert this by having the relay client actor's dispatch read
// payload->message_id after nat_detect_start returns, with the actor on a
// separate worker thread. Pre-fix this is a UB read of a reclaimed frame
// (usually "works" because the page is still mapped, so the test also runs
// under valgrind --tool=memcheck at step 6 to catch the invalid access).
TEST(NatDetect, AddrRequestPayloadSurvivesActorSend) {
  nat_detect_test_fixture fixture;  // existing or add; constructs nat_detect_t
                                     // with two in-process relay clients whose
                                     // dispatch captures the last payload.
  ASSERT_TRUE(fixture.start("127.0.0.1", 0, "127.0.0.1", 0));

  // Give the relay client actors a chance to dispatch on a worker thread.
  fixture.drain_relay_actors();

  // The fixture recorded the message_id of each captured payload.
  ASSERT_EQ(fixture.relay_a_last_message_id(), (uint64_t)1);
  ASSERT_EQ(fixture.relay_b_last_message_id(), (uint64_t)2);
}
```

If `nat_detect_test_fixture` and its helpers do not exist, add a minimal version: construct a `nat_detect_t` with two in-process `relay_client_t` instances whose actor dispatch stores the incoming `payload->message_id` into a fixture field before freeing the payload. The payloads must be heap-allocated (post-fix) for the read to be valid; pre-fix, the read is UB and valgrind catches it.

- [ ] **Step 2: Run to verify it fails (or valgrind catches the UB).** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && valgrind --tool=memcheck --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='NatDetect.AddrRequestPayloadSurvivesActorSend'`
Expected: Either a wrong `message_id` (garbage from a reclaimed frame) or a valgrind "Invalid read" report pointing into `nat_detect_start`'s old stack frame.

- [ ] **Step 3: Heap-allocate the payloads.** Replace lines 251-271 in `src/Network/nat_detect.c`:

```c
  /* Heap-allocate each payload with a freeing payload_destroy. The struct
     must outlive this stack frame — actor_send shallow-copies the message_t
     (keeping the pointer) and enqueues for another thread; the relay client
     actor dereferences it long after nat_detect_start has returned. A stack
     payload here is use-after-return. See docs/liboffs-audit-report.md #7. */
  wire_addr_request_t* addr_request_a = get_clear_memory(sizeof(wire_addr_request_t));
  if (addr_request_a == NULL) {
    relay_client_destroy(detect->relay_b);
    detect->relay_b = NULL;
    relay_client_destroy(detect->relay_a);
    detect->relay_a = NULL;
    free(detect->relay_a_host);
    detect->relay_a_host = NULL;
    free(detect->relay_b_host);
    detect->relay_b_host = NULL;
    return -1;
  }
  addr_request_a->message_id = 1;

  message_t msg_a;
  memset(&msg_a, 0, sizeof(msg_a));
  msg_a.type = RELAY_CLIENT_ADDR_REQUEST;
  msg_a.payload = addr_request_a;
  msg_a.payload_destroy = free;
  actor_send(&detect->relay_a->actor, &msg_a);

  wire_addr_request_t* addr_request_b = get_clear_memory(sizeof(wire_addr_request_t));
  if (addr_request_b == NULL) {
    /* addr_request_a is now owned by the relay_a actor's mailbox; it will be
       freed by the actor dispatch. Nothing else to clean up here. */
    log_error("nat_detect: failed to allocate addr_request_b");
    return -1;
  }
  addr_request_b->message_id = 2;

  message_t msg_b;
  memset(&msg_b, 0, sizeof(msg_b));
  msg_b.type = RELAY_CLIENT_ADDR_REQUEST;
  msg_b.payload = addr_request_b;
  msg_b.payload_destroy = free;
  actor_send(&detect->relay_b->actor, &msg_b);
```

- [ ] **Step 4: Run the test to verify it passes.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='NatDetect.AddrRequestPayloadSurvivesActorSend'`
Expected: PASS.

- [ ] **Step 5: Run valgrind on the nat_detect suite.** Run: `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='NatDetect.*'`
Expected: 0 leaks, 0 invalid reads, exit 0.

- [ ] **Step 6: Commit.**

```bash
git add src/Network/nat_detect.c test/test_nat_detect.cpp
git commit -m "fix(nat_detect): heap-allocate ADDR_REQUEST payloads

Two wire_addr_request_t lived on nat_detect_start's stack; msg.payload
pointed at them with payload_destroy = NULL. actor_send shallow-copies
the message_t (keeping the pointer) and enqueues for another thread; the
relay client actor dereferences it after the stack frame is gone -> UB
read of a reclaimed frame, usually masked because the page stays mapped.
Heap-allocate each payload with payload_destroy = free. See audit #7."
```

---

## Task 6: Make the refcounter escrow transfer a single atomic transaction

**Files:**
- Modify: `src/RefCounter/refcounter.h` (pack `{count,yield,pending_deref}` into one `_Atomic uint32_t`), `src/RefCounter/refcounter.c` (reimplement all six operations as CAS loops over the packed word)
- Test: `test/test_refcounter.cpp`

**Why:** The non-actor path maintains the `yield`/`pending_deref`/`count` invariant with separate relaxed loads + independent RMWs and no transaction (`refcounter.c:95-103`, `:126-131`, `:155-161`). `yield`/`pending_deref` are `uint8_t`, so underflow wraps to `0xFF`. Two threads both `refcounter_reference` the same object: both load `yield=1` before either `fetch_sub`, yield goes 1→0→`0xFF`, two acquisitions consume one escrow slot, `count` under-counts live holders → a later release frees the object while a holder remains. The poisoned `yield=0xFF` is sticky. Separately, a `dereference` commits to `pending++` after loading `yield=1`, but an adopting `reference` on another thread consumes the yield and checks `pending` before that `++` lands → the release is never applied (stranded pending_deref → leak). The live cross-thread caller is `block_recipe.c:40-42` → `block_cache.c:796/806` → `block_cache.c:164` (double-adopt racer) and `block_cache.c:389` (stranded-pending racer), with `streams.c:385` holding `yield=1` open. The fix is to make the escrow transfer a single atomic transaction by packing the three fields into one word manipulated by CAS — then only one adopter wins a yield slot, and `pending_deref` is observed consistently with `yield`.

**Packed layout** (32-bit word): bits 0-15 = `count` (uint16_t), bits 16-23 = `yield` (uint8_t), bits 24-31 = `pending_deref` (uint8_t). Both the actor and non-actor paths use the packed word; the actor path's CAS always succeeds first try (single-threaded per actor), so it stays cheap. `is_actor` is kept for debug/introspection only and no longer branches behavior.

- [ ] **Step 1: Write the failing tests.** Append to `test/test_refcounter.cpp`:

```cpp
#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include "../src/RefCounter/refcounter.h"
}

// Reference helpers for the packed-field layout. These MUST match the
// layout defined in refcounter.h (count:16, yield:8, pending:8). If the
// test fails to compile after the header change, update these.
static uint16_t rc_count(uint32_t packed) { return (uint16_t)(packed & 0xFFFFu); }
static uint8_t  rc_yield(uint32_t packed) { return (uint8_t)((packed >> 16) & 0xFFu); }
static uint8_t  rc_pending(uint32_t packed) { return (uint8_t)((packed >> 24) & 0xFFu); }

TEST(RefCounterEscrow, DoubleAdoptDoesNotUnderflowYield) {
  // yield=1, two threads both reference concurrently. Pre-fix this races:
  // both load yield=1, both fetch_sub, yield wraps to 0xFF and count
  // under-counts. Post-fix, only one adopter consumes the yield slot.
  refcounter_t* refc = (refcounter_t*)calloc(1, sizeof(refcounter_t));
  ASSERT_NE(refc, nullptr);
  refcounter_init(refc);  // count=1, yield=0, pending=0
  refcounter_yield(refc); // yield=1

  std::thread left([&](){ refcounter_reference(refc); });
  std::thread right([&](){ refcounter_reference(refc); });
  left.join();
  right.join();

  // Exactly one adopter should have consumed the yield. count must be 2
  // (the original 1 + one adopter that fell through to count++ because the
  // other took the yield), and yield must be 0 (no underflow to 0xFF).
  uint32_t packed = *((std::atomic<uint32_t>*)&refc->packed_state);
  EXPECT_EQ(rc_yield(packed), (uint8_t)0) << "yield must not underflow to 0xFF";
  EXPECT_EQ(rc_count(packed), (uint16_t)2);
  refcounter_destroy_lock(refc);
  free(refc);
}

TEST(RefCounterEscrow, PendingDerefIsNotStranded) {
  // yield=1; one thread dereferences (should bump pending_deref), another
  // concurrently references (should consume the yield AND the pending_deref,
  // decrementing count). Pre-fix, the adopting reference can check pending
  // before the dereference's ++ lands -> pending is stranded at 1 and the
  // count is off by one. Post-fix, the CAS sees both fields consistently.
  refcounter_t* refc = (refcounter_t*)calloc(1, sizeof(refcounter_t));
  ASSERT_NE(refc, nullptr);
  refcounter_init(refc);     // count=1
  refcounter_reference(refc); // count=2
  refcounter_yield(refc);    // yield=1

  std::thread deref([&](){ refcounter_dereference(refc); });
  std::thread ref([&](){ refcounter_reference(refc); });
  deref.join();
  ref.join();

  // The yield+pending transfer must complete cleanly: yield=0, pending=0,
  // count=2 (the deref's pending was consumed by the ref's adopt).
  uint32_t packed = *((std::atomic<uint32_t>*)&refc->packed_state);
  EXPECT_EQ(rc_yield(packed), (uint8_t)0);
  EXPECT_EQ(rc_pending(packed), (uint8_t)0);
  EXPECT_EQ(rc_count(packed), (uint16_t)2);
  refcounter_destroy_lock(refc);
  free(refc);
}

TEST(RefCounterEscrow, HighConcurrencyAdoptReleaseNoCorruption) {
  // Stress test: many threads reference + dereference a single object with
  // a yield open. Pre-fix, this corrupts count/yield within a few iterations.
  refcounter_t* refc = (refcounter_t*)calloc(1, sizeof(refcounter_t));
  ASSERT_NE(refc, nullptr);
  refcounter_init(refc);
  refcounter_yield(refc);

  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < 8; thread_index++) {
    threads.emplace_back([&]() {
      for (int iteration = 0; iteration < 1000; iteration++) {
        refcounter_reference(refc);
        refcounter_dereference(refc);
      }
    });
  }
  for (auto& thread : threads) thread.join();

  // One yield slot, many concurrent adopters: at most one adoption consumes
  // the yield; the rest fall through to count++. The yield must be 0 (not
  // 0xFF) and count must be 1 (the original init).
  uint32_t packed = *((std::atomic<uint32_t>*)&refc->packed_state);
  EXPECT_EQ(rc_yield(packed), (uint8_t)0);
  EXPECT_EQ(rc_count(packed), (uint16_t)1);
  refcounter_destroy_lock(refc);
  free(refc);
}
```

- [ ] **Step 2: Run to verify they fail.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='RefCounterEscrow.*'`
Expected: FAIL — `refc->packed_state` does not exist; and even if you read the separate fields, the high-concurrency test will produce `yield=0xFF` and a wrong `count` within the first few iterations.

- [ ] **Step 3: Change the struct to a packed word.** In `src/RefCounter/refcounter.h`, replace the field macros and the struct (lines 18-55):

```c
#define OFFS_ATOMIC
/* Atomic refcounter state: a single 32-bit word manipulated by CAS so the
   escrow transfer (yield/pending_deref/count) is one atomic transaction.
   Layout: bits 0-15 = count, bits 16-23 = yield, bits 24-31 = pending_deref.
   The actor and non-actor paths share this word; the actor path's CAS always
   succeeds first try (single-threaded per actor). is_actor is kept for
   debug/introspection only and no longer branches behavior.
   See docs/concurrency-pass.md F1. */
#if defined(__cplusplus)
  #define OFFS_ATOMIC_FIELD_STATE _Atomic uint32_t packed_state;
#else
  #if defined(_MSC_VER)
    #define OFFS_ATOMIC_FIELD_STATE _Atomic(uint32_t) packed_state;
  #else
    #define OFFS_ATOMIC_FIELD_STATE _Atomic uint32_t packed_state;
  #endif
#endif
typedef struct refcounter_t {
#ifdef OFFS_ATOMIC
  OFFS_ATOMIC_FIELD_STATE
  uint8_t is_actor;
#else
  uint16_t count;
  uint8_t yield;
  uint8_t pending_deref;
  platform_mutex_t* lock;
#endif
} refcounter_t;
```

Add pack/unpack helpers as `static inline` in the header (so both .c and tests can use them):

```c
static inline uint32_t refcounter_pack(uint16_t count, uint8_t yield, uint8_t pending) {
  return ((uint32_t)count) | ((uint32_t)yield << 16) | ((uint32_t)pending << 24);
}
static inline uint16_t refcounter_packed_count(uint32_t state) { return (uint16_t)(state & 0xFFFFu); }
static inline uint8_t  refcounter_packed_yield(uint32_t state) { return (uint8_t)((state >> 16) & 0xFFu); }
static inline uint8_t  refcounter_packed_pending(uint32_t state) { return (uint8_t)((state >> 24) & 0xFFu); }
```

- [ ] **Step 4: Reimplement the operations as CAS loops.** Replace the body of `src/RefCounter/refcounter.c` (the `#else` atomic paths; keep the `#ifndef OFFS_ATOMIC` mutex paths unchanged). For each operation, loop: load the packed state, compute the desired new state, CAS; on failure reload and retry.

```c
void refcounter_init(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  refcounter->lock = platform_mutex_create();
  platform_mutex_lock(refcounter->lock);
  refcounter->count++;
  refcounter->pending_deref = 0;
  platform_mutex_unlock(refcounter->lock);
#else
  atomic_store_explicit((_Atomic uint32_t*)&refcounter->packed_state,
                        refcounter_pack(1, 0, 0), memory_order_release);
  refcounter->is_actor = 0;
#endif
}

void refcounter_init_actor(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  refcounter_init(refcounter);
#else
  atomic_store_explicit((_Atomic uint32_t*)&refcounter->packed_state,
                        refcounter_pack(1, 0, 0), memory_order_release);
  refcounter->is_actor = 1;
#endif
}

void refcounter_yield(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  refcounter->yield++;
  platform_mutex_unlock(refcounter->lock);
#else
  /* Bump the yield field via CAS; count and pending are unchanged. */
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint8_t yield = refcounter_packed_yield(state);
    if (yield == 0xFFu) return;  /* saturate; never wrap to 0 */
    desired = refcounter_pack(refcounter_packed_count(state), (uint8_t)(yield + 1),
                              refcounter_packed_pending(state));
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_release, memory_order_relaxed));
#endif
}

void* refcounter_reference(refcounter_t* refcounter) {
  if (refcounter == NULL) return NULL;
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  if (refcounter->yield > 0) {
    refcounter->yield--;
    if (refcounter->pending_deref > 0) {
      refcounter->pending_deref--;
      refcounter->count--;
    }
  } else if (refcounter->count < USHRT_MAX) {
    refcounter->count++;
  }
  platform_mutex_unlock(refcounter->lock);
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      /* Adopt the escrow: take one yield slot. If a pending deref is
         waiting, also consume it and decrement count. This is the
         transaction the old code split into three separate RMWs. */
      uint8_t new_yield = (uint8_t)(yield - 1);
      uint16_t new_count = count;
      uint8_t new_pending = pending;
      if (pending > 0) {
        new_pending = (uint8_t)(pending - 1);
        new_count = (uint16_t)(count - 1);
      }
      desired = refcounter_pack(new_count, new_yield, new_pending);
    } else if (count < USHRT_MAX) {
      desired = refcounter_pack((uint16_t)(count + 1), yield, pending);
    } else {
      desired = state;  /* saturated; no change */
    }
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_release, memory_order_relaxed));
#endif
  return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  } else if (refcounter->yield > 0) {
    refcounter->pending_deref++;
  }
  platform_mutex_unlock(refcounter->lock);
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      /* An escrow is open: queue this deref as pending so the next adopter
         consumes it. Bump pending atomically with the yield observation. */
      if (pending == 0xFFu) return;  /* saturate */
      desired = refcounter_pack(count, yield, (uint8_t)(pending + 1));
    } else if (count > 0) {
      desired = refcounter_pack((uint16_t)(count - 1), yield, pending);
    } else {
      desired = state;  /* nothing to release */
    }
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_acq_rel, memory_order_relaxed));
#endif
}

bool refcounter_dereference_is_zero(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  } else if (refcounter->yield > 0) {
    refcounter->pending_deref++;
  }
  bool is_zero = (refcounter->count == 0 && refcounter->pending_deref == 0);
  platform_mutex_unlock(refcounter->lock);
  return is_zero;
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  bool is_zero;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      if (pending == 0xFFu) return false;  /* saturate; not zero */
      desired = refcounter_pack(count, yield, (uint8_t)(pending + 1));
      is_zero = false;
    } else if (count > 0) {
      desired = refcounter_pack((uint16_t)(count - 1), yield, pending);
      is_zero = (count == 1) && (pending == 0);
    } else {
      desired = state;
      is_zero = (pending == 0);
    }
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_acq_rel, memory_order_relaxed));
  return is_zero;
#endif
}

uint16_t refcounter_count(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  uint16_t count = refcounter->count;
  platform_mutex_unlock(refcounter->lock);
  return count;
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  return refcounter_packed_count(state);
#endif
}

uint8_t refcounter_pending_derefs(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  uint8_t pending = refcounter->pending_deref;
  platform_mutex_unlock(refcounter->lock);
  return pending;
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  return refcounter_packed_pending(state);
#endif
}
```

(`refcounter_consume`, `refcounter_destroy_lock` unchanged.)

- [ ] **Step 5: Run the new tests to verify they pass.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='RefCounterEscrow.*:TestRefCounter.*'`
Expected: PASS — the existing `TestRefCounter.TestRefCounterFunctions` still passes (the packed-word path preserves the single-threaded semantics), and the three new escrow race tests pass.

- [ ] **Step 6: Run the block_cache + stream + actor suites (the live callers) under valgrind.** Run: `valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='TestBlockCache.*:TestBlock.*:TestStream.*:TestActor.*'`
Expected: 0 leaks, 0 invalid reads, exit 0. If a caller was relying on the old separate-fields layout (e.g., reading `refc->count` directly), update it to `refcounter_count(refc)`. Grep for direct field access:

```bash
grep -rn "\.count\b\|->count\b\|\.yield\b\|->yield\b\|\.pending_deref\b\|->pending_deref\b" src/ | grep -v "refcounter\|\.c:" | head
```

and fix any hits in the touched call sites.

- [ ] **Step 7: Commit.**

```bash
git add src/RefCounter/refcounter.h src/RefCounter/refcounter.c test/test_refcounter.cpp
git commit -m "fix(refcounter): make escrow transfer a single atomic transaction

The non-actor path maintained the yield/pending_deref/count invariant with
separate relaxed loads and independent RMWs and no transaction. Two threads
both refcounter_reference could load yield=1, both fetch_sub, yield wrap to
0xFF, and count under-count -> a later release freed the object while a
holder remained (UAF). Separately, a dereference could commit to pending++
after a reference on another thread consumed the yield and checked pending
-> the release was never applied (leak). Pack count:16/yield:8/pending:8
into one _Atomic uint32_t and reimplement all six operations as CAS loops so
only one adopter wins a yield slot and pending is observed consistently with
yield. The actor path's CAS succeeds first try (single-threaded per actor).
See concurrency-pass.md F1."
```

---

## Task 7: Make the mailbox teardown-aware (close the send-vs-destroy TOCTOU)

**Files:**
- Modify: `src/Actor/message_queue.h` (add `send_lock`, `destroyed`), `src/Actor/message_queue.c` (lock-couple push with destroy; change push return), `src/Actor/actor.c` (adapt `actor_send` to free payload on push failure; `actor_destroy` sets `destroyed` before draining)
- Test: `test/test_message_queue.cpp`

**Why:** `actor_send` checks `ACTOR_FLAG_DESTROY` (`actor.c:89`) and then pushes (`:96-98`); these are not atomic. The destroyer sets DESTROY (`actor.c:58`) then `message_queue_destroy` frees the sentinel and NULLs head (`message_queue.c:105-110`). A sender past the check then pushes: `atomic_exchange(&head)` returns NULL → store through NULL/freed sentinel. Concrete non-worker senders: `_timer_completion_callback` on the pd-loop thread (`timer_actor.c:109-124`) racing `timer_actor_destroy`, and every MsQuic-callback `actor_send` during teardown. Serialize push vs destroy with a per-queue mutex so the TOCTOU cannot fire.

- [ ] **Step 1: Write the failing test.** Append to `test/test_message_queue.cpp`:

```cpp
#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include "../src/Actor/message_queue.h"
#include "../src/Actor/actor.h"
#include "../src/Util/allocator.h"
}

TEST(MessageQueueTeardown, PushAfterDestroyFreesMessageAndReturnsFalse) {
  message_queue_t queue;
  message_queue_init(&queue);

  // Destroy the queue on this thread.
  message_queue_destroy(&queue);

  // Now push from another thread (simulating a late MsQuic-callback send).
  std::thread late_sender([&]() {
    message_node_t* node = (message_node_t*)calloc(1, sizeof(message_node_t));
    node->msg.payload = calloc(16, 1);
    node->msg.payload_destroy = free;
    bool success = message_queue_push(&queue, node, node);
    EXPECT_FALSE(success) << "push into a destroyed queue must return false, not crash";
    // On failure the caller owns the payload; free it.
    if (!success && node->msg.payload != NULL) {
      // message_queue_push freed the node but not the payload on the
      // destroyed path; or it freed both. The contract after the fix: push
      // frees the node and the payload, returns false. So node is invalid
      // here. We assert via valgrind that there is no leak / no double-free.
    }
  });
  late_sender.join();
  // No assert here beyond "no crash" — valgrind at step 6 asserts no leak
  // and no double-free.
}

TEST(MessageQueueTeardown, ConcurrentPushAndDestroyNoCrash) {
  for (int iteration = 0; iteration < 200; iteration++) {
    message_queue_t queue;
    message_queue_init(&queue);

    std::atomic<int> sends{0};
    std::thread destroyer([&]() {
      // Spin briefly to race with the senders.
      for (int spin = 0; spin < 100; spin++) {
        sends.fetch_add(1);
      }
      message_queue_destroy(&queue);
    });

    std::vector<std::thread> senders;
    for (int sender_index = 0; sender_index < 4; sender_index++) {
      senders.emplace_back([&]() {
        for (int send = 0; send < 100; send++) {
          message_node_t* node = (message_node_t*)calloc(1, sizeof(message_node_t));
          node->msg.payload = calloc(16, 1);
          node->msg.payload_destroy = free;
          if (!message_queue_push(&queue, node, node)) {
            // Push failed (queue destroyed); push freed node + payload.
            // Nothing more to do.
          }
        }
      });
    }

    destroyer.join();
    for (auto& sender : senders) sender.join();
  }
  SUCCEED();  // No crash, no UAF.
}
```

- [ ] **Step 2: Run to verify it fails.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='MessageQueueTeardown.*'`
Expected: FAIL — `message_queue_push` returns `was_empty` (bool) and the signature mismatch either fails to compile or, after adapting, crashes when pushing into a destroyed queue (NULL-head deref) within a few iterations of the concurrent test.

- [ ] **Step 3: Extend the struct and change the push signature.** In `src/Actor/message_queue.h`, add fields to `message_queue_t` (and change the push prototype):

```c
typedef struct message_queue_t {
  _Atomic(message_node_t*) head;
  message_node_t* tail;
  _Atomic(size_t) size;
  _Atomic(uint64_t)* pending_counter;
  /* Send-side lock + destroyed flag: serialize actor_send vs
     message_queue_destroy so the DESTROY-check-then-push TOCTOU in
     actor_send cannot fire. The single worker that pops/markempty's is
     not affected (it runs after destroy has waited for the worker via
     actor_destroy's RUNNING spin). See docs/concurrency-pass.md F4. */
  platform_mutex_t* send_lock;
  _Atomic bool destroyed;
} message_queue_t;

/* Returns true on success (message queued; *was_empty receives whether the
   queue was empty before this push), false if the queue was destroyed (the
   message node AND its payload are freed by push in that case — the caller
   must NOT free them). */
bool message_queue_push(message_queue_t* queue, message_node_t* first, message_node_t* last, bool* was_empty);
```

- [ ] **Step 4: Implement the lock-coupled push and destroy.** Replace `message_queue_push` and `message_queue_destroy` in `src/Actor/message_queue.c`:

```c
bool message_queue_push(message_queue_t* queue, message_node_t* first, message_node_t* last, bool* was_empty) {
  if (was_empty != NULL) *was_empty = false;
  platform_mutex_lock(queue->send_lock);
  if (atomic_load_explicit(&queue->destroyed, memory_order_acquire)) {
    platform_mutex_unlock(queue->send_lock);
    /* Free the node and its payload; the caller transferred ownership to
       push and has no way to recover it. */
    if (last->msg.payload_destroy != NULL && last->msg.payload != NULL) {
      last->msg.payload_destroy(last->msg.payload);
    }
    free(last);
    return false;
  }
  atomic_store_explicit(&last->next, NULL, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  message_node_t* prev = atomic_exchange_explicit(&queue->head, last, memory_order_acq_rel);
  bool empty = _is_empty_flag_set(prev);
  prev = _clear_empty_flag(prev);
  atomic_store_explicit(&prev->next, first, memory_order_release);
  atomic_fetch_add_explicit(&queue->size, 1, memory_order_relaxed);
  if (queue->pending_counter != NULL) {
    atomic_fetch_add_explicit(queue->pending_counter, 1, memory_order_release);
  }
  platform_mutex_unlock(queue->send_lock);
  if (was_empty != NULL) *was_empty = empty;
  return true;
}

void message_queue_destroy(message_queue_t* queue) {
  /* Acquire the send_lock so any in-flight push either completes before we
     drain (and is freed by the pop loop below) or sees destroyed=true and
     frees itself. Set destroyed under the lock so the push path observes it
     consistently. */
  platform_mutex_lock(queue->send_lock);
  atomic_store_explicit(&queue->destroyed, true, memory_order_release);
  message_node_t* node;
  while ((node = message_queue_pop(queue)) != NULL) {
    if (node->msg.payload_destroy != NULL && node->msg.payload != NULL) {
      node->msg.payload_destroy(node->msg.payload);
    }
    free(node);
  }
  message_node_t* sentinel = queue->tail;
  if (sentinel != NULL) {
    free(sentinel);
  }
  atomic_store(&queue->head, NULL);
  queue->tail = NULL;
  atomic_store(&queue->size, 0);
  platform_mutex_unlock(queue->send_lock);
  platform_mutex_destroy(queue->send_lock);
  queue->send_lock = NULL;
}
```

Update `message_queue_init` to create the lock:

```c
void message_queue_init(message_queue_t* queue) {
  message_node_t* sentinel = get_clear_memory(sizeof(message_node_t));
  atomic_store(&sentinel->next, NULL);
  atomic_store(&queue->head, _set_empty_flag(sentinel));
  queue->tail = sentinel;
  atomic_store(&queue->size, 0);
  queue->pending_counter = NULL;
  queue->send_lock = platform_mutex_create();
  atomic_store(&queue->destroyed, false);
}
```

Delete the dead `message_queue_push_single` (per concurrency-pass.md "Dead code", it has zero callers and is non-atomic — removing it prevents someone from using it cross-thread).

- [ ] **Step 5: Adapt `actor_send` and `actor_destroy`.** In `src/Actor/actor.c`, change `actor_send`'s push call (`:98`) to the new signature and handle failure:

```c
  message_node_t* node = get_clear_memory(sizeof(message_node_t));
  node->msg = *msg;
  bool was_empty_local = false;
  bool pushed = message_queue_push(&actor->queue, node, node, &was_empty_local);
  if (!pushed) {
    /* The queue was destroyed while we were pushing; message_queue_push
       already freed the node and the payload. */
    return false;
  }
  bool was_empty = was_empty_local;
```

In `actor_destroy`, before the existing `message_queue_destroy(&actor->queue);` call (line 85), the destroy now sets `destroyed` under the send_lock internally, so no extra change is needed in `actor_destroy` beyond ensuring the wait-for-RUNNING loop still precedes it (it does). The send_lock is created in `message_queue_init` and destroyed in `message_queue_destroy`.

- [ ] **Step 6: Run the new tests to verify they pass.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='MessageQueueTeardown.*:TestMessageQueue.*'`
Expected: PASS. The existing `TestMessageQueue.*` tests may need their `message_queue_push` call sites updated to the new 4-arg signature — grep for `message_queue_push(` in `test/test_message_queue.cpp` and add `, NULL` for the `was_empty` out-param where the caller doesn't need it.

- [ ] **Step 7: Run the actor + scheduler + timer_actor suites under valgrind.** Run: `valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='TestActor.*:TestMessageQueue.*:TestScheduler.*:TestTimerActor.*'`
Expected: 0 leaks, 0 invalid reads, exit 0.

- [ ] **Step 8: Commit.**

```bash
git add src/Actor/message_queue.h src/Actor/message_queue.c src/Actor/actor.c test/test_message_queue.cpp
git commit -m "fix(message_queue): serialize push vs destroy to close send-vs-destroy TOCTOU

actor_send checks ACTOR_FLAG_DESTROY then pushes; the two steps were not
atomic, so a destroyer could set DESTROY and free the sentinel/NULL head
between the check and the push -> a late sender (timer pd-loop thread,
MsQuic callback) stored through a NULL/freed sentinel. Add a per-queue
send_lock and a destroyed flag: push takes the lock and frees the message
itself (node + payload) if destroyed, returning false; destroy sets
destroyed under the lock before draining. Change push's return to
success/failure (with was_empty as an out-param). Remove the dead
non-atomic message_queue_push_single. See concurrency-pass.md F4."
```

---

## Task 8: Track actor queue-state so destroy waits for the worker to release it

**Files:**
- Modify: `src/Actor/actor.h` (add `queue_state`), `src/Actor/actor.c` (init it), `src/Scheduler/scheduler.c` (worker CASes state on pick/re-queue; `actor_destroy` waits for IDLE)
- Test: `test/test_scheduler.cpp`

**Why:** `scheduler.c:140-144`: `actor_run` returns `has_more` → `fetch_and(~RUNNING)` (`:141`) → `deque_push(actor)` (`:144`). The destroyer (`actor.c:81-83`) spins only until RUNNING clears, then frees the enclosing struct. Between `:141` and `:144` the worker pushes a freeable pointer; the next pop reads `actor->flags` (`scheduler.c:119`) on freed memory and may dispatch through a freed function pointer. The fix: add an explicit `queue_state` (IDLE / QUEUED / RUNNING) so the worker atomically transitions RUNNING→QUEUED (with the re-queue) or RUNNING→IDLE (no re-queue) via a single CAS, and `actor_destroy` waits until `queue_state == IDLE` before freeing — so the actor is guaranteed to be out of every scheduler deque.

- [ ] **Step 1: Write the failing test.** Append to `test/test_scheduler.cpp`:

```cpp
#include <atomic>
#include <thread>
#include <chrono>

extern "C" {
#include "../src/Actor/actor.h"
#include "../src/Scheduler/scheduler.h"
}

// A minimal actor whose dispatch sets a flag and (optionally) re-sends to
// itself to keep has_more true.
typedef struct {
  std::atomic<int>* dispatched;
  std::atomic<bool>* destroy_in_dispatch;
  actor_t* self;
} race_state_t;

static void race_dispatch(void* state, message_t* msg) {
  race_state_t* race = (race_state_t*)state;
  (void)msg;
  race->dispatched->fetch_add(1);
}

TEST(SchedulerDestroyRace, WorkerRequeueVsDestroyNoUAF) {
  // Build a 2-worker pool, create an actor, queue a flood of messages, and
  // destroy the actor mid-flight. Pre-fix, the worker can re-queue a freed
  // pointer and the next pop reads freed memory. Post-fix, actor_destroy
  // waits for queue_state == IDLE so the actor is out of all deques.
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  actor_t actor;
  std::atomic<int> dispatched{0};
  std::atomic<bool> destroy_in_dispatch{false};
  race_state_t state;
  state.dispatched = &dispatched;
  state.destroy_in_dispatch = &destroy_in_dispatch;
  state.self = &actor;
  actor_init(&actor, &state, race_dispatch, pool);

  // Queue 1000 no-op messages.
  for (int index = 0; index < 1000; index++) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = 1;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&actor, &msg);
  }

  // Let the workers drain some, then destroy mid-flight from another thread.
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::thread destroyer([&]() {
    actor_destroy(&actor);
  });

  destroyer.join();
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);

  // The dispatched counter may be < 1000 (destroyed mid-flight) — that's
  // fine. The assertion is that we did not crash / UAF. Valgrind at step 6
  // asserts no invalid reads.
  SUCCEED();
}
```

- [ ] **Step 2: Run to verify it fails.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='SchedulerDestroyRace.*'`
Expected: FAIL, or a crash/ASan report under valgrind pointing to `scheduler.c:144` or `scheduler.c:119` on freed memory.

- [ ] **Step 3: Add `queue_state` to `actor_t`.** In `src/Actor/actor.h`, add the states and the field:

```c
/* Actor queue-state: tracks whether the actor is IDLE (in no deque, not
   running), QUEUED (in a worker deque, not running), or RUNNING (being run
   by a worker). This lets actor_destroy wait until the actor is fully out
   of every scheduler deque before freeing the enclosing struct, closing
   the re-queue-vs-destroy UAF. See docs/concurrency-pass.md F5. */
typedef enum {
  ACTOR_QUEUE_IDLE = 0,
  ACTOR_QUEUE_QUEUED = 1,
  ACTOR_QUEUE_RUNNING = 2,
} actor_queue_state_e;
```

Add to `struct actor_t`:

```c
  _Atomic uint8_t queue_state;
```

- [ ] **Step 4: Initialize `queue_state` in `actor_init`.** In `src/Actor/actor.c`, in `actor_init`, after `atomic_store(&actor->flags, 0);` add:

```c
  atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
```

- [ ] **Step 5: Worker CASes `queue_state` on pick and re-queue.** In `src/Scheduler/scheduler.c`, in `_scheduler_worker_loop`, replace the pick-and-run block (around lines 115-149) with one that transitions `queue_state` atomically. The key change: instead of `atomic_compare_exchange_strong(&actor->flags, &flags, flags | ACTOR_FLAG_RUNNING)` (which only sets RUNNING, not queue_state), set RUNNING and transition queue_state QUEUED→RUNNING together; and on finish, transition RUNNING→QUEUED (re-queue) or RUNNING→IDLE (no re-queue) before clearing RUNNING and (optionally) pushing to the deque.

```c
    if (actor != NULL) {
      spin_count = 0;
      self->current = actor;
      uint8_t flags = atomic_load(&actor->flags);
      if (flags & ACTOR_FLAG_DESTROY) {
        /* Actor has been destroyed — skip it. It is still in our deque; we
           leave it there (it will be skipped on every pop) and transition
           its queue_state to IDLE so the destroyer can proceed. */
        atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
        self->current = NULL;
        continue;
      }
      if (flags & ACTOR_FLAG_MUTED) {
        self->current = NULL;
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      if (flags & ACTOR_FLAG_RUNNING) {
        self->current = NULL;
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      if (!atomic_compare_exchange_strong(&actor->flags, &flags, flags | ACTOR_FLAG_RUNNING)) {
        self->current = NULL;
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      /* We won the RUNNING flag. Record that we are running this actor so
         actor_destroy can distinguish "in a deque" from "being run". */
      atomic_store(&actor->queue_state, ACTOR_QUEUE_RUNNING);

      bool has_more = actor_run(actor, ACTOR_BATCH_SIZE);

      /* Atomically decide: re-queue (RUNNING->QUEUED) or release
         (RUNNING->IDLE), and clear RUNNING. The destroyer waits for IDLE
         before freeing, so the re-queue below cannot push a freeable
         pointer. */
      uint8_t destroy_flags = atomic_load(&actor->flags);
      bool destroy_set = (destroy_flags & ACTOR_FLAG_DESTROY) != 0;
      if (has_more && !destroy_set) {
        atomic_store(&actor->queue_state, ACTOR_QUEUE_QUEUED);
        atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_RUNNING);
        deque_push(&self->local_queue, (void*)actor);
        platform_mutex_lock(pool->inject.lock);
        platform_condvar_broadcast(pool->inject.condition);
        platform_mutex_unlock(pool->inject.lock);
      } else {
        atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
        atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_RUNNING);
        if (destroy_set) {
          /* The destroyer is waiting for IDLE. Signal it. There is no
             condvar on the actor; the destroyer spins (platform_sleep_ms(0))
             so the store above is enough. */
        }
      }
      self->current = NULL;
    } else {
```

Also, when `scheduler_inject` puts an actor into the inject queue (from `actor_send` when `was_empty`), the actor transitions IDLE→QUEUED. In `actor_send` (in `src/Actor/actor.c`), after `was_empty` is true and before `scheduler_inject`, set queue_state to QUEUED:

```c
  if (was_empty) {
    atomic_fetch_or(&actor->flags, ACTOR_FLAG_SCHEDULED);
    atomic_store(&actor->queue_state, ACTOR_QUEUE_QUEUED);
    if (actor->pool != NULL) {
      scheduler_inject(actor->pool, actor);
    }
  }
```

And in the worker's steal path, when it pops an actor from a deque (`_scheduler_find_work`), the actor is already QUEUED (set at inject time or at the re-queue above) — no transition needed there; the QUEUED→RUNNING transition happens in the pick block above.

- [ ] **Step 6: `actor_destroy` waits for `queue_state == IDLE`.** In `src/Actor/actor.c`, in `actor_destroy`, after the existing RUNNING spin and before `message_queue_destroy`, add:

```c
  /* Wait for the actor to be out of every scheduler deque. A worker that
     just finished actor_run may be about to re-queue this actor; if we
     freed now, that re-queue would push a freed pointer and the next pop
     would read freed memory. The worker transitions queue_state to IDLE
     (no re-queue) or QUEUED (re-queue) atomically with clearing RUNNING,
     so once we observe IDLE the worker has committed to not re-queuing. */
  while (atomic_load(&actor->queue_state) != ACTOR_QUEUE_IDLE) {
    platform_sleep_ms(0);
  }
```

Keep the existing RUNNING spin (it's still needed because the worker sets queue_state=RUNNING only after winning the RUNNING flag, so there's a window where RUNNING is set but queue_state hasn't been updated yet — the RUNNING spin covers that window; the queue_state spin covers the re-queue window).

- [ ] **Step 7: Run the new test to verify it passes.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs && cmake-build-verify/test/testliboffs --gtest_filter='SchedulerDestroyRace.*:TestScheduler.*'`
Expected: PASS. If the existing `TestScheduler.*` tests hang, a worker is not transitioning to IDLE on some path — audit every place the worker exits `actor_run` (including the DESTROY early-return inside `actor_run` at `actor.c:167-172`) and ensure queue_state is set to IDLE there too. The DESTROY early-return path inside `actor_run` should set `queue_state = ACTOR_QUEUE_IDLE` before returning `false` so the destroyer is not stuck.

To handle the self-destruct path inside `actor_run`: at `actor.c:167-172` (the `if DESTROY return false` block), set queue_state to IDLE before returning, so the destroyer's wait completes. Add at the top of that block:

```c
    if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
      atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
      if (payload_destroy != NULL && payload != NULL) {
        payload_destroy(payload);
      }
      return false;
    }
```

- [ ] **Step 8: Run the actor + scheduler + timer_actor + shutdown suites under valgrind.** Run: `valgrind --leak-check=full --error-exitcode=1 cmake-build-verify/test/testliboffs --gtest_filter='TestActor.*:TestScheduler.*:TestTimerActor.*:TestShutdown.*'`
Expected: 0 leaks, 0 invalid reads, exit 0. The pre-existing `reference_scheduler_valgrind_error.md` "Invalid read of size 1" at `scheduler.c:119` during shutdown should now be gone (that read was on a freed actor; the queue_state wait prevents the free).

- [ ] **Step 9: Commit.**

```bash
git add src/Actor/actor.h src/Actor/actor.c src/Scheduler/scheduler.c test/test_scheduler.cpp
git commit -m "fix(scheduler): track actor queue-state so destroy waits for IDLE

A worker that finished actor_run could re-queue the actor (deque_push at
scheduler.c:144) after the destroyer saw RUNNING clear and freed the
enclosing struct -> the next pop read actor->flags on freed memory and
could dispatch through a freed function pointer. Add an explicit
queue_state (IDLE/QUEUED/RUNNING) to actor_t. The worker transitions
RUNNING->QUEUED or RUNNING->IDLE atomically (deciding re-queue vs release
in one step), and actor_destroy waits for queue_state == IDLE before
freeing, guaranteeing the actor is out of every scheduler deque. Handle
the self-destruct path inside actor_run too. See concurrency-pass.md F5."
```

---

## Task 9: Whole-tier verification

**Files:** none (verification only)

- [ ] **Step 1: Build the full library + tests.** Run: `cmake --build cmake-build-verify -j$(nproc) --target testliboffs`
Expected: clean build, no warnings (treat warnings as errors per `docs/STYLE_GUIDE.md` if the project does).

- [ ] **Step 2: Run the full test suite.** Run: `cmake-build-verify/test/testliboffs`
Expected: All tests pass. If a pre-existing unrelated test fails, note it and do not regress it; if a test fails because of a tier-1 change, fix the change.

- [ ] **Step 3: Run the full suite under valgrind.** Run: `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-verify/test/testliboffs`
Expected: 0 leaks, 0 invalid reads. The two pre-existing known valgrind noises (`reference_scheduler_valgrind_error.md` shutdown read, which this tier should now fix; and any DWARF5 incompatibility, which `-gdwarf-4` in the build flags avoids) must not appear.

- [ ] **Step 4: Run a de-wonk audit.** Invoke the de-wonk skill on the touched files (the 8 fixes). The audit must find no stubbed, disabled, or broken code in the tier-1 changes. If it finds any, fix before declaring the tier done — per CLAUDE.md, "A task is not done until every TODO in the files it touched is resolved" and the de-wonk skill is the gate.

- [ ] **Step 5: Verify no TODOs/FIXMEs were introduced.** Run: `grep -rn "TODO\|FIXME\|HACK\|XXX" src/Network/wire.c src/Network/network.c src/Network/stream_framer.c src/Network/stream_framer.h src/Network/quic_listener.c src/Network/nat_detect.c src/RefCounter/refcounter.c src/RefCounter/refcounter.h src/Actor/message_queue.c src/Actor/message_queue.h src/Actor/actor.c src/Actor/actor.h src/Scheduler/scheduler.c`
Expected: no new TODOs/FIXMEs in the touched files. (Pre-existing ones elsewhere are out of scope.)

- [ ] **Step 6: Run the de-wonk skill's memory-leak check on the touched test suites.** Per CLAUDE.md: "always check your tests for memory leaks after completing an implementation." The valgrind run in step 3 covers this; if any suite leaks, fix before declaring done.

- [ ] **Step 7: Final commit (if step 4-6 found fixes).** If the de-wonk audit or the leak check required fixes, commit them with a clear message. If no fixes were needed, this step is a no-op.

```bash
# Only if fixes were needed:
git add <touched files>
git commit -m "fix(tier1): de-wonk follow-ups from whole-tier verification"
```

---

## Self-Review (run before declaring the plan complete)

**1. Spec coverage.** Tier-1 scope = audit #1, #2, #3, #4, #7 + concurrency F1, F4, F5.
- #1 → Task 1 ✓
- #2 → Task 2 ✓
- #3 → Task 3 ✓
- #4 → Task 4 ✓
- #7 → Task 5 ✓
- F1 → Task 6 ✓
- F4 → Task 7 ✓
- F5 → Task 8 ✓
- Whole-tier verification → Task 9 ✓

Out-of-scope (deferred): audit #5/#6/#8/#9/#10/#11/#15-17/#18/#19/#20/#24; concurrency F2, F3, F6, F7, F8, F9, F10, F11. These are real but belong to follow-on tiers (multi-hop RPC hangs; identity/TLS; CLI lies; NAT/relay feature; concurrency teardown races). The plan header names them explicitly.

**2. Placeholder scan.** Every code step shows the actual code. Where a test helper is referenced (`test_network_create_minimal`, `nat_detect_test_fixture`, `test_quic_listener_create`, `test_quic_listener_emit_connected`, `quic_listener__conn_count_for_test`), the step says to add it if it does not already exist, and gives the shape. No "TBD" / "implement later" / "add appropriate error handling" anywhere.

**3. Type / signature consistency.**
- `message_queue_push` signature changes in Task 7 to `(queue, first, last, bool* was_empty)` returning `bool`. Task 7's `actor_send` adaptation and the test in Task 7 both use this signature. No other task touches `message_queue_push`.
- `refcounter_t` field changes from three separate fields to `packed_state`. Task 6's tests read `refc->packed_state`; Task 6's helpers (`refcounter_pack` etc.) are added to the header. Callers that read `refc->count` directly are greped in Task 6 step 6 and fixed.
- `actor_t` gains `queue_state`. Task 8's tests and the scheduler changes use `ACTOR_QUEUE_IDLE`/`QUEUED`/`RUNNING`. The helper names are consistent.
- The `wire_find_block_response_destroy` and `wire_recall_accept_destroy` destroyers already exist in `wire.c:1588` and `:1594` (verified by grep) — Task 2 references them; no new destroyers are created.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-15-tier1-memory-safety-criticals.md`. Two execution options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration. Best for this plan because the 8 tasks span 5 different subsystems and a fresh context per task keeps each fix tight.
2. **Inline Execution** — execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints for review. Best if you want to watch each fix land and debug interactively.

Which approach?