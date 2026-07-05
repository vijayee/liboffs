# Network Direct-Return Block Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pass retrieved block data directly from the network to the requesting stream in `NETWORK_FIND_BLOCK_RESULT`, so the stream uses it without re-fetching from the block cache — eliminating the stall/loop when `block_cache_put` fails on cache-full.

**Architecture:** Extend `network_find_block_result_payload_t` with a nullable `block_t* block` field. On the 3 remote-receipt paths in `network.c`, attach the block (referenced) to the result payload and set `reply_to` on the best-effort `block_cache_put`. Add a minimal `CACHE_PUT_RESULT` handler to the network actor (log + continue — the GET no longer depends on the store). The 3 stream consumers (`readable_off_stream`, `readable_descriptor`, `block_recipe`) use the block directly when `block != NULL`, and fall back to the existing `block_cache_get` re-fetch when `block == NULL` (local-cache-hit paths).

**Tech Stack:** C11, libcbor, BLAKE3, Google Test, CMake, POSIX sockets. liboffs is the library; OFFS is the application at `../OFFS/` (separate repo, depends on liboffs via git submodule).

**Spec:** `docs/superpowers/specs/2026-07-04-network-direct-return-block-design.md`

---

## File Structure

**liboffs (this repo):**
- `src/Actor/message.h` — add `block_t* block` to `network_find_block_result_payload_t`
- `src/Network/network.c` — update `network_find_block_result_destroy`; add `CACHE_PUT_RESULT` case to `network_dispatch`; update 3 remote-receipt paths (lines 1755, 1958, 2550)
- `src/OFFStreams/readable_off_stream.c` — `NETWORK_FIND_BLOCK_RESULT` handler (line 264)
- `src/OFFStreams/readable_descriptor.c` — `NETWORK_FIND_BLOCK_RESULT` handler (line 274)
- `src/OFFStreams/block_recipe.c` — `NETWORK_FIND_BLOCK_RESULT` handler (line 392)
- `test/test_network_*.cpp` or `test/test_stream_network.cpp` — integration tests

---

## Task 1: Extend payload struct and update destroy function

**Files:**
- Modify: `src/Actor/message.h:234-238` (the `network_find_block_result_payload_t` struct)
- Modify: `src/Network/network.c:88-95` (the `network_find_block_result_destroy` function)
- Test: `test/test_stream_network.cpp` (add a destroy-function test)

- [ ] **Step 1: Write the failing test**

Append to `test/test_stream_network.cpp` (or the most appropriate network test file — read the existing tests first to find the right place):

```cpp
TEST(NetworkFindBlockResultPayload, DestroyHandlesNullBlock) {
  network_find_block_result_payload_t* result =
      get_clear_memory(sizeof(network_find_block_result_payload_t));
  result->hash = buffer_create_from_pointer_copy("hashcontent", 12);
  result->found = 1;
  result->block = NULL;  /* local-cache-hit path: no block attached */

  network_find_block_result_destroy(result);
  /* Verify no crash, no leak — valgrind in Task 7 confirms */
  SUCCEED();
}

TEST(NetworkFindBlockResultPayload, DestroyHandlesAttachedBlock) {
  /* Build a block to attach */
  buffer_t* data = buffer_create_from_pointer_copy("blockdata1234567890", 20);
  block_t* block = block_create_existing_data_by_type(data, standard);
  DESTROY(data, buffer);

  network_find_block_result_payload_t* result =
      get_clear_memory(sizeof(network_find_block_result_payload_t));
  result->hash = buffer_create_from_pointer_copy("hashcontent", 12);
  result->found = 1;
  result->block = (block_t*)refcounter_reference((refcounter_t*)block);

  network_find_block_result_destroy(result);
  block_destroy(block);  /* release our local reference */
  SUCCEED();
}
```

