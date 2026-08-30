# Cache Load Command Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `load` command on every client surface (HTTP `?load=1`, wire frames 39–41, C client, JS client, Dart binding, `offs load` CLI) that pulls a file's tuples into the daemon's block cache without transferring file data, streaming tuple-level progress to the client.

**Architecture:** A `load_mode` flag on `readable_off_stream` makes tuple-level block failures skip the tuple (and tally it) instead of tearing down the stream; every consumer reuses the pipeline's existing events. A shared load-consumer shape forwards progress as ndjson lines (HTTP `GET ...?load=1`) or as new wire frames `LOAD_PROGRESS 40` / `LOAD_END 41`, requested by `LOAD_REQUEST 39` (GET_REQUEST-shaped). C/JS/Dart bindings and the CLI are thin wrappers.

**Tech Stack:** C (liboffs core + ClientAPI), libcbor, GTest, JS (offs-client, vite), Dart/Flutter (off_api.dart), the `offs` CLI (OFFS repo).

**Spec:** `docs/superpowers/specs/2026-08-28-cache-load-design.md`

**Working agreement (from the QR feature execution):**
- Subagents stage ONLY the files each task lists; the repo has unrelated uncommitted user work (src/Actor, src/BlockCache, demo/, docs/ARCHITECTURE.md, src/ClientLibs/js/offs-client/src/ofd.js, etc.) — never stage it. No Co-Authored-By lines. No TODOs in completed work (CLAUDE.md).
- Full-suite gate: `cmake --build build --target testliboffs -j$(nproc) && ./build/test/testliboffs` (875 tests / 186 suites green at plan time; counts grow).
- TDD on every code task: failing test first, observed failure, then implement.
- `dist/` files are gitignored-but-tracked: stage them by explicit path.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `src/OFFStreams/readable_off_stream.h/.c` | Modify | `load_mode` flag: skip-on-tuple-miss, tallies, `tuple_loaded_event` |
| `src/Streams/stream.h` | Modify | New event enum value `tuple_loaded_event = 15` |
| `test/test_readable_load.cpp` | Create | Load-mode stream unit tests |
| `src/ClientAPI/client_api_wire.h/.c` | Modify | `LOAD_REQUEST 39` / `LOAD_PROGRESS 40` / `LOAD_END 41` |
| `test/test_load_wire.cpp` | Create | Wire frame tests |
| `src/ClientAPI/HTTP/off_routes.c` | Modify | `?load=1` branch → ndjson streaming |
| `src/ClientAPI/Unix/unix_connection.c` | Modify | `LOAD_REQUEST` dispatch → frames 40/41 |
| `src/ClientAPI/WS/ws_connection.c` | Modify | Load dispatch (mirrors its GET subset) |
| `src/ClientAPI/TCP/tcp_connection.c` | Modify | Load dispatch (mirrors its GET support) |
| `src/ClientLibs/c/offs_client.h/.c` | Modify | `offs_client_load` + callbacks + dispatch |
| `src/ClientLibs/js/offs-client/src/{wire.js,index.js,transports/http-transport.js}` | Modify | `load()` both transports; `dist/` rebuilt |
| `examples/off_client/lib/services/off_api.dart` | Modify | `load()` + QR catch-up (`connectPeerImage`/`addFriendImage`) |
| `OFFS/src/offs/commands/load.c` (OFFS repo) | Create | `offs load <ori>` |
| `OFFS/src/offs/cli_util.c`, `OFFS/src/offs/l10n/en.h` | Modify | Register command, usage strings |
| `test/test_off_routes_load.cpp` | Create | `?load=1` ndjson surface test |
| `docs/OFFS_API_CLI_SPEC.md` | Modify | Document the load surface |

Task order (each builds on the last): 1 wire frames → 2 stream load-mode + tests → 3 unix transport → 4 HTTP → 5 WS/TCP → 6 C client → 7 JS client → 8 Dart binding → 9 CLI → 10 e2e + docs + close.

---

### Task 1: Wire frames 39/40/41 (TDD)

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h` (message-type defines ~line 45, structs ~line 100-110 near GET frames, decls near the GET encoders)
- Modify: `src/ClientAPI/client_api_wire.c` (encode/decode next to the GET implementations at ~line 363)
- Create: `test/test_load_wire.cpp`
- Modify: `test/CMakeLists.txt` (add `test_load_wire.cpp` to the `add_executable(testliboffs ...)` list, next to `test_qr_wire.cpp`)

- [ ] **Step 1: Write the failing test** — create `test/test_load_wire.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include <cbor.h>
}

namespace load_wire_test {

static client_api_load_request_t _make_req(const char* ori, uint8_t has_range,
                                           size_t start, size_t end) {
  client_api_load_request_t req;
  memset(&req, 0, sizeof(req));
  req.ori_string = (char*)ori;
  req.has_range = has_range;
  req.range_start = start;
  req.range_end = end;
  return req;
}

TEST(LoadRequestWire, EncodeDecodeRoundTripNoRange) {
  client_api_load_request_t req = _make_req("http://n/offsystem/v3/standard/10/a/b/f", 0, 0, 0);
  cbor_item_t* frame = client_api_load_request_encode(&req);
  ASSERT_NE(frame, nullptr);

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, client_api_load_request_decode(frame, &decoded));
  EXPECT_STREQ(decoded.ori_string, req.ori_string);
  EXPECT_EQ(0, decoded.has_range);

