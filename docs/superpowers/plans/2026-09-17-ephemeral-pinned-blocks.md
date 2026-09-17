# Ephemeral Blocks & Pin Counts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement per-block ephemeral claim counts (`uint16_t ephemeral_count`) and pin counts (`uint32_t pin_count`) in the block cache, with an actor-owned elastic-bloom-filter ephemeral registry, recycler enforcement, representation-level commit/delete/pin/unpin APIs, and a list-ephemerals maintenance API, exposed over HTTP, the client API wire (WS/TCP/Unix/WT), the C client, and the JS client.

**Architecture:** Metadata lives on `index_entry_t` (persisted via an extended 7-element CBOR snapshot + one new WAL record type `'m'`). Block-level mutation goes through new block cache actor messages. A dedicated `ephemeral_registry` actor (created inside `block_cache_create`) owns a persistent `elastic_bloom_filter_t` keyed by descriptor hash with two-file rotation. Recyclers enforce ephemerality exactly at block-fetch time. Representation-level APIs walk a representation's descriptor chain in a new `representation_actor` and issue block-level ops.

**Tech Stack:** C11 (libcbor, XXH, googletest), existing actor/scheduler/refcounter framework, existing `src/Bloom/elastic_bloom_filter.h` (supports add/contains/**remove** natively).

**Spec:** `docs/superpowers/specs/2026-09-17-ephemeral-pinned-blocks-design.md` (read it before starting).

**Conventions (from CLAUDE.md + docs/STYLE_GUIDE.md):**
- Types use `_t` suffix; functions `type_action()`; create functions use `get_clear_memory()`; reference-counted structs have `refcounter_t refcounter` as first member; DESTROY/REFERENCE/CONSUME macros for refcount handling; no TODOs left behind; no single-letter variable names.
- Build/test commands: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=<Filter>`. New test files must be added to `test/CMakeLists.txt`.
- Every task ends with a focused commit. Do NOT add "Co-Authored-By" lines.

---

## File Structure (what each file gains)

- `src/BlockCache/index.h` / `index.c` — `ephemeral_count`/`pin_count` fields, 7-element CBOR, snapshot CRC, WAL `'m'` record write + replay.
- `src/BlockCache/wal.h` — `metadata = 'm'` record type.
- `src/Actor/message.h` — new message types + payload structs (appended at END of the enum — it is internal-only, never serialized).
- `src/BlockCache/block_cache.h` / `block_cache.c` — `CACHE_EPHEMERAL`/`CACHE_PIN`/`CACHE_UNPIN`/`CACHE_EPHEMERAL_LIST` messages + wrappers, `force` on remove, new result codes, ephemeral acquire flag on put, respiration victim exclusion, registry field + wiring.
- `src/BlockCache/ephemeral_registry.h` / `ephemeral_registry.c` — NEW actor owning the elastic bloom filter + persistence.
- `src/Configuration/config.h` / `config.c` — registry sizing fields + defaults.
- `src/Network/network.c` — remote find-block skips ephemeral blocks (local find path must NOT skip).
- `src/OFFStreams/writeable_off_stream.h` / `.c` — ephemeral put mode, claim acquisition, announce suppression, created-block tracking + failure cleanup.
- `src/OFFStreams/writeable_descriptor.h` / `.c` — same for descriptor blocks.
- `src/OFFStreams/block_recipe.h` / `.c` — recycler fetch-time enforcement, `commit`/`propagate` modes, self-heal add, acquired-claim tracking.
- `src/OFFStreams/representation_actor.h` / `.c` — NEW descriptor-walk actor implementing mark-permanent / delete-ephemeral / pin / unpin.
- `src/ClientAPI/HTTP/off_routes.c` — new routes.
- `src/ClientAPI/client_api_wire.h` / `.c` — new op codes 42–51 + encode/decode.
- `src/ClientAPI/representation_api.h` / `.c` — NEW shared transport handler.
- `src/ClientAPI/Unix/unix_connection.c`, `src/ClientAPI/TCP/tcp_connection.c`, `src/ClientAPI/WebSocket/ws_connection.c`, `src/ClientAPI/WebTransport/wt_connection.c`, `src/ClientAPI/WebTransport/webtransport_h3.c` — dispatch cases for the new ops.
- `src/ClientLibs/c/offs_client.h` / `.c` — new client functions.
- `src/ClientLibs/js/offs-client/src/{wire.js,index.js,types.js}` + dist rebuild.
- `test/test_ephemeral.cpp` — NEW test file (all core tests), added to `test/CMakeLists.txt`.
- `docs/CONFIG_FIELDS.md` — new config fields documented.

---

### Task 1: Index entry fields + CBOR + snapshot CRC

**Files:**
- Modify: `src/BlockCache/index.h:18-25` (struct), `src/BlockCache/index.h:28-29` (declarations), `src/BlockCache/index.c:51-60` (`index_entry_from`), `src/BlockCache/index.c:81-128` (CBOR encode/decode), `src/BlockCache/index.c:644-694` (`_index_node_to_crc`)
- Test: `test/test_ephemeral.cpp` (NEW), `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `test/test_ephemeral.cpp`:

```cpp
#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "../src/BlockCache/index.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/sections.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/atomic_compat.h"
#include "../src/Platform/platform_time.h"
#include "../src/Platform/platform_file.h"
#include "../src/Util/allocator.h"
#include <cbor.h>
}

/* ---- index entry CBOR round-trip with the new fields ---- */

TEST(TestEphemeralIndex, EntryCborRoundTripNewFields) {
  buffer_t* hash = block_create_random_block_by_type(standard)->hash;
  buffer_t* hash_ref = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  index_entry_t* entry = index_entry_from(hash_ref, 7, 3, 12345,
                                          fibonacci_hit_counter_create(),
                                          /*ephemeral_count=*/2, /*pin_count=*/5);
  cbor_item_t* cbor = index_entry_to_cbor(entry);
  ASSERT_NE(cbor, nullptr);
  EXPECT_EQ(cbor_array_size(cbor), 7u);
  index_entry_t* decoded = cbor_to_index_entry(cbor);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->ephemeral_count, 2u);
  EXPECT_EQ(decoded->pin_count, 5u);
  EXPECT_EQ(decoded->section_id, 7u);
  EXPECT_EQ(decoded->section_index, 3u);
  EXPECT_EQ(decoded->ejection_date, 12345u);
  EXPECT_EQ(buffer_compare(decoded->hash, hash), 0);
  cbor_decref(&cbor);
  index_entry_destroy(decoded);
  index_entry_destroy(entry);
  DESTROY(hash, buffer);
}

TEST(TestEphemeralIndex, EntryCborDecodeOldFiveElementArray) {
  /* Old-format 5-element array must decode with ephemeral=0, pin=0. */
  buffer_t* hash = block_create_random_block_by_type(standard)->hash;
  cbor_item_t* array = cbor_new_definite_array(5);
  (void)cbor_array_push(array, cbor_move(fibonacci_hit_counter_to_cbor(&fibonacci_hit_counter_create())));
  (void)cbor_array_push(array, cbor_move(buffer_to_cbor(hash)));
  (void)cbor_array_push(array, cbor_move(cbor_build_uint64(3)));
  (void)cbor_array_push(array, cbor_move(cbor_build_uint64(7)));
  (void)cbor_array_push(array, cbor_move(cbor_build_uint64(12345)));
  index_entry_t* decoded = cbor_to_index_entry(array);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->ephemeral_count, 0u);
  EXPECT_EQ(decoded->pin_count, 0u);
  cbor_decref(&array);
  index_entry_destroy(decoded);
  DESTROY(hash, buffer);
}
```

Add to `test/CMakeLists.txt` in the `add_executable(testliboffs ...)` source list (next to `test_block_cache.cpp`): `test_ephemeral.cpp`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestEphemeralIndex.*`
Expected: compile error — `index_entry_from` takes 5 args; `ephemeral_count` not a member of `index_entry_t`.

- [ ] **Step 3: Implement**

In `src/BlockCache/index.h`, extend the struct (keep `refcounter` first):

```c
typedef struct {
  refcounter_t refcounter;
  fibonacci_hit_counter_t counter;
  buffer_t* hash;
  size_t section_id;
  size_t section_index;
  uint64_t ejection_date;
  uint16_t ephemeral_count; /* 0 = permanent; >0 = ephemeral claims held */
  uint32_t pin_count;       /* >0 = permanent blocks resist deletion */
} index_entry_t;
```

Change the declaration of `index_entry_from` in `index.h` to:

```c
index_entry_t* index_entry_from(buffer_t* hash, size_t section_id, size_t section_index,
                                uint64_t ejection_date, fibonacci_hit_counter_t counter,
                                uint16_t ephemeral_count, uint32_t pin_count);
```

In `src/BlockCache/index.c:51-60`, update the body — add two lines before `return`:

```c
  entry->ephemeral_count = ephemeral_count;
  entry->pin_count = pin_count;
```

Update `index_entry_to_cbor` (index.c:81-94) — array of 7, two extra pushes:

```c
cbor_item_t* index_entry_to_cbor(index_entry_t* entry) {
  cbor_item_t* array = cbor_new_definite_array(7);
  bool success = cbor_array_push(array, cbor_move(fibonacci_hit_counter_to_cbor(&entry->counter)));
  success &= cbor_array_push(array, cbor_move(buffer_to_cbor(entry->hash)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint64(entry->section_index)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint64(entry->section_id)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint64(entry->ejection_date)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint(entry->ephemeral_count)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint(entry->pin_count)));
  if (!success) {
    cbor_decref(&array);
    return NULL;
  }
  return array;
}
```

Update `cbor_to_index_entry` (index.c:96-128) — accept `>= 5`, read optional trailing fields:

```c
index_entry_t* cbor_to_index_entry(cbor_item_t* cbor) {
  if (cbor == NULL || !cbor_isa_array(cbor) || cbor_array_size(cbor) < 5) {
    return NULL;
  }
  cbor_item_t* item0 = cbor_array_get(cbor, 0);
  cbor_item_t* item1 = cbor_array_get(cbor, 1);
  cbor_item_t* item2 = cbor_array_get(cbor, 2);
  cbor_item_t* item3 = cbor_array_get(cbor, 3);
  cbor_item_t* item4 = cbor_array_get(cbor, 4);
  if (!cbor_isa_array(item0) || !cbor_isa_bytestring(item1) ||
      !cbor_isa_uint(item2) || !cbor_isa_uint(item3) || !cbor_isa_uint(item4)) {
    cbor_decref(&item0);
    cbor_decref(&item1);
    cbor_decref(&item2);
    cbor_decref(&item3);
    cbor_decref(&item4);
    return NULL;
  }
  fibonacci_hit_counter_t counter = cbor_to_fibonacci_hit_counter(item0);
  buffer_t* hash = cbor_to_buffer(item1);
  size_t section_index = (size_t) cbor_get_int(item2);
  size_t section_id = (size_t) cbor_get_int(item3);
  uint64_t ejection_date = cbor_get_int(item4);
  cbor_decref(&item0);
  cbor_decref(&item1);
  cbor_decref(&item2);
  cbor_decref(&item3);
  cbor_decref(&item4);
  uint16_t ephemeral_count = 0;
  uint32_t pin_count = 0;
  if (cbor_array_size(cbor) >= 6) {
    cbor_item_t* ephemeral_item = cbor_array_get(cbor, 5);
    if (cbor_isa_uint(ephemeral_item)) {
      uint64_t ephemeral_value = cbor_get_int(ephemeral_item);
      ephemeral_count = (ephemeral_value > UINT16_MAX) ? UINT16_MAX : (uint16_t)ephemeral_value;
    }
    cbor_decref(&ephemeral_item);
  }
  if (cbor_array_size(cbor) >= 7) {
    cbor_item_t* pin_item = cbor_array_get(cbor, 6);
    if (cbor_isa_uint(pin_item)) {
      uint64_t pin_value = cbor_get_int(pin_item);
      pin_count = (pin_value > UINT32_MAX) ? UINT32_MAX : (uint32_t)pin_value;
    }
    cbor_decref(&pin_item);
  }
  refcounter_yield((refcounter_t*) hash);
  return index_entry_from(hash, section_id, section_index, ejection_date, counter,
                         ephemeral_count, pin_count);
}
```

Update `_index_node_to_crc` (index.c:644-694) — inside the bucket loop, after the `section_id` update, add:

```c
      uint32_t ephemeral_count = htobe32((uint32_t)cur_entry->ephemeral_count);
      if (XXH64_update(state, &ephemeral_count, sizeof(uint32_t)) == XXH_ERROR) {
        log_error("failed to update crc with ephemeral count");
        return 8;
      }

      uint32_t pin_count = htobe32(cur_entry->pin_count);
      if (XXH64_update(state, &pin_count, sizeof(uint32_t)) == XXH_ERROR) {
        log_error("failed to update crc with pin count");
        return 9;
      }
```

Fix the one other caller of `index_entry_from` (grep `index_entry_from(` in `src/` — the snapshot-load path in `cbor_to_index_node` calls `cbor_to_index_entry`, but any direct callers get `, 0, 0` appended).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestEphemeralIndex.*`
Expected: 2 PASS. Also run `--gtest_filter=TestIndex.*` (or `*Index*`) to confirm no regressions in existing index tests.

- [ ] **Step 5: Commit**

```bash
git add src/BlockCache/index.h src/BlockCache/index.c test/test_ephemeral.cpp test/CMakeLists.txt
git commit -m "feat: add ephemeral_count and pin_count to index entries"
```

---

### Task 2: WAL metadata record `'m'`

**Files:**
- Modify: `src/BlockCache/wal.h:13-24` (enum), `src/BlockCache/index.h` (declare `index_write_entry_metadata`), `src/BlockCache/index.c:203-302` (`_index_replay_wal` switch)
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_ephemeral.cpp`:

```cpp
/* ---- WAL 'm' metadata record: write + replay ---- */

static index_t* _ephemeral_test_index_create(const char* folder_name) {
  static char location_buf[512];
  snprintf(location_buf, sizeof(location_buf), "/tmp/%s", folder_name);
  rm_rf(location_buf);
  mkdir_p(location_buf);
  int error_code = 0;
  index_t* index = index_create(25, location_buf, 60000, 60000, 3, 3, &error_code);
  EXPECT_NE(index, nullptr);
  return index;
}

TEST(TestEphemeralIndex, WalMetadataRecordRoundTrip) {
  index_t* index = _ephemeral_test_index_create("EphemeralWalTest");
  ASSERT_NE(index, nullptr);

  buffer_t* hash = block_create_random_block_by_type(standard)->hash;
  index_entry_t* entry = index_entry_create(hash);
  entry->section_id = 1;
  entry->section_index = 2;
  index_entry_t* held = (index_entry_t*)refcounter_reference((refcounter_t*)entry);
  index_add(index, CONSUME(entry, index_entry_t));

  /* Acquire two claims, pin once — through the metadata writer. */
  held->ephemeral_count = 2;
  index_write_entry_metadata(index, held);
  held->pin_count = 1;
  index_write_entry_metadata(index, held);

  /* Force the debounced snapshot + WAL rollover to flush, then reload. */
  index_debounce(index);
  index_sync(index);
  char* location = strdup(index->location);
  index_entry_destroy(held);
  index_destroy(index);
  free(location);

  int error_code = 0;
  index_t* reloaded = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(reloaded, nullptr);
  index_entry_t* found = index_find(reloaded, hash);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ephemeral_count, 2u);
  EXPECT_EQ(found->pin_count, 1u);
  index_entry_destroy(found);
  index_destroy(reloaded);
  free(location);
  DESTROY(hash, buffer);
}
```

Note: if `index_destroy`/`index->location` differ from what's declared in `index.h`, adapt to the actual API (check `index.h` for the destroy function name and the `location` field — both exist; `index_t` has `char* location` at index.h:60).

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestEphemeralIndex.WalMetadataRecordRoundTrip`
Expected: compile error — `index_write_entry_metadata` undeclared.

- [ ] **Step 3: Implement**

In `src/BlockCache/wal.h`, extend the enum:

```c
typedef enum wal_type_e {
  addition = 'a',
  removal = 'r',
  increment = 'i',
  ejection = 'e',
  metadata = 'm'
} wal_type_e;
```

In `src/BlockCache/index.h`, declare next to the other entry functions:

```c
void index_write_entry_metadata(index_t* index, index_entry_t* entry);
```

In `src/BlockCache/index.c`, add (model it on `_index_increment` at index.c:761-771):

```c
/* Persist a metadata mutation (ephemeral_count / pin_count) as an 'm' WAL
   record carrying the full entry CBOR. Replay applies only the two metadata
   fields, so re-reading a stale record is idempotent. */
void index_write_entry_metadata(index_t* index, index_entry_t* entry) {
  if (!index->is_rebuilding) {
    cbor_item_t* cbor_entry = index_entry_to_cbor(entry);
    uint8_t* cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor_entry, &cbor_data, &cbor_size);
    buffer_t* cbor_buf = buffer_create_from_existing_memory(cbor_data, cbor_size);
    wal_write(index->wal, metadata, cbor_buf);
    buffer_destroy(cbor_buf);
    cbor_decref(&cbor_entry);
  }
}
```

In `_index_replay_wal` (index.c:203-302), add a case to the switch — before `default:`:

```c
      case 'm':
        cbor = cbor_load(data->data, data->size, &result);
        if (result.error.code == CBOR_ERR_NONE) {
          index_entry_t* entry = cbor_to_index_entry(cbor);
          if (entry != NULL) {
            index_entry_t* from_index = REFERENCE(index_find(index, entry->hash), index_entry_t);
            if (from_index != NULL) {
              from_index->ephemeral_count = entry->ephemeral_count;
              from_index->pin_count = entry->pin_count;
              DESTROY(from_index, index_entry);
            }
            DESTROY(entry, index_entry);
          }
          cbor_decref(&cbor);
        } else {
          cbor_decref(&cbor);
        }
        break;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestEphemeralIndex.*`
Expected: 3 PASS.

- [ ] **Step 5: Commit**

```bash
git add src/BlockCache/wal.h src/BlockCache/index.h src/BlockCache/index.c test/test_ephemeral.cpp
git commit -m "feat: persist index entry ephemeral/pin metadata via WAL 'm' records"
```

---

### Task 3: Block cache ephemeral/pin messages + remove force flag

**Files:**
- Modify: `src/Actor/message.h` (append message types + payloads at END of the enum), `src/BlockCache/block_cache.h` (payloads, result codes, wrappers, remove force), `src/BlockCache/block_cache.c` (dispatch cases, wrappers, shared remove helper)
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/test_ephemeral.cpp` (uses the `bc_completion_t` pattern from `test/test_block_cache.cpp:37-106` — copy the `bc_completion_t` struct, `bc_completion_dispatch`, and `bc_put_sync`/`bc_get_sync`/`bc_remove_sync` helpers into this file, then extend the completion dispatch with the new result cases):