**Note:** The implementer should read the existing test patterns in `test/test_stream_network.cpp` first to match the includes and fixture style. `buffer_create_from_pointer_copy` and `block_create_existing_data_by_type` may need adjustment to match the actual signatures — verify by reading `src/Buffer/buffer.h` and `src/BlockCache/block.h`.

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
./build-gdwarf4/test/testliboffs --gtest_filter=NetworkFindBlockResultPayload.* 2>&1 | tail -20
```
Expected: FAIL — `block` field not in struct, compile error.

**Note:** The build directory is `build-gdwarf4` (not `build`). The test target is `testliboffs` (all test sources compile into one binary at `build-gdwarf4/test/testliboffs`).

- [ ] **Step 3: Add the `block` field to the struct**

In `src/Actor/message.h`, modify the `network_find_block_result_payload_t` struct (lines 234-238) to add the `block` field. Also add the `block.h` include if not already present (check the top of the file):

```c
#include "../BlockCache/block.h"  /* add if not already present */

/* Network-to-stream: result of FindBlock */
typedef struct {
  buffer_t* hash;       /* same hash from the request */
  int       found;      /* 1 = found, 0 = not found */
  block_t*  block;      /* the retrieved block, when found via remote path.
                         * NULL when found=0, or when found via local-cache-hit
                         * (the consumer re-fetches from cache in that case). */
} network_find_block_result_payload_t;
```

**Verify:** Read the top of `src/Actor/message.h` to check if `block.h` is already included (it may be, transitively). If `block_t` is already visible (check for existing uses), don't add the include.

- [ ] **Step 4: Update the destroy function**

In `src/Network/network.c`, update `network_find_block_result_destroy` (lines 88-95) to destroy the `block` field if present:

```c
static void network_find_block_result_destroy(void* ptr) {
  network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)ptr;
  if (result->hash != NULL) {
    buffer_destroy(result->hash);
  }
  if (result->block != NULL) {
    block_destroy(result->block);
  }
  free(result);
}
```

**Verify:** The existing destroy function at lines 88-95 may be `static` or have a different exact form. Read it first and match the existing style — only add the `if (result->block != NULL) { block_destroy(result->block); }` block.

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
./build-gdwarf4/test/testliboffs --gtest_filter=NetworkFindBlockResultPayload.* 2>&1 | tail -20
```
Expected: PASS — 2 tests pass.

- [ ] **Step 6: Run the full suite to verify no regression**

Run:
```bash
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: 687 pass (685 baseline + 2 new), 7 pre-existing SSL failures, no new failures.

- [ ] **Step 7: Commit**

```bash
git add src/Actor/message.h src/Network/network.c test/test_stream_network.cpp
git commit -m "feat(network): add block field to network_find_block_result_payload_t"
```

---

## Task 2: Update `readable_off_stream` consumer

**Files:**
- Modify: `src/OFFStreams/readable_off_stream.c:264-283` (the `NETWORK_FIND_BLOCK_RESULT` case)

- [ ] **Step 1: Read the current handler**

Read `src/OFFStreams/readable_off_stream.c:264-283` (the `NETWORK_FIND_BLOCK_RESULT` case) and the existing `CACHE_GET_RESULT` success path (lines 241-262) to understand the XOR-accumulate logic. The direct-return path will mirror the `CACHE_GET_RESULT` success path.

- [ ] **Step 2: Update the handler to use the block directly when present**

In `src/OFFStreams/readable_off_stream.c`, replace the `NETWORK_FIND_BLOCK_RESULT` case (lines 264-283) with:

```c
    case NETWORK_FIND_BLOCK_RESULT: {
      network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
      if (result->found) {
        if (result->block != NULL) {
          /* Direct-return: network provided the block. XOR-accumulate it
           * directly instead of re-fetching from the cache. */
          if (stream->xor_accumulator == NULL) {
            stream->xor_accumulator = buffer_copy(result->block->data);
          } else {
            buffer_t* xored = buffer_xor(stream->xor_accumulator, result->block->data);
            DESTROY(stream->xor_accumulator, buffer);
            stream->xor_accumulator = xored;
          }
          stream->blocks_received++;
          if (stream->blocks_received >= stream->blocks_expected) {
            _finish_decode_and_render(stream);
          }
        } else {
          /* Local path: block is in the cache. Re-fetch as before. */
          buffer_t* fetch_hash = result->hash;
          if (fetch_hash != NULL) {
            block_cache_get(stream->bc, fetch_hash, &stream->stream.actor);
          }
        }
      } else {
        /* Block not found on network — deactivate */
        stream_deactivate((stream_t*)stream, OFFS_ERROR("Block not found on network"));
        stream->stream.is_deactivated = 1;
      }
      break;
    }