  client_api_load_request_destroy(&decoded);
  client_api_load_request_destroy(&req);
  cbor_decref(&frame);
}

TEST(LoadRequestWire, EncodeDecodeRoundTripWithRange) {
  client_api_load_request_t req = _make_req("ori-string", 1, 128000, 256000);
  cbor_item_t* frame = client_api_load_request_encode(&req);
  ASSERT_NE(frame, nullptr);

  client_api_load_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  ASSERT_EQ(0, client_api_load_request_decode(frame, &decoded));
  EXPECT_EQ(1, decoded.has_range);
  EXPECT_EQ(128000u, decoded.range_start);
  EXPECT_EQ(256000u, decoded.range_end);

  client_api_load_request_destroy(&decoded);
  client_api_load_request_destroy(&req);
  cbor_decref(&frame);
}

TEST(LoadProgressWire, EncodesCounts) {
  /* [40, tuples_loaded, tuples_total] */
  cbor_item_t* frame = client_api_load_progress_encode(7, 20);
  ASSERT_NE(frame, nullptr);
  size_t loaded = 0, total = 0;
  ASSERT_EQ(0, client_api_load_progress_decode(frame, &loaded, &total));
  EXPECT_EQ(7u, loaded);
  EXPECT_EQ(20u, total);
  cbor_decref(&frame);
}

TEST(LoadEndWire, EncodesFullTally) {
  /* [41, status, tuples_loaded, tuples_total] */
  cbor_item_t* frame = client_api_load_end_encode(1, 180, 200);
  ASSERT_NE(frame, nullptr);

  uint8_t status = 99;
  size_t loaded = 0, total = 0;
  ASSERT_EQ(0, client_api_load_end_decode(frame, &status, &loaded, &total));
  EXPECT_EQ(1, status);
  EXPECT_EQ(180u, loaded);
  EXPECT_EQ(200u, total);
  cbor_decref(&frame);
}

TEST(LoadWire, FrameTypesDoNotCollide) {
  EXPECT_EQ(39, CLIENT_API_LOAD_REQUEST);
  EXPECT_EQ(40, CLIENT_API_LOAD_PROGRESS);
  EXPECT_EQ(41, CLIENT_API_LOAD_END);
}

}  // namespace load_wire_test
```

Add `test_load_wire.cpp` to the test source list (one line).

- [ ] **Step 2: Run to verify compile failure**

```bash
cmake --build build --target testliboffs -j$(nproc)
```
Expected: undeclared `client_api_load_request_encode` etc.

- [ ] **Step 3: Header additions** — in `src/ClientAPI/client_api_wire.h`:

With the other type defines (~line 38, between CONFIG_RELOAD_RESPONSE 38 and ERROR 11... place numerically with the others):

```c
#define CLIENT_API_LOAD_REQUEST          39
#define CLIENT_API_LOAD_PROGRESS         40
#define CLIENT_API_LOAD_END              41
```

Structs (after the GET group, mirroring `client_api_get_request_t`'s comment):

```c
// --- Load Request ---
// [type, ori_string, has_range?, range_start?, range_end?] — same optional-range
// shape as GET_REQUEST. Asks the daemon to pull the file's blocks into its
// block cache without sending file data; progress arrives as LOAD_PROGRESS
// frames, terminated by LOAD_END.
typedef struct {
  char* ori_string;
  uint8_t has_range;    /* 0 → no range elements; 1 → following two present */
  size_t range_start;
  size_t range_end;
} client_api_load_request_t;

// --- Load Progress ---
// [type, tuples_loaded: uint, tuples_total: uint]
// (tuples_total - tuples_loaded includes both in-flight and skipped tuples)

