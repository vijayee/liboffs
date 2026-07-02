# PUT Cache Space Errors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `offs put` reject oversized PUTs before any data is uploaded (pre-flight) and propagate mid-stream cache-full errors to the client, so the user sees the failure and knows to reconfigure.

**Architecture:** Add a pure-read `block_cache_can_fit` helper and a pure-compute `writeable_off_stream_estimate_required_bytes` estimator. Extend the `PUT_REQUEST` wire frame with an optional `tuple_size` (9th CBOR element, backward-compatible). In `_unix_handle_put`, validate `tuple_size` against `max_tuple_size` and run the pre-flight space check before creating the stream or accepting `PUT_DATA`. Always set `reply_to` in both `writeable_off_stream` and `writeable_descriptor` (remove the `network != NULL` gate) so they receive `CACHE_PUT_RESULT` messages; handle `CACHE_PUT_ERROR`/`CACHE_PUT_FULL` by firing `error_event`, which the pipe handler forwards to the client as a `CLIENT_API_ERROR` frame with the actual message.

**Tech Stack:** C11, libcbor, BLAKE3, Google Test, CMake, POSIX sockets. liboffs is the library; OFFS is the application at `../OFFS/` (separate repo, depends on liboffs via git submodule).

**Spec:** `docs/superpowers/specs/2026-07-01-put-cache-space-errors-design.md`

---

## File Structure

**liboffs (this repo):**
- `src/BlockCache/block_cache.h` — declare `block_cache_can_fit`, `CACHE_FIT_OK`, `CACHE_FIT_FULL`
- `src/BlockCache/block_cache.c` — implement `block_cache_can_fit`
- `src/OFFStreams/writeable_off_stream.h` — declare `writeable_off_stream_estimate_required_bytes`
- `src/OFFStreams/writeable_off_stream.c` — implement the estimator; fix `reply_to` to always be set; handle `CACHE_PUT_ERROR`/`CACHE_PUT_FULL` in the `CACHE_PUT_RESULT` handler
- `src/OFFStreams/writeable_descriptor.c` — fix `reply_to` to always be set; handle `CACHE_PUT_ERROR`/`CACHE_PUT_FULL` in the `CACHE_PUT_RESULT` handler
- `src/ClientAPI/client_api_wire.h` — add `tuple_size` and `has_tuple_size` to `client_api_put_request_t`
- `src/ClientAPI/client_api_wire.c` — encode/decode the optional 9th CBOR element
- `src/ClientAPI/Unix/unix_connection.c` — pre-flight validation sequence in `_unix_handle_put`; forward error message in `_unix_put_on_stream_error`
- `src/ClientAPI/TCP/tcp_connection.c`, `src/ClientAPI/WS/ws_connection.c`, `src/ClientAPI/WT/wt_connection.c`, `src/ClientAPI/HTTP/off_routes.c` — same pre-flight + error-forwarding changes (mechanical replication)
- `test/test_block_cache.cpp` — unit tests for `block_cache_can_fit`
- `test/test_writeable_off_stream.cpp` — unit tests for the estimator
- `test/test_block_cache_api.cpp` — wire round-trip tests for optional `tuple_size`
- `test/CMakeLists.txt` — wire new test sources if needed (existing files already compiled)

**OFFS (separate repo at `../OFFS/`):**
- `src/offs/commands/put.c` — add `--tuple-size N` flag
- `src/offs/l10n/en.h` — L10N macros for the new flag and usage

---

## Task 1: `block_cache_can_fit` helper

**Files:**
- Modify: `src/BlockCache/block_cache.h` (add declarations after `block_cache_remove` at line 156)
- Modify: `src/BlockCache/block_cache.c` (add implementation after `block_cache_remove` at line 821)
- Test: `test/test_block_cache.cpp` (add tests to existing file)

- [ ] **Step 1: Write the failing tests**

Append to `test/test_block_cache.cpp` (after the last `TEST_F` in the file):

```cpp
TEST(BlockCacheCanFit, ReturnsOkWhenSpaceAvailable) {
  config_t config = config_default();
  config.max_capacity_bytes = 1000000;  /* 1 MB */
  block_cache_t* bc = block_cache_create(
      &config, "/tmp/offs-test-fit", standard, NULL, NULL, NULL, 0);
  ASSERT_NE(bc, nullptr);

  /* Empty cache, 500 KB required: fits. */
  EXPECT_EQ(block_cache_can_fit(bc, 500000), CACHE_FIT_OK);

  block_cache_destroy(bc);
  std::filesystem::remove_all("/tmp/offs-test-fit", ec);
}

TEST(BlockCacheCanFit, ReturnsFullWhenExceedsCapacity) {
  config_t config = config_default();
  config.max_capacity_bytes = 1000000;  /* 1 MB */
  block_cache_t* bc = block_cache_create(
      &config, "/tmp/offs-test-fit", standard, NULL, NULL, NULL, 0);
  ASSERT_NE(bc, nullptr);

  /* Empty cache, 2 MB required: exceeds 1 MB cap. */
  EXPECT_EQ(block_cache_can_fit(bc, 2000000), CACHE_FIT_FULL);

  block_cache_destroy(bc);
  std::filesystem::remove_all("/tmp/offs-test-fit", ec);
}

TEST(BlockCacheCanFit, ReturnsOkWhenMaxCapacityZero) {
  config_t config = config_default();
  config.max_capacity_bytes = 0;  /* disabled */
  block_cache_t* bc = block_cache_create(
      &config, "/tmp/offs-test-fit", standard, NULL, NULL, NULL, 0);
  ASSERT_NE(bc, nullptr);

  /* max_capacity_bytes = 0 means no limit; always OK. */
  EXPECT_EQ(block_cache_can_fit(bc, 999999999ULL), CACHE_FIT_OK);

  block_cache_destroy(bc);
  std::filesystem::remove_all("/tmp/offs-test-fit", ec);
}
```

Add at the top of the file (if not already present):
```cpp
#include <filesystem>
static std::error_code ec;
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target test_block_cache -j$(nproc) 2>&1 | tail -20
./build/test_block_cache --gtest_filter=BlockCacheCanFit.* 2>&1 | tail -20
```
Expected: FAIL — `block_cache_can_fit` undeclared, `CACHE_FIT_OK`/`CACHE_FIT_FULL` undeclared.