```

**Verify:** Read the existing `CACHE_GET_RESULT` success path (lines 241-262) to confirm the XOR-accumulate logic matches exactly — the direct-return path should use the same `buffer_copy` / `buffer_xor` / `blocks_received++` / `_finish_decode_and_render` sequence. The `found=0` path should match the existing deactivate logic.

- [ ] **Step 3: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Run the full suite to verify no regression**

Run:
```bash
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: 687 pass, 7 pre-existing SSL failures. (Until Task 6 attaches blocks, `result->block` is always NULL via `get_clear_memory`, so this handler always takes the re-fetch path — no behavior change.)

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/readable_off_stream.c
git commit -m "feat(readable_off_stream): use direct-returned block when present in NETWORK_FIND_BLOCK_RESULT"
```

---

## Task 3: Update `readable_descriptor` consumer

**Files:**
- Modify: `src/OFFStreams/readable_descriptor.c:274-288` (the `NETWORK_FIND_BLOCK_RESULT` case)

- [ ] **Step 1: Read the current handler and the CACHE_GET_RESULT success path**

Read `src/OFFStreams/readable_descriptor.c:192-272` (the `CACHE_GET_RESULT` case) and `:274-288` (the `NETWORK_FIND_BLOCK_RESULT` case). The direct-return path will mirror the `CACHE_GET_RESULT` success path at lines 256-272: `_process_descriptor(desc, block_data)` + handle `need_more`.

- [ ] **Step 2: Update the handler to use the block directly when present**

In `src/OFFStreams/readable_descriptor.c`, replace the `NETWORK_FIND_BLOCK_RESULT` case (lines 274-288) with:

```c
    case NETWORK_FIND_BLOCK_RESULT: {
      network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
      if (result->found) {
        if (result->block != NULL) {
          /* Direct-return: network provided the block. Process it the same
           * way as CACHE_GET_RESULT success (lines 256-272). */
          buffer_t* block_data = result->block->data;
          int need_more = _process_descriptor(desc, block_data);
          DESTROY(result->block, block);
          result->block = NULL;
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
            result->hash = NULL;
          }
          if (need_more && desc->next_descriptor_hash != NULL && !desc->stream.is_deactivated) {
            buffer_t* hash = desc->next_descriptor_hash;
            desc->next_descriptor_hash = NULL;
            _fetch_descriptor_block(desc, hash);
            DESTROY(hash, buffer);
          }
        } else {
          /* Local path: block is in the cache. Re-fetch as before. */
          if (result->hash != NULL) {
            block_cache_get(desc->bc, result->hash, &desc->stream.actor);
          }
          desc->state = DESCRIPTOR_FETCHING_BLOCK;
        }
      } else {
        /* Block not found on network — deactivate */
        stream_deactivate((stream_t*)desc, OFFS_ERROR("Descriptor block not found on network"));
        desc->stream.is_deactivated = 1;
      }
      break;
    }