// --- Load End ---
// [type, status: uint, tuples_loaded: uint, tuples_total: uint]
// status: 0 = loaded, 1 = partial (some tuples skipped), 2 = failed
```

Declarations (next to the GET encoders):

```c
cbor_item_t* client_api_load_request_encode(const client_api_load_request_t* msg);
int client_api_load_request_decode(cbor_item_t* item, client_api_load_request_t* msg);
void client_api_load_request_destroy(client_api_load_request_t* msg);
cbor_item_t* client_api_load_progress_encode(size_t tuples_loaded, size_t tuples_total);
int client_api_load_progress_decode(cbor_item_t* item, size_t* tuples_loaded, size_t* tuples_total);
cbor_item_t* client_api_load_end_encode(uint8_t status, size_t tuples_loaded, size_t tuples_total);
int client_api_load_end_decode(cbor_item_t* item, uint8_t* status, size_t* tuples_loaded, size_t* tuples_total);
```

- [ ] **Step 4: Implement in `client_api_wire.c`** — model each on the GET equivalents (`client_api_get_request_encode` at line 363, `client_api_get_response_start_*`, and the get_data decode). Reference shapes:

```c
cbor_item_t* client_api_load_request_encode(const client_api_load_request_t* msg) {
  /* 2 elements without a range, 4 with — same convention as GET_REQUEST. */
  size_t count = msg->has_range ? 4 : 2;
  cbor_item_t* array = cbor_new_definite_array(count);
  /* [0] = type, [1] = ori_string, optional [2] range_start, [3] range_end.
     Copy the element-building style used by client_api_get_request_encode
     immediately above (cbor_build_uint8 / cbor_build_string / cbor_build_uint64
     as used there; decref each built item after push). */
  ...build per the GET encoder...
}
```

Decode validates: array, `[0] == CLIENT_API_LOAD_REQUEST`, `ori_string` is a tstr; range elements only when 4 elements, `has_range` set to 1 by shape (mirror exactly how `client_api_get_request_decode` handles its optional range elements — read it and follow). `client_api_load_progress_encode(size_t, size_t)` builds `[40, loaded, total]` (definite array of 3, uints); decode reads three uints with the same `cbor_isa_uint` guards used by the peer_connect decoders. `client_api_load_end_encode(uint8_t status, ...)` builds `[41, status, loaded, total]`; decode extracts the three ints, no range validation needed beyond uint-ness. Destroy semantics: copy what `client_api_get_request_destroy` does for `ori_string` (free the malloc'd copy; decode must copy the string like the GET decoder does — verify by reading it).

- [ ] **Step 5: Run tests**

```bash
cmake --build build --target testliboffs -j$(nproc) && ./build/test/testliboffs --gtest_filter='LoadWire*:LoadRequestWire*:LoadProgressWire*:LoadEndWire*' && ./build/test/testliboffs 2>&1 | tail -2
```
Expected: all new tests PASS; full suite green (875 + 5 new).

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c test/test_load_wire.cpp test/CMakeLists.txt
git commit -m "feat(wire): LOAD_REQUEST/LOAD_PROGRESS/LOAD_END frames (39-41) for cache-only fetch"
```

---

### Task 2: Load mode in `readable_off_stream` (TDD)

**Files:**
- Modify: `src/OFFStreams/readable_off_stream.h` (struct + constructor decl)
- Modify: `src/OFFStreams/readable_off_stream.c` (skip paths, load event, tallies)
- Modify: `src/Streams/stream.h:40` (new event enum value)
- Test: `test/test_readable_load.cpp` (create)
- Modify: `test/CMakeLists.txt` (add one line)

**Design decisions locked in this task:**
- New event: add `load_tuple_event = 15` as the LAST enumerator of `stream_event_e` in `src/Streams/stream.h` (after `error_event = 14`). Payload = `test_read_load_tuple_payload_t` (see below) heap-allocated, freed with `free`.
- The load constructor is additive; `readable_off_stream_create` keeps its exact signature and delegates with `load_mode = 0`.

- [ ] **Step 1: Write the failing test** — create `test/test_readable_load.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "../src/OFFStreams/readable_off_stream.h"
#include "../src/OFFStreams/readable_descriptor.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/Buffer/buffer.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include <string.h>
#include <stdlib.h>
}

namespace readable_load_test {

#include "test_off_stream_fixture.inc"   /* see note below */
```

**IMPORTANT — fixture reuse:** `test/test_readable_off_stream.cpp` already contains a working fixture that creates a scheduler pool, temp cache dir, block cache, and tuple cache, and tests that write tuples through `readable_off_stream` with a real `block_cache` and assert on emitted events. READ that file first and reuse its fixture pattern verbatim (copy its setUp/tearDown and its helper that builds a `block_cache_t` seeded with computed XOR-recipe blocks). Write these four tests against that pattern:

```cpp
TEST(ReadableOffLoad, CountsTuplesViaLoadEvent) {
  // load mode constructor; write 3 complete tuples (all blocks seeded in cache);
  // subscribe load-event; assert 3 notifications, each payload's tuples_loaded
  // counting 1,2,3 and tuples_skipped == 0; data_event still fires (rendering
  // unchanged); close_event fires at the end.
}

TEST(ReadableOffLoad, MissingNetworkTupleSkipsAndContinues) {
  // load mode; network = NULL (local-only); tuple 2's block hashes are NOT in
  // the cache. Assert: stream does NOT deactivate; 2 load events with
  // tuples_loaded 1 then 2; tuples_skipped == 1 in the second event's payload;
  // tuple 3 (seeded) renders after the skip.
}

TEST(ReadableOffNormal, NetworkNullMissStillDeactivates) {
  // default constructor path (regression guard): same scenario as above,
  // assert the stream DOES deactivate and error_event fires — normal GET
  // behavior is unchanged.
}

TEST(ReadableOffLoad, TuplesSkippedReportedInPayload) {
  // two missing tuples interleaved (skip, load, skip, load): final
  // tuples_loaded == 2, tuples_skipped == 2 across 4 events.
}
```

The test needs the stream's *public* observable surface only (stream_subscribe on events). If the existing test file's fixture cannot seed a cache with XOR-recipe blocks directly (it likely already does — check `test_readable_off_stream.cpp`/`test_readable_descriptor.cpp` for helpers), reuse them.