```cpp
/* ---- block-level ephemeral/pin ops ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  int put_result;
  int ephemeral_result;
  uint16_t ephemeral_previous;
  uint16_t ephemeral_new;
  int pin_result;
  uint32_t pin_previous;
  uint32_t pin_new;
  int remove_result;
} eph_completion_t;

static void eph_completion_dispatch(void* state, message_t* msg) {
  eph_completion_t* cs = (eph_completion_t*)state;
  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* r = (cache_put_result_payload_t*)msg->payload;
      cs->put_result = r->result;
      break;
    }
    case CACHE_EPHEMERAL_RESULT: {
      cache_ephemeral_result_payload_t* r = (cache_ephemeral_result_payload_t*)msg->payload;
      cs->ephemeral_result = r->result;
      cs->ephemeral_previous = r->previous_count;
      cs->ephemeral_new = r->new_count;
      break;
    }
    case CACHE_PIN_RESULT: {
      cache_pin_result_payload_t* r = (cache_pin_result_payload_t*)msg->payload;
      cs->pin_result = r->result;
      cs->pin_previous = r->previous_count;
      cs->pin_new = r->new_count;
      break;
    }
    case CACHE_REMOVE_RESULT: {
      cache_remove_result_payload_t* r = (cache_remove_result_payload_t*)msg->payload;
      cs->remove_result = r->result;
      break;
    }
    default:
      break;
  }
  ATOMIC_STORE(&cs->done, 1);
}

#define BLOCK_COUNT_EPH 4
class TestEphemeralCache : public testing::Test {
public:
  block_size_e type = standard;
  char* location;
  timer_actor_t* timer_actor;
  scheduler_pool_t* pool;
  block_cache_t* block_cache;
  block_t* blocks[BLOCK_COUNT_EPH];
  config_t config;
  void SetUp() override {
    location = path_join("/tmp", "EphemeralCacheTest");
    rm_rf(location);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    timer_actor = timer_actor_create(pool);
    mkdir_p(location);
    config = config_default();
    config.index_wait = 60000;
    config.index_max_wait = 60000;
    for (size_t i = 0; i < BLOCK_COUNT_EPH; i++) {
      blocks[i] = block_create_random_block_by_type(type);
    }
    block_cache = NULL;
  }
  void TearDown() override {
    scheduler_pool_wait_for_idle(pool);
    if (block_cache != NULL) {
      block_cache_sync(block_cache);
      block_cache_destroy(block_cache);
    }
    timer_actor_destroy(timer_actor);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    free(location);
    for (size_t i = 0; i < BLOCK_COUNT_EPH; i++) {
      block_destroy(blocks[i]);
    }
  }
};

static void _eph_acquire_sync(block_cache_t* bc, buffer_t* hash, scheduler_pool_t* pool,
                              int* result, uint16_t* previous, uint16_t* new_count) {
  eph_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_ephemeral(bc, hash, CACHE_EPHEMERAL_ACQUIRE, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  *result = cs.ephemeral_result;
  *previous = cs.ephemeral_previous;
  *new_count = cs.ephemeral_new;
}

TEST_F(TestEphemeralCache, EphemeralAcquireReleaseClear) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_EQ(bc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);

  int result;
  uint16_t previous, new_count;
  _eph_acquire_sync(block_cache, blocks[0]->hash, pool, &result, &previous, &new_count);
  EXPECT_EQ(result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(previous, 0u);
  EXPECT_EQ(new_count, 1u);

  /* Second acquire → 2 (simulates a propagated recycle). */
  _eph_acquire_sync(block_cache, blocks[0]->hash, pool, &result, &previous, &new_count);
  EXPECT_EQ(new_count, 2u);

  /* Release → 1, block survives. */
  eph_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_ephemeral(block_cache, blocks[0]->hash, CACHE_EPHEMERAL_RELEASE, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  EXPECT_EQ(cs.ephemeral_result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(cs.ephemeral_new, 1u);
  EXPECT_NE(block_cache_count(block_cache), 0u);

  /* Final release → 0 → block deleted regardless of pin status. */
  memset(&cs, 0, sizeof(cs));
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_ephemeral(block_cache, blocks[0]->hash, CACHE_EPHEMERAL_RELEASE, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  EXPECT_EQ(cs.ephemeral_result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(block_cache_count(block_cache), 0u);
}

TEST_F(TestEphemeralCache, PinnedPermanentRemoveRejectedAndForced) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_EQ(bc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);

  /* Pin once. */
  eph_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_pin(block_cache, blocks[0]->hash, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  EXPECT_EQ(cs.pin_result, 0);
  EXPECT_EQ(cs.pin_new, 1u);

  /* Unforced remove → CACHE_REMOVE_PINNED, block kept. */
  memset(&cs, 0, sizeof(cs));
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_remove_ex(block_cache, blocks[0]->hash, /*force=*/0, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  EXPECT_EQ(cs.remove_result, CACHE_REMOVE_PINNED);
  EXPECT_EQ(block_cache_count(block_cache), 1u);

  /* Forced remove → deleted, pin reset. */
  memset(&cs, 0, sizeof(cs));
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_remove_ex(block_cache, blocks[0]->hash, /*force=*/1, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  EXPECT_EQ(cs.remove_result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 0u);
}

TEST_F(TestEphemeralCache, ClaimedEphemeralRemoveRejected) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_EQ(bc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);
  int result; uint16_t previous, new_count;
  _eph_acquire_sync(block_cache, blocks[0]->hash, pool, &result, &previous, &new_count);
  ASSERT_EQ(result, CACHE_EPHEMERAL_OK);

  eph_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_remove_ex(block_cache, blocks[0]->hash, /*force=*/0, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  EXPECT_EQ(cs.remove_result, CACHE_REMOVE_EPHEMERAL_CLAIMED);
  EXPECT_EQ(block_cache_count(block_cache), 1u);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestEphemeralCache.*`
Expected: compile errors — `CACHE_EPHEMERAL_RESULT`, `block_cache_ephemeral`, etc. undeclared.

- [ ] **Step 3: Implement**

In `src/Actor/message.h`, at the **end of the `message_type_e` enum** (after the last existing entry; the enum is internal-only, never serialized, so appending avoids renumbering), add:

```c
  /* Ephemeral/pin block metadata */
  CACHE_EPHEMERAL,
  CACHE_PIN,
  CACHE_UNPIN,
  CACHE_EPHEMERAL_LIST,
  CACHE_EPHEMERAL_RESULT,
  CACHE_PIN_RESULT,
  CACHE_UNPIN_RESULT,
  CACHE_EPHEMERAL_LIST_RESULT,
  EPHEMERAL_REGISTRY_ADD,
  EPHEMERAL_REGISTRY_CHECK,
  EPHEMERAL_REGISTRY_CHECK_RESULT,
  EPHEMERAL_REGISTRY_REMOVE,
```

In `src/BlockCache/block_cache.h`, add after the `cache_remove_payload_t` block (near line 79):

```c
/* CACHE_EPHEMERAL / CACHE_PIN op semantics */
#define CACHE_EPHEMERAL_OK        0
#define CACHE_EPHEMERAL_NOT_FOUND -1
#define CACHE_EPHEMERAL_OVERFLOW  -2

/* CACHE_REMOVE extended results (result is otherwise 0 / -1) */
#define CACHE_REMOVE_PINNED             -3
#define CACHE_REMOVE_EPHEMERAL_CLAIMED  -4

typedef enum {
  CACHE_EPHEMERAL_ACQUIRE = 0,  /* count += 1; error at UINT16_MAX */
  CACHE_EPHEMERAL_RELEASE = 1, /* count -= 1; block deleted when it reaches 0 */
  CACHE_EPHEMERAL_CLEAR = 2     /* count = 0; commit (announce is caller's job) */
} cache_ephemeral_op_e;

/* Payload for CACHE_EPHEMERAL message */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
  cache_ephemeral_op_e op;
  int result;
  uint16_t previous_count;
  uint16_t new_count;
} cache_ephemeral_payload_t;

/* Result payload for CACHE_EPHEMERAL_RESULT. previous_count > 0 marks a block
   that was actually ephemeral before a CLEAR — the caller announces those. */
typedef struct {
  int result;
  uint16_t previous_count;
  uint16_t new_count;
  actor_t* reply_to;
} cache_ephemeral_result_payload_t;

/* Payload shared by CACHE_PIN / CACHE_UNPIN (and their results) */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
  int result;
  uint32_t previous_count;
  uint32_t new_count;
} cache_pin_payload_t;

typedef struct {
  int result;
  uint32_t previous_count;
  uint32_t new_count;
  actor_t* reply_to;
} cache_pin_result_payload_t;

/* Payload for CACHE_EPHEMERAL_LIST — enumerates every ephemeral block.
   hashes[i] are referenced buffers; arrays are owned by the payload. */
typedef struct {
  actor_t* reply_to;
  size_t count;
  buffer_t** hashes;
  uint16_t* ephemeral_counts;
  uint32_t* pin_counts;
} cache_ephemeral_list_payload_t;
```

Extend `cache_put_payload_t` (block_cache.h:56-61) with:

```c
  uint8_t acquire_ephemeral; /* nonzero: acquire an ephemeral claim on this put */
```

Extend `cache_remove_payload_t` with:

```c
  uint8_t force; /* nonzero: bypass pin/claim protection */
```

Extend the async API declarations (block_cache.h:158-161):

```c
void block_cache_ephemeral(block_cache_t* block_cache, buffer_t* hash, cache_ephemeral_op_e op, actor_t* reply_to);
void block_cache_pin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);
void block_cache_unpin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to);
void block_cache_list_ephemeral(block_cache_t* block_cache, actor_t* reply_to);
void block_cache_remove_ex(block_cache_t* block_cache, buffer_t* hash, uint8_t force, actor_t* reply_to);
```

In `src/BlockCache/block_cache.c`:

1. Factor the removal body of the `CACHE_REMOVE` case (block_cache.c:765-806) into a static helper and reuse it from both `CACHE_REMOVE` and the RELEASE-at-zero path:

```c
static void _block_cache_delete_entry(block_cache_t* block_cache, index_entry_t* entry) {
  size_t section_index = entry->section_index;
  index_remove(block_cache->index, entry->hash);
  timer_actor_debounce(block_cache->timer_actor, block_cache->index_wait, 0, &block_cache->actor, INDEX_SAVE);
  block_cache->current_bytes -= (size_t)block_cache->type;
  block_cache_update_capacity(block_cache);
  block_lru_cache_delete(block_cache->lru, entry->hash);
  section_deallocate_payload_t dealloc_payload;
  dealloc_payload.index = section_index;
  dealloc_payload.reply_to = NULL;
  dealloc_payload.result = -1;
  message_t dealloc_msg;
  dealloc_msg.type = SECTION_DEALLOCATE;
  dealloc_msg.payload = &dealloc_payload;
  dealloc_msg.payload_destroy = NULL;
  sections_dispatch(block_cache->sections, &dealloc_msg);
}
```

2. Rewrite the `CACHE_REMOVE` case to guard first (using the same entry lookup already there):

```c
    case CACHE_REMOVE: {
      cache_remove_payload_t* p = (cache_remove_payload_t*)msg->payload;
      p->result = -1;
      index_entry_t* entry = block_lru_cache_peek_entry(block_cache->lru, p->hash);
      if (entry == NULL) {
        entry = index_peek(block_cache->index, p->hash);
      }
      if (entry == NULL) {
        p->result = 0;
      } else if (entry->ephemeral_count > 0 && !p->force) {
        log_warn("CACHE_REMOVE: block has %u ephemeral claims — rejected (use force or release)",
                 (unsigned)entry->ephemeral_count);
        p->result = CACHE_REMOVE_EPHEMERAL_CLAIMED;
      } else if (entry->pin_count > 0 && !p->force) {
        log_warn("CACHE_REMOVE: block is pinned (pin_count=%u) — rejected (use force)",
                 (unsigned)entry->pin_count);
        p->result = CACHE_REMOVE_PINNED;
      } else {
        if (p->force && entry->pin_count > 0) {
          entry->pin_count = 0;
          index_write_entry_metadata(block_cache->index, entry);
        }
        _block_cache_delete_entry(block_cache, entry);
        p->result = 0;
      }
      /* ... keep the existing DESTROY(p->hash) + async reply block unchanged ... */
```

3. Add dispatch cases before `default:`:

```c
    case CACHE_EPHEMERAL: {
      cache_ephemeral_payload_t* p = (cache_ephemeral_payload_t*)msg->payload;
      index_entry_t* entry = block_lru_cache_peek_entry(block_cache->lru, p->hash);
      if (entry == NULL) {
        entry = index_peek(block_cache->index, p->hash);
      }
      if (entry == NULL) {
        p->result = CACHE_EPHEMERAL_NOT_FOUND;
        p->previous_count = 0;
        p->new_count = 0;
      } else {
        p->previous_count = entry->ephemeral_count;
        switch (p->op) {
          case CACHE_EPHEMERAL_ACQUIRE:
            if (entry->ephemeral_count == UINT16_MAX) {
              p->result = CACHE_EPHEMERAL_OVERFLOW;
              p->new_count = entry->ephemeral_count;
              break;
            }
            entry->ephemeral_count += 1;
            p->result = CACHE_EPHEMERAL_OK;
            p->new_count = entry->ephemeral_count;
            index_write_entry_metadata(block_cache->index, entry);
            break;
          case CACHE_EPHEMERAL_RELEASE:
            if (entry->ephemeral_count > 0) {
              entry->ephemeral_count -= 1;
              index_write_entry_metadata(block_cache->index, entry);
            }
            p->result = CACHE_EPHEMERAL_OK;
            p->new_count = entry->ephemeral_count;
            if (entry->ephemeral_count == 0) {
              /* Last claim released — delete regardless of pin status. */
              _block_cache_delete_entry(block_cache, entry);
            }
            break;
          case CACHE_EPHEMERAL_CLEAR:
            if (entry->ephemeral_count > 0) {
              entry->ephemeral_count = 0;
              index_write_entry_metadata(block_cache->index, entry);
            }
            p->result = CACHE_EPHEMERAL_OK;
            p->new_count = 0;
            break;
        }
      }
      if (p->reply_to != NULL) {
        cache_ephemeral_result_payload_t* result = get_clear_memory(sizeof(cache_ephemeral_result_payload_t));
        result->result = p->result;
        result->previous_count = p->previous_count;
        result->new_count = p->new_count;
        result->reply_to = NULL;
        message_t reply;
        reply.type = CACHE_EPHEMERAL_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case CACHE_PIN:
    case CACHE_UNPIN: {
      cache_pin_payload_t* p = (cache_pin_payload_t*)msg->payload;
      index_entry_t* entry = block_lru_cache_peek_entry(block_cache->lru, p->hash);
      if (entry == NULL) {
        entry = index_peek(block_cache->index, p->hash);
      }
      if (entry == NULL) {
        p->result = -1;
        p->previous_count = 0;
        p->new_count = 0;
      } else {
        p->previous_count = entry->pin_count;
        if (msg->type == CACHE_PIN) {
          if (entry->pin_count < UINT32_MAX) {
            entry->pin_count += 1;
          } else {
            log_warn("CACHE_PIN: pin_count saturated for block");
          }
        } else if (entry->pin_count > 0) {
          entry->pin_count -= 1;
        }
        p->result = 0;
        p->new_count = entry->pin_count;
        index_write_entry_metadata(block_cache->index, entry);
      }
      if (p->reply_to != NULL) {
        cache_pin_result_payload_t* result = get_clear_memory(sizeof(cache_pin_result_payload_t));
        result->result = p->result;
        result->previous_count = p->previous_count;
        result->new_count = p->new_count;
        result->reply_to = NULL;
        message_t reply;
        reply.type = (msg->type == CACHE_PIN) ? CACHE_PIN_RESULT : CACHE_UNPIN_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case CACHE_EPHEMERAL_LIST: {
      cache_ephemeral_list_payload_t* p = (cache_ephemeral_list_payload_t*)msg->payload;
      index_entry_vec_t* entries = index_to_array(block_cache->index);
      size_t ephemeral_total = 0;
      for (size_t idx = 0; idx < entries->length; idx++) {
        if (entries->data[idx]->ephemeral_count > 0) ephemeral_total++;
      }
      p->count = ephemeral_total;
      p->hashes = NULL;
      p->ephemeral_counts = NULL;
      p->pin_counts = NULL;
      if (ephemeral_total > 0) {
        p->hashes = get_clear_memory(sizeof(buffer_t*) * ephemeral_total);
        p->ephemeral_counts = get_clear_memory(sizeof(uint16_t) * ephemeral_total);
        p->pin_counts = get_clear_memory(sizeof(uint32_t) * ephemeral_total);
        size_t write_idx = 0;
        for (size_t idx = 0; idx < entries->length; idx++) {
          index_entry_t* cur_entry = entries->data[idx];
          if (cur_entry->ephemeral_count > 0) {
            p->hashes[write_idx] = (buffer_t*)refcounter_reference((refcounter_t*)cur_entry->hash);
            p->ephemeral_counts[write_idx] = cur_entry->ephemeral_count;
            p->pin_counts[write_idx] = cur_entry->pin_count;
            write_idx++;
          }
        }
      }
      if (p->reply_to != NULL) {
        actor_send(p->reply_to, msg);  /* mirror the payload back; caller owns it */
      }
      for (size_t idx = 0; idx < entries->length; idx++) {
        index_entry_destroy(entries->data[idx]);
      }
      vec_deinit(entries);
      free(entries);
      break;
    }
```