```

**Note:** The `DESTROY(result->block, block)` + `result->block = NULL` pattern matches the `CACHE_GET_RESULT` success path (lines 258-259). The block was `refcounter_reference`'d in the network path (Task 6); this `DESTROY` releases that reference, and nulling the field prevents `network_find_block_result_destroy` from double-freeing.

- [ ] **Step 3: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Run the full suite to verify no regression**

Run:
```bash
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: 687 pass, 7 pre-existing SSL failures.

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/readable_descriptor.c
git commit -m "feat(readable_descriptor): use direct-returned block when present in NETWORK_FIND_BLOCK_RESULT"
```

---

## Task 4: Update `block_recipe` consumer

**Files:**
- Modify: `src/OFFStreams/block_recipe.c:392-428` (the `NETWORK_FIND_BLOCK_RESULT` case)

- [ ] **Step 1: Read the current handler and the CACHE_GET_RESULT success path**

Read `src/OFFStreams/block_recipe.c:271-348` (the `CACHE_GET_RESULT` case — it has two branches: `loading_descriptor` at lines 284-347, and data block at lines 350+) and `:392-428` (the current `NETWORK_FIND_BLOCK_RESULT` case). The direct-return path must handle both branches.

Also read the `CONSUME` macro definition (search for `#define CONSUME` in `src/RefCounter/` or `src/Util/`) to understand whether `CONSUME(ptr, type)` nulls the source pointer after yielding the reference. This determines whether `result->block = NULL` is needed after `CONSUME` to prevent `network_find_block_result_destroy` from double-freeing.

- [ ] **Step 2: Update the handler to use the block directly when present**

In `src/OFFStreams/block_recipe.c`, replace the `NETWORK_FIND_BLOCK_RESULT` case (lines 392-428) with:

```c
    case NETWORK_FIND_BLOCK_RESULT: {
      network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
      if (result->found) {
        if (result->block != NULL) {
          /* Direct-return: network provided the block. Process it the same
           * way as CACHE_GET_RESULT success (two branches: descriptor vs data). */
          if (recipe->loading_descriptor) {
            /* Descriptor block: process it (mirror CACHE_GET_RESULT lines 328-345) */
            buffer_t* block_data = result->block->data;
            int need_more = _process_descriptor_block(recipe, block_data);
            DESTROY(result->block, block);
            result->block = NULL;
            if (need_more && recipe->next_descriptor_hash != NULL) {
              buffer_t* hash = recipe->next_descriptor_hash;
              recipe->next_descriptor_hash = NULL;
              block_cache_get(recipe->recipe.bc, hash, &recipe->recipe.stream.actor);
            } else {
              _finish_descriptor_load(recipe);
              if (recipe->pending_pull > 0 && !recipe->recipe.stream.is_deactivated) {
                _try_fetch_next(recipe);
              }
            }
          } else {
            /* Data block: transfer ownership to stream_notify (mirror CACHE_GET_RESULT lines 350-354) */
            stream_notify((stream_t*)recipe, data_event,
                          CONSUME(result->block, block_t), (void (*)(void*))block_destroy);
            recipe->pending_pull--;
            result->block = NULL;  /* ownership transferred via CONSUME; null to prevent destroy double-free */
          }
        } else {
          /* Local path: block is in the cache. Re-fetch as before. */
          block_cache_get(recipe->recipe.bc, recipe->pending_fetch_hash, &recipe->recipe.stream.actor);
          recipe->state = RECIPE_FETCHING_BLOCK;
        }
      } else {
        /* Block not found on network */
        if (recipe->loading_descriptor) {
          /* Descriptor block not found on network — skip this ori */
          for (int i = 0; i < recipe->front_hashes.length; i++) {
            DESTROY(recipe->front_hashes.data[i], buffer);
          }
          vec_deinit(&recipe->front_hashes);
          for (int i = 0; i < recipe->back_hashes.length; i++) {
            DESTROY(recipe->back_hashes.data[i], buffer);
          }
          vec_deinit(&recipe->back_hashes);
          if (recipe->next_descriptor_hash != NULL) {
            DESTROY(recipe->next_descriptor_hash, buffer);
            recipe->next_descriptor_hash = NULL;
          }
          recipe->ori_index++;
          recipe->loading_descriptor = 0;
          _start_descriptor_load(recipe);
        } else {
          /* Data block not found on network — deactivate */
          stream_deactivate((stream_t*)recipe, OFFS_ERROR("Block not found on network"));
          recipe->recipe.stream.is_deactivated = 1;
        }
      }
      if (recipe->pending_fetch_hash != NULL) {
        DESTROY(recipe->pending_fetch_hash, buffer);
        recipe->pending_fetch_hash = NULL;
      }
      break;
    }
```