- [ ] **Step 2: Run to verify compile failure** — `cmake --build build --target testliboffs -j$(nproc)` → undeclared `readable_off_stream_create_load`.

- [ ] **Step 3: Implement**

In `src/OFFStreams/readable_off_stream.h` — add to the struct:

```c
  /* Load mode: cache-only fetch. Missing data tuples are skipped and
     tallied instead of tearing the stream down; each resolved tuple emits
     load_tuple_event. Rendering still happens (consumers discard it). */
  uint8_t load_mode;
  size_t tuples_loaded;
  size_t tuples_skipped;
```

and:

```c
/* Payload for load_tuple_event (load mode only). Heap-allocated; freed by the
   subscriber with free(). */
typedef struct {
  size_t tuples_loaded;
  size_t tuples_skipped;
} load_tuple_payload_t;

readable_off_stream_t* readable_off_stream_create_ex(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network, uint8_t load_mode);
```

`readable_off_stream.h` needs `#include "../Streams/stream.h"` (already there).

In `readable_off_stream.c`:

(a) Add a small notifier next to `_render_origin_data`:

```c
/* fire the load event after each tuple resolves (complete OR skipped) — consumers
   count tuple progress without watching render events (which byte-trimming can
   batch at range boundaries). */
static void _notify_load_tuple(readable_off_stream_t* stream) {
  if (!stream->load_mode) return;
  load_tuple_payload_t* payload = get_clear_memory(sizeof(load_tuple_payload_t));
  payload->tuples_loaded = stream->tuples_loaded;
  payload->tuples_skipped = stream->tuples_skipped;
  stream_notify((stream_t*)stream, load_tuple_event, payload, free);
}
```

Call site 1 — end of `_finish_decode_and_render` (before `_drain_tuple_queue`):

```c
  stream->tuples_loaded++;
  if (stream->load_mode) {
    _notify_load_tuple(stream);
  }
```

Call site 2 — every tuple-completion success path: `_render_origin_data` fires complete/close when `sent_bytes >= final_byte`; rendering with offset trimming still one-data-event-per-tuple in practice, but do NOT depend on it — `load_tuple_event` is the count.

(b) Skip-on-miss. In `CACHE_GET_RESULT`'s `result->block == NULL` branch (readable_off_stream.c:206-238):
- If `stream->network != NULL`: keep the existing NETWORK_LOCAL_FIND_BLOCK path unchanged.
- Else (local-only): if `stream->load_mode`, replace the `stream_deactivate` block with `_skip_pending_tuple(stream)`; else keep `stream_deactivate`.

In `NETWORK_FIND_BLOCK_RESULT`'s `else` branch (`found == 0`, line 289-298):
- If `stream->load_mode`, replace the cleanup+`stream_deactivate` with `_skip_pending_tuple(stream)`; else keep existing.

Add the shared helper above the dispatch function:

```c
/* Load mode only: abandon the current tuple (its blocks never arrived) and
   continue with the next queued one. In-flight cache fetches for this tuple
   are drained via the pending_fetches staleness check in CACHE_GET_RESULT. */
static void _skip_pending_tuple(readable_off_stream_t* stream) {
  stream->tuples_skipped++;
  if (stream->load_mode) {
    _notify_load_tuple(stream);
  }
  if (stream->xor_accumulator != NULL) {
    DESTROY(stream->xor_accumulator, buffer);
    stream->xor_accumulator = NULL;
  }
  if (stream->pending_tuple != NULL) {
    DESTROY(stream->pending_tuple, tuple);
    stream->pending_tuple = NULL;
  }
  stream->blocks_expected = 0;
  stream->blocks_received = 0;
  pending_block_fetch_t* fetch = stream->pending_fetches;
  while (fetch != NULL) {
    pending_block_fetch_t* next = fetch->next;
    /* Move hash to stale list rather than freeing: late results for this
       tuple's hashes must be recognized and dropped (see stale check). */
    ... see stale-list design below ...
  }
  stream->pending_fetches = NULL;
  _drain_tuple_queue(stream);
}
```

**Stale-fetch handling (critical correctness point):** after a skip, the daemon may still be awaiting results for hashes the abandoned tuple requested (`pending_get_t` in block_cache resolves later; a `CACHE_GET_RESULT` for an old hash would otherwise be XOR-accumulated into the NEXT tuple). Add to the struct:

```c
  pending_block_fetch_t* stale_fetches;   /* hashes of an abandoned tuple */
```

In `_skip_pending_tuple`, move the pending fetches' hashes into `stale_fetches` (transfer ownership, do not free the `buffer_t*` hashes). In `CACHE_GET_RESULT` (both block!=NULL and block==NULL arms) and `NETWORK_FIND_BLOCK_RESULT` (found=1 direct-return), FIRST check whether `result->hash` matches a stale hash (walk `stale_fetches`, compare with `buffer_compare(result->hash, stale->hash) == 0`); if it matches, remove the entry from the stale list (destroy hash node), DESTROY the result's block/hash buffers, and `break` without touching `xor_accumulator` or `blocks_received`. Cap the staleness list implicitly — it lives only until the matching late results arrive, and tuples are processed one at a time.