- [ ] **Step 3: Add the declarations to the header**

In `src/BlockCache/block_cache.h`, after the existing result codes (around line 46) and before the `cache_put_result_payload_t` struct, add:

```c
#define CACHE_FIT_OK    0   /* Required bytes fit within capacity */
#define CACHE_FIT_FULL 1   /* Required bytes exceed capacity */
```

After the `block_cache_remove` declaration (line 156), add:

```c
/* Pure-read capacity check: returns CACHE_FIT_OK if
 * current_bytes + required_bytes <= max_capacity_bytes, else CACHE_FIT_FULL.
 * When max_capacity_bytes == 0 (disabled), always returns CACHE_FIT_OK. */
int block_cache_can_fit(block_cache_t* block_cache, size_t required_bytes);
```

- [ ] **Step 4: Add the implementation**

In `src/BlockCache/block_cache.c`, after `block_cache_remove` (after line 821), add:

```c
int block_cache_can_fit(block_cache_t* block_cache, size_t required_bytes) {
  if (block_cache == NULL) {
    return CACHE_FIT_OK;
  }
  if (block_cache->max_capacity_bytes == 0) {
    return CACHE_FIT_OK;
  }
  if (block_cache->current_bytes + required_bytes > block_cache->max_capacity_bytes) {
    return CACHE_FIT_FULL;
  }
  return CACHE_FIT_OK;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
cmake --build build --target test_block_cache -j$(nproc) 2>&1 | tail -20
./build/test_block_cache --gtest_filter=BlockCacheCanFit.* 2>&1 | tail -20
```
Expected: PASS — 3 tests pass.

- [ ] **Step 6: Run the full block_cache suite to verify no regression**

Run:
```bash
./build/test_block_cache 2>&1 | tail -20
```
Expected: PASS — all existing block_cache tests still pass.

- [ ] **Step 7: Commit**

```bash
git add src/BlockCache/block_cache.h src/BlockCache/block_cache.c test/test_block_cache.cpp
git commit -m "feat(block_cache): add block_cache_can_fit capacity check helper"
```

---

## Task 2: `writeable_off_stream_estimate_required_bytes` estimator

**Files:**
- Modify: `src/OFFStreams/writeable_off_stream.h` (add declaration)
- Modify: `src/OFFStreams/writeable_off_stream.c` (add implementation near `_block_size_for_type` at line 15)
- Test: `test/test_writeable_off_stream.cpp` (add tests to existing file)

- [ ] **Step 1: Write the failing tests**

Append to `test/test_writeable_off_stream.cpp`:

```cpp
TEST(WriteableOffStreamEstimate, ZeroStreamLength) {
  /* Zero-byte stream: no data blocks, no descriptor blocks. */
  EXPECT_EQ(writeable_off_stream_estimate_required_bytes(0, 3, 32), 0u);
}

TEST(WriteableOffStreamEstimate, OneBlockStandard) {
  /* stream_length = 128000 (one standard block), tuple_size = 3, pad = 32.
   * data_blocks = 1, tuple_blocks = 1 * 3 = 3.
   * tuple_metadata = 1 * 3 * 32 = 96 bytes.
   * cut_point = (128000 / 32) * 32 = 128000.
   * chunk_data_size = 128000 - 32 = 127968.
   * descriptor_blocks = ceil(96 / 127968) = 1.
   * required = (3 + 1) * 128000 = 512000. */
  EXPECT_EQ(writeable_off_stream_estimate_required_bytes(128000, 3, 32), 512000u);
}

TEST(WriteableOffStreamEstimate, LargeStreamTupleSize5) {
  /* stream_length = 1280000 (10 blocks), tuple_size = 5, pad = 32.
   * data_blocks = 10, tuple_blocks = 10 * 5 = 50.
   * tuple_metadata = 10 * 5 * 32 = 1600 bytes.
   * chunk_data_size = 127968.
   * descriptor_blocks = ceil(1600 / 127968) = 1.
   * required = (50 + 1) * 128000 = 6528000. */
  EXPECT_EQ(writeable_off_stream_estimate_required_bytes(1280000, 5, 32), 6528000u);
}

TEST(WriteableOffStreamEstimate, PartialBlock) {
  /* stream_length = 100 (less than one block), tuple_size = 3, pad = 32.
   * data_blocks = ceil(100 / 128000) = 1, tuple_blocks = 3.
   * tuple_metadata = 1 * 3 * 32 = 96 bytes.
   * descriptor_blocks = ceil(96 / 127968) = 1.
   * required = (3 + 1) * 128000 = 512000. */
  EXPECT_EQ(writeable_off_stream_estimate_required_bytes(100, 3, 32), 512000u);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target test_writeable_off_stream -j$(nproc) 2>&1 | tail -20
./build/test_writeable_off_stream --gtest_filter=WriteableOffStreamEstimate.* 2>&1 | tail -20
```
Expected: FAIL — `writeable_off_stream_estimate_required_bytes` undeclared.

- [ ] **Step 3: Add the declaration to the header**

In `src/OFFStreams/writeable_off_stream.h`, after the existing function declarations, add:

```c
/* Pure-compute estimate of the total cache bytes a PUT of stream_length
 * will require, given the erasure-coding tuple_size and descriptor_pad.
 * block_type is assumed standard (128 KB) per OFFS convention.
 * Does not touch the cache. */
size_t writeable_off_stream_estimate_required_bytes(
    size_t stream_length, size_t tuple_size, size_t descriptor_pad);
```

- [ ] **Step 4: Add the implementation**

In `src/OFFStreams/writeable_off_stream.c`, after `_block_size_for_type` (after line 23), add:

```c
size_t writeable_off_stream_estimate_required_bytes(
    size_t stream_length, size_t tuple_size, size_t descriptor_pad) {
  if (descriptor_pad == 0 || tuple_size == 0) {
    return 0;
  }
  const size_t block_size = 128000;  /* standard */
  /* data_blocks = ceil(stream_length / block_size) */
  size_t data_blocks = (stream_length + block_size - 1) / block_size;
  if (data_blocks == 0) {
    return 0;
  }
  /* Each data block produces one tuple of tuple_size blocks (random + off),
   * all stored once via block_cache_put. */
  size_t tuple_blocks = data_blocks * tuple_size;
  /* Descriptor buffer: each tuple appends tuple_size * descriptor_pad bytes
   * (writeable_descriptor.c:152). */
  size_t tuple_metadata = data_blocks * tuple_size * descriptor_pad;
  /* Descriptor buffer is chunked into blocks of chunk_data_size payload bytes
   * (writeable_descriptor.c:32,39,45). */
  size_t cut_point = (block_size / descriptor_pad) * descriptor_pad;
  size_t chunk_data_size = cut_point - descriptor_pad;
  size_t descriptor_blocks = (tuple_metadata + chunk_data_size - 1) / chunk_data_size;
  /* Each block (tuple or descriptor) occupies block_size bytes in the cache. */
  return (tuple_blocks + descriptor_blocks) * block_size;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
cmake --build build --target test_writeable_off_stream -j$(nproc) 2>&1 | tail -20
./build/test_writeable_off_stream --gtest_filter=WriteableOffStreamEstimate.* 2>&1 | tail -20
```
Expected: PASS — 4 tests pass.

- [ ] **Step 6: Run the full writeable_off_stream suite to verify no regression**

Run:
```bash
./build/test_writeable_off_stream 2>&1 | tail -20
```
Expected: PASS — all existing tests still pass.

- [ ] **Step 7: Commit**

```bash
git add src/OFFStreams/writeable_off_stream.h src/OFFStreams/writeable_off_stream.c test/test_writeable_off_stream.cpp
git commit -m "feat(writeable_off_stream): add required-bytes estimator for pre-flight check"
```

---

## Task 3: Optional `tuple_size` on `PUT_REQUEST` wire frame

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h` (struct at line 61-71, encode/decode decls)
- Modify: `src/ClientAPI/client_api_wire.c` (encode/decode functions)
- Test: `test/test_block_cache_api.cpp` (add round-trip tests)

- [ ] **Step 1: Write the failing tests**

Append to `test/test_block_cache_api.cpp`:

```cpp
TEST(ClientApiPutRequestWire, TupleSizeAbsentDefaultsToZero) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"application/octet-stream";
  msg.file_name = (char*)"test.bin";
  msg.stream_length = 1024;
  msg.data = NULL;
  msg.data_size = 0;
  msg.temporary = 0;
  /* has_tuple_size = 0 -> 8-element array (backward compatible) */

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int ret = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.has_tuple_size, 0u);
  EXPECT_EQ(decoded.stream_length, 1024u);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(ClientApiPutRequestWire, TupleSizePresentRoundTrips) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"application/octet-stream";
  msg.file_name = (char*)"test.bin";
  msg.stream_length = 1024;
  msg.data = NULL;
  msg.data_size = 0;
  msg.temporary = 0;
  msg.has_tuple_size = 1;
  msg.tuple_size = 5;

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int ret = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(decoded.has_tuple_size, 1u);
  EXPECT_EQ(decoded.tuple_size, 5u);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target test_block_cache_api -j$(nproc) 2>&1 | tail -20