Note: for `CACHE_EPHEMERAL_LIST` the reply mirrors the message — the caller (completion actor) must free the arrays and dereference the hashes. Add a destroy helper in block_cache.c and declare it in the header:

```c
void cache_ephemeral_list_payload_destroy(cache_ephemeral_list_payload_t* payload) {
  if (payload == NULL) return;
  if (payload->hashes != NULL) {
    for (size_t idx = 0; idx < payload->count; idx++) {
      DESTROY(payload->hashes[idx], buffer);
    }
    free(payload->hashes);
  }
  free(payload->ephemeral_counts);
  free(payload->pin_counts);
  free(payload);
}
```

4. Add the async wrappers (model them on `block_cache_put`, block_cache.c:815-828):

```c
void block_cache_ephemeral(block_cache_t* block_cache, buffer_t* hash, cache_ephemeral_op_e op, actor_t* reply_to) {
  cache_ephemeral_payload_t* payload = get_clear_memory(sizeof(cache_ephemeral_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->op = op;
  payload->result = CACHE_EPHEMERAL_NOT_FOUND;
  message_t msg;
  msg.type = CACHE_EPHEMERAL;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&block_cache->actor, &msg);
}

void block_cache_pin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to) {
  cache_pin_payload_t* payload = get_clear_memory(sizeof(cache_pin_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->result = -1;
  message_t msg;
  msg.type = CACHE_PIN;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&block_cache->actor, &msg);
}

void block_cache_unpin(block_cache_t* block_cache, buffer_t* hash, actor_t* reply_to) {
  cache_pin_payload_t* payload = get_clear_memory(sizeof(cache_pin_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->result = -1;
  message_t msg;
  msg.type = CACHE_UNPIN;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&block_cache->actor, &msg);
}

void block_cache_list_ephemeral(block_cache_t* block_cache, actor_t* reply_to) {
  cache_ephemeral_list_payload_t* payload = get_clear_memory(sizeof(cache_ephemeral_list_payload_t));
  payload->reply_to = reply_to;
  payload->count = 0;
  payload->hashes = NULL;
  payload->ephemeral_counts = NULL;
  payload->pin_counts = NULL;
  message_t msg;
  msg.type = CACHE_EPHEMERAL_LIST;
  msg.payload = payload;
  msg.payload_destroy = (void (*)(void*))cache_ephemeral_list_payload_destroy;
  actor_send(&block_cache->actor, &msg);
}

void block_cache_remove_ex(block_cache_t* block_cache, buffer_t* hash, uint8_t force, actor_t* reply_to) {
  cache_remove_payload_t* payload = get_clear_memory(sizeof(cache_remove_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
  payload->reply_to = reply_to;
  payload->force = force;
  payload->result = -1;
  message_t msg;
  msg.type = CACHE_REMOVE;
  msg.payload = payload;
  msg.payload_destroy = cache_remove_payload_destroy;
  actor_send(&block_cache->actor, &msg);
}
```

CAREFUL with the `CACHE_EPHEMERAL_LIST` reply mirroring: `actor_send(p->reply_to, msg)` sends the same `message_t`, and after dispatch returns, `actor_run` invokes `msg->payload_destroy` (i.e. `cache_ephemeral_list_payload_destroy`) on the payload — the mirrored message must NOT also destroy it. The completion actor receiving it must steal the arrays and null them, then let the destroy run on an emptied payload (destroy tolerates NULL arrays). The rep/HTTP/C-client consumers do exactly that (see Task 11/12/13). Document this in a one-line comment above the case.

Also note `cache_ephemeral_payload_t` has a `hash` that its `free` destroy leaks — give it a dedicated destroy instead of `free`:

```c
static void cache_ephemeral_payload_destroy(void* ptr) {
  cache_ephemeral_payload_t* payload = (cache_ephemeral_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}
```

and use it in `block_cache_ephemeral`. Same for `cache_pin_payload_t` (shared static `cache_pin_payload_destroy`). Verify with valgrind at the end.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*'`
Expected: all PASS. Then run `--gtest_filter=TestBlockCache.*` — the existing remove tests must still pass (`block_cache_remove` now means force=0, which is what they exercise on unpinned permanent blocks).

- [ ] **Step 5: Commit**

```bash
git add src/Actor/message.h src/BlockCache/block_cache.h src/BlockCache/block_cache.c test/test_ephemeral.cpp
git commit -m "feat: add CACHE_EPHEMERAL/PIN/UNPIN/LIST messages and remove force flag"
```

---

### Task 4: Ephemeral-aware put + respiration victim exclusion + servable helper

**Files:**
- Modify: `src/BlockCache/block_cache.c` (CACHE_PUT acquire branch; `block_cache_update_capacity` victim loop; new `block_cache_put_ephemeral` wrapper), `src/BlockCache/block_cache.h` (declare wrapper + `block_cache_entry_is_sheddable` + `index_entry_is_servable` in `index.h`), `src/BlockCache/index.h` (servable helper), `src/Network/network.c:2713-2785` (remote find-block skip)
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/test_ephemeral.cpp`:

```cpp
TEST(TestEphemeralHelpers, SheddableAndServable) {
  index_entry_t* entry = index_entry_create(block_create_random_block_by_type(standard)->hash);
  /* Freshly put block: sheddable, servable. */
  EXPECT_TRUE(block_cache_entry_is_sheddable(entry));
  EXPECT_TRUE(index_entry_is_servable(entry));
  /* Pinned permanent: not sheddable, servable. */
  entry->pin_count = 1;
  EXPECT_FALSE(block_cache_entry_is_sheddable(entry));
  EXPECT_TRUE(index_entry_is_servable(entry));
  /* Ephemeral: not sheddable, not servable to peers. */
  entry->pin_count = 0;
  entry->ephemeral_count = 1;
  EXPECT_FALSE(block_cache_entry_is_sheddable(entry));
  EXPECT_FALSE(index_entry_is_servable(entry));
  index_entry_destroy(entry);
}

TEST_F(TestEphemeralCache, EphemeralPutAcquiresClaim) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  block_t* ref_block = (block_t*)refcounter_reference((refcounter_t*)blocks[0]);
  refcounter_yield((refcounter_t*)ref_block);
  block_cache_put_ephemeral(block_cache, ref_block, NULL);
  scheduler_pool_wait_for_idle(pool);
  actor_run(&block_cache->actor, ACTOR_BATCH_SIZE);
  index_entry_t* entry = index_peek(block_cache->index, blocks[0]->hash);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->ephemeral_count, 1u);
  block_t* fetched = bc_get_sync(block_cache, blocks[0]->hash, pool);
  EXPECT_NE(fetched, nullptr);   /* local reads still work while ephemeral */
  if (fetched != NULL) block_destroy(fetched);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeralHelpers.*:TestEphemeralCache.EphemeralPutAcquiresClaim'`
Expected: compile error — helpers undeclared.

- [ ] **Step 3: Implement**

In `src/BlockCache/index.h`:

```c
/* Ephemeral blocks are local-only: never served to peers in find-block
   responses and never pushed by respiration. */
bool index_entry_is_servable(const index_entry_t* entry);
```

In `src/BlockCache/index.c`:

```c
bool index_entry_is_servable(const index_entry_t* entry) {
  return entry != NULL && entry->ephemeral_count == 0;
}
```

In `src/BlockCache/block_cache.h`:

```c
/* Victim-candidate check for respiration exhale: pinned permanent blocks and
   ephemeral blocks are never shed. LRU/capacity behavior ignores both fields. */
bool block_cache_entry_is_sheddable(const index_entry_t* entry);

/* Put that acquires an ephemeral claim (count starts at 1) on the stored block. */
void block_cache_put_ephemeral(block_cache_t* block_cache, block_t* block, actor_t* reply_to);
```

In `src/BlockCache/block_cache.c`:

```c
bool block_cache_entry_is_sheddable(const index_entry_t* entry) {
  return entry != NULL && entry->pin_count == 0 && entry->ephemeral_count == 0;
}
```

In the `CACHE_PUT` case of `block_cache_dispatch`, after the existing-entry `else` branch sets `p->result = CACHE_PUT_EXISTS;` (block_cache.c:665) and inside the `entry == NULL` success path after `p->result = CACHE_PUT_NEW;` (block_cache.c:656), apply the claim. Concretely, right before the `/* Async: send result back if reply_to is set */` comment add:

```c
      if (p->acquire_ephemeral && p->result != CACHE_PUT_ERROR && p->result != CACHE_PUT_FULL) {
        if (entry->ephemeral_count < UINT16_MAX) {
          entry->ephemeral_count += 1;
          index_write_entry_metadata(block_cache->index, entry);
        } else {
          log_error("CACHE_PUT: ephemeral claim overflow for existing block — put proceeds without claim");
        }
      }
```

(Note: in the EXISTS branch `entry` is the peeked existing entry; in the NEW branch it is the freshly added one. Both are non-NULL at this point in their respective paths — guard with `if (entry != NULL && p->acquire_ephemeral && ...)`.)

Add the wrapper next to `block_cache_put`:

```c
void block_cache_put_ephemeral(block_cache_t* block_cache, block_t* block, actor_t* reply_to) {
  cache_put_payload_t* payload = get_clear_memory(sizeof(cache_put_payload_t));
  payload->block = (block_t*)refcounter_reference((refcounter_t*)block);
  payload->incoming_fib = 0;
  payload->reply_to = reply_to;
  payload->acquire_ephemeral = 1;
  payload->result = CACHE_PUT_ERROR;
  message_t msg;
  msg.type = CACHE_PUT;
  msg.payload = payload;
  msg.payload_destroy = cache_put_payload_destroy;
  actor_send(&block_cache->actor, &msg);
}
```

Also update the plain `block_cache_put` wrapper to set `payload->acquire_ephemeral = 0;` (get_clear_memory zeroes it, but set it explicitly for clarity — optional).

In `block_cache_update_capacity` (block_cache.c:754-788), exclude non-sheddable entries from the victim payload: replace the copy loop with:

```c
        size_t candidate_count = 0;
        for (size_t idx = 0; idx < entries->length; idx++) {
          if (block_cache_entry_is_sheddable(entries->data[idx])) candidate_count++;
        }
        if (candidate_count == 0) {
          vec_deinit(entries);
          free(entries);
          break;
        }
        payload->count = candidate_count;
        payload->hashes = get_clear_memory(sizeof(buffer_t*) * candidate_count);
        payload->ejection_dates = get_clear_memory(sizeof(uint64_t) * candidate_count);
        payload->capacity = capacity;
        size_t write_idx = 0;
        for (size_t idx = 0; idx < entries->length; idx++) {
          if (!block_cache_entry_is_sheddable(entries->data[idx])) continue;
          payload->hashes[write_idx] = (buffer_t*)refcounter_reference((refcounter_t*)entries->data[idx]->hash);
          payload->ejection_dates[write_idx] = entries->data[idx]->ejection_date;
          write_idx++;
        }
```

(Keep the surrounding `entries != NULL` handling; the `vec_deinit/free` after the send stays.)

In `src/Network/network.c`, `network_handle_find_block` (network.c:2713-2785): the local-cache check at line ~2799 becomes:

```c
      index_entry_t* entry = index_peek(network->block_cache->index, hash_buf);
      if (entry != NULL && !index_entry_is_servable(entry)) {
        /* Ephemeral blocks are local-only — treat as not-found for peers. */
        entry = NULL;
      }
      if (entry != NULL) {
```

Do NOT touch `network_handle_local_find_block` (network.c:4199) — that path serves local stream reads (including reads of ephemeral representations) and must keep finding ephemeral blocks.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*:*BlockCache*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/BlockCache/index.h src/BlockCache/index.c src/BlockCache/block_cache.h src/BlockCache/block_cache.c src/Network/network.c test/test_ephemeral.cpp
git commit -m "feat: ephemeral-aware put claims, respiration exclusion, peer servable check"
```

---

### Task 5: Ephemeral registry actor (elastic bloom filter + rotation persistence)

**Files:**
- Create: `src/BlockCache/ephemeral_registry.h`, `src/BlockCache/ephemeral_registry.c`
- Modify: `src/Actor/message.h` (payloads — types already added in Task 3), `src/BlockCache/block_cache.h` (registry field), `src/BlockCache/block_cache.c` (create/destroy wiring), `src/Configuration/config.h` + `config.c` (fields + defaults), root `CMakeLists.txt` (add `src/BlockCache/ephemeral_registry.c` to the offs library sources list — find the BlockCache source group)
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_ephemeral.cpp`:

```cpp
/* ---- ephemeral registry actor ---- */

#include "../src/BlockCache/ephemeral_registry.h"
#include "../src/Bloom/elastic_bloom_filter.h"

typedef struct {
  ATOMIC(uint8_t) done;
  uint8_t present;
} registry_completion_t;

static void registry_completion_dispatch(void* state, message_t* msg) {
  registry_completion_t* cs = (registry_completion_t*)state;
  if (msg->type == EPHEMERAL_REGISTRY_CHECK_RESULT) {
    ephemeral_registry_check_result_payload_t* result =
        (ephemeral_registry_check_result_payload_t*)msg->payload;
    cs->present = result->present;
  }
  ATOMIC_STORE(&cs->done, 1);
}

TEST(TestEphemeralRegistry, AddCheckRemovePersist) {
  rm_rf("/tmp/EphemeralRegistryTest");
  mkdir_p("/tmp/EphemeralRegistryTest");
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  config_t config = config_default();
  buffer_t* descriptor_hash = block_create_random_block_by_type(standard)->hash;

  ephemeral_registry_t* registry = ephemeral_registry_create("/tmp/EphemeralRegistryTest", config, pool);
  ASSERT_NE(registry, nullptr);

  /* Check before add → miss. */
  registry_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, registry_completion_dispatch, pool);
  ephemeral_registry_check(registry, descriptor_hash, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  EXPECT_EQ(cs.present, 0u);

  /* Add → check hit. */
  ephemeral_registry_add(registry, descriptor_hash);
  platform_sleep_ms(50);   /* add flushes synchronously on the actor */
  memset(&cs, 0, sizeof(cs));
  actor_init(&comp, &cs, registry_completion_dispatch, pool);
  ephemeral_registry_check(registry, descriptor_hash, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  EXPECT_EQ(cs.present, 1u);

  /* Remove → check miss again (native elastic BF deletion, no rebuild). */
  ephemeral_registry_remove(registry, descriptor_hash);
  platform_sleep_ms(50);
  memset(&cs, 0, sizeof(cs));
  actor_init(&comp, &cs, registry_completion_dispatch, pool);
  ephemeral_registry_check(registry, descriptor_hash, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  EXPECT_EQ(cs.present, 0u);

  /* Persistence: add, destroy, reload → still present. */
  ephemeral_registry_add(registry, descriptor_hash);
  platform_sleep_ms(50);
  scheduler_pool_wait_for_idle(pool);
  ephemeral_registry_destroy(registry);

  ephemeral_registry_t* reloaded = ephemeral_registry_create("/tmp/EphemeralRegistryTest", config, pool);
  ASSERT_NE(reloaded, nullptr);
  memset(&cs, 0, sizeof(cs));
  actor_init(&comp, &cs, registry_completion_dispatch, pool);
  ephemeral_registry_check(reloaded, descriptor_hash, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  EXPECT_EQ(cs.present, 1u);
  ephemeral_registry_destroy(reloaded);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  DESTROY(descriptor_hash, buffer);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestEphemeralRegistry.*`
Expected: compile error — `ephemeral_registry.h` does not exist.

- [ ] **Step 3: Implement**

`src/BlockCache/ephemeral_registry.h`:

```c
#ifndef OFFS_EPHEMERAL_REGISTRY_H
#define OFFS_EPHEMERAL_REGISTRY_H

#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Bloom/elastic_bloom_filter.h"
#include "../Configuration/config.h"
#include "../Scheduler/scheduler.h"
#include <stdint.h>

/* Actor-owned registry of ephemeral representation descriptor hashes.
   Backed by a persistent elastic bloom filter (native add/remove). Advisory
   in both directions: the recycler's fetch-time index check is exact; a
   filter miss is healed by an ADD when ephemeral blocks are discovered. */
typedef struct ephemeral_registry_t {
  actor_t actor;
  scheduler_pool_t* pool;
  elastic_bloom_filter_t* filter;
  char* current_file;   /* <location>/ephemeral_registry.bf     */
  char* backup_file;    /* <location>/ephemeral_registry.bf.last */
  char* temp_file;      /* <location>/ephemeral_registry.bf.tmp  */
} ephemeral_registry_t;

ephemeral_registry_t* ephemeral_registry_create(const char* location, config_t config, scheduler_pool_t* pool);
void ephemeral_registry_destroy(ephemeral_registry_t* registry);
void ephemeral_registry_dispatch(void* state, message_t* msg);

/* Async API — actor messages */
void ephemeral_registry_add(ephemeral_registry_t* registry, buffer_t* descriptor_hash);
void ephemeral_registry_remove(ephemeral_registry_t* registry, buffer_t* descriptor_hash);
void ephemeral_registry_check(ephemeral_registry_t* registry, buffer_t* descriptor_hash, actor_t* reply_to);

#endif // OFFS_EPHEMERAL_REGISTRY_H
```