- [ ] **Step 4: Add the constructor** (readable_off_stream.c):

```c
readable_off_stream_t* readable_off_stream_create_ex(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network, uint8_t load_mode) {
  ... existing create body ...
  stream->load_mode = load_mode;
  ...
}

readable_off_stream_t* readable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network) {
  return readable_off_stream_create_ex(pool, bc, tc, ori, descriptor_pad, network, 0);
}
```

Header: declare `readable_off_stream_create_ex` with a comment explaining the flag ("load mode: missing data tuples are skipped and tallied via load_tuple_event instead of deactivating the stream; descriptor misses remain fatal").

- [ ] **Step 5: Run tests**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null && cmake --build build --target testliboffs -j$(nproc) && ./build/test/testliboffs --gtest_filter='ReadableOffLoad*:ReadableOffNormal*' && ./build/test/testliboffs 2>&1 | tail -2
```
Expected: 4 new tests PASS; full suite green (the existing readable-off-stream/descriptor suites prove normal mode is untouched).

- [ ] **Step 6: Commit**

```bash
git add src/Streams/stream.h src/OFFStreams/readable_off_stream.h src/OFFStreams/readable_off_stream.c test/test_readable_load.cpp test/CMakeLists.txt
git commit -m "feat(off-stream): load mode skips missing tuples and emits counted progress events"
```

---

### Task 3: Unix socket LOAD dispatch

**Files:**
- Modify: `src/ClientAPI/Unix/unix_connection.c` (`_unix_handle_get` at 347 is the template; add `_unix_handle_load` + its pipeline type + dispatch case)

NOTE this transport's GET passes `network = NULL` today (`readable_off_stream_create(..., NULL)` at ~line 415). For LOAD, pass the network actor — the connection has one: `conn->peer_ctx.network` (set at unix_connection.c:1169 from `transport->config_node->network`). If `peer_ctx` is only populated on the authenticated path, read how `config_node` reaches the transport and use the same access; if it is genuinely unavailable on some connections, load proceeds cache-only (network == NULL is a legal mode) — but DO pass it when present; verify at implementation time which connection path peer_ctx is filled on (line 1169 context) and report.

- [ ] **Step 1: Add the pipeline struct + handlers** (model exactly on `unix_get_pipeline_t` and `_unix_get_on_rs_data/_unix_get_on_rs_close/_unix_get_on_rs_error` — read them first, they are just above `_unix_handle_get`):

```c
typedef struct {
  refcounter_t refcounter;
  unix_connection_t* conn;
  readable_off_stream_t* rs;
  readable_descriptor_t* desc;
  size_t tuples_total;   /* ceil(final_byte / block_size) - offset_tuple */
} unix_load_pipeline_t;

static void _unix_load_on_tuple_loaded(stream_t* stream_source, void* user, void* payload, void (*payload_destroy)(void*)) {
  (void)stream_source;
  unix_load_pipeline_t* pipeline = (unix_load_pipeline_t*)user;
  /* payload is load_tuple_payload_t{tuples_loaded, tuples_skipped} — heap, free here */
  load_tuple_payload_t* progress = (load_tuple_payload_t*)payload;
  cbor_item_t* frame = client_api_load_progress_encode(progress->tuples_loaded, pipeline->tuples_total);
  _unix_connection_send_frame(pipeline->conn, frame);
  free(progress);
}