**Note on `CONSUME` and `result->block = NULL`:** The `CONSUME(result->block, block_t)` macro yields the block's reference to `stream_notify`. Whether `CONSUME` nulls the source pointer is macro-dependent — the implementer MUST read the `CONSUME` definition (search `src/RefCounter/refcounter.h` or `src/Util/allocator.h` for `#define CONSUME`). If `CONSUME` already nulls the source, the explicit `result->block = NULL;` line is redundant but harmless. If it does NOT null the source, the explicit null is REQUIRED to prevent `network_find_block_result_destroy` from calling `block_destroy` on a pointer whose reference was already yielded (double-deref / use-after-free). Keep the explicit null either way — it's defensive and correct.

- [ ] **Step 3: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Run the full suite to verify no regression**

Run:
```bash
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: 687 pass, 7 pre-existing SSL failures.

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/block_recipe.c
git commit -m "feat(block_recipe): use direct-returned block when present in NETWORK_FIND_BLOCK_RESULT"
```

---

## Task 5: Add `CACHE_PUT_RESULT` handler to the network actor

**Files:**
- Modify: `src/Network/network.c:2882+` (the `network_dispatch` function, add a new case before the `default:`)

- [ ] **Step 1: Read the network dispatch function**

Read `src/Network/network.c:2882-2940` to understand the `network_dispatch` switch structure. Find the `default:` case (around line 3938) — the new `CACHE_PUT_RESULT` case goes before it.

Also read `src/BlockCache/block_cache.h` to confirm `CACHE_PUT_RESULT`, `CACHE_PUT_ERROR`, `CACHE_PUT_FULL`, and `cache_put_result_payload_t` are declared (they are — from the prior PUT cache space errors work).

- [ ] **Step 2: Add the `CACHE_PUT_RESULT` case**

In `src/Network/network.c`, in `network_dispatch`, add a new case before the `default:` (around line 3938):

```c
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
      if (result->result == CACHE_PUT_ERROR || result->result == CACHE_PUT_FULL) {
        /* Best-effort cache store failed. The block was already delivered
         * directly to the requesting stream via NETWORK_FIND_BLOCK_RESULT,
         * so this failure does not affect the current GET. No action needed. */
      }
      /* CACHE_PUT_NEW / CACHE_PUT_EXISTS: the block is in the cache for
       * future GETs. Network announce for CACHE_PUT_NEW is handled by the
       * writable_off_stream path during PUT, not here. */
      break;
    }
```

**Note:** The handler intentionally does nothing on failure (the GET already got the block directly) and nothing on success (the cache is populated; announce is handled elsewhere). The empty-if block with the comment documents the intent. If the project has a logging macro (search for `LOG_` or `log_` in `network.c`), the implementer may add a log line inside the failure branch — but it's optional. Do NOT add a TODO.

**Verify:** Check that `cache_put_result_payload_t` and `CACHE_PUT_RESULT` are visible in `network.c` (they should be, via the includes — `network.c` already calls `block_cache_put` which produces these). If not, add the appropriate include.

- [ ] **Step 3: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Run the full suite to verify no regression**