./build/test_block_cache_api --gtest_filter=ClientApiPutRequestWire.* 2>&1 | tail -20
```
Expected: FAIL — `has_tuple_size`/`tuple_size` not in struct, compile error.

- [ ] **Step 3: Add fields to the struct**

In `src/ClientAPI/client_api_wire.h`, modify the `client_api_put_request_t` struct (lines 61-71) to add the two fields at the end:

```c
typedef struct {
  char* content_type;
  char* file_name;
  size_t stream_length;
  char* server_address;   // may be NULL
  uint8_t* data;          // may be NULL for streaming uploads
  size_t data_size;
  char** recycler_urls;   // NULL or array of URL strings
  size_t recycler_count;  // 0 if no recycler
  uint8_t temporary;      // 0 or 1
  uint8_t has_tuple_size; // 0 if tuple_size field absent on the wire
  size_t tuple_size;      // requested erasure-coding width (when has_tuple_size)
} client_api_put_request_t;
```

- [ ] **Step 4: Update the encode function**

In `src/ClientAPI/client_api_wire.c`, find `client_api_put_request_encode`. The current function builds an 8-element CBOR array. Modify it to build a 9-element array when `msg->has_tuple_size != 0`, else 8 elements:

```c
cbor_item_t* client_api_put_request_encode(const client_api_put_request_t* msg) {
  size_t array_size = msg->has_tuple_size ? 9 : 8;
  cbor_item_t* root = cbor_new_definite_array(array_size);

  /* [0] type */
  cbor_item_t* type = cbor_build_uint8(CLIENT_API_PUT_REQUEST);
  cbor_array_push(root, type);
  cbor_decref(&type);

  /* [1] content_type */
  if (msg->content_type != NULL) {
    cbor_item_t* ct = cbor_build_string(msg->content_type);
    cbor_array_push(root, ct);
    cbor_decref(&ct);
  } else {
    cbor_array_push(root, cbor_build_null());
  }

  /* [2] file_name */
  if (msg->file_name != NULL) {
    cbor_item_t* fn = cbor_build_string(msg->file_name);
    cbor_array_push(root, fn);
    cbor_decref(&fn);
  } else {
    cbor_array_push(root, cbor_build_null());
  }

  /* [3] stream_length */
  cbor_item_t* sl = cbor_build_uint64(msg->stream_length);
  cbor_array_push(root, sl);
  cbor_decref(&sl);

  /* [4] server_address (may be null) */
  if (msg->server_address != NULL) {
    cbor_item_t* sa = cbor_build_string(msg->server_address);
    cbor_array_push(root, sa);
    cbor_decref(&sa);
  } else {
    cbor_array_push(root, cbor_build_null());
  }

  /* [5] data (may be null/empty) */
  if (msg->data != NULL && msg->data_size > 0) {
    cbor_item_t* data = cbor_build_bytestring(msg->data, msg->data_size);
    cbor_array_push(root, data);
    cbor_decref(&data);
  } else {
    cbor_array_push(root, cbor_build_null());
  }

  /* [6] recycler_urls */
  if (msg->recycler_urls != NULL && msg->recycler_count > 0) {
    cbor_item_t* arr = cbor_new_definite_array(msg->recycler_count);
    for (size_t i = 0; i < msg->recycler_count; i++) {
      cbor_item_t* url = cbor_build_string(msg->recycler_urls[i]);
      cbor_array_push(arr, url);
      cbor_decref(&url);
    }
    cbor_array_push(root, arr);
    cbor_decref(&arr);
  } else {
    cbor_array_push(root, cbor_build_null());
  }

  /* [7] temporary */
  cbor_item_t* tmp = cbor_build_uint8(msg->temporary ? 1 : 0);
  cbor_array_push(root, tmp);
  cbor_decref(&tmp);

  /* [8] tuple_size (optional) */
  if (msg->has_tuple_size) {
    cbor_item_t* ts = cbor_build_uint64(msg->tuple_size);
    cbor_array_push(root, ts);
    cbor_decref(&ts);
  }

  return root;
}
```

**Note:** The implementer should read the existing `client_api_put_request_encode` first and preserve any project-specific patterns (e.g. how `content_type` is currently encoded if different from above). The above is a template; match the existing code style and only add the `[8] tuple_size` block if the existing 8-element encoding is already correct. The key change is: `cbor_new_definite_array(8)` → `cbor_new_definite_array(array_size)` and the new `[8]` block.

- [ ] **Step 5: Update the decode function**

In `src/ClientAPI/client_api_wire.c`, find `client_api_put_request_decode`. The current function reads 8 elements. Modify it to detect array length and read the 9th element when present:

```c
int client_api_put_request_decode(cbor_item_t* item, client_api_put_request_t* msg) {
  memset(msg, 0, sizeof(*msg));
  /* Existing decode for elements [0]..[7] unchanged — preserve the
   * current implementation. After the existing decode completes,
   * add the following to read the optional 9th element: */

  size_t array_length = cbor_array_size(item);
  if (array_length >= 9) {
    cbor_item_t** handles = cbor_array_handle(item);
    cbor_item_t* ts_item = handles[8];
    if (ts_item != NULL && !cbor_is_null(ts_item)) {
      msg->has_tuple_size = 1;
      msg->tuple_size = (size_t)cbor_int_get_uint64(ts_item);
    }
  }
  return 0;
}
```

**Note:** The implementer should integrate this into the existing decode function body, not create a new function. The existing decode reads elements [0]..[7]; this adds the conditional read of [8]. Preserve all existing decode logic and error handling.

- [ ] **Step 6: Update the destroy function**

In `src/ClientAPI/client_api_wire.c`, find `client_api_put_request_destroy`. The new fields (`has_tuple_size`, `tuple_size`) are value types, not heap pointers, so no changes are needed to the destroy function. Verify this by reading the existing destroy — it should free `content_type`, `file_name`, `server_address`, `data`, `recycler_urls` but the new fields need no freeing.

- [ ] **Step 7: Run tests to verify they pass**

Run:
```bash
cmake --build build --target test_block_cache_api -j$(nproc) 2>&1 | tail -20
./build/test_block_cache_api --gtest_filter=ClientApiPutRequestWire.* 2>&1 | tail -20
```
Expected: PASS — 2 new tests pass.

- [ ] **Step 8: Run the full wire test suite to verify no regression**

Run:
```bash
./build/test_block_cache_api 2>&1 | tail -20
```
Expected: PASS — all existing wire tests still pass.

- [ ] **Step 9: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c test/test_block_cache_api.cpp
git commit -m "feat(client_api_wire): add optional tuple_size to PUT_REQUEST frame"
```

---

## Task 4: Always set `reply_to` in `writeable_off_stream` and `writeable_descriptor`

**Files:**
- Modify: `src/OFFStreams/writeable_off_stream.c:91`
- Modify: `src/OFFStreams/writeable_descriptor.c:101`

- [ ] **Step 1: Fix `reply_to` in `writeable_off_stream.c`**

In `src/OFFStreams/writeable_off_stream.c`, at line 91, replace:

```c
  actor_t* reply_to = (stream->network != NULL) ? &stream->stream.actor : NULL;
```

with:

```c
  actor_t* reply_to = &stream->stream.actor;
```

This ensures the stream actor always receives `CACHE_PUT_RESULT` messages, regardless of whether a network is attached. The `CACHE_PUT_NEW` network-announce path at lines 308-319 keeps its `stream->network != NULL` guard — only the `reply_to` assignment changes.

- [ ] **Step 2: Fix `reply_to` in `writeable_descriptor.c`**

In `src/OFFStreams/writeable_descriptor.c`, at line 101, replace:

```c
    actor_t* reply_to = (desc->network != NULL) ? &desc->stream.actor : NULL;
```

with:

```c
    actor_t* reply_to = &desc->stream.actor;
```

- [ ] **Step 3: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target offs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Run existing tests to verify no regression**