static void _unix_load_on_close(stream_t* stream_source, void* user) {
  unix_load_pipeline_t* pipeline = (unix_load_pipeline_t*)user;
  uint8_t status = pipeline->tuples_skipped() ... /* see below */
  cbor_item_t* frame = client_api_load_end_encode(status, pipeline->tuples_loaded, pipeline->tuples_total);
  _unix_connection_send_frame(pipeline->conn, frame);
  DEREFERENCE(pipeline, unix_load_pipeline_t);   /* terminal */
}
```

Concrete rules for the implementer:
- Track on the pipeline (not the stream): `tuples_total` computed from the ORI exactly as `readable_descriptor` does — `total = ceil(url->stream_length_final / block_size) - (file_offset / block_size)` where `block_size` comes from `_block_size_for_type(ori->block_type)` (copy that 10-line static helper or expose it — prefer exposing: add `size_t off_block_size_for_type(block_size_e type);` to `src/OFFStreams/readable_off_stream.h` and have both call it).
- `LOAD_END` status: 0 if no tuple was skipped and stream closed normally; 1 if ≥1 skipped; 2 if the desc/rs `error_event` fired (descriptor unrecoverable or fatal). Subscribe to `error_event` on BOTH desc and rs → send LOAD_END(2) and release.
- Subscribe to `load_tuple_event` (Task 2's event) on rs for progress; to `close_event` on rs for the terminal; unsubscribe/deref symmetric to the GET pipeline (copy its subscription/refcount discipline exactly — the GET pipeline struct at unix_connection.c is the model, including its destroy).
- Dispatch: in the switch (~line 630) add `case CLIENT_API_LOAD_REQUEST: _unix_handle_load(conn, frame); break;` guarded by the same `_check_authenticated` pattern as `_unix_handle_get`. ORI parsing, directory (OFD) rejection: a load of an `offsystem/directory` URL resolves the OFD then loads the resolved entry — for v1, REJECT directory ORIs with `CLIENT_API_STATUS_BAD_REQUEST, "Load requires a file ORI, not a directory"` (same posture as the sync GET path: directories are resolved in HTTP land; extend later if asked). Note this decision in the commit message.
- The pipeline holds refs to `ori`, `rs`, `desc` using the same REFERENCE pattern `_unix_handle_get` uses.

Also add WS and TCP equivalents in the SAME task only if their GET handlers exist: `src/ClientAPI/WS/ws_connection.c:893` handles `CLIENT_API_GET_REQUEST` — mirror a `CLIENT_API_LOAD_REQUEST` case with the same pipeline shape (WS connection carries the same `config_node`/network access — verify, it was added for peer routes). `src/ClientAPI/TCP/tcp_connection.c` has an equivalent GET handler (line ~397) — mirror there too. If a transport's GET handler differs materially (WT), leave WT alone and note it in the report.

- [ ] **Step 1: implement** per above **Step 2: full suite green** `./build/test/testliboffs 2>&1 | tail -2` **Step 3: commit**

```bash
git add src/ClientAPI/Unix/unix_connection.c src/ClientAPI/WS/ws_connection.c src/ClientAPI/TCP/tcp_connection.c src/OFFStreams/readable_off_stream.h src/OFFStreams/readable_off_stream.c
git commit -m "feat(transports): dispatch LOAD frames on unix/tcp/ws transports"
```

---

### Task 4: HTTP `?load=1` streaming route (TDD where practical)

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c` (URL handler ~340-540; the `?ofd=raw` branch at ~369-384 is the pattern)

- [ ] **Step 1: Write the failing test** — create `test/test_off_routes_load.cpp` modeled EXACTLY on `test/test_off_routes.cpp` (same fixture: `scheduler_pool_create`, `mkdtemp` cache dir, `block_cache_create` with the config it uses — the block cache must be seeded with real readable content, which the existing test_off_stream fixtures already do; reuse the seeding helper from `test_off_stream_integration` if present). Request bytes use one of the existing `_send_and_recv` helpers. Test:

```cpp
TEST_F(TestOffRoutesLoad, LoadStreamsNdjsonProgress) {
  // 1. Upload a small file through the normal PUT flow (fixture helper).
  // 2. GET its OFF URL with "?load=1" appended; assert Content-Type
  //    application/x-ndjson; parse body: one "\n"-delimited progress line per
  //    tuple, each {"tuples_loaded":n,"tuples_total":m}; final line
  //    {"status":"loaded","tuples_loaded":N,"tuples_total":N}.
  // 3. GET the SAME url WITHOUT ?load=1; assert normal bytes (regression pin).
  // 4. GET "?load=1" for an ORI with descriptor-hash of 32 zero bytes:
  //    stream ends "failed" (descriptor unrecoverable).
}
```

- [ ] **Step 2: implement** — in `_off_get_handler` (off_routes.c:340-432): after URL parse, before the plain-data path, add:

```c
  if (request->query_string != NULL && strstr(request->query_string, "load") != NULL) {
    _off_load_handler(request, response, ...same ctx...);
    return;
  }
```

(precedent: the `?ofd=` check at off_routes.c:165). The load branch: build the SAME pipeline as `_off_stream_file_get` does (readable_descriptor + readable_off_stream via `_setup_stream_pipeline` with the load-mode constructor), then, instead of `http_response_pipe`, subscribe:

```c
http_response_set_status(response, HTTP_STATUS_OK);
http_response_set_header(response, "Content-Type", "application/x-ndjson");
http_response_set_header(response, "Cache-Control", "no-store");
/* stream events → http_response_write per progress tuple (ndjson line),
   close → terminal line + http_response_end */
```

Model the ctx on `get_pipeline_t` (off_routes.c:161-173 — refcounted struct holding `http_response_t* response`), emitting `{"tuples_loaded":%zu,"tuples_total":%zu}\n` via `snprintf` + `http_response_write`, and the terminal line per the design doc. Use `http_response_write` after headers are set (no Content-Length for a length-unknown body — check how other unbounded streaming responses are sent; `_send_stream_response` (`off_routes.c:287`) sets `Content-Length` explicitly: for load mode, use *chunked/close-delimited* streaming — mirror whatever the existing pipe path does for Content-Length (it uses `body_length`... check `http_response_pipe`); if a Content-Length is mandatory on this server, use the tuple-total-derived upper bound and rely on terminal-line + http_response_end).

Also: `load` must NOT also match OFD handling — query check must be `strstr(..., "?load=1")` or param parse on the raw query string, tested for both `?load=1` and bare `?load`.

- [ ] **Step 3: full suite green; run new test file**; **Step 4: commit**

```bash
git add src/ClientAPI/HTTP/off_routes.c test/test_off_routes_load.cpp test/CMakeLists.txt
git commit -m "feat(http): ?load=1 streams ndjson tuple progress for cache-only fetch"
```