Run:
```bash
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: 687 pass, 7 pre-existing SSL failures.

- [ ] **Step 5: Commit**

```bash
git add src/Network/network.c
git commit -m "feat(network): add CACHE_PUT_RESULT handler for best-effort cache store results"
```

---

## Task 6: Update the 3 remote-receipt paths to attach the block

**Files:**
- Modify: `src/Network/network.c` (3 paths: around lines 1755, 1958, 2550)

This is the core change: the 3 remote-receipt paths that have block data in hand now attach the block to the `NETWORK_FIND_BLOCK_RESULT` payload and set `reply_to` on the best-effort `block_cache_put`.

- [ ] **Step 1: Read the 3 remote-receipt paths**

Read these 3 sections of `src/Network/network.c`:
- **Path A (line 1721-1767):** the FindBlock response handler. Block data is at `response->block_data` (line 1721). The `block_t` is built at line 1731. Currently calls `block_cache_put(..., NULL)` at line 1734, destroys block at 1735, sends `found=1` at 1755.
- **Path B (line 1940-1965):** the StoreBlock push handler. Block data is at `store->block_data`. The `block_t` is built similarly. Sends `found=1` at 1958.
- **Path C (line 2535-2560):** the Recall/Accept handler. Block data is at `accept->block_data`. Sends `found=1` at 2550.

For each path, identify:
1. Where the `block_t*` is built (the `block_create_existing_data_hash_by_type` call)
2. Where `block_cache_put` is called (and its current `reply_to` argument)
3. Where the block is destroyed (`DESTROY(block, block)`)
4. Where `NETWORK_FIND_BLOCK_RESULT` is constructed and sent (the `result->found = 1` line)

- [ ] **Step 2: Update Path A (FindBlock response, line 1721-1767)**

In `src/Network/network.c`, in the FindBlock response handler, modify the block-storage and result-sending section:

1. Change `block_cache_put(network->block_cache, block, response->block_fib, NULL)` to `block_cache_put(network->block_cache, block, response->block_fib, &network->actor)`.
2. Before `DESTROY(block, block)`, attach the block to the result payload: change the result construction to `result->block = (block_t*)refcounter_reference((refcounter_t*)block);`.
3. Keep `DESTROY(block, block)` — this releases the network's local reference; the payload's reference keeps the block alive.

The implementer should read the exact current code around lines 1734-1755 and apply these changes, preserving the surrounding logic (the `if (block != NULL)` guard, the `DESTROY(data_buf, buffer)` / `DESTROY(hash_buf, buffer)` cleanup, the wanted_list notification).

**Key pattern:**
```c
      if (block != NULL) {
        block_cache_put(network->block_cache, block, response->block_fib, &network->actor);
        /* block is referenced for the payload below; DESTROY releases the local ref */
      }
      /* ... existing wanted_list notification ... */
      if (requesters != NULL) {
        wanted_requester_t* req = requesters;
        while (req != NULL) {
          network_find_block_result_payload_t* result =
              get_clear_memory(sizeof(network_find_block_result_payload_t));
          result->hash = REFERENCE(hash_buf, buffer_t);
          result->found = 1;
          result->block = (block != NULL) ? (block_t*)refcounter_reference((refcounter_t*)block) : NULL;
          message_t result_msg = {0};
          result_msg.type = NETWORK_FIND_BLOCK_RESULT;
          result_msg.payload = result;
          result_msg.payload_destroy = network_find_block_result_destroy;
          actor_send(req->actor, &result_msg);
          req = req->next;
        }
        wanted_requester_list_destroy(requesters);
      }
      if (block != NULL) {
        DESTROY(block, block);
      }