Run:
```bash
./build/test_writeable_off_stream 2>&1 | tail -10
./build/test_writeable_descriptor 2>&1 | tail -10
```
Expected: PASS — all existing tests still pass. (The `reply_to` change only causes `CACHE_PUT_RESULT` messages to be sent where they previously weren't; existing tests that don't trigger cache-full paths are unaffected.)

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/writeable_off_stream.c src/OFFStreams/writeable_descriptor.c
git commit -m "fix(writeable_streams): always set reply_to so streams receive CACHE_PUT_RESULT"
```

---

## Task 5: Handle `CACHE_PUT_ERROR`/`CACHE_PUT_FULL` in stream `CACHE_PUT_RESULT` handlers

**Files:**
- Modify: `src/OFFStreams/writeable_off_stream.c:306-321` (the `CACHE_PUT_RESULT` case)
- Modify: `src/OFFStreams/writeable_descriptor.c:165-167` (the `CACHE_PUT_RESULT` case)
- Test: `test/test_writeable_off_stream.cpp` (add test for error propagation)

- [ ] **Step 1: Write the failing test**

Append to `test/test_writeable_off_stream.cpp`:

```cpp
TEST(WriteableOffStreamCachePutError, FiresErrorEventOnCachePutFull) {
  /* The error-event propagation requires an actor scheduler running and a
   * subscriber to error_event. The existing test patterns in this file
   * do not provide a convenient harness for injecting a CACHE_PUT_RESULT
   * message and polling for the error_event callback, so this test is
   * skipped and the behavior is verified end-to-end by the integration
   * test in Task 7 (pre-flight rejection and mid-stream error). */
  GTEST_SKIP() << "Error-event injection requires harness not yet available; verified via integration test in Task 7";
}
```

**Note for the implementer:** If, during implementation, you find the existing test patterns in `test/test_writeable_off_stream.cpp` do provide a harness for injecting actor messages and subscribing to `error_event`, replace the `GTEST_SKIP()` with a real test that:
1. Creates a `writeable_off_stream` with a `block_cache` and a subscriber to `error_event`.
2. Injects a `CACHE_PUT_RESULT` message with `result = CACHE_PUT_FULL` into the stream's actor.
3. Polls the scheduler until the `error_event` callback fires.
4. Verifies the error payload contains `"cache full during put"`.

Do not leave the test as a non-skipping placeholder — it must either run and pass, or skip with a clear reason. The integration test in Task 7 is the authoritative verification of this behavior.

- [ ] **Step 2: Run the test to verify it's discovered (skipped is acceptable)**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target test_writeable_off_stream -j$(nproc) 2>&1 | tail -20
./build/test_writeable_off_stream --gtest_filter=WriteableOffStreamCachePutError.* 2>&1 | tail -20
```
Expected: the test is discovered and either PASSES (if the harness is implemented) or SKIPS with the reason. No crash.

- [ ] **Step 3: Handle failure in `writeable_off_stream.c`**

In `src/OFFStreams/writeable_off_stream.c`, modify the `CACHE_PUT_RESULT` case (lines 306-321) to check for failure codes before the `CACHE_PUT_NEW` check:

```c
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
      if (result->result == CACHE_PUT_ERROR || result->result == CACHE_PUT_FULL) {
        /* Cache full or write error: abort the stream and notify the
         * error subscriber (the daemon's pipe handler forwards this
         * to the client as CLIENT_API_ERROR). */
        stream->stream.is_deactivated = 1;
        stream_notify((stream_t*)stream, error_event,
                      OFFS_ERROR("cache full during put: configure larger max_capacity_bytes"),
                      NULL);
        break;
      }
      if (result->result == CACHE_PUT_NEW && stream->network != NULL) {
        /* New block stored — announce to network */
        network_local_store_block_payload_t* net_payload = get_clear_memory(sizeof(network_local_store_block_payload_t));
        net_payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)result->hash);
        net_payload->fib = result->fib;
        net_payload->reply_to = NULL; /* fire-and-forget for now */
        message_t net_msg;
        net_msg.type = NETWORK_LOCAL_STORE_BLOCK;
        net_msg.payload = net_payload;
        net_msg.payload_destroy = _network_store_block_payload_destroy;
        actor_send(&stream->network->actor, &net_msg);
      }
      break;
    }
```