---

### Task 4: C client `offs_client_load` (TDD-lite, pattern-verified)

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h` (callbacks + decls after the peer ops)
- Modify: `src/ClientLibs/c/offs_client.c` (struct fields, snapshot, switch cases, function)

- [ ] **Step 1: callbacks + decls** (offs_client.h, next to the peer typedefs):

```c
typedef void (*offs_load_progress_cb_t)(void* ctx, size_t tuples_loaded, size_t tuples_total);
typedef void (*offs_load_end_cb_t)(void* ctx, uint8_t status, size_t tuples_loaded, size_t tuples_total);
```

```c
/* Load a file's blocks into the daemon's block cache without receiving file
   data. Progress fires per reconstructed tuple; END fires exactly once with
   the terminal status (0=loaded, 1=partial, 2=failed). Errors from the
   daemon (bad ORI, unauthorized) arrive on the error callback registered
   via offs_client_get()-style callbacks (see peer ops' error caveat). */
int offs_client_load(offs_client_t* client, const char* ori_string,
                     offs_load_progress_cb_t progress_cb, void* progress_ctx,
                     offs_load_end_cb_t end_cb, void* end_ctx);
```

- [ ] **Step 2: implement** following the `offs_client_peer_info_ex` pattern exactly (encode-before-register ordering, format: none here — build `client_api_load_request_t{ori_string=ori, has_range=0}` and `client_api_load_request_encode`; `_send_frame`; return -1 guards). Dispatch in `_handle_frame` (snapshot-under-lock discipline identical to the peer ops):

```c
    case CLIENT_API_LOAD_PROGRESS: {
      size_t tuples_loaded = 0, tuples_total = 0;
      if (client_api_load_progress_decode(frame, &tuples_loaded, &tuples_total) == 0) {
        if (load_progress_cb != NULL) load_progress_cb(load_progress_cb_ctx, tuples_loaded, tuples_total);
      }
      break;
    }
    case CLIENT_API_LOAD_END: {
      uint8_t status = 0; size_t loaded = 0, total = 0;
      if (client_api_load_end_decode(frame, &status, &loaded, &total) == 0) {
        if (load_end_cb != NULL) load_end_cb(end_cb_ctx, status, loaded, total);
      }
      break;
    }
```

- [ ] **Step 3: build + full suite; commit** `git commit -m "feat(client): C client library load operation"` (stage the two offs_client files).

---

### Task 5: JS client `load()` + rebuild dist

**Files:**
- Modify: `src/ClientLibs/js/offs-client/src/wire.js` (frames 39/40/41: `encodeLoadRequest(ori, range)`, `decodeLoadProgress`, `isLoadEnd`, `decodeLoadEnd`; `MSG.LOAD_REQUEST: 39` etc.)
- Modify: `src/ClientLibs/js/offs-client/src/transports/http-transport.js`: `load(oriOrUrl, callbacks, range)` → `fetch(url + '?load=1')`, read `response.body` as a stream, split on `\n`, JSON.parse each line, `onProgress` per progress line, `onEnd` on the terminal line.
- Modify: `src/ClientLibs/js/offs-client/src/index.js`: add `load(ori, callbacks, range)` after `get` — HTTP path as above; CBOR transports: `wire.encodeLoadRequest(ori, range)` then loop `this._waitForResponse([wire.MSG.LOAD_PROGRESS, wire.MSG.LOAD_END])` mirroring the GET loop (index.js get()).

- [ ] Implement all three; **verify with `npm test`** (package's vitest suite) and `npm run build`; **grep dist** for `encodeLoadRequest`. **Commit** the three src files + 4 dist files (explicit paths; dist is gitignored but tracked) with `feat(js-client): load() — cache-only fetch with tuple progress` (all in ONE commit so dist matches src).

---

### Task 6: Dart/Flutter binding — `load()` + QR catch-up

**Files:**
- Modify: `examples/off_client/lib/services/off_api.dart`

- [ ] Implement (following the file's existing patterns — read `uploadFile`/`downloadFile`/`connectPeer` first; this binding is HTTP-only):

```dart
  /// Load a file's blocks into the daemon's block cache without downloading
  /// the data. Streams application/x-ndjson progress: one
  /// {"tuples_loaded":n,"tuples_total":m} line per resolved tuple, terminal
  /// line {"status":"loaded|partial|failed",...}.
  Future<Map<String, dynamic>> loadContent(
    String offUrl, {
    void Function(int loaded, int total)? onProgress,
  }) async {
    /* stream-load offUrl + '?load=1' via the same http client used by
       downloadFile; read response lines; onProgress per progress line;
       parse + return the terminal line. */
  }

  Future<String> connectPeerImage(Uint8List ppmBytes) async {
    /* POST /peer/connect with Content-Type: image/x-portable-pixmap, body =
       ppmBytes; parse {"status": n} JSON (match connectPeer's status mapping). */
  }

  Future<String> addFriendImage(Uint8List ppmBytes) async {
    /* POST /friends with Content-Type: image/x-portable-pixmap; mirror addFriend. */
  }