```

**Note:** The exact structure may differ — the `block` variable may be scoped such that it's not visible at the `result` construction site. The implementer must read the current code and ensure `block` is in scope when `result->block` is set. If the `block` is destroyed before the result is constructed (as in the current line 1735), the order must be rearranged: construct the result (with `result->block = REFERENCE(block, block_t)`) BEFORE `DESTROY(block, block)`.

- [ ] **Step 3: Update Path B (StoreBlock push, line 1940-1965)**

Apply the same pattern as Path A to the StoreBlock push handler. Read the current code, find where the `block_t` is built, where `block_cache_put` is called, where the block is destroyed, and where `NETWORK_FIND_BLOCK_RESULT` is sent. Apply:
1. Set `reply_to = &network->actor` on `block_cache_put`.
2. Attach `result->block = REFERENCE(block, block_t)` (if `block != NULL`).
3. `DESTROY(block, block)` after the result is constructed.

- [ ] **Step 4: Update Path C (Recall/Accept, line 2535-2560)**

Apply the same pattern to the Recall/Accept handler. Read the current code, find the `block_t` build, `block_cache_put`, destroy, and result-send sites. Apply the same 3 changes.

- [ ] **Step 5: Build to verify no compile errors**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 6: Run the full suite to verify no regression**

Run:
```bash
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: 687 pass, 7 pre-existing SSL failures. (The network tests may exercise these paths — verify no new failures.)

- [ ] **Step 7: Commit**

```bash
git add src/Network/network.c
git commit -m "feat(network): attach retrieved block directly to NETWORK_FIND_BLOCK_RESULT and set reply_to on best-effort put"
```

---

## Task 7: Integration tests and valgrind

**Files:**
- Test: `test/test_stream_network.cpp` (add integration tests for the direct-return path)
- Manual: valgrind verification

- [ ] **Step 1: Read existing network/stream integration test patterns**

Read `test/test_stream_network.cpp` to understand the existing test fixtures (e.g. `WriteableOffStreamNetworkTest`). Find tests that exercise the network find-block path. If there's an existing test that mocks a peer response with block data, use it as the pattern for the new tests.

- [ ] **Step 2: Add a direct-return integration test**

Add a test that verifies the stream's `NETWORK_FIND_BLOCK_RESULT` handler uses the block directly when `block != NULL`. The test should:
1. Create a `readable_off_stream` with a tuple whose blocks are NOT in the cache.
2. Mock the network responding with `NETWORK_FIND_BLOCK_RESULT{found=1, block=<a real block>}`.
3. Verify the stream XOR-accumulates the block and produces data without calling `block_cache_get`.

If the existing test harness doesn't support mocking network responses with block data, this test may need to be a unit-level test of the dispatch handler (dispatch a `NETWORK_FIND_BLOCK_RESULT` message directly to the stream actor and verify the result). Use the `WriteableOffStreamNetworkTest` fixture pattern from Task 5 of the prior PUT cache space errors work as a reference.

- [ ] **Step 3: Add a local-re-fetch fallback test**

Add a test that verifies the stream re-fetches from the cache when `found=1` and `block == NULL` (the local-cache-hit path). The test should:
1. Create a `readable_off_stream` with a tuple whose blocks ARE in the cache.
2. Mock the network responding with `NETWORK_FIND_BLOCK_RESULT{found=1, block=NULL}`.
3. Verify the stream re-issues `block_cache_get` and succeeds.

- [ ] **Step 4: Build and run the new tests**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -20
./build-gdwarf4/test/testliboffs --gtest_filter='*DirectReturn*:*LocalRefetch*' 2>&1 | tail -20
```
Expected: new tests pass.

- [ ] **Step 5: Run the full suite to verify no regression**

Run:
```bash
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: all tests pass (687+ new tests), 7 pre-existing SSL failures.

- [ ] **Step 6: Run valgrind on the new and modified tests**

Run:
```bash
valgrind --leak-check=full --error-exitcode=1 ./build-gdwarf4/test/testliboffs \
  --gtest_filter='NetworkFindBlockResultPayload.*:*DirectReturn*:*LocalRefetch*:WriteableOffStreamNetworkTest.CachePutErrorNoCrash:WriteableOffStreamNetworkTest.CachePutFullNoCrash' 2>&1 | tail -30
```
Expected: 0 bytes in use at exit (no leaks). The pre-existing `scheduler.c:119` "Invalid read" is acceptable (see memory note `reference_scheduler_valgrind_error.md`).