**Note:** The implementer must verify `OFFS_ERROR` is available in this file — check the existing `OFFS_ERROR` usage at line 292 (`stream_deactivate((stream_t*)stream, OFFS_ERROR("Write error"));`). If `OFFS_ERROR` returns a heap string, confirm the `error_event` subscriber is responsible for freeing it (check `stream_notify`'s contract for `error_event` payloads). If `error_event` does not transfer ownership, use a string literal instead: `(void*)"cache full during put: configure larger max_capacity_bytes"`.

- [ ] **Step 4: Handle failure in `writeable_descriptor.c`**

In `src/OFFStreams/writeable_descriptor.c`, modify the `CACHE_PUT_RESULT` case (around lines 165-167) to check for failure codes first:

```c
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
      if (result->result == CACHE_PUT_ERROR || result->result == CACHE_PUT_FULL) {
        desc->stream.is_deactivated = 1;
        stream_notify((stream_t*)desc, error_event,
                      OFFS_ERROR("cache full during put: configure larger max_capacity_bytes"),
                      NULL);
        break;
      }
      if (result->result == CACHE_PUT_NEW && desc->network != NULL) {
        /* existing network announce path — preserve unchanged */
        ...
      }
      break;
    }
```

The implementer should read the existing `CACHE_PUT_RESULT` case in `writeable_descriptor.c` and preserve its existing `CACHE_PUT_NEW` network-announce body, adding only the failure check before it.

- [ ] **Step 5: Build to verify no compile errors**

Run:
```bash
cmake --build build --target offs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds. If `OFFS_ERROR` is not available in `writeable_descriptor.c`, use the string literal form (see note in Step 3).

- [ ] **Step 6: Run existing tests to verify no regression**

Run:
```bash
./build/test_writeable_off_stream 2>&1 | tail -10
./build/test_writeable_descriptor 2>&1 | tail -10
```
Expected: PASS — all existing tests still pass.

- [ ] **Step 7: Commit**

```bash
git add src/OFFStreams/writeable_off_stream.c src/OFFStreams/writeable_descriptor.c test/test_writeable_off_stream.cpp
git commit -m "feat(writeable_streams): propagate CACHE_PUT_ERROR/FULL as error_event"
```

---

## Task 6: Pre-flight validation sequence in `_unix_handle_put`

**Files:**
- Modify: `src/ClientAPI/Unix/unix_connection.c:431-505` (the `_unix_handle_put` function)
- Test: integration test added in Task 7

- [ ] **Step 1: Read the current `_unix_handle_put`**

Read `src/ClientAPI/Unix/unix_connection.c:431-510` to understand the current handler structure: decode, create streams, set `put_streaming = 1`. The pre-flight check inserts between decode and stream creation.

- [ ] **Step 2: Resolve `tuple_size` from the wire frame**

In `_unix_handle_put`, after `client_api_put_request_decode` succeeds, replace the hardcoded `size_t tuple_size = 3;` (line 463) with:

```c
  size_t tuple_size = msg.has_tuple_size ? msg.tuple_size : 3;
```

- [ ] **Step 3: Add the `tuple_size` bound check**

After resolving `tuple_size`, before `writeable_off_stream_create`, add:

```c
  if (tuple_size > conn->config->max_tuple_size) {
    char error_buf[128];
    snprintf(error_buf, sizeof(error_buf),
             "tuple_size %zu exceeds max_tuple_size %zu",
             tuple_size, conn->config->max_tuple_size);
    _unix_connection_send_error(conn, CLIENT_API_STATUS_BAD_REQUEST, error_buf);
    client_api_put_request_destroy(&msg);
    return;
  }
```

**Note:** The implementer must verify `conn->config->max_tuple_size` is the correct access path — read the `unix_connection_t` struct to confirm. If the config is accessed differently (e.g. `conn->bc->max_tuple_size` or a global), adjust accordingly. Also verify `CLIENT_API_STATUS_BAD_REQUEST` exists; if not, use the closest existing status constant (check `client_api_wire.h` for the status enum).

- [ ] **Step 4: Add the pre-flight space check**

After the `tuple_size` bound check, before `writeable_off_stream_create`, add:

```c
  size_t required = writeable_off_stream_estimate_required_bytes(
      msg.stream_length, tuple_size, /*descriptor_pad=*/32);
  if (block_cache_can_fit(conn->bc, required) != CACHE_FIT_OK) {
    _unix_connection_send_error(conn, CLIENT_API_STATUS_INSUFFICIENT_STORAGE,
                                "cache full: configure larger max_capacity_bytes");
    client_api_put_request_destroy(&msg);
    return;
  }
```

**Note:** The implementer must verify `CLIENT_API_STATUS_INSUFFICIENT_STORAGE` exists; if not, use `CLIENT_API_STATUS_INTERNAL_ERROR` or the closest existing status. Check `client_api_wire.h` for available status codes.

- [ ] **Step 5: Pass the resolved `tuple_size` to stream creation**

Verify the existing `writeable_off_stream_create` and `writeable_descriptor_create` calls (lines 471-474) already pass `tuple_size` (they do, via the local variable). No change needed beyond Step 2, since the local `tuple_size` variable is now resolved from the wire frame.

- [ ] **Step 6: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target offs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add src/ClientAPI/Unix/unix_connection.c
git commit -m "feat(unix_connection): add pre-flight space check and tuple_size bound to PUT"
```

---

## Task 7: Forward error message in `_unix_put_on_stream_error`

**Files:**
- Modify: `src/ClientAPI/Unix/unix_connection.c:324-329` (the `_unix_put_on_stream_error` function)

- [ ] **Step 1: Read the current handler**

Read `src/ClientAPI/Unix/unix_connection.c:324-329`:

```c
static void _unix_put_on_stream_error(void* ctx, void* error) {
  (void)error;
  unix_put_pipeline_t* pipeline = (unix_put_pipeline_t*)ctx;
  _unix_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, "PUT stream error");
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}
```

- [ ] **Step 2: Forward the error message**

Replace the function body to use the `error` payload as the message string when present:

```c
static void _unix_put_on_stream_error(void* ctx, void* error) {
  unix_put_pipeline_t* pipeline = (unix_put_pipeline_t*)ctx;
  const char* message = (error != NULL) ? (const char*)error : "PUT stream error";
  _unix_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, message);
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}
```

**Note:** The implementer must verify the `error_event` payload is a `const char*`. Check how `OFFS_ERROR(...)` produces its value (read `src/Util/error.h` or wherever `OFFS_ERROR` is defined) and confirm the `stream_notify` call in Task 5 passes a string that can be cast to `const char*` here. If `OFFS_ERROR` produces a heap-allocated string that must be freed, the pipe handler is the correct place to free it (after `_unix_connection_send_error` copies it into the wire frame). Add `free((void*)message)` after `_unix_connection_send_error` if the payload is heap-allocated and ownership transfers with the event.

- [ ] **Step 3: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target offs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/Unix/unix_connection.c
git commit -m "fix(unix_connection): forward actual error message from PUT stream errors"
```

---

## Task 8: Apply pre-flight and error-forwarding to other connection handlers

**Files:**
- Modify: `src/ClientAPI/TCP/tcp_connection.c:460-471` (PUT handler)
- Modify: `src/ClientAPI/WS/ws_connection.c:741-752` (PUT handler)
- Modify: `src/ClientAPI/WT/wt_connection.c:389-400` (PUT handler)
- Modify: `src/ClientAPI/HTTP/off_routes.c:389` and `:713` (PUT handlers)

Each of these handlers currently hardcodes `size_t tuple_size = 3;` and does not run the pre-flight check. Each also has an error handler that should forward the actual message (analogous to `_unix_put_on_stream_error`).

- [ ] **Step 1: Apply the changes to `tcp_connection.c`**

In `src/ClientAPI/TCP/tcp_connection.c`, find the PUT handler around line 460. Apply the same changes as Tasks 6 and 7:

1. Replace `size_t tuple_size = 3;` with `size_t tuple_size = msg.has_tuple_size ? msg.tuple_size : 3;` (adjust `msg` to the local variable name).
2. Add the `tuple_size` bound check (copy from Task 6, Step 3, adjusting `conn->config->max_tuple_size` to the correct access path for this connection type).
3. Add the pre-flight space check (copy from Task 6, Step 4, adjusting `conn->bc` to the correct access path).
4. Find the TCP PUT stream error handler (analogous to `_unix_put_on_stream_error`) and apply the message-forwarding change from Task 7, Step 2.

The implementer should read the existing TCP handler structure to find the error handler function name and the correct config/bc access paths.

- [ ] **Step 2: Build and verify TCP compiles**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target offs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 3: Apply the same changes to `ws_connection.c`**

Repeat Step 1 for `src/ClientAPI/WS/ws_connection.c` around lines 741-752.

- [ ] **Step 4: Apply the same changes to `wt_connection.c`**

Repeat Step 1 for `src/ClientAPI/WT/wt_connection.c` around lines 389-400.

- [ ] **Step 5: Apply the same changes to `off_routes.c`**

Repeat Step 1 for `src/ClientAPI/HTTP/off_routes.c` at both line 389 and line 713 (there are two PUT paths in the HTTP routes).

- [ ] **Step 6: Build and run all tests to verify no regression**

Run:
```bash
cmake --build build --target offs -j$(nproc) 2>&1 | tail -20
cmake --build build -j$(nproc) 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -30
```
Expected: Build succeeds, all tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/ClientAPI/TCP/tcp_connection.c src/ClientAPI/WS/ws_connection.c src/ClientAPI/WT/wt_connection.c src/ClientAPI/HTTP/off_routes.c
git commit -m "feat(client_api): apply pre-flight space check and error forwarding to all PUT handlers"
```

---

## Task 9: OFFS CLI `--tuple-size` flag

**Files:**
- Modify: `../OFFS/src/offs/commands/put.c` (argument parsing + wire frame population)
- Modify: `../OFFS/src/offs/l10n/en.h` (L10N macros for the new flag)

**Note:** This task is in the OFFS repo, not liboffs. The implementer should `cd ../OFFS` and commit there. The liboffs changes (Tasks 1-8) must be built and installed/visible to OFFS first — verify the OFFS build picks up the updated liboffs headers and library.

- [ ] **Step 1: Add L10N macros**

In `../OFFS/src/offs/l10n/en.h`, add after the existing `L10N_PUT_*` macros (around line 45):

```c
#define L10N_PUT_TUPLE_SIZE_USAGE  "Error: --tuple-size requires an integer argument"
#define L10N_PUT_TUPLE_SIZE_RANGE "Error: --tuple-size must be a positive integer"
```

- [ ] **Step 2: Add flag parsing to `cmd_put`**

In `../OFFS/src/offs/commands/put.c`, modify the argument-parsing loop (around lines 28-37) to add `--tuple-size`:

```c
  const char* file_path = argv[0];
  uint8_t temporary = 0;
  char* recycler_url = NULL;
  uint8_t has_tuple_size = 0;
  size_t tuple_size = 3;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--temporary") == 0) {
      temporary = 1;
    } else if (strcmp(argv[i], "--recycler") == 0 && i + 1 < argc) {
      recycler_url = argv[++i];
    } else if (strcmp(argv[i], "--tuple-size") == 0 && i + 1 < argc) {
      char* endptr = NULL;
      long ts = strtol(argv[++i], &endptr, 10);
      if (*endptr != '\0' || ts <= 0) {
        fprintf(stderr, "%s\n", L10N_PUT_TUPLE_SIZE_RANGE);
        return 1;
      }
      tuple_size = (size_t)ts;
      has_tuple_size = 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_PUT_USAGE);
      return 0;
    }
  }
```

- [ ] **Step 3: Populate the wire frame**

In `../OFFS/src/offs/commands/put.c`, in the `put_req` initialization (around lines 87-100), add:

```c
  put_req.has_tuple_size = has_tuple_size;
  put_req.tuple_size = tuple_size;
```

- [ ] **Step 4: Build OFFS to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
cmake --build build-release --target offs_cli -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds. If the build fails because OFFS can't find the updated liboffs headers, rebuild liboffs in the submodule:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build --target offs -j$(nproc) 2>&1 | tail -10
# Then re-run the OFFS build above.
```

- [ ] **Step 5: Manual smoke test of the flag**

Start the daemon:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offsd --foreground --unix /tmp/offs.sock \
  --cache-dir /tmp/offs-cache --data-dir /tmp/offs-data &
sleep 1
```

PUT a small file with `--tuple-size 5`:
```bash
./build-release/offs --socket /tmp/offs.sock put /etc/hostname --tuple-size 5
```
Expected: ORI printed, exit 0. The server accepts the 5-wide tuple (assuming `max_tuple_size >= 5`, which is the default).

PUT with an out-of-range tuple-size:
```bash
./build-release/offs --socket /tmp/offs.sock put /etc/hostname --tuple-size 100
```
Expected: `Error: tuple_size 100 exceeds max_tuple_size 5`, exit 1.

PUT with a non-integer tuple-size:
```bash
./build-release/offs --socket /tmp/offs.sock put /etc/hostname --tuple-size abc
```
Expected: `Error: --tuple-size must be a positive integer`, exit 1.

Stop the daemon:
```bash
kill %1
```

- [ ] **Step 6: Commit (in the OFFS repo)**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/offs/commands/put.c src/offs/l10n/en.h
git commit -m "feat(offs): add --tuple-size flag to offs put"
```

---

## Task 10: Integration test — pre-flight rejection and mid-stream error

**Files:**
- Test: `test/test_file_transfer_integration.cpp` (add integration tests) OR a manual test script

**Note:** This task verifies the end-to-end behavior. If the existing integration test harness can configure `max_capacity_bytes` and drive a PUT through the daemon, add automated tests there. Otherwise, document the manual test procedure and run it.

- [ ] **Step 1: Pre-flight rejection test**

If `test/test_file_transfer_integration.cpp` has a harness that starts a daemon with a custom config:

Add a test that configures `max_capacity_bytes = 100000` (100 KB), then PUTs a 1 MB file. Expected: `CLIENT_API_ERROR` with message containing `"cache full: configure larger max_capacity_bytes"`, before any data is uploaded. Verify `block_cache->current_bytes` is unchanged (no blocks stored).

If the harness doesn't support custom configs, run the manual test:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
# Create a config with max_capacity_bytes = 100000
cat > /tmp/offs-test.conf <<'EOF'
max_capacity_bytes = 100000
EOF
./build-release/offsd --foreground --unix /tmp/offs.sock \
  --cache-dir /tmp/offs-cache-test --data-dir /tmp/offs-data-test \
  --config /tmp/offs-test.conf &
sleep 1

# Create a 1 MB file
dd if=/dev/urandom of=/tmp/offs-test-1mb.bin bs=1M count=1 2>/dev/null

# PUT: expect error
./build-release/offs --socket /tmp/offs.sock put /tmp/offs-test-1mb.bin
# Expected: "Error: cache full: configure larger max_capacity_bytes", exit 1

# Verify no blocks were stored
ls /tmp/offs-cache-test/blocks/ 2>/dev/null | wc -l
# Expected: 0 or minimal (no data blocks)

kill %1
rm -rf /tmp/offs-cache-test /tmp/offs-data-test /tmp/offs-test.conf /tmp/offs-test-1mb.bin
```

- [ ] **Step 2: tuple_size bound test**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offsd --foreground --unix /tmp/offs.sock \
  --cache-dir /tmp/offs-cache-test --data-dir /tmp/offs-data-test &
sleep 1

# PUT with tuple_size exceeding max_tuple_size (default 5)
./build-release/offs --socket /tmp/offs.sock put /etc/hostname --tuple-size 100
# Expected: "Error: tuple_size 100 exceeds max_tuple_size 5", exit 1

kill %1
rm -rf /tmp/offs-cache-test /tmp/offs-data-test
```

- [ ] **Step 3: Backward-compatible wire test**

Verify an 8-element PUT_REQUEST (no `tuple_size`) still works:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offsd --foreground --unix /tmp/offs.sock \
  --cache-dir /tmp/offs-cache-test --data-dir /tmp/offs-data-test &
sleep 1

# PUT without --tuple-size: server uses default 3
./build-release/offs --socket /tmp/offs.sock put /etc/hostname
# Expected: ORI printed, exit 0

# GET the ORI back and verify
ORI=$(./build-release/offs --socket /tmp/offs.sock put /etc/hostname | awk '{print $NF}')
./build-release/offs --socket /tmp/offs.sock get "$ORI" --output /tmp/hostname-out
diff /etc/hostname /tmp/hostname-out
# Expected: no diff, exit 0

kill %1
rm -rf /tmp/offs-cache-test /tmp/offs-data-test /tmp/hostname-out
```

- [ ] **Step 4: Run the full liboffs test suite to verify no regression**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build -j$(nproc) 2>&1 | tail -10
ctest --test-dir build --output-on-failure 2>&1 | tail -30
```
Expected: All tests pass (682+ tests as of the last verified baseline).

- [ ] **Step 5: Run valgrind on the new tests to verify no memory leaks**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
valgrind --leak-check=full --error-exitcode=1 ./build/test_block_cache --gtest_filter=BlockCacheCanFit.* 2>&1 | tail -20
valgrind --leak-check=full --error-exitcode=1 ./build/test_writeable_off_stream --gtest_filter=WriteableOffStreamEstimate.* 2>&1 | tail -20
valgrind --leak-check=full --error-exitcode=1 ./build/test_block_cache_api --gtest_filter=ClientApiPutRequestWire.* 2>&1 | tail -20
```
Expected: 0 leaks, 0 errors. (Per memory: valgrind 3.18.1 requires DWARF-4; if builds use DWARF-5, rebuild with `-gdwarf-4`.)

- [ ] **Step 6: Final commit (if integration tests were added)**

If automated integration tests were added:
```bash
git add test/test_file_transfer_integration.cpp
git commit -m "test(integration): add pre-flight rejection and tuple_size bound tests"
```

If only manual tests were run, no commit needed — the manual test results are reported in the PR description.

---

## Task 11: De-wonk and final verification

**Files:**
- Review: all files modified in Tasks 1-10

- [ ] **Step 1: De-wonk audit**

Use the de-wonk skill: read every file modified in Tasks 1-10 and ask "Is anything unimplemented, stubbed, disabled, broken, or weird?"

Specifically check:
- No TODO/FIXME/HACK/XXX comments left in the modified code (per CLAUDE.md).
- No skipped tests that should be implemented (the Task 5 skipped test is acceptable only if the harness genuinely doesn't exist; otherwise implement it).
- No hardcoded `tuple_size = 3` remaining in any connection handler (all should read from the wire frame).
- No `reply_to = ... ? ... : NULL` conditional remaining in `writeable_off_stream.c` or `writeable_descriptor.c`.

- [ ] **Step 2: Verify all tests pass**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build -j$(nproc) 2>&1 | tail -10
ctest --test-dir build --output-on-failure 2>&1 | tail -30
```
Expected: All tests pass.

- [ ] **Step 3: Verify OFFS builds and the CLI works**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
cmake --build build-release --target offsd offs_cli -j$(nproc) 2>&1 | tail -10
./build-release/offs put --help
```
Expected: Build succeeds, `--help` shows the usage including `--tuple-size`.

- [ ] **Step 4: Commit any de-wonk fixes**

If the de-wonk audit found and fixed issues:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add -A
git commit -m "fix: de-wonk cleanup for PUT cache space errors implementation"
```

If no issues found, no commit needed.

---

## Self-Review Notes

**Spec coverage:**
- Spec section 1 (`block_cache_can_fit`) → Task 1.
- Spec section 2 (estimator) → Task 2.
- Spec section 3 (optional `tuple_size` on wire) → Task 3.
- Spec section 4 (pre-flight validation sequence) → Tasks 6, 8.
- Spec section 5a (always set `reply_to`) → Task 4.
- Spec section 5b (handle failure in `CACHE_PUT_RESULT`) → Task 5.
- Spec section 5d (pipe error handler forwards message) → Tasks 7, 8.
- Spec section 6 (CLI `--tuple-size`) → Task 9.
- Spec testing section → Task 10.
- De-wonk per CLAUDE.md → Task 11.

**Dropped spec item:** Spec section 5c (cleanup of orphaned blocks) was explicitly dropped during brainstorming because content-addressed blocks may be shared across concurrent PUTs. No task implements it. This is documented in the spec's "Non-goals" section.

**Type consistency:** `block_cache_can_fit` returns `int` (`CACHE_FIT_OK`/`CACHE_FIT_FULL`) — used consistently in Tasks 1 and 6. `writeable_off_stream_estimate_required_bytes` returns `size_t` — used consistently in Tasks 2 and 6. `client_api_put_request_t` fields `has_tuple_size` (`uint8_t`) and `tuple_size` (`size_t`) — used consistently in Tasks 3, 6, 8, 9.

**Notes for the implementer:** Several steps include "verify X exists" notes because the plan was written without reading every helper macro (`OFFS_ERROR`, `CLIENT_API_STATUS_*`). The implementer must read the existing code and adapt these to the actual project conventions, not blindly copy the templates. This is intentional — the plan provides the structure and the key code, but the implementer is expected to match the existing style for the glue.