In `src/Actor/message.h`, add the check-result payload next to the other payload structs:

```c
/* Result payload for EPHEMERAL_REGISTRY_CHECK_RESULT */
typedef struct {
  uint8_t present;   /* 1 = filter contains the descriptor hash (advisory) */
  actor_t* reply_to;
} ephemeral_registry_check_result_payload_t;
```

`src/BlockCache/ephemeral_registry.c`:

```c
#include "ephemeral_registry.h"
#include "../Util/allocator.h"
#include "../Util/path_join.h"
#include "../Platform/platform_file.h"
#include "../Util/error.h"
#include <cbor.h>
#include <stdio.h>

static void _registry_flush(ephemeral_registry_t* registry) {
  if (registry->filter == NULL) return;
  cbor_item_t* cbor = elastic_bloom_filter_encode(registry->filter);
  if (cbor == NULL) {
    log_error("ephemeral_registry: filter encode failed — keeping previous file");
    return;
  }
  uint8_t* cbor_data = NULL;
  size_t cbor_size = 0;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  cbor_decref(&cbor);
  if (cbor_data == NULL || cbor_size == 0) return;

  /* Write temp, fsync, rotate: current → backup, temp → current. */
  platform_file_t* file = platform_file_open(registry->temp_file,
                                             PLATFORM_O_WRONLY | PLATFORM_O_CREAT | PLATFORM_O_TRUNC, 0644);
  if (file == NULL) {
    free(cbor_data);
    return;
  }
  ssize_t written = platform_file_write(file, cbor_data, cbor_size);
  free(cbor_data);
  if (written <= 0) {
    platform_file_close(file);
    log_error("ephemeral_registry: flush write failed");
    return;
  }
  platform_file_sync(file);
  platform_file_close(file);

  platform_file_remove(registry->backup_file);
  platform_file_rename(registry->current_file, registry->backup_file);
  platform_file_rename(registry->temp_file, registry->current_file);
}

static elastic_bloom_filter_t* _registry_load_filter(ephemeral_registry_t* registry, const char* file_name) {
  platform_file_t* file = platform_file_open(file_name, PLATFORM_O_RDONLY, 0);
  if (file == NULL) return NULL;
  /* Read whole file (filters are small). */
  uint8_t* data = NULL;
  size_t data_size = 0;
  size_t capacity = 65536;
  data = get_memory(capacity);
  ssize_t chunk;
  while ((chunk = platform_file_read(file, data + data_size, capacity - data_size)) > 0) {
    data_size += (size_t)chunk;
    if (data_size == capacity) {
      uint8_t* grown = get_memory(capacity * 2);
      memcpy(grown, data, data_size);
      free(data);
      data = grown;
      capacity *= 2;
    }
  }
  platform_file_close(file);
  elastic_bloom_filter_t* filter = NULL;
  if (data_size > 0) {
    struct cbor_load_result result;
    cbor_item_t* cbor = cbor_load(data, data_size, &result);
    if (result.error.code == CBOR_ERR_NONE) {
      filter = elastic_bloom_filter_decode(cbor);
      cbor_decref(&cbor);
    }
  }
  free(data);
  return filter;
}

static void _registry_check_file(ephemeral_registry_t* registry) {
  if (registry->filter != NULL) return;
  log_warn("ephemeral_registry: filter file missing or corrupt — starting empty (recoverable via index walk by operators)");
}

void ephemeral_registry_dispatch(void* state, message_t* msg) {
  ephemeral_registry_t* registry = (ephemeral_registry_t*)state;
  if (registry == NULL || registry->filter == NULL) return;
  switch (msg->type) {
    case EPHEMERAL_REGISTRY_ADD: {
      buffer_t* hash = (buffer_t*)msg->payload;
      if (hash != NULL && hash->data != NULL) {
        if (elastic_bloom_filter_add(registry->filter, hash->data, hash->size)) {
          _registry_flush(registry);
        }
      }
      break;
    }
    case EPHEMERAL_REGISTRY_REMOVE: {
      buffer_t* hash = (buffer_t*)msg->payload;
      if (hash != NULL && hash->data != NULL) {
        if (elastic_bloom_filter_remove(registry->filter, hash->data, hash->size)) {
          _registry_flush(registry);
        }
      }
      break;
    }
    case EPHEMERAL_REGISTRY_CHECK: {
      registry_check_request_payload_t* payload = (registry_check_request_payload_t*)msg->payload;
      if (payload == NULL || payload->reply_to == NULL) break;
      ephemeral_registry_check_result_payload_t* result =
          get_clear_memory(sizeof(ephemeral_registry_check_result_payload_t));
      result->present = payload->hash != NULL && payload->hash->data != NULL &&
          elastic_bloom_filter_contains(registry->filter, payload->hash->data, payload->hash->size);
      result->reply_to = NULL;
      message_t reply;
      reply.type = EPHEMERAL_REGISTRY_CHECK_RESULT;
      reply.payload = result;
      reply.payload_destroy = free;
      actor_send(payload->reply_to, &reply);
      break;
    }
    default:
      break;
  }
}
```

For the CHECK message the payload needs to carry both the hash and the reply actor. Add to `message.h`:

```c
/* Payload for EPHEMERAL_REGISTRY_CHECK */
typedef struct {
  buffer_t* hash;
  actor_t* reply_to;
} registry_check_request_payload_t;
```

Wrappers in `ephemeral_registry.c`:

```c
void ephemeral_registry_add(ephemeral_registry_t* registry, buffer_t* descriptor_hash) {
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)descriptor_hash);
  message_t msg;
  msg.type = EPHEMERAL_REGISTRY_ADD;
  msg.payload = hash;
  msg.payload_destroy = (void (*)(void*))buffer_destroy_wrapper;
  actor_send(&registry->actor, &msg);
}
```

Use a small static wrapper `static void buffer_destroy_wrapper(void* ptr) { DESTROY((buffer_t*)ptr, buffer); }` (DESTROY handles the refcount-yield bookkeeping; the reference taken here is consumed by the destroy).

`ephemeral_registry_remove` is identical with `EPHEMERAL_REGISTRY_REMOVE`. `ephemeral_registry_check`:

```c
void ephemeral_registry_check(ephemeral_registry_t* registry, buffer_t* descriptor_hash, actor_t* reply_to) {
  registry_check_request_payload_t* payload = get_clear_memory(sizeof(registry_check_request_payload_t));
  payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)descriptor_hash);
  payload->reply_to = reply_to;
  message_t msg;
  msg.type = EPHEMERAL_REGISTRY_CHECK;
  msg.payload = payload;
  msg.payload_destroy = free;   /* hash leak avoided below */
  actor_send(&registry->actor, &msg);
}
```

Give the check request a proper destroy instead of `free` (same pattern as `cache_ephemeral_payload_destroy`):

```c
static void registry_check_request_destroy(void* ptr) {
  registry_check_request_payload_t* payload = (registry_check_request_payload_t*)ptr;
  if (payload->hash != NULL) {
    DESTROY(payload->hash, buffer);
  }
  free(payload);
}
```

Create/destroy:

```c
ephemeral_registry_t* ephemeral_registry_create(const char* location, config_t config, scheduler_pool_t* pool) {
  if (location == NULL || pool == NULL) return NULL;
  ephemeral_registry_t* registry = get_clear_memory(sizeof(ephemeral_registry_t));
  registry->pool = pool;
  registry->current_file = path_join(location, "ephemeral_registry.bf");
  registry->backup_file = path_join(location, "ephemeral_registry.bf.last");
  registry->temp_file = path_join(location, "ephemeral_registry.bf.tmp");
  registry->filter = _registry_load_filter(registry, registry->current_file);
  if (registry->filter == NULL) {
    registry->filter = _registry_load_filter(registry, registry->backup_file);
    _registry_check_file(registry);
  }
  if (registry->filter == NULL) {
    registry->filter = elastic_bloom_filter_create(config.ephemeral_registry_size,
                                                  config.ephemeral_registry_hash_count,
                                                  config.ephemeral_registry_omega,
                                                  config.ephemeral_registry_fp_bits);
  }
  if (registry->filter == NULL) {
    free(registry->current_file);
    free(registry->backup_file);
    free(registry->temp_file);
    free(registry);
    return NULL;
  }
  actor_init(&registry->actor, registry, ephemeral_registry_dispatch, pool);
  return registry;
}

void ephemeral_registry_destroy(ephemeral_registry_t* registry) {
  if (registry == NULL) return;
  scheduler_pool_wait_for_idle(registry->pool);
  actor_destroy(&registry->actor);
  elastic_bloom_filter_destroy(registry->filter);
  free(registry->current_file);
  free(registry->backup_file);
  free(registry->temp_file);
  free(registry);
}
```

If `platform_file_remove`/`platform_file_rename` do not exist in `src/Platform/platform_file.h`, add them there (POSIX: `remove()`/`rename()`, Windows: `DeleteFileA`/`MoveFileExA` with `MOVEFILE_REPLACE_EXISTING`) following the existing platform_file wrapper style — check that header first and mirror its conventions.

Config fields — in `src/Configuration/config.h` add:

```c
  size_t ephemeral_registry_size;        /* initial elastic filter capacity */
  uint32_t ephemeral_registry_hash_count; /* hashes per element */
  float ephemeral_registry_omega;        /* load ratio before expansion */
  uint32_t ephemeral_registry_fp_bits;   /* fingerprint bits per entry */
```

In `src/Configuration/config.c` `config_default()` add:

```c
  config.ephemeral_registry_size = 1024;
  config.ephemeral_registry_hash_count = 4;
  config.ephemeral_registry_omega = 0.85f;
  config.ephemeral_registry_fp_bits = EBF_DEFAULT_FP_BITS;
```

(config.c must include `../Bloom/elastic_bloom_filter.h` for `EBF_DEFAULT_FP_BITS` — or hardcode 8.)

Wire into `block_cache_t` (block_cache.h:92-108, add `ephemeral_registry_t* registry;`) and `block_cache_create` (block_cache.c:672-718, after the index creation block):

```c
  block_cache->registry = ephemeral_registry_create(folder, config, pool);
```

and `block_cache_destroy` (before `actor_destroy`, following the existing teardown ordering): `ephemeral_registry_destroy(block_cache->registry);`.

Add the new `.c` to the library build: find the BlockCache group in root `CMakeLists.txt` (grep `block_cache.c`) and add `src/BlockCache/ephemeral_registry.c`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*'`
Expected: PASS. Also confirm `TestBlockCache.*` still passes (block_cache_create now also creates the registry — watch for teardown order issues under `--gtest_filter=TestBlockCache.*`).

- [ ] **Step 5: Commit**

```bash
git add src/BlockCache/ephemeral_registry.h src/BlockCache/ephemeral_registry.c src/Actor/message.h src/BlockCache/block_cache.h src/BlockCache/block_cache.c src/Configuration/config.h src/Configuration/config.c CMakeLists.txt src/Platform/platform_file.h src/Platform/platform_file.c test/test_ephemeral.cpp
git commit -m "feat: add actor-owned ephemeral registry with persistent elastic bloom filter"
```

---

### Task 6: Ephemeral put mode in writeable_off_stream + writeable_descriptor

**Files:**
- Modify: `src/OFFStreams/writeable_off_stream.h:39-60` (struct + setter), `src/OFFStreams/writeable_off_stream.c:93-150` (`_create_tuple`), `src/OFFStreams/writeable_off_stream.c:332-374` (CACHE_PUT_RESULT), `src/OFFStreams/writeable_descriptor.h` / `writeable_descriptor.c:100-108` + `:165-204`
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_ephemeral.cpp`:

```cpp
/* ---- ephemeral put through the writeable off stream ---- */

#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/writeable_descriptor.h"
#include "../src/OFFStreams/new_blocks_recipe.h"

typedef struct {
  ATOMIC(uint8_t) done;
} stream_completion_t;

static void stream_completion_dispatch(void* state, message_t* msg) {
  stream_completion_t* cs = (stream_completion_t*)state;
  (void)msg;
  ATOMIC_STORE(&cs->done, 1);
}

TEST_F(TestEphemeralCache, EphemeralPutMarksCreatedBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);

  vec_block_recipe_t recipes;
  vec_init(&recipes);
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, block_cache, standard);
  vec_push(&recipes, (block_recipe_t*)recipe);

  writeable_off_stream_t* ws = writeable_off_stream_create(
      pool, block_cache, NULL, standard, /*tuple_size=*/3, 32, recipes, NULL);
  writeable_off_stream_set_ephemeral(ws, 1);

  stream_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, stream_completion_dispatch, pool);
  stream_once((stream_t*)ws, complete_event, &comp, NULL);

  buffer_t* upload = buffer_create(standard);
  upload->size = standard;   /* one full block of data */
  writeable_off_stream_write(ws, upload);
  writeable_off_stream_finalize(ws);
  buffer_destroy(upload);

  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);

  /* Every block the put created carries exactly one ephemeral claim. */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  size_t ephemeral_blocks = 0;
  for (size_t idx = 0; idx < entries->length; idx++) {
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 1u) << "block " << idx;
    if (entries->data[idx]->ephemeral_count > 0) ephemeral_blocks++;
  }
  EXPECT_EQ(ephemeral_blocks, entries->length);
  for (size_t idx = 0; idx < entries->length; idx++) {
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);

  stream_deferred_deref((stream_t*)ws);
  scheduler_pool_wait_for_idle(pool);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;   /* TearDown must not double-destroy */
}
```

(If `new_blocks_recipe.h` isn't a standalone header, include `../src/OFFStreams/block_recipe.h` which declares it. If `stream_once`'s callback signature needs `(void*, void*)` cast like off_routes.c:1247, cast accordingly.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestEphemeralCache.EphemeralPutMarksCreatedBlocks`
Expected: compile error — `writeable_off_stream_set_ephemeral` undeclared.

- [ ] **Step 3: Implement**

**Claim ownership (critical — exactly one claim per block):** for an ephemeral put B, each block gets exactly ONE claim from exactly ONE owner:
- new_blocks_recipe acquires the claim when it puts a fresh random block (it knows the put is ephemeral),
- the recycler recipe acquires the claim on recycled source blocks it fetches (Task 8),
- the stream acquires the claim only on the off_blocks it creates in `_create_tuple`,
- the descriptor stream acquires the claim on descriptor blocks.
The stream's re-put of `random_blocks` in `_create_tuple` therefore uses the PLAIN put (the recipe already claimed them) — otherwise blocks would be double-claimed and one delete would strand the other claim.

In `src/OFFStreams/block_recipe.h`, add to the base recipe struct (so every recipe subtype carries it):

```c
typedef struct {
  stream_t stream;
  block_cache_t* bc;
  block_size_e block_type;
  uint8_t put_is_ephemeral;  /* consuming put is ephemeral — recipe claims its own outputs */
  uint8_t is_recycler;       /* set by recycler_recipe_create; stream cleanup skips these blocks */
} block_recipe_t;
```

In `writeable_off_stream.h`, add to the struct:

```c
  uint8_t is_ephemeral;   /* ephemeral put: created blocks acquire claims, no network announce */
```

and declare:

```c
void writeable_off_stream_set_ephemeral(writeable_off_stream_t* stream, uint8_t is_ephemeral);
```

In `writeable_off_stream.c`:

```c
void writeable_off_stream_set_ephemeral(writeable_off_stream_t* stream, uint8_t is_ephemeral) {
  stream->is_ephemeral = is_ephemeral;
  /* Recipes must know too: new_blocks_recipe claims its own random blocks
     when the consuming put is ephemeral (one claim per block, see plan). */
  for (int idx = 0; idx < stream->recipes.length; idx++) {
    stream->recipes.data[idx]->put_is_ephemeral = is_ephemeral;
  }
}
```

In `block_recipe.c`, `new_blocks_recipe_dispatch`'s READABLE_PULL case (block_recipe.c:16-45) — claim at the recipe's own put:

```c
      if (recipe->recipe.put_is_ephemeral) {
        block_cache_put_ephemeral(recipe->recipe.bc, block, NULL);
      } else {
        block_cache_put(recipe->recipe.bc, block, 0, NULL);
      }
```

In `_create_tuple` (writeable_off_stream.c:93-150), replace the block-store block (lines 116-121) with:

```c
  /* Store blocks in cache — announce to network if this is a new block.
     random_blocks were already claimed by their recipe when the put is
     ephemeral (new_blocks_recipe claims fresh blocks; the recycler claims
     recycled source blocks in Task 8) — so only the off_block gets its claim
     here. One claim per block, one owner per claim. */
  actor_t* reply_to = &stream->stream.actor;
  for (int i = 0; i < entry->random_blocks.length; i++) {
    block_cache_put(stream->bc, entry->random_blocks.data[i], 0, reply_to);
  }
  if (stream->is_ephemeral) {
    block_cache_put_ephemeral(stream->bc, off_block, reply_to);
  } else {
    block_cache_put(stream->bc, off_block, 0, reply_to);
  }
```

In the `CACHE_PUT_RESULT` handler (writeable_off_stream.c:332-374), gate the announcement:

```c
      if (result->result == CACHE_PUT_NEW && stream->network != NULL && !stream->is_ephemeral) {
```

`writeable_descriptor`: add the same `uint8_t is_ephemeral` field + `writeable_descriptor_set_ephemeral()` setter; in `_build_descriptor_blocks` (writeable_descriptor.c:100-108) switch the `block_cache_put(desc->bc, block, 0, reply_to)` to `block_cache_put_ephemeral` when set; in its `CACHE_PUT_RESULT` handler (writeable_descriptor.c:165-204) gate the announcement with `&& !desc->is_ephemeral`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/writeable_off_stream.h src/OFFStreams/writeable_off_stream.c src/OFFStreams/writeable_descriptor.h src/OFFStreams/writeable_descriptor.c test/test_ephemeral.cpp
git commit -m "feat: ephemeral put mode acquires claims and suppresses network announcement"
```

---

### Task 7: Thread ephemeral flag through put entry points + registry ADD on completion

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c:1265-1401` (buffered put), `off_routes.c:1479-1609` (streaming put), `src/ClientAPI/Unix/unix_connection.c:648-762` (`_unix_handle_put`), `src/ClientAPI/TCP/tcp_connection.c:811` area, `src/ClientAPI/WebSocket/ws_connection.c:959`/`1092` area, `src/ClientAPI/WebTransport/wt_connection.c:414`/`536` area, `src/ClientAPI/WebTransport/webtransport_h3.c:894` area
- Test: covered by Task 6's stream test + existing HTTP server tests