- [ ] **Step 7: Commit**

```bash
git add test/test_stream_network.cpp
git commit -m "test(network): add direct-return and local-refetch integration tests"
```

---

## Task 8: De-wonk and final verification

**Files:**
- Review: all files modified in Tasks 1-7

- [ ] **Step 1: De-wonk audit**

Use the de-wonk skill: read every file modified in Tasks 1-7 and ask "Is anything unimplemented, stubbed, disabled, broken, or weird?"

Specifically check:
- No TODO/FIXME/HACK/XXX comments left in the modified code (per CLAUDE.md).
- No `reply_to = NULL` remaining on the 3 remote-receipt paths' `block_cache_put` calls (all should now be `&network->actor`).
- The 2 local-cache-hit paths (lines 2675, 2730) still send `block = NULL` (unchanged — they don't have block data in hand).
- The `network_find_block_result_destroy` function frees `block` when non-NULL.
- The 3 consumers' `found=1` paths handle both `block != NULL` (direct) and `block == NULL` (re-fetch).

- [ ] **Step 2: Verify all tests pass**

Run:
```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
cmake --build build-gdwarf4 --target testliboffs -j$(nproc) 2>&1 | tail -10
./build-gdwarf4/test/testliboffs 2>&1 | tail -10
```
Expected: all tests pass, 7 pre-existing SSL failures.

- [ ] **Step 3: Run valgrind on the full new-test set**

Run:
```bash
valgrind --leak-check=full ./build-gdwarf4/test/testliboffs \
  --gtest_filter='NetworkFindBlockResultPayload.*:*DirectReturn*:*LocalRefetch*:WriteableOffStreamNetworkTest.CachePut*:BlockCacheCanFit.*:WriteableOffStreamEstimate.*:ClientApiPutRequestWire.*' 2>&1 | tail -15
```
Expected: 0 bytes in use at exit (pre-existing scheduler.c:119 invalid read is acceptable).

- [ ] **Step 4: Commit any de-wonk fixes**

If the de-wonk audit found and fixed issues:
```bash
git add -A
git commit -m "fix: de-wonk cleanup for network direct-return block implementation"
```

If no issues found, no commit needed.

---

## Self-Review Notes

**Spec coverage:**
- Spec section 1 (payload struct) → Task 1.
- Spec section 2 (destroy function) → Task 1.
- Spec section 3 (3 remote-receipt paths) → Task 6.
- Spec section 4 (network actor CACHE_PUT_RESULT handler) → Task 5.
- Spec section 5 (3 stream consumers) → Tasks 2, 3, 4.
- Spec testing section → Task 7.
- De-wonk per CLAUDE.md → Task 8.

**Type consistency:** `network_find_block_result_payload_t.block` is `block_t*` — used consistently in Tasks 1, 2, 3, 4, 6. `block_destroy(block_t*)` is the destroy function (block.h:36). `refcounter_reference((refcounter_t*)block)` is the reference pattern (matches existing usage in the codebase).

**Ordering rationale:** Task 1 (struct) is the foundation. Tasks 2-4 (consumers) handle `block != NULL` but block is always NULL until Task 6, so they take the re-fetch path (no regression). Task 5 (CACHE_PUT_RESULT handler) exists before Task 6 sets `reply_to`. Task 6 (remote paths) attaches blocks — now consumers use them. Task 7 (tests) verifies. Task 8 (de-wonk) finalizes.

**Notes for the implementer:** Tasks 3 and 4 include the full block-processing logic (mirrored from the existing `CACHE_GET_RESULT` success paths in `readable_descriptor.c:256-272` and `block_recipe.c:328-345` / `:351-354`). The implementer should verify the line numbers still match when implementing (they may have shifted) and confirm the `CONSUME` macro behavior in Task 4 (read the macro definition to confirm whether `result->block = NULL` after `CONSUME` is redundant or required).