```

Run `dart analyze examples/off_client` (if the Flutter toolchain is present — if not, report inspection-only, same as the CLI task in the QR feature).

- [ ] **Verify + commit**: `git add examples/off_client/lib/services/off_api.dart && git commit -m "feat(binding): Dart loadContent + QR peer image catch-up"`

---

### Task 7: `offs load` CLI

**Files (OFFS repo `/home/victor/Workspace/src/github.com/vijayee/OFFS`):**
- Create: `src/offs/commands/load.c`
- Modify: `src/offs/cli_util.c` (command table + `main.c` dispatch if separate), `src/offs/l10n/en.h` (`L10N_LOAD_USAGE`)

- [ ] **Implement `cmd_load`** modeled exactly on `commands/get.c`'s frame loop (shown in the controller context; mirror its structure):

```c
int cmd_load(int argc, char** argv, cli_client_t* client) {
  /* args: offs load <ori> ; parse --help only */
  client_api_load_request_t req; memset(&req, 0, sizeof(req));
  req.ori_string = (char*)ori; req.has_range = 0;
  /* send LOAD_REQUEST via cli_client_send_frame (copy get.c's send+error flow) */
  /* loop cli_client_recv_frame:
     - LOAD_PROGRESS [40]: decode; fprintf(stderr, "Loading %s: %zu/%zu tuples (%d%%)\r",
         ori, loaded, total, (int)(100 * loaded / max(total,1)));   \r or \n? use \n like put.c's progress
     - LOAD_END [41]: decode status/loaded/total; break;
     - ERROR: print message, had_error = true; break;      */
  /* after loop: if no LOAD_END -> "load truncated" error 1.
     exit 0 for status 0/1 (partial prints
     "Warning: partial load (%zu/%zu tuples)" on stderr); exit 1 for failed. */
}
```

Register `"load"` in the command table (`src/offs/cli_util.c:25-40`) pointing at `cmd_load`; add `L10N_LOAD_USAGE`, `L10N_LOAD_STAGED`-style strings to `src/offs/l10n/en.h` following the existing naming/grammar conventions. Update the OFFS `deps/liboffs` submodule ONLY IF a liboffs commit containing frames 39–41 is available to the submodule clone (same fetch-from-local-repo dance as the QR e2e; otherwise verification is compile-check against the liboffs working tree — state which you did).

**Commit**: `feat(cli): offs load streams tuple progress while warming the daemon block cache`.

---

### Task 8: End-to-end verification (manual)

- [ ] Build `offsd` + `offs` in the OFFS repo against a temporary `deps/liboffs` checkout of this repo's master (the QR e2e proved the pattern; the helper memory `reference_offs_sibling_repo.md` documents it). Start offsd with node certs on a test port. Verify: (1) `offs put` a small file; (2) wipe... no — simpler: `offs get` confirm normal data flow; `offs load <ori>` prints tuple progress lines and a loaded terminal; re-run `offs load` on the same ORI → completes immediately (all tuples already cached — fast path proves blocks were cached, not re-fetched); (3) `curl ?load=1` over HTTP with bearer → ndjson lines; (4) curl an ORI with a garbage descriptor hash → `failed` terminal within the 30 s deadline window; (5) CLI missing ORI path and unauthorized (401) behavior. Post evidence to the Harmony ticket.

---

### Task 9: Docs, valgrind, close

- [ ] Update `docs/OFFS_API_CLI_SPEC.md`: new load surface (§2.1 query flag, §3 frames 39–41, JS/Dart/C binding sections, CLI table row `offs load`, §9 checklist item). Update `docs/HowOFFSWorks.md` client-features §8/9 if it lists commands.
- [ ] Valgrind `-gdwarf-4` on new suites (`ReadableOffLoad*`, `LoadWire*`, HTTP load test) — 0 leaks, 0 errors.
- [ ] De-wonk audit + Harmony ticket close (create the OFFS-xxx ticket for this feature at plan start, close at end with the required summary/notify actions).

---

## Self-review notes

- **Spec coverage**: §1 semantics → Task 2 (+ fixture tests); §2 load consumer → Tasks 3/4; §3 wire → Task 1; §4 HTTP → Task 4; §5 bindings → Tasks 4 (C), 5 (JS), 5 (Dart incl. QR catch-up), 7 (CLI); §6 testing → Task 2-5 unit suites + Task 8 e2e + Task 9 valgrind; §7 exclusions respected (no parallel prefetch, no block metrics, no job API).
- **Type consistency**: `client_api_load_request_t{ori_string,has_range,range_start,range_end}` used consistently; `load_tuple_payload_t{tuples_loaded,tuples_skipped}` defined in Task 2 and consumed in Task 3/4; statuses 0/1/2 identical across wire/HTTP/CLI.
- **Known risk called out for implementers**: HTTP load response framing (Content-Length vs close-delimited) must follow what `http_response_pipe`/chunked support actually allows — verify in Task 4 Step 2 against `http_response.h` rather than assuming.
- WT transport intentionally omitted (its GET support is partial; load rides the transports where GET exists). Revisit if WT GET lands.