- [ ] **Step 1: HTTP buffered path**

In `_off_put_handler` (off_routes.c:1265-1401), after `writeable_off_stream_create(...)` and `writeable_descriptor_create(...)` (before the first `writeable_off_stream_write`), add:

```c
    if (is_temporary) {
        writeable_off_stream_set_ephemeral(ws, 1);
        writeable_descriptor_set_ephemeral(desc, 1);
    }
```

In `_put_on_descriptor_close` (off_routes.c:1074-1135), after `url->descriptor_hash = buffer_copy(put_ctx->descriptor_hash);`, add the registry ADD:

```c
    if (put_ctx->temporary && put_ctx->descriptor_hash != NULL && put_ctx->bc->registry != NULL) {
        ephemeral_registry_add(put_ctx->bc->registry, put_ctx->descriptor_hash);
    }
```

(Add `#include "../BlockCache/ephemeral_registry.h"` to off_routes.c.)

- [ ] **Step 2: HTTP streaming path**

Same two edits in `_off_put_headers_complete` (off_routes.c:1479-1609): after `put_ctx->temporary = is_temporary;` (line 1591) the setters go right after the stream/descriptor creation lines; the registry ADD goes in `_put_on_descriptor_close` (shared, already done).

- [ ] **Step 3: Unix/TCP/WS/WT put handlers**

In each daemon-side put handler that decodes `client_api_put_request_t msg` and creates `ws`/`desc` (unix_connection.c `_unix_handle_put` ~648-762; tcp_connection.c ~811; ws_connection.c ~1092; wt_connection.c ~536; webtransport_h3.c ~894), after `writeable_off_stream_create(...)`/`writeable_descriptor_create(...)` add:

```c
  if (msg.temporary) {
    writeable_off_stream_set_ephemeral(ws, 1);
    writeable_descriptor_set_ephemeral(desc, 1);
  }
```

and where the handler has the connection's `bc` and completes with a descriptor hash (the response-send site where the ORI string is built), add the same `ephemeral_registry_add(bc->registry, descriptor_hash)` guarded by `msg.temporary`. Each handler differs slightly in where the descriptor hash lives — grep each file for where it builds the PUT response to find the exact spot.

- [ ] **Step 4: Build + run full test suite + existing HTTP tests**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*:TestBlockCache*:TestHttpServer*'`
Expected: PASS (temporary flag now live end-to-end on HTTP; the wire flag consumption added here covers the C/JS client's `temporary` option).

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/HTTP/off_routes.c src/ClientAPI/Unix/unix_connection.c src/ClientAPI/TCP/tcp_connection.c src/ClientAPI/WebSocket/ws_connection.c src/ClientAPI/WebTransport/wt_connection.c src/ClientAPI/WebTransport/webtransport_h3.c
git commit -m "feat: thread ephemeral put flag through HTTP and wire transports"
```

---

### Task 8: Recycler enforcement (fetch-time exact check, commit/propagate modes, self-heal)

**Files:**
- Modify: `src/OFFStreams/block_recipe.h` (recycler struct + create signature), `src/OFFStreams/block_recipe.c` (`recycler_recipe_dispatch` data branch ~350-390 and NETWORK_FIND_BLOCK_RESULT branch ~392-456, `_start_descriptor_load` ~102-128, `recycler_recipe_create` ~472-501), `src/ClientAPI/HTTP/off_routes.c` (recycler creation + `recycle-ephemeral` header), `src/ClientAPI/client_api_wire.h`/`.c` (put request index 9)
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Design recap for the implementer**

- Recyclers check `EPHEMERAL_REGISTRY_CHECK` (advisory) per source descriptor hash and enforce **exactly at data-block fetch time** by peeking the block's `ephemeral_count` in the index (reading the index from another actor via `index_peek` is established practice — `network_handle_find_block` does it).
- Non-ephemeral put, no override (`RECYCLE_EPHEMERAL_NONE`): discovering an ephemeral block → deactivate the recipe with an error, heal-ADD the source descriptor hash to the registry.
- Non-ephemeral put, `RECYCLE_EPHEMERAL_COMMIT`: CLEAR the discovered blocks (announce via `NETWORK_LOCAL_STORE_BLOCK` when network is present).
- Ephemeral put, or `RECYCLE_EPHEMERAL_PROPAGATE`: ACQUIRE a claim on the discovered block and track the hash for failure rollback.
- The CHECK result only sets an advisory flag + `log_warn` — never errors on its own (bloom false positives are resolved by the exact fetch-time check).

- [ ] **Step 2: Write the failing tests**

Append to `test/test_ephemeral.cpp`. First a reusable helper that puts one ephemeral representation and returns its descriptor hash (Task 10's tests reuse it):

```cpp
/* ---- shared helper: put one ephemeral representation, return descriptor hash ---- */

#include "../src/OFFStreams/block_recipe.h"
#include "../src/OFFStreams/ori.h"
#include "../src/OFFStreams/writeable_descriptor.h"

typedef struct {
  buffer_t* descriptor_hash;
  void* desc_handle;          /* the writeable_descriptor_t the tuples feed into */
  ATOMIC(uint8_t) done;
} rep_put_context_t;

static void _test_capture_descriptor_hash(void* ctx, void* data) {
  rep_put_context_t* put_ctx = (rep_put_context_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (put_ctx->descriptor_hash != NULL) {
    buffer_destroy(put_ctx->descriptor_hash);
  }
  put_ctx->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
}

static void _test_capture_tuple(void* ctx, void* data) {
  rep_put_context_t* put_ctx = (rep_put_context_t*)ctx;
  tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)data);
  writeable_descriptor_write((writeable_descriptor_t*)put_ctx->desc_handle, tuple);
  tuple_destroy(tuple);
}

static void _test_rep_put_close(void* ctx, void* unused) {
  (void)unused;
  ATOMIC_STORE(&((rep_put_context_t*)ctx)->done, 1);
}

static buffer_t* _test_put_ephemeral(block_cache_t* bc, scheduler_pool_t* pool,
                                     size_t data_size) {
  rep_put_context_t put_ctx;
  memset(&put_ctx, 0, sizeof(put_ctx));

  writeable_descriptor_t* desc = writeable_descriptor_create(pool, bc, standard, 32, 3, data_size, NULL);
  writeable_descriptor_set_ephemeral(desc, 1);
  put_ctx.desc_handle = desc;
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  vec_push(&recipes, (block_recipe_t*)new_blocks_recipe_create(pool, bc, standard));
  writeable_off_stream_t* ws = writeable_off_stream_create(pool, bc, NULL, standard, 3, 32, recipes, NULL);
  writeable_off_stream_set_ephemeral(ws, 1);

  stream_subscribe((stream_t*)ws, data_event, &put_ctx, (void (*)(void*, void*))_test_capture_tuple, NULL);
  stream_once((stream_t*)desc, close_event, &put_ctx, (void (*)(void*, void*))_test_rep_put_close, NULL);

  buffer_t* upload = buffer_create(data_size);
  upload->size = data_size;
  writeable_off_stream_write(ws, upload);
  writeable_off_stream_finalize(ws);
  buffer_destroy(upload);

  while (!ATOMIC_LOAD(&put_ctx.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  stream_deferred_deref((stream_t*)ws);
  stream_deferred_deref((stream_t*)desc);
  return put_ctx.descriptor_hash;   /* caller owns the reference */
}
```

Then the recycler tests:

```cpp
/* ---- recycler enforcement ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  ATOMIC(uint8_t) errored;
} recipe_watch_t;

static void recipe_error_watch(void* ctx, void* error) {
  recipe_watch_t* watch = (recipe_watch_t*)ctx;
  (void)error;
  ATOMIC_STORE(&watch->errored, 1);
  ATOMIC_STORE(&watch->done, 1);
}

static void recipe_close_watch(void* ctx, void* unused) {
  (void)unused;
  ATOMIC_STORE(&((recipe_watch_t*)ctx)->done, 1);
}

static ori_t* _test_ori_for(buffer_t* descriptor_hash) {
  ori_t* source_ori = ori_create(standard);
  source_ori->descriptor_hash = buffer_copy(descriptor_hash);
  source_ori->block_type = standard;
  source_ori->tuple_size = 3;
  return source_ori;
}

TEST_F(TestEphemeralCache, RecyclerRejectsEphemeralSourceOnPermanentPut) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);

  /* Non-ephemeral put recycling ephemeral A → recipe must error. */
  vec_ori_t oris;
  vec_init(&oris);
  vec_push(&oris, _test_ori_for(descriptor_hash));

  vec_block_recipe_t recipes;
  vec_init(&recipes);
  recycler_recipe_t* recycler = recycler_recipe_create(
      pool, block_cache, standard, oris, NULL,
      /*put_is_ephemeral=*/0, RECYCLE_EPHEMERAL_NONE);
  vec_push(&recipes, (block_recipe_t*)recycler);

  recipe_watch_t watch;
  memset(&watch, 0, sizeof(watch));
  stream_subscribe((stream_t*)recycler, error_event, &watch, recipe_error_watch, NULL);
  stream_once((stream_t*)recycler, close_event, &watch, recipe_close_watch, NULL);

  recycler_recipe_pull(recycler);
  while (!ATOMIC_LOAD(&watch.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(ATOMIC_LOAD(&watch.errored), 1);

  stream_deferred_deref((stream_t*)recycler);
  scheduler_pool_wait_for_idle(pool);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

TEST_F(TestEphemeralCache, RecyclerPropagatesForEphemeralPut) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);

  /* Ephemeral put recycling ephemeral A → accepted; A's blocks gain B's claim
     (count 2: one from A, one from the propagating recycle). */
  vec_ori_t oris;
  vec_init(&oris);
  vec_push(&oris, _test_ori_for(descriptor_hash));

  vec_block_recipe_t recipes;
  vec_init(&recipes);
  recycler_recipe_t* recycler = recycler_recipe_create(
      pool, block_cache, standard, oris, NULL,
      /*put_is_ephemeral=*/1, RECYCLE_EPHEMERAL_NONE);
  vec_push(&recipes, (block_recipe_t*)recycler);

  recipe_watch_t watch;
  memset(&watch, 0, sizeof(watch));
  stream_subscribe((stream_t*)recycler, error_event, &watch, recipe_error_watch, NULL);
  stream_once((stream_t*)recycler, close_event, &watch, recipe_close_watch, NULL);

  recycler_recipe_pull(recycler);
  while (!ATOMIC_LOAD(&watch.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(ATOMIC_LOAD(&watch.errored), 0);

  /* A's data blocks now hold two claims. The cache contains A's blocks only
     (B's stream never got data), so every non-descriptor entry that A owns
     has count >= 1; the recycled ones specifically reach 2. Check that at
     least one block has count 2 — the propagated claim was acquired. */
  bool saw_propagated_claim = false;
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  for (size_t idx = 0; idx < entries->length; idx++) {
    if (entries->data[idx]->ephemeral_count >= 2) saw_propagated_claim = true;
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);
  EXPECT_TRUE(saw_propagated_claim);

  stream_deferred_deref((stream_t*)recycler);
  scheduler_pool_wait_for_idle(pool);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}
```

(If `error_event`/`close_event` subscription on a recipe stream needs the `stream_once` vs `stream_subscribe` semantics adjusted — recipes are `readable_stream`s — mirror how `writeable_off_stream.c:_register_recipe` subscribes to its recipes (grep `_register_recipe`) and use the same calls.)

Also add the mode-stability unit test (the modes ride the wire at put-request index 9, Task 8 Step 3):

```cpp
TEST(TestRecyclerModes, ModeEnumDefaults) {
  EXPECT_EQ((int)RECYCLE_EPHEMERAL_NONE, 0);
  EXPECT_EQ((int)RECYCLE_EPHEMERAL_COMMIT, 1);
  EXPECT_EQ((int)RECYCLE_EPHEMERAL_PROPAGATE, 2);
}
```

- [ ] **Step 3: Implement**

In `src/OFFStreams/block_recipe.h`, add to the header (before `recycler_recipe_t`):

```c
typedef enum {
  RECYCLE_EPHEMERAL_NONE = 0,      /* default: error on ephemeral source */
  RECYCLE_EPHEMERAL_COMMIT = 1,     /* clear source blocks to permanent + announce */
  RECYCLE_EPHEMERAL_PROPAGATE = 2   /* acquire claims; new rep may stay ephemeral */
} recycle_ephemeral_e;
```

Extend `recycler_recipe_t` with:

```c
  recycle_ephemeral_e override_mode;
  uint8_t source_flagged;        /* registry CHECK said source may be ephemeral */
  vec_buffer_t acquired_hashes; /* claims this recipe acquired (propagate) */
```

(`put_is_ephemeral` and `is_recycler` live on the base `block_recipe_t`, added in Task 6 — the recycler reads `recipe->recipe.put_is_ephemeral` and `recycler_recipe_create` sets `recipe->recipe.is_recycler = 1;`.)

Change `recycler_recipe_create` to:

```c
recycler_recipe_t* recycler_recipe_create(
    scheduler_pool_t* pool, block_cache_t* bc, block_size_e block_type,
    vec_ori_t oris, network_t* network,
    uint8_t put_is_ephemeral, recycle_ephemeral_e override_mode);
```

(Update the existing call sites — grep `recycler_recipe_create`: off_routes.c buffered + streaming handlers. Others pass `0, RECYCLE_EPHEMERAL_NONE`.)

In `recycler_recipe_create` (block_recipe.c:472-501) initialize the new fields (`recipe->recipe.is_recycler = 1; recipe->recipe.put_is_ephemeral = put_is_ephemeral; recipe->override_mode = override_mode; vec_init(&recipe->acquired_hashes);`), and in `recycler_recipe_destroy` release acquired hashes (`DESTROY` each + `vec_deinit`).

In `_start_descriptor_load` (block_recipe.c:102-128), before the `block_cache_get(...)` of the descriptor, send the advisory CHECK (fire-and-forget reply to the recipe's own actor):

```c
  if (recipe->recipe.bc->registry != NULL) {
    ephemeral_registry_check(recipe->recipe.bc->registry, current_ori->descriptor_hash,
                             &recipe->recipe.stream.actor);
  }
```

Add a dispatch case for `EPHEMERAL_REGISTRY_CHECK_RESULT` in `recycler_recipe_dispatch`:

```c
    case EPHEMERAL_REGISTRY_CHECK_RESULT: {
      ephemeral_registry_check_result_payload_t* result =
          (ephemeral_registry_check_result_payload_t*)msg->payload;
      if (result->present) {
        recipe->source_flagged = 1;
        log_warn("recycler: source flagged ephemeral by registry — enforcing at fetch time");
      }
      break;
    }
```

In the data-block branch of `CACHE_GET_RESULT` (block_recipe.c:350-390) — and identically in the found path of `NETWORK_FIND_BLOCK_RESULT` (block_recipe.c:392-456) — insert enforcement right before `stream_notify((stream_t*)recipe, data_event, ...)`:

```c
      if (result->block != NULL) {
        index_entry_t* entry = index_peek(recipe->recipe.bc->index, result->block->hash);
        if (entry != NULL && entry->ephemeral_count > 0) {
          /* Discovered an ephemeral block. Self-heal the registry for this source. */
          if (recipe->recipe.bc->registry != NULL) {
            ori_t* source_ori = recipe->oris.data[recipe->ori_index];
            if (source_ori->descriptor_hash != NULL) {
              ephemeral_registry_add(recipe->recipe.bc->registry, source_ori->descriptor_hash);
            }
          }
          if (recipe->recipe.put_is_ephemeral || recipe->override_mode == RECYCLE_EPHEMERAL_PROPAGATE) {
            /* Propagate: acquire the consuming representation's claim. */
            block_cache_ephemeral(recipe->recipe.bc, result->block->hash,
                                  CACHE_EPHEMERAL_ACQUIRE, NULL);
            vec_push(&recipe->acquired_hashes,
                     (buffer_t*)refcounter_reference((refcounter_t*)result->block->hash));
          } else if (recipe->override_mode == RECYCLE_EPHEMERAL_COMMIT) {
            /* Commit: clear the source blocks to permanent and announce them. */
            block_cache_ephemeral(recipe->recipe.bc, result->block->hash,
                                  CACHE_EPHEMERAL_CLEAR, NULL);
            if (recipe->network != NULL) {
              network_local_store_block_payload_t* store_payload =
                  get_clear_memory(sizeof(network_local_store_block_payload_t));
              store_payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)result->block->hash);
              store_payload->fib = entry->counter.fib > 0 ? entry->counter.fib : 1;
              store_payload->reply_to = NULL;
              message_t store_msg;
              store_msg.type = NETWORK_LOCAL_STORE_BLOCK;
              store_msg.payload = store_payload;
              store_msg.payload_destroy = network_local_store_block_payload_destroy;
              actor_send(&recipe->network->actor, &store_msg);
            }
          } else {
            /* Default: hard error — unverified data must not enter a permanent representation. */
            if (result->block != NULL) DESTROY(result->block, block);
            if (result->hash != NULL) DESTROY(result->hash, buffer);
            stream_deactivate((stream_t*)recipe, OFFS_ERROR("recycle source is ephemeral/unverified"));
            recipe->recipe.stream.is_deactivated = 1;
            break;
          }
        }
      }
```

`result->hash` vs `result->block->hash`: `cache_get_result_payload_t` carries the requested hash in `result->hash` and the block (with its own `->hash`) in `result->block`. Prefer `result->hash` for the peek/ACQUIRE key and `result->block` for the notify — they are the same; use `result->hash` where non-NULL, else fall back to `result->block->hash`.

Release rollback on failure: in `recycler_recipe_destroy`, before freeing, send RELEASE for each acquired hash:

```c
  for (size_t idx = 0; idx < recipe->acquired_hashes.length; idx++) {
    block_cache_ephemeral(recipe->recipe.bc, recipe->acquired_hashes.data[idx],
                          CACHE_EPHEMERAL_RELEASE, NULL);
    DESTROY(recipe->acquired_hashes.data[idx], buffer);
  }
  vec_deinit(&recipe->acquired_hashes);
```

(On the error path the recipe's deferred destroy runs after the stream tears down — the cache actor processes the RELEASEs; blocks reaching 0 are deleted. This is the failure-cleanup path for acquired claims.)

Put options threading: in `off_routes.c` `_off_put_handler` and `_off_put_headers_complete`, parse the new header:

```c
    const char* recycle_ephemeral_header = http_request_header(request, "recycle-ephemeral");
    recycle_ephemeral_e recycle_mode = RECYCLE_EPHEMERAL_NONE;
    if (recycle_ephemeral_header != NULL) {
        if (strcmp(recycle_ephemeral_header, "commit") == 0) {
            recycle_mode = RECYCLE_EPHEMERAL_COMMIT;
        } else if (strcmp(recycle_ephemeral_header, "propagate") == 0) {
            recycle_mode = RECYCLE_EPHEMERAL_PROPAGATE;
        }
    }
```

and pass to `recycler_recipe_create(ctx->pool, ctx->bc, standard, recycler_oris, NULL, is_temporary, recycle_mode)`.

Wire: `client_api_put_request_t` gains `uint8_t recycle_ephemeral;` (0/1/2). Encode at optional index 9 (after tuple_size) — `cbor_new_definite_array(msg->has_tuple_size ? 9 : 8)` becomes `(... + (msg->recycle_ephemeral ? 1 : 0))`; decode: `if (cbor_array_size(item) >= 10) { ... msg->recycle_ephemeral = cbor_get_uint8(...); }`. Daemon put handlers pass it to `recycler_recipe_create` where they build recyclers (only off_routes currently builds recyclers; the wire transports get recycler support by passing `msg.recycle_ephemeral` through when they add recycler headers — out of scope here, the HTTP path carries it).

- [ ] **Step 4: Build and run**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*:TestRecyclerModes*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/block_recipe.h src/OFFStreams/block_recipe.c src/ClientAPI/HTTP/off_routes.c src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c test/test_ephemeral.cpp
git commit -m "feat: recycler ephemeral enforcement with commit/propagate modes and self-heal"
```

---

### Task 9: Failure cleanup of created blocks

**Files:**
- Modify: `src/OFFStreams/writeable_off_stream.c` (created-hash tracking + release on error/deactivate)
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Design recap**

The ephemeral put tracks the hash of every block it creates (randoms, off_blocks — NOT blocks received from recipes). If the put fails mid-stream (CACHE_PUT_ERROR/FULL, error_event, deactivation before completion), the cleanup RELEASEs each created block — its only claim is the failed put's, so it reaches 0 and is deleted. If cleanup cannot run (crash), operators find orphans via list-ephemerals (Task 12/13).

- [ ] **Step 2: Write the failing test**

```cpp
TEST_F(TestEphemeralCache, FailedEphemeralPutCleansUpCreatedBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  config_t tiny_config = config;
  (void)tiny_config;

  vec_block_recipe_t recipes;
  vec_init(&recipes);
  vec_push(&recipes, (block_recipe_t*)new_blocks_recipe_create(pool, block_cache, standard));
  writeable_off_stream_t* ws = writeable_off_stream_create(
      pool, block_cache, NULL, standard, 3, 32, recipes, NULL);
  writeable_off_stream_set_ephemeral(ws, 1);

  /* Simulate failure mid-stream: write data, then force an error on the
     stream and let the deferred destroy run cleanup. */
  stream_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, stream_completion_dispatch, pool);
  stream_once((stream_t*)ws, close_event, &comp, NULL);

  buffer_t* upload = buffer_create(standard);
  upload->size = standard;
  writeable_off_stream_write(ws, upload);
  buffer_destroy(upload);

  /* Fail the stream: deactivate with an error, like a CACHE_PUT_FULL would. */
  stream_deactivate((stream_t*)ws, OFFS_ERROR("simulated failure"));
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);

  /* All blocks created by the failed put must be gone. */
  EXPECT_EQ(block_cache_count(block_cache), 0u);

  stream_deferred_deref((stream_t*)ws);
  scheduler_pool_wait_for_idle(pool);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
}
```

(If `stream_deactivate` with an error from an external actor is not usable this way, drive the failure through a tiny `max_capacity_bytes` block_cache so a real `CACHE_PUT_FULL` fires — create the block_cache with `max_capacity_bytes = standard` so the second block put fails. Either approach is acceptable; prefer the capacity one if it exercises the real error path more faithfully.)

- [ ] **Step 3: Implement**

In `writeable_off_stream_t` (writeable_off_stream.h) add:

```c
  vec_buffer_t created_hashes;   /* blocks this put created (ephemeral mode): random blocks
                                    from non-recycler recipes + off_blocks. Recycler-delivered
                                    blocks are EXCLUDED — the recycler rolls back its own
                                    acquired claims (Task 8), so tracking them here too
                                    would double-release. */
```

`vec_init(&stream->created_hashes);` in create; in destroy, deref all entries + deinit.

Recording happens where blocks attach to a tuple entry, not in `_create_tuple`: in `_get_random_blocks` (or wherever `entry->random_blocks` is populated from `stream->current_recipe`), when `stream->is_ephemeral && !stream->current_recipe->is_recycler`, push the block's hash; and in `_create_tuple`, when `stream->is_ephemeral`, push the off_block's hash:

```c
  /* in the random-block attach path, per attached block: */
  if (stream->is_ephemeral && !stream->current_recipe->is_recycler) {
    vec_push(&stream->created_hashes,
             (buffer_t*)refcounter_reference((refcounter_t*)block->hash));
  }
  /* in _create_tuple, after creating off_block: */
  if (stream->is_ephemeral) {
    vec_push(&stream->created_hashes,
             (buffer_t*)refcounter_reference((refcounter_t*)off_block->hash));
  }
```

(Grep `writeable_off_stream.c` for where `vec_push(&entry->random_blocks, ...)` happens to find the exact attach point — the recipe-delivered blocks land there via the recipe's data events.)

Add the cleanup helper and call it from the destroy path (a stream destroy with `is_deactivated && !completed`) and from the `CACHE_PUT_RESULT` error branch before `stream_notify(error_event, ...)`:

```c
static void _release_ephemeral_claims(writeable_off_stream_t* stream) {
  if (!stream->is_ephemeral) return;
  for (size_t idx = 0; idx < stream->created_hashes.length; idx++) {
    block_cache_ephemeral(stream->bc, stream->created_hashes.data[idx],
                          CACHE_EPHEMERAL_RELEASE, NULL);
    DESTROY(stream->created_hashes.data[idx], buffer);
  }
  stream->created_hashes.length = 0;
}
```

Call it: (a) in the `CACHE_PUT_RESULT` error branch (before the first error notify, guarded by `if (!stream->stream.is_deactivated)` alongside the existing guard), and (b) in `writeable_off_stream_destroy` when `stream->is_ephemeral && stream->stream.is_deactivated` and the stream never fired `complete_event` (add a `uint8_t completed;` flag set in `_create_tuple`'s `is_final` branch and in `_maybe_finalize`'s completion path — check where `finished_event` fires and set it there).

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/writeable_off_stream.h src/OFFStreams/writeable_off_stream.c test/test_ephemeral.cpp
git commit -m "feat: ephemeral put failure cleanup releases created block claims"
```

---

### Task 10: Representation actor (mark-permanent / delete-ephemeral / pin / unpin)

**Files:**
- Create: `src/OFFStreams/representation_actor.h`, `src/OFFStreams/representation_actor.c`
- Modify: root `CMakeLists.txt` (add the source)
- Test: `test/test_ephemeral.cpp`

- [ ] **Step 1: Design recap**

The actor walks a representation's descriptor chain exactly like `recycler_recipe._process_descriptor_block` (data hashes at `descriptor_pad` (32-byte) slices up to `cut_point`, next descriptor hash in the last `descriptor_pad` bytes), issuing block-level ops for every hash, then replies to `reply_to` with a summary. Mark-permanent additionally announces each block whose `previous_count > 0` (was ephemeral) to the network, and sends `EPHEMERAL_REGISTRY_REMOVE` for the representation's descriptor hash. Delete-ephemeral uses RELEASE (blocks deleted at 0; pinned blocks are NOT protected while ephemeral). Pin/unpin apply to every block in the representation.

- [ ] **Step 2: Write the failing test**

Append to `test/test_ephemeral.cpp`:

```cpp
/* ---- representation actor ops ---- */

#include "../src/OFFStreams/representation_actor.h"

typedef struct {
  ATOMIC(uint8_t) done;
  int result;
  size_t blocks_touched;
} rep_completion_t;

static void rep_completion_dispatch(void* state, message_t* msg) {
  rep_completion_t* cs = (rep_completion_t*)state;
  if (msg->type == REPRESENTATION_OP_RESULT) {
    representation_op_result_payload_t* result =
        (representation_op_result_payload_t*)msg->payload;
    cs->result = result->result;
    cs->blocks_touched = result->blocks_touched;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* _test_put_ephemeral (with its rep_put_context_t and capture helpers) was
   added to this file in Task 8 — reuse it directly here. */

TEST_F(TestEphemeralCache, MarkPermanentClearsClaimsAndKeepsBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  size_t entries_before = block_cache_count(block_cache);
  EXPECT_GT(entries_before, 0u);

  rep_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, rep_completion_dispatch, pool);
  representation_actor_t* rep = representation_mark_permanent(block_cache, NULL, descriptor_hash, &comp);
  ASSERT_NE(rep, nullptr);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(cs.result, 0);
  EXPECT_GT(cs.blocks_touched, 0u);

  /* Every block survives, now permanent. */
  EXPECT_EQ(block_cache_count(block_cache), entries_before);
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  for (size_t idx = 0; idx < entries->length; idx++) {
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 0u);
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);

  representation_actor_destroy(rep);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

TEST_F(TestEphemeralCache, DeleteEphemeralRemovesOnlyClaimedBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  size_t entries_before = block_cache_count(block_cache);

  rep_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, rep_completion_dispatch, pool);
  representation_actor_t* rep = representation_delete_ephemeral(block_cache, descriptor_hash, &comp);
  ASSERT_NE(rep, nullptr);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(cs.result, 0);

  /* All blocks were exclusively claimed by this representation → all deleted. */
  EXPECT_EQ(block_cache_count(block_cache), 0u);
  representation_actor_destroy(rep);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}
```

Declare the tiny static helpers above the tests in the exact order the compiler needs (forward-declare `_test_rep_put_close` and `_test_capture_tuple` before `_test_put_ephemeral`).

- [ ] **Step 3: Implement**

`src/OFFStreams/representation_actor.h`:

```c
#ifndef OFFS_REPRESENTATION_ACTOR_H
#define OFFS_REPRESENTATION_ACTOR_H

#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../BlockCache/block_cache.h"
#include "../Buffer/buffer.h"
#include "../Scheduler/scheduler.h"
#include "../Network/network.h"   /* network_t forward-declared in message.h; include if needed */
#include <stdint.h>

typedef enum {
  REPRESENTATION_OP_MARK_PERMANENT = 0,
  REPRESENTATION_OP_DELETE_EPHEMERAL = 1,
  REPRESENTATION_OP_PIN = 2,
  REPRESENTATION_OP_UNPIN = 3
} representation_op_e;

/* Result message payload for REPRESENTATION_OP_RESULT */
typedef struct {
  int result;             /* 0 = ok, -1 = descriptor missing, -2 = walk error */
  size_t blocks_touched;  /* number of blocks the op was applied to */
  actor_t* reply_to;
} representation_op_result_payload_t;

typedef struct representation_actor_t {
  actor_t actor;
  block_cache_t* bc;
  network_t* network;          /* NULL = local-only; announce skipped */
  buffer_t* descriptor_hash;   /* the entry descriptor block being walked */
  buffer_t* next_descriptor_hash;
  representation_op_e op;
  actor_t* reply_to;
  size_t blocks_touched;
  size_t outstanding_ops;      /* CACHE_EPHEMERAL/PIN results not yet returned */
  uint8_t walk_done;
  uint8_t ops_done;
} representation_actor_t;

representation_actor_t* representation_actor_create(block_cache_t* bc, network_t* network,
                                                     buffer_t* descriptor_hash,
                                                     representation_op_e op, actor_t* reply_to);
void representation_actor_destroy(representation_actor_t* actor);
void representation_actor_dispatch(void* state, message_t* msg);

/* Convenience constructors */
representation_actor_t* representation_mark_permanent(block_cache_t* bc, network_t* network,
                                                      buffer_t* descriptor_hash, actor_t* reply_to);
representation_actor_t* representation_delete_ephemeral(block_cache_t* bc, buffer_t* descriptor_hash,
                                                        actor_t* reply_to);
representation_actor_t* representation_pin(block_cache_t* bc, buffer_t* descriptor_hash,
                                           actor_t* reply_to);
representation_actor_t* representation_unpin(block_cache_t* bc, buffer_t* descriptor_hash,
                                            actor_t* reply_to);

#endif // OFFS_REPRESENTATION_ACTOR_H
```

Add `REPRESENTATION_OP_RESULT` to the end of the `message_type_e` enum in `src/Actor/message.h`.

`src/OFFStreams/representation_actor.c` — the walk mirrors `block_recipe.c:133-189` (`_process_descriptor_block`) with `descriptor_pad = 32`, `cut_point = (block_size / 32) * 32`:

```c
#include "representation_actor.h"
#include "../Util/allocator.h"
#include "../Util/error.h"

static void _rep_maybe_finish(representation_actor_t* actor) {
  if (actor->walk_done && actor->outstanding_ops == 0 && actor->reply_to != NULL) {
    representation_op_result_payload_t* result =
        get_clear_memory(sizeof(representation_op_result_payload_t));
    result->result = 0;
    result->blocks_touched = actor->blocks_touched;
    result->reply_to = NULL;
    message_t reply;
    reply.type = REPRESENTATION_OP_RESULT;
    reply.payload = result;
    reply.payload_destroy = free;
    actor_send(actor->reply_to, &reply);
    actor->reply_to = NULL;  /* reply exactly once */
  }
}

static void _rep_apply_to_hash(representation_actor_t* actor, buffer_t* hash) {
  switch (actor->op) {
    case REPRESENTATION_OP_MARK_PERMANENT:
      actor->outstanding_ops++;
      block_cache_ephemeral(actor->bc, hash, CACHE_EPHEMERAL_CLEAR, &actor->actor);
      break;
    case REPRESENTATION_OP_DELETE_EPHEMERAL:
      /* RELEASE only touches ephemeral blocks; permanent blocks unaffected. */
      actor->outstanding_ops++;
      block_cache_ephemeral(actor->bc, hash, CACHE_EPHEMERAL_RELEASE, &actor->actor);
      break;
    case REPRESENTATION_OP_PIN:
      actor->outstanding_ops++;
      block_cache_pin(actor->bc, hash, &actor->actor);
      break;
    case REPRESENTATION_OP_UNPIN:
      actor->outstanding_ops++;
      block_cache_unpin(actor->bc, hash, &actor->actor);
      break;
  }
  actor->blocks_touched++;
}

static void _rep_process_descriptor_block(representation_actor_t* actor, buffer_t* block_data) {
  /* Extract data hashes at descriptor_pad(=32) slices up to cut_point; the last
     32 bytes are the next descriptor hash. Mirrors the recycler's walk. */
  size_t descriptor_pad = 32;
  size_t block_size = block_data->size;
  size_t cut_point = (block_size / descriptor_pad) * descriptor_pad;
  size_t data_end = cut_point > 0 ? cut_point - descriptor_pad : 0;
  for (size_t offset = 0; offset + descriptor_pad <= data_end; offset += descriptor_pad) {
    if (block_data->data[offset] == 0) continue;  /* zero-padded slot */
    buffer_t* hash = buffer_create(descriptor_pad);
    memcpy(hash->data, block_data->data + offset, descriptor_pad);
    hash->size = descriptor_pad;
    _rep_apply_to_hash(actor, hash);
    buffer_destroy(hash);
  }
  size_t next_start = block_size - descriptor_pad;
  if (next_start >= data_end) {
    uint8_t all_zero = 1;
    for (size_t idx = 0; idx < descriptor_pad; idx++) {
      if (block_data->data[next_start + idx] != 0) { all_zero = 0; break; }
    }
    if (!all_zero) {
      actor->next_descriptor_hash = buffer_create(descriptor_pad);
      memcpy(actor->next_descriptor_hash->data, block_data->data + next_start, descriptor_pad);
      actor->next_descriptor_hash->size = descriptor_pad;
    }
  }
}

void representation_actor_dispatch(void* state, message_t* msg) {
  representation_actor_t* actor = (representation_actor_t*)state;
  if (actor == NULL) return;
  switch (msg->type) {
    case READABLE_PULL: {
      /* Kick off the walk: fetch the first descriptor block. */
      block_cache_get(actor->bc, actor->descriptor_hash, &actor->actor);
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      if (result->block == NULL) {
        if (actor->reply_to != NULL) {
          representation_op_result_payload_t* payload =
              get_clear_memory(sizeof(representation_op_result_payload_t));
          payload->result = -1;   /* descriptor block missing */
          payload->blocks_touched = actor->blocks_touched;
          payload->reply_to = NULL;
          message_t reply;
          reply.type = REPRESENTATION_OP_RESULT;
          reply.payload = payload;
          reply.payload_destroy = free;
          actor_send(actor->reply_to, &reply);
          actor->reply_to = NULL;
        }
        if (result->hash != NULL) DESTROY(result->hash, buffer);
        actor->walk_done = 1;
        _rep_maybe_finish(actor);
        break;
      }
      _rep_process_descriptor_block(actor, result->block->data);
      if (actor->op == REPRESENTATION_OP_MARK_PERMANENT ||
          actor->op == REPRESENTATION_OP_DELETE_EPHEMERAL) {
        /* The descriptor blocks themselves are part of the representation. */
        _rep_apply_to_hash(actor, result->block->hash);
      }
      DESTROY(result->block, block);
      if (result->hash != NULL) DESTROY(result->hash, buffer);
      if (actor->next_descriptor_hash != NULL) {
        buffer_t* next = actor->next_descriptor_hash;
        actor->next_descriptor_hash = NULL;
        block_cache_get(actor->bc, next, &actor->actor);
        buffer_destroy(next);
      } else {
        actor->walk_done = 1;
        _rep_maybe_finish(actor);
      }
      break;
    }
    case CACHE_EPHEMERAL_RESULT: {
      cache_ephemeral_result_payload_t* result = (cache_ephemeral_result_payload_t*)msg->payload;
      if (result->result == CACHE_EPHEMERAL_OK &&
          actor->op == REPRESENTATION_OP_MARK_PERMANENT &&
          result->previous_count > 0 && actor->network != NULL) {
        /* Newly-committed block — announce it like a put would. */
        network_local_store_block_payload_t* store_payload =
            get_clear_memory(sizeof(network_local_store_block_payload_t));
        store_payload->hash = NULL;  /* hash not carried in the result — see note below */
        (void)store_payload;
      }
      actor->outstanding_ops--;
      _rep_maybe_finish(actor);
      break;
    }
    case CACHE_PIN_RESULT:
    case CACHE_UNPIN_RESULT: {
      actor->outstanding_ops--;
      _rep_maybe_finish(actor);
      break;
    }
    default:
      break;
  }
}
```

**Note on announce (implementer must handle):** the `CACHE_EPHEMERAL_RESULT` payload does not carry the block hash. Extend `cache_ephemeral_result_payload_t` with a `buffer_t* hash` (referenced from the request payload in the dispatch case; NULL when `reply_to` is NULL) so the mark-permanent path can announce; the sender destroys it via a dedicated `cache_ephemeral_result_payload_destroy`. Add that field in this task (update the Task 3 dispatch code accordingly — it's a strict addition), then the announce code becomes:

```c
        network_local_store_block_payload_t* store_payload =
            get_clear_memory(sizeof(network_local_store_block_payload_t));
        store_payload->hash = (buffer_t*)refcounter_reference((refcounter_t*)result->hash);
        store_payload->fib = 1;
        store_payload->reply_to = NULL;
        message_t store_msg;
        store_msg.type = NETWORK_LOCAL_STORE_BLOCK;
        store_msg.payload = store_payload;
        store_msg.payload_destroy = network_local_store_block_payload_destroy;
        actor_send(&actor->network->actor, &store_msg);
```

At `walk_done`, also: for MARK_PERMANENT and DELETE_EPHEMERAL send `EPHEMERAL_REGISTRY_REMOVE` for `actor->descriptor_hash` (if `bc->registry != NULL`), and for MARK_PERMANENT apply CLEAR to the entry descriptor hash itself (already done above via `_rep_apply_to_hash(actor, result->block->hash)` for each descriptor block fetched).

Create/destroy:

```c
representation_actor_t* representation_actor_create(block_cache_t* bc, network_t* network,
                                                    buffer_t* descriptor_hash,
                                                    representation_op_e op, actor_t* reply_to) {
  if (bc == NULL || descriptor_hash == NULL) return NULL;
  representation_actor_t* actor = get_clear_memory(sizeof(representation_actor_t));
  actor->bc = bc;
  actor->network = network;
  actor->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)descriptor_hash);
  actor->op = op;
  actor->reply_to = reply_to;
  actor_init(&actor->actor, actor, representation_actor_dispatch, bc->pool);
  message_t msg;
  msg.type = READABLE_PULL;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&actor->actor, &msg);
  return actor;
}

void representation_actor_destroy(representation_actor_t* actor) {
  if (actor == NULL) return;
  scheduler_pool_wait_for_idle(actor->bc->pool);
  actor_destroy(&actor->actor);
  if (actor->descriptor_hash != NULL) DESTROY(actor->descriptor_hash, buffer);
  if (actor->next_descriptor_hash != NULL) buffer_destroy(actor->next_descriptor_hash);
  free(actor);
}
```

(If `block_cache_t` has no public `pool` accessor, it does — `struct scheduler_pool_t* pool` is a public struct member, block_cache.h:99.)

Constructors: each wraps `representation_actor_create` with the right `op`.

Add `src/OFFStreams/representation_actor.c` to the root `CMakeLists.txt` source list.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*'`
Expected: PASS (the two representation tests + everything before).

- [ ] **Step 5: Commit**

```bash
git add src/OFFStreams/representation_actor.h src/OFFStreams/representation_actor.c src/Actor/message.h src/BlockCache/block_cache.h src/BlockCache/block_cache.c CMakeLists.txt test/test_ephemeral.cpp
git commit -m "feat: representation actor walks descriptors for commit/delete/pin/unpin ops"
```

---

### Task 11: HTTP routes for representation ops + list-ephemerals

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c` (route registration at 1611-1652 + new handlers)
- Test: `test/test_ephemeral.cpp` (route-level tests are covered by the rep-actor tests; run the HTTP server suite for regressions)

- [ ] **Step 1: Implement the handlers**

In `off_routes.c`, add (after `_off_get_handler`'s helpers, before `off_routes_register`):

```c
/* --- Representation-level ephemeral/pin operations --- */

typedef struct {
    http_response_t* response;
    http_connection_t* connection;
    scheduler_pool_t* pool;
    representation_actor_t* rep_actor;   /* one-shot; destroyed on completion */
    actor_t completion;                  /* embedded; deferred-destroyed with the context */
} rep_route_context_t;

static void _rep_route_completion_destroy(void* ptr) {
    rep_route_context_t* route_ctx = (rep_route_context_t*)ptr;
    actor_destroy(&route_ctx->completion);
    free(route_ctx);
}

static void _rep_route_completion_dispatch(void* state, message_t* msg) {
    rep_route_context_t* route_ctx = (rep_route_context_t*)state;
    if (msg->type == REPRESENTATION_OP_RESULT) {
        representation_op_result_payload_t* result =
            (representation_op_result_payload_t*)msg->payload;
        char body[128];
        snprintf(body, sizeof(body),
                 result->result == 0 ? "{\"result\":\"ok\",\"blocks\":%zu}"
                                     : "{\"result\":\"error\",\"blocks\":%zu}",
                 result->blocks_touched);
        http_response_set_header(route_ctx->response, "Content-Type", "application/json");
        http_response_write(route_ctx->response, body, strlen(body));
    }
    http_response_end(route_ctx->response);
    http_response_destroy(route_ctx->response);
    if (route_ctx->connection) {
        http_connection_destroy(route_ctx->connection);
    }
    if (route_ctx->rep_actor != NULL) {
        representation_actor_destroy(route_ctx->rep_actor);
        route_ctx->rep_actor = NULL;
    }
    scheduler_pool_defer_cleanup(route_ctx->pool, route_ctx, _rep_route_completion_destroy);
}

static void _rep_route_start(http_request_t* request, http_response_t* response,
                             off_routes_context_t* ctx, representation_op_e op) {
    if (request->body == NULL || request->body->data == NULL || request->body->size == 0) {
        http_response_set_status(response, 400);
        http_response_write(response, "Missing OFF URL in request body", 31);
        http_response_end(response);
        return;
    }
    char* url_str = get_memory(request->body->size + 1);
    memcpy(url_str, request->body->data, request->body->size);
    url_str[request->body->size] = '\0';
    off_url_t* url = off_url_parse(url_str);
    free(url_str);
    if (url == NULL || url->descriptor_hash == NULL) {
        if (url != NULL) off_url_destroy(url);
        http_response_set_status(response, 400);
        http_response_write(response, "Invalid OFF URL", 15);
        http_response_end(response);
        return;
    }

    rep_route_context_t* route_ctx = get_clear_memory(sizeof(rep_route_context_t));
    route_ctx->response = response;
    route_ctx->connection = response->connection;
    route_ctx->pool = ctx->pool;
    response->is_piped = 1;
    response->connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response);
    refcounter_reference((refcounter_t*)response->connection);
    actor_init(&route_ctx->completion, route_ctx, _rep_route_completion_dispatch, ctx->pool);

    route_ctx->rep_actor = representation_actor_create(
        ctx->bc, ctx->network, url->descriptor_hash, op, &route_ctx->completion);
    off_url_destroy(url);
    if (route_ctx->rep_actor == NULL) {
        http_response_set_status(response, 500);
        http_response_end(response);
        /* Undo the piped refs taken above. */
        refcounter_dereference((refcounter_t*)response);
        refcounter_dereference((refcounter_t*)response->connection);
        actor_destroy(&route_ctx->completion);
        free(route_ctx);
        return;
    }
}
```

Lifecycle caveat: the completion actor and its state are destroyed via `scheduler_pool_defer_cleanup` from inside the completion dispatch — the pool's destroy stack runs them at a safe point after the dispatch returns, which is the established pattern for destroying actors you are currently running in (see the server-actor-threading pattern and `_put_on_descriptor_close`'s deferred derefs, off_routes.c:1309-1313). `representation_actor_destroy` currently calls `scheduler_pool_wait_for_idle`, which must NOT run from inside a dispatch on that same pool (it waits for the very worker executing this dispatch — a deadlock). Before relying on the code above, verify which side is safe: if `representation_actor_destroy` deadlocks here, change the completion dispatch to defer the rep actor separately — `scheduler_pool_defer_cleanup(route_ctx->pool, route_ctx->rep_actor, (void (*)(void*))representation_actor_destroy)` — and have `_rep_route_completion_destroy` NOT destroy it. Test the route with a live HTTP request in Step 2 to confirm no hang either way.

Then the four handlers + list:

```c
static void _off_mark_permanent_handler(http_request_t* request, http_response_t* response, void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data, REPRESENTATION_OP_MARK_PERMANENT);
}
static void _off_delete_ephemeral_handler(http_request_t* request, http_response_t* response, void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data, REPRESENTATION_OP_DELETE_EPHEMERAL);
}
static void _off_pin_handler(http_request_t* request, http_response_t* response, void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data, REPRESENTATION_OP_PIN);
}
static void _off_unpin_handler(http_request_t* request, http_response_t* response, void* user_data) {
    _rep_route_start(request, response, (off_routes_context_t*)user_data, REPRESENTATION_OP_UNPIN);
}

static void _off_list_ephemeral_handler(http_request_t* request, http_response_t* response, void* user_data) {
    (void)request;
    off_routes_context_t* ctx = (off_routes_context_t*)user_data;
    /* block_cache_list_ephemeral mirrors the payload back to the completion
       actor, which builds the JSON response. */
    list_route_context_t* route_ctx = get_clear_memory(sizeof(list_route_context_t));
    route_ctx->response = response;
    route_ctx->connection = response->connection;
    route_ctx->pool = ctx->pool;
    response->is_piped = 1;
    response->connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response);
    refcounter_reference((refcounter_t*)response->connection);
    actor_init(&route_ctx->completion, route_ctx, _list_route_completion_dispatch, ctx->pool);
    block_cache_list_ephemeral(ctx->bc, &route_ctx->completion);
}

static void _list_route_completion_dispatch(void* state, message_t* msg) {
    list_route_context_t* route_ctx = (list_route_context_t*)state;
    if (msg->type == CACHE_EPHEMERAL_LIST) {
        cache_ephemeral_list_payload_t* payload = (cache_ephemeral_list_payload_t*)msg->payload;
        /* Build JSON: [{"hash":"<hex>","claims":N,"pins":M},...] — hex-encode hashes. */
        size_t body_capacity = payload->count * 96 + 32;
        char* body = get_memory(body_capacity);
        size_t offset = 0;
        offset += snprintf(body + offset, body_capacity - offset, "[");
        for (size_t idx = 0; idx < payload->count; idx++) {
            char hex[65];
            size_t hash_size = payload->hashes[idx]->size < 32 ? payload->hashes[idx]->size : 32;
            for (size_t byte_idx = 0; byte_idx < hash_size; byte_idx++) {
                sprintf(hex + byte_idx * 2, "%02x", payload->hashes[idx]->data[byte_idx]);
            }
            hex[hash_size * 2] = '\0';
            offset += snprintf(body + offset, body_capacity - offset,
                               "%s{\"hash\":\"%s\",\"claims\":%u,\"pins\":%u}",
                               idx == 0 ? "" : ",", hex,
                               (unsigned)payload->ephemeral_counts[idx],
                               (unsigned)payload->pin_counts[idx]);
        }
        offset += snprintf(body + offset, body_capacity - offset, "]");
        http_response_set_header(route_ctx->response, "Content-Type", "application/json");
        http_response_write(route_ctx->response, body, offset);
        free(body);
        /* Steal the arrays so the mirrored payload destroy runs on an empty shell. */
        free(payload->hashes); payload->hashes = NULL;
        free(payload->ephemeral_counts); payload->ephemeral_counts = NULL;
        free(payload->pin_counts); payload->pin_counts = NULL;
        payload->count = 0;
    }
    http_response_end(route_ctx->response);
    http_response_destroy(route_ctx->response);
    if (route_ctx->connection) {
        http_connection_destroy(route_ctx->connection);
    }
    scheduler_pool_defer_cleanup(route_ctx->pool, route_ctx, _list_route_completion_destroy);
}
```

(`_list_route_completion_destroy` mirrors `_rep_route_completion_destroy`: `actor_destroy(&route_ctx->completion); free(route_ctx);` — declare `list_route_context_t` with `response`, `connection`, `pool`, and the embedded `completion` actor, exactly like `rep_route_context_t` minus the rep actor. The same lifecycle caveat as above applies: never destroy the embedded actor inline — always through `scheduler_pool_defer_cleanup`.)

In `off_routes_register` (off_routes.c:1611-1652), add after the existing PUT registration:

```c
    http_server_post_with_data(server, "/offsystem/ephemeral/commit",
                               _off_mark_permanent_handler, ctx, NULL);
    http_server_post_with_data(server, "/offsystem/ephemeral/delete",
                               _off_delete_ephemeral_handler, ctx, NULL);
    http_server_post_with_data(server, "/offsystem/pin", _off_pin_handler, ctx, NULL);
    http_server_post_with_data(server, "/offsystem/unpin", _off_unpin_handler, ctx, NULL);
    http_server_get_with_data(server, "/offsystem/ephemeral/list",
                              _off_list_ephemeral_handler, ctx, NULL);
```

Add the needed include: `#include "../OFFStreams/representation_actor.h"`.

- [ ] **Step 2: Build and run**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestEphemeral*:TestHttpServer*'`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add src/ClientAPI/HTTP/off_routes.c
git commit -m "feat: HTTP routes for representation ephemeral/pin ops and ephemeral list"
```

---

### Task 12: Client API wire ops + transport handlers

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h` (op codes + structs), `src/ClientAPI/client_api_wire.c` (encode/decode)
- Create: `src/ClientAPI/representation_api.h`, `src/ClientAPI/representation_api.c` (shared transport-side handler)
- Modify: `src/ClientAPI/Unix/unix_connection.c`, `src/ClientAPI/TCP/tcp_connection.c`, `src/ClientAPI/WebSocket/ws_connection.c`, `src/ClientAPI/WebTransport/wt_connection.c`, `src/ClientAPI/WebTransport/webtransport_h3.c` (one dispatch case each), root `CMakeLists.txt`
- Test: `test/test_ephemeral.cpp` (wire encode/decode round-trip)

- [ ] **Step 1: Write the failing test**

```cpp
/* ---- wire encode/decode round-trip ---- */

#include "../src/ClientAPI/client_api_wire.h"

TEST(TestRepWire, RequestResponseRoundTrip) {
  client_api_rep_request_t request;
  memset(&request, 0, sizeof(request));
  request.url = strdup("/offsystem/v3/application%2Foctet-stream/1024/abc/def/name.txt");
  cbor_item_t* frame = client_api_rep_request_encode(CLIENT_API_REP_MARK_PERMANENT_REQUEST, &request);
  ASSERT_NE(frame, nullptr);
  client_api_rep_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(client_api_rep_request_decode(frame, &decoded), 0);
  EXPECT_STREQ(decoded.url, request.url);
  cbor_decref(&frame);
  client_api_rep_request_destroy(&decoded);
  client_api_rep_request_destroy(&request);

  client_api_rep_response_t response;
  memset(&response, 0, sizeof(response));
  response.status = 0;
  response.blocks = 12;
  cbor_item_t* response_frame = client_api_rep_response_encode(CLIENT_API_REP_MARK_PERMANENT_RESPONSE, &response);
  ASSERT_NE(response_frame, nullptr);
  client_api_rep_response_t decoded_response;
  memset(&decoded_response, 0, sizeof(decoded_response));
  ASSERT_EQ(client_api_rep_response_decode(response_frame, &decoded_response), 0);
  EXPECT_EQ(decoded_response.blocks, 12u);
  cbor_decref(&response_frame);
  client_api_rep_response_destroy(&decoded_response);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestRepWire.*`
Expected: compile error — types undeclared.

- [ ] **Step 3: Implement**

In `src/ClientAPI/client_api_wire.h`, append op codes (42 was the next free):

```c
#define CLIENT_API_REP_MARK_PERMANENT_REQUEST   42
#define CLIENT_API_REP_MARK_PERMANENT_RESPONSE  43
#define CLIENT_API_REP_DELETE_EPHEMERAL_REQUEST  44
#define CLIENT_API_REP_DELETE_EPHEMERAL_RESPONSE 45
#define CLIENT_API_REP_PIN_REQUEST               46
#define CLIENT_API_REP_PIN_RESPONSE              47
#define CLIENT_API_REP_UNPIN_REQUEST             48
#define CLIENT_API_REP_UNPIN_RESPONSE            49
#define CLIENT_API_EPHEMERAL_LIST_REQUEST        50
#define CLIENT_API_EPHEMERAL_LIST_RESPONSE       51

/* Representation op request: [type, url]
   Response: [type, status, blocks_touched]
   Ephemeral list response: [type, status, [[hash, claims, pins], ...]] */
typedef struct {
  char* url;
} client_api_rep_request_t;

typedef struct {
  int status;      /* 0 = ok */
  size_t blocks;   /* blocks touched (0 for list) */
} client_api_rep_response_t;

typedef struct {
  int status;
  size_t count;
  uint8_t** hashes;      /* count × 32-byte hashes (hex-encode at the client) */
  uint16_t* claims;
  uint32_t* pins;
} client_api_ephemeral_list_response_t;
```

In `client_api_wire.c`, add encode/decode functions following the existing style (`_encode_string`/`_decode_string` helpers already exist there):

```c
cbor_item_t* client_api_rep_request_encode(int type, const client_api_rep_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8((uint8_t)type);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  item = _encode_string(msg->url);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_rep_request_decode(cbor_item_t* item, client_api_rep_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));
  cbor_item_t* url_item = cbor_array_get(item, 1);
  msg->url = _decode_string(url_item, OFFS_MAX_ORI_STRING_LEN);
  cbor_decref(&url_item);
  if (msg->url == NULL || validate_ori_string(msg->url) != 0) {
    client_api_rep_request_destroy(msg);
    return -1;
  }
  return 0;
}

void client_api_rep_request_destroy(client_api_rep_request_t* msg) {
  if (msg == NULL) return;
  free(msg->url);
}

cbor_item_t* client_api_rep_response_encode(int type, const client_api_rep_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item = cbor_build_uint8((uint8_t)type);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  item = cbor_build_int8((int8_t)msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  item = cbor_build_uint64(msg->blocks);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_rep_response_decode(cbor_item_t* item, client_api_rep_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));
  cbor_item_t* status_item = cbor_array_get(item, 1);
  if (cbor_isa_negint(status_item)) {
    msg->status = -(int)cbor_get_int(status_item);
  } else if (cbor_isa_uint(status_item)) {
    msg->status = (int)cbor_get_int(status_item);
  }
  cbor_decref(&status_item);
  cbor_item_t* blocks_item = cbor_array_get(item, 2);
  msg->blocks = _decode_size(blocks_item);
  cbor_decref(&blocks_item);
  return 0;
}
```

Shared transport handler — `src/ClientAPI/representation_api.h`:

```c
#ifndef OFFS_REPRESENTATION_API_H
#define OFFS_REPRESENTATION_API_H

#include "../OFFStreams/representation_actor.h"
#include "../ClientAPI/client_api_wire.h"
#include <cbor.h>

/* Transport-agnostic representation-op handler. Decodes the request frame,
 * spins a representation actor (or the ephemeral list), and sends the response
 * frame back via send_response(conn, frame). The connection remains owned by
 * the caller. */
void client_api_representation_handle(block_cache_t* bc, scheduler_pool_t* pool,
                                      network_t* network, cbor_item_t* frame,
                                      void (*send_response)(void* conn, cbor_item_t* frame),
                                      void* conn,
                                      void (*send_error)(void* conn, int status, const char* message));

#endif // OFFS_REPRESENTATION_API_H
```

`representation_api.c`: decode the frame, map op code → `representation_op_e`, `off_url_parse` the URL, create a heap completion actor (mirroring the Task 11 pattern — completion actor + `scheduler_pool_defer_cleanup` for its destruction) that encodes `client_api_rep_response_t` and calls `send_response`. For `CLIENT_API_EPHEMERAL_LIST_REQUEST`: call `block_cache_list_ephemeral` with a completion actor that steals the arrays, encodes the list response, frees them, and sends.

Transport integration — each connection file has a CBOR frame dispatch (first array element = op). Add one case per op in each file's dispatch switch (locations: unix_connection.c ~847, tcp_connection.c ~811, ws_connection.c ~1092, wt_connection.c ~536, webtransport_h3.c ~894 — find the switch that routes `CLIENT_API_PUT_REQUEST` and add alongside):

```c
    case CLIENT_API_REP_MARK_PERMANENT_REQUEST:
    case CLIENT_API_REP_DELETE_EPHEMERAL_REQUEST:
    case CLIENT_API_REP_PIN_REQUEST:
    case CLIENT_API_REP_UNPIN_REQUEST:
    case CLIENT_API_EPHEMERAL_LIST_REQUEST: {
      if (!conn->is_authenticated) {
        _conn_send_error(conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
        break;
      }
      client_api_representation_handle(conn->bc, conn->pool, conn->network, frame,
                                       _conn_send_frame_cb, conn, _conn_send_error_cb);
      break;
    }
```

Adapt names per transport (`_unix_connection_send_error` etc.; each has a raw frame sender — wrap it in a `(void(*)(void*, cbor_item_t*))` adapter). If a transport lacks `conn->network`, pass `NULL` (announce skipped).

Add `src/ClientAPI/representation_api.c` to root `CMakeLists.txt`.

- [ ] **Step 4: Run tests**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter='TestRepWire*:TestEphemeral*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c src/ClientAPI/representation_api.h src/ClientAPI/representation_api.c src/ClientAPI/Unix/unix_connection.c src/ClientAPI/TCP/tcp_connection.c src/ClientAPI/WebSocket/ws_connection.c src/ClientAPI/WebTransport/wt_connection.c src/ClientAPI/WebTransport/webtransport_h3.c CMakeLists.txt test/test_ephemeral.cpp
git commit -m "feat: client API wire ops for representation ephemeral/pin operations"
```

---

### Task 13: C client functions

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h` (declarations + `offs_put_options_t.recycle_ephemeral`), `src/ClientLibs/c/offs_client.c` (request senders + response dispatch cases)
- Test: manual/e2e (the C client is exercised from ../OFFS e2e per the sibling-repo memory; wire round-trip is covered in Task 12)

- [ ] **Step 1: Add the options field**

In `offs_put_options_t` (offs_client.h:37-48) add:

```c
  uint8_t recycle_ephemeral;  /* 0 = error on ephemeral source, 1 = commit, 2 = propagate */
```

In `_fill_put_request` (offs_client.c:2119-2226) add `msg->recycle_ephemeral = options->recycle_ephemeral;` and in `client_api_put_request_encode` the field lands at wire index 9 (Task 8 added it).

- [ ] **Step 2: Add client-side callbacks + functions**

In `offs_client.h`:

```c
/* Representation-level ephemeral/pin operations. url is the full OFF URL.
 * Callback receives status (0 = ok) and blocks_touched. */
typedef void (*offs_rep_op_cb_t)(void* ctx, int status, size_t blocks_touched);
typedef void (*offs_ephemeral_list_cb_t)(void* ctx, int status, size_t count,
                                         const uint8_t* const* hashes, /* count × 32 */
                                         const uint16_t* claims, const uint32_t* pins);

int offs_client_mark_permanent(offs_client_t* client, const char* url,
                               offs_rep_op_cb_t callback, void* ctx);
int offs_client_delete_ephemeral(offs_client_t* client, const char* url,
                                 offs_rep_op_cb_t callback, void* ctx);
int offs_client_pin_representation(offs_client_t* client, const char* url,
                                   offs_rep_op_cb_t callback, void* ctx);
int offs_client_unpin_representation(offs_client_t* client, const char* url,
                                     offs_rep_op_cb_t callback, void* ctx);
int offs_client_list_ephemerals(offs_client_t* client, offs_ephemeral_list_cb_t callback, void* ctx);
```

In `offs_client.c`: add single-slot callback fields to `offs_client_t` (`rep_op_cb`, `rep_op_cb_ctx`, `ephemeral_list_cb`, `ephemeral_list_cb_ctx`) following the exact pattern of `block_put_cb` (offs_client.c:751-765): lock, store, unlock in the request functions; in the frame-dispatch switch (offs_client.c:622+) add:

```c
    case CLIENT_API_REP_MARK_PERMANENT_RESPONSE:
    case CLIENT_API_REP_DELETE_EPHEMERAL_RESPONSE:
    case CLIENT_API_REP_PIN_RESPONSE:
    case CLIENT_API_REP_UNPIN_RESPONSE: {
      client_api_rep_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_rep_response_decode(frame, &msg) == 0) {
        if (rep_op_cb != NULL) {
          rep_op_cb(rep_op_cb_ctx, msg.status, msg.blocks);
          _clear_delivered_slot(client, rep_op_cb);
        }
      }
      break;
    }
```

(read the surrounding code at offs_client.c:622-810 first and mirror how the existing single-slot callbacks are read out under the client lock — the memory note "Clear delivered single-response op callback slots" (commit 37c4544) is the pattern: clear the slot when the response is delivered to avoid double-delivery).

Request functions (mirror `offs_client_put_ex`'s single-frame path):

```c
static int _offs_client_rep_op(offs_client_t* client, int op_code, const char* url,
                               offs_rep_op_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected || url == NULL) return -1;
  platform_mutex_lock(client->lock);
  client->rep_op_cb = callback;
  client->rep_op_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);
  client_api_rep_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.url = (char*)url;
  cbor_item_t* frame = client_api_rep_request_encode(op_code, &msg);
  _send_frame(client, frame);
  return 0;
}

int offs_client_mark_permanent(offs_client_t* client, const char* url,
                               offs_rep_op_cb_t callback, void* ctx) {
  return _offs_client_rep_op(client, CLIENT_API_REP_MARK_PERMANENT_REQUEST, url, callback, ctx);
}
/* ... delete_ephemeral / pin / unpin identical with their op codes ... */
```

`offs_client_list_ephemerals` sends `CLIENT_API_EPHEMERAL_LIST_REQUEST` and its dispatch case decodes `client_api_ephemeral_list_response_t`, passes the arrays to the callback, `_hold_payload`s the hash array (mirror the block_get handler at offs_client.c:767-781) and clears the slot.

- [ ] **Step 3: Build**

Run: `cd build-test && cmake --build . --target testliboffs`
Expected: clean build (the client compiles as part of the library/test binary).

- [ ] **Step 4: Commit**

```bash
git add src/ClientLibs/c/offs_client.h src/ClientLibs/c/offs_client.c
git commit -m "feat: C client representation ops and ephemeral list"
```

---

### Task 14: JS client

**Files:**
- Modify: `src/ClientLibs/js/offs-client/src/wire.js`, `src/ClientLibs/js/offs-client/src/index.js`, `src/ClientLibs/js/offs-client/src/types.js`
- Rebuild dist: `src/ClientLibs/js/offs-client/dist/*`

- [ ] **Step 1: wire.js — MSG constants + encode/decode**

Add to `MSG` (wire.js:6-46, values mirror client_api_wire.h):

```js
  REP_MARK_PERMANENT_REQUEST: 42,
  REP_MARK_PERMANENT_RESPONSE: 43,
  REP_DELETE_EPHEMERAL_REQUEST: 44,
  REP_DELETE_EPHEMERAL_RESPONSE: 45,
  REP_PIN_REQUEST: 46,
  REP_PIN_RESPONSE: 47,
  REP_UNPIN_REQUEST: 48,
  REP_UNPIN_RESPONSE: 49,
  EPHEMERAL_LIST_REQUEST: 50,
  EPHEMERAL_LIST_RESPONSE: 51
```

Add encode/decode:

```js
export function encodeRepRequest(opCode, url) {
  return encoder.encode([opCode, url]);
}

export function decodeRepResponse(bytes) {
  const [type, status, blocks] = decoder.decode(bytes);
  return { status, blocks };
}

export function decodeEphemeralListResponse(bytes) {
  const [type, status, entries] = decoder.decode(bytes);
  return {
    status,
    entries: (entries || []).map(([hash, claims, pins]) => ({
      hash: String.fromCharCode(...hash),
      claims,
      pins
    }))
  };
}
```

- [ ] **Step 2: types.js + index.js**

In `types.js` extend the `OffsPutOptions` typedef with `@property {number} [recycleEphemeral]` (0/1/2). In `wire.js`'s `encodePutRequest`, push it at index 9 when present:

```js
  if (options.recycleEphemeral !== undefined) {
    payload.push(options.recycleEphemeral);
  }
```

In `index.js`, add methods:

```js
  async markPermanent(url) {
    const requestBytes = wire.encodeRepRequest(wire.MSG.REP_MARK_PERMANENT_REQUEST, url);
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.REP_MARK_PERMANENT_RESPONSE);
    return wire.decodeRepResponse(responseBytes);
  }
  async deleteEphemeral(url) { /* same with REQUEST 44 / RESPONSE 45 */ }
  async pinRepresentation(url) { /* 46 / 47 */ }
  async unpinRepresentation(url) { /* 48 / 49 */ }
  async listEphemerals() {
    const requestBytes = encoder.encode([wire.MSG.EPHEMERAL_LIST_REQUEST]);
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.EPHEMERAL_LIST_RESPONSE);
    return wire.decodeEphemeralListResponse(responseBytes);
  }
```

(Match the existing `_sendAndWait` usage in `put()` — index.js:234-303. Use the same private method name that file actually has.)

- [ ] **Step 3: Rebuild dist**

Run: `cd src/ClientLibs/js/offs-client && npm run build` (check `package.json` for the actual build script name first). Confirm the four dist files (`offs-client.esm.js`, `offs-client.umd.js` + maps) update — they are committed to the repo (git status shows them tracked).

- [ ] **Step 4: Commit**

```bash
git add src/ClientLibs/js/offs-client
git commit -m "feat: JS client representation ops and ephemeral list"
```

---

### Task 15: Config docs + full suite + valgrind + de-wonk

**Files:**
- Modify: `docs/CONFIG_FIELDS.md`
- Full verification pass

- [ ] **Step 1: Document config fields**

Add the four `ephemeral_registry_*` fields to `docs/CONFIG_FIELDS.md` following the existing entries' format (description, default, whether runtime-mutable — these are startup-only, like `index_*`, so they are NOT added to `config_json.c`'s field tables).

- [ ] **Step 2: Run the entire test suite**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs`
Expected: all PASS, no regressions.

- [ ] **Step 3: Valgrind leak check**

Run: `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 build-vg/test/testliboffs --gtest_filter='TestEphemeral*:TestBlockCache*:TestRepWire*'`
Expected: 0 leaks, 0 errors (rebuild `build-vg` first if needed: `cd build-vg && cmake --build . --target testliboffs`). Known pre-existing issue to ignore: the scheduler `_scheduler_worker_loop` "Invalid read of size 1" (see project memory — not a regression).

- [ ] **Step 4: De-wonk audit**

Use the de-wonk skill on the completed implementation: verify no TODO/FIXME/HACK/XXX in touched files, no stubbed/disabled code paths (e.g., an announce path silently compiled out, a `force` flag never honored), and that the list-ephemerals mirrored-payload destroy contract is honored at every consumer (HTTP, wire, C client).

- [ ] **Step 5: Final commit**

```bash
git add docs/CONFIG_FIELDS.md
git commit -m "docs: document ephemeral registry configuration fields"
```

---

## Self-Review notes (already applied)

- **Spec coverage:** data model & persistence (Tasks 1-2), block-level ops incl. pin/unpin/list (Tasks 3-4), LRU untouched (no task touches `block_lru_cache_*` — correct per spec), respiration exclusion (Task 4), network local-only + announce-on-commit (Tasks 4, 6, 8, 10), registry actor incl. native deletion, rotation backup, FP/FN handling + self-heal (Tasks 5, 8), ephemeral put with only-created-blocks claims (Tasks 4, 6), recycle enforcement incl. ephemeral-put acceptance and both explicit modes (Task 8), failure cleanup (Task 9), transitive commit (Task 10 — CLEAR applies to every block in the walk including recycled shared blocks), representation-level APIs (Tasks 10-12), get/load + pin (NOT YET COVERED — see below), list-ephemerals (Tasks 3, 11, 12, 13, 14), config (Tasks 5, 15).
- **Known gap — get/load `pin` argument:** the spec's "pin argument on get/load walks the representation after the read completes and issues pin-all" is implemented by exposing the pin/unpin representation ops (Tasks 10-14); the `?pin=1` GET-argument variant on the HTTP GET route is a thin addition on top of `_setup_stream_pipeline`'s close path (subscribe once to `rs` close → create a pin representation actor for the URL). If desired, add it during Task 11 as one extra step; otherwise it ships as the explicit `/offsystem/pin` route, which satisfies the spec's API requirement.
- **Type consistency:** `cache_ephemeral_result_payload_t.hash` is added in Task 10 and must be threaded back into the Task 3 dispatch code when Task 10 lands (single edit, called out in the task). `RECYCLE_EPHEMERAL_*` values are asserted stable in tests because they ride the wire at put-request index 9.
- **Claim ownership (fixed during self-review):** exactly one claim per block, one owner per claim — new_blocks_recipe claims its random blocks, the recycler claims recycled source blocks, the stream claims only its off_blocks, the descriptor stream claims descriptor blocks. `_create_tuple`'s re-put of random blocks uses the plain put (a double-claim there would strand a claim on delete). Failure cleanup splits the same way: the stream releases `created_hashes` (off_blocks + non-recycler randoms, tracked at attach time via `current_recipe->is_recycler`), the recycler releases `acquired_hashes` — no overlap, no double-release.