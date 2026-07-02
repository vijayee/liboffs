# PUT Cache Space Errors — Design

## Problem

The OFFS daemon silently swallows cache-full errors during `offs put`. When
`block_cache_put` fails (`CACHE_PUT_FULL` or `CACHE_PUT_ERROR`), the failure is
not propagated back to the client. The client's `offs put` reports success
(`File imported: <ori>`) even though the ORI points at blocks that were never
stored. A subsequent `offs get` for that ORI fails — the user only discovers
the breakage at retrieval time, with no signal that the original PUT was at
fault.

Two root causes:

1. **`reply_to` is NULL in the daemon's local PUT path.**
   `writeable_off_stream.c:91` sets `reply_to` only when `stream->network !=
   NULL`. The daemon passes `network = NULL` (`unix_connection.c:472`), so
   `block_cache_put` has no actor to send `CACHE_PUT_RESULT` to. The stream
   never learns that the put failed. The same bug exists in
   `writeable_descriptor.c:101`.

2. **The `CACHE_PUT_RESULT` handler only covers `CACHE_PUT_NEW`.**
   `writeable_off_stream.c:306-321` and `writeable_descriptor.c:165-167`
   check `CACHE_PUT_NEW` (to announce to the network) but ignore
   `CACHE_PUT_ERROR` and `CACHE_PUT_FULL`. Even if `reply_to` were set, the
   failure codes would be dropped.

There is also no pre-flight check: `PUT_REQUEST` carries `stream_length`
(the total file size), but the server never compares it against available
cache space before accepting `PUT_DATA` frames. The client can upload
hundreds of megabytes only to have the put fail mid-stream — or, per the
bug above, appear to succeed while actually losing data.

## Goals

1. **Pre-flight:** reject a PUT before any `PUT_DATA` is accepted when the
   cache cannot hold the required bytes. The client never wastes bandwidth
   uploading data that can't be stored.
2. **Mid-stream:** if `block_cache_put` fails after the pre-flight passes
   (concurrent puts, fragmentation edge cases), propagate the error to the
   client as a `CLIENT_API_ERROR` frame so the user sees the failure and
   knows to reconfigure.
3. **ORI preamble on the wire:** carry the per-PUT `tuple_size` on the
   `PUT_REQUEST` frame so the server uses the client's requested tuple
   width, not a hardcoded 3, for both storage and the pre-flight formula.
4. **Defaults unchanged:** `max_capacity_bytes` stays at 5 GiB. Section
   files continue to accumulate on disk until defragmentation reclaims
   them — this is by design.

## Non-goals

- **No on-disk section cleanup.** Sections grow until defragmented; the
  respiration cycle may recover storage for reuse. This is the existing
  design and is not changing.
- **No cleanup of orphaned blocks on PUT failure.** Content-addressed
  blocks are shared across concurrent PUTs (duplicate uploads, identical
  headers, shared padding). Unconditional `block_cache_remove` on failure
  would corrupt surviving PUTs that reference the same hash. The
  `index_entry_t` counter has no per-PUT ownership refcount, and adding
  one is out of scope. Orphaned blocks from a failed PUT stay on disk
  until defragmentation reclaims them — same behavior as today's
  LRU-evicted blocks.
- **No change to `block_type`.** Stays hardcoded `standard` (128 KB) in
  OFFS; the other enum values are test-only.
- **No change to `descriptor_pad`.** Stays server-side (32); it is a
  stream-creation parameter, not an ORI field.
- **No resumable uploads.** The streaming protocol remains non-resumable.

## Architecture

Three coordinated changes in liboffs, plus one CLI flag in OFFS:

### 1. `block_cache` — new capacity-check helper

**Files:** `src/BlockCache/block_cache.h`, `src/BlockCache/block_cache.c`

A new pure-read helper:

```c
#define CACHE_FIT_OK    0
#define CACHE_FIT_FULL 1

int block_cache_can_fit(block_cache_t* block_cache, size_t required_bytes);
```

Returns `CACHE_FIT_OK` if `block_cache->current_bytes + required_bytes <=
block_cache->max_capacity_bytes`, else `CACHE_FIT_FULL`. No mutation, no
eviction, no locking beyond reading the two `size_t` fields. Used by the
pre-flight check.

### 2. `writeable_off_stream` — required-bytes estimator

**Files:** `src/OFFStreams/writeable_off_stream.h`, `src/OFFStreams/writeable_off_stream.c`

A new pure-compute helper:

```c
size_t writeable_off_stream_estimate_required_bytes(
    size_t stream_length, size_t tuple_size, size_t descriptor_pad);
```

Returns the total cache bytes a PUT of `stream_length` bytes will require,
given `tuple_size` (erasure-coding width — blocks per tuple) and
`descriptor_pad` (per-hash overhead in the descriptor). Each block is
stored once via `block_cache_put` (which writes to one section slot, not
`tuple_size` replicas). The formula:

```
block_size       = 128000  /* standard */
data_blocks      = ceil(stream_length / block_size)
tuple_blocks     = data_blocks * tuple_size          /* random + off per tuple */
tuple_metadata   = data_blocks * tuple_size * descriptor_pad
cut_point        = (block_size / descriptor_pad) * descriptor_pad
chunk_data_size  = cut_point - descriptor_pad
descriptor_blocks = ceil(tuple_metadata / chunk_data_size)
required_bytes   = (tuple_blocks + descriptor_blocks) * block_size
```

`tuple_blocks` counts the erasure-coded blocks (each data block produces
one tuple of `tuple_size` blocks: `tuple_size - 1` random blocks plus one
off block, all stored once). `tuple_metadata` is the descriptor buffer
size: each tuple appends `tuple_size * descriptor_pad` bytes to the
descriptor (`writeable_descriptor.c:152`). The descriptor buffer is
chunked into blocks of `chunk_data_size` bytes of payload (plus
`descriptor_pad` for the prior-hash chain), yielding `descriptor_blocks`
blocks. All blocks are `block_size` bytes in the cache.

This function does not touch the cache; it is pure arithmetic over the
layout parameters.

### 3. `client_api_wire` — optional `tuple_size` on PUT_REQUEST

**Files:** `src/ClientAPI/client_api_wire.h`, `src/ClientAPI/client_api_wire.c`

The `client_api_put_request_t` struct gains a `size_t tuple_size` field
plus a `uint8_t has_tuple_size` flag. The CBOR array grows from 8 to 9
elements; the 9th element is optional:

```
[type, content_type, file_name, stream_length, server_address, data,
 recycler_urls, temporary, tuple_size?]
```

- `tuple_size` absent (array length 8) → server uses default 3 (today's
  hardcoded behavior).
- `tuple_size` present (array length 9) → server uses the requested value,
  subject to the `max_tuple_size` cap.

`client_api_put_request_encode` writes the 9th element only when
`has_tuple_size != 0`. `client_api_put_request_decode` sets `has_tuple_size`
based on array length and reads `tuple_size` when present. Existing
clients that send 8-element arrays continue to work unchanged.

### 4. `_unix_handle_put` — pre-flight validation sequence

**File:** `src/ClientAPI/Unix/unix_connection.c` (around line 431-505)

When `PUT_REQUEST` arrives, the handler runs these checks in order, before
creating the writable stream or accepting any `PUT_DATA`:

1. **Decode + basic validation** — `client_api_put_request_decode` succeeds;
   `file_name` non-NULL, `stream_length` > 0.
2. **Resolve tuple_size** — if `msg.has_tuple_size`, use `msg.tuple_size`;
   else use 3 (the default).
3. **tuple_size bound** — if `tuple_size > config->max_tuple_size`, send
   `CLIENT_API_ERROR` with message `"tuple_size N exceeds max_tuple_size M"`
   and return. Do not set `put_streaming = 1`.
4. **Pre-flight space** — compute:
   ```c
   size_t required = writeable_off_stream_estimate_required_bytes(
       msg.stream_length, tuple_size, /*descriptor_pad=*/32);
   if (block_cache_can_fit(conn->bc, required) != CACHE_FIT_OK) {
     _unix_connection_send_error(conn, CLIENT_API_STATUS_INSUFFICIENT_STORAGE,
                                 "cache full: configure larger max_capacity_bytes");
     return;
   }
   ```
   Do not set `put_streaming = 1`.
5. **Create stream** — pass the resolved `tuple_size` (not the hardcoded 3)
   to `writeable_off_stream_create` and `writeable_descriptor_create`. Now
   set `conn->put_streaming = 1`.

The same validation sequence applies to the other connection handlers
(TCP, WS, WT, HTTP `off_routes`) that create writable streams. Each
currently hardcodes `tuple_size = 3`; each gains the ORI-preamble read and
the pre-flight check. This is mechanical replication, not new design.

**`PUT_DATA` guard:** `_unix_handle_put_data` and `_unix_handle_put_end`
already check `conn->put_streaming` (lines 525, 552). If a client sends
`PUT_DATA` after a pre-flight rejection (when `put_streaming == 0`), the
frame is dropped. This is existing behavior; no change needed.

### 5. `writeable_off_stream` and `writeable_descriptor` — mid-stream error propagation

**Files:** `src/OFFStreams/writeable_off_stream.c`, `src/OFFStreams/writeable_descriptor.c`

**5a. Always set `reply_to`:**

`writeable_off_stream.c:91` — replace:
```c
actor_t* reply_to = (stream->network != NULL) ? &stream->stream.actor : NULL;
```
with:
```c
actor_t* reply_to = &stream->stream.actor;
```

`writeable_descriptor.c:101` — same change.

Both streams now receive every `CACHE_PUT_RESULT`, whether or not a
network is attached. The `CACHE_PUT_NEW` network-announce path keeps its
`stream->network != NULL` guard — only the `reply_to` assignment becomes
unconditional.

**5b. Handle failure in `CACHE_PUT_RESULT`:**

`writeable_off_stream.c:306-321` — extend the `CACHE_PUT_RESULT` handler:
```c
case CACHE_PUT_RESULT: {
  cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
  if (result->result == CACHE_PUT_ERROR || result->result == CACHE_PUT_FULL) {
    stream->stream.is_deactivated = 1;
    stream_notify((stream_t*)stream, error_event,
                  OFFS_ERROR("cache full during put: configure larger max_capacity_bytes"),
                  NULL);
    break;
  }
  if (result->result == CACHE_PUT_NEW && stream->network != NULL) {
    /* existing network announce path unchanged */
    ...
  }
  break;
}
```

`writeable_descriptor.c:165-167` — same extension. The descriptor fires
its own `error_event` on cache failure; the pipe handler (see 5d) routes
it to the client.

**5d. Pipe error handler routes the message:**

`src/ClientAPI/Unix/unix_connection.c:324-329` — the existing
`_unix_put_on_stream_error` already sends `CLIENT_API_ERROR` and
deactivates the stream. Change it to forward the actual error message
from the `error_event` payload instead of the hardcoded `"PUT stream
error"`:

```c
static void _unix_put_on_stream_error(void* ctx, void* error) {
  unix_put_pipeline_t* pipeline = (unix_put_pipeline_t*)ctx;
  const char* message = (error != NULL) ? (const char*)error : "PUT stream error";
  _unix_connection_send_error(pipeline->connection, CLIENT_API_STATUS_INTERNAL_ERROR, message);
  stream_deactivate((stream_t*)pipeline->ws, NULL);
}
```

`OFFS_ERROR(...)` produces a heap-allocated string; the handler treats the
`error` payload as a `const char*`. If the payload is NULL (defensive),
fall back to the generic message. The streams' `error_event` subscribers
in the other connection handlers (TCP, WS, WT) get the same forwarding
change.

### 6. OFFS CLI — `--tuple-size` flag

**Files:** `src/offs/commands/put.c`, `src/offs/l10n/en.h`

`offs put` gains `--tuple-size N` (integer, default 3). The CLI sets
`has_tuple_size = 1` and `tuple_size = N` on the `PUT_REQUEST` frame when
the flag is present. No `--block-type` flag (block type stays `standard`).

Error handling: the existing `PUT_RESPONSE | ERROR` dispatch in `cmd_put`
already prints `Error: <message>` to stderr and exits 1. No change needed
beyond verifying the error path handles the new pre-flight rejection
cleanly (it does — the frame is a standard `CLIENT_API_ERROR`).

## Error messages

Two distinct messages so the user can tell which gate fired:

- **Pre-flight (section 4, step 4):** `"cache full: configure larger max_capacity_bytes"`
  — proactive, before any data is uploaded.
- **Mid-stream (section 5b):** `"cache full during put: configure larger max_capacity_bytes"`
  — reactive, mid-upload.
- **tuple_size bound (section 4, step 3):** `"tuple_size N exceeds max_tuple_size M"`
  — validation, before any data is uploaded.

All three are delivered as `CLIENT_API_ERROR` frames. The CLI prints them
prefixed with `"Error: "` per the existing `cmd_put` error path.

## Testing

### Unit tests

- `block_cache_can_fit` — returns `CACHE_FIT_OK` when
  `current + required <= max`, `CACHE_FIT_FULL` when
  `current + required > max`, `CACHE_FIT_OK` when `max == 0` (disabled).
- `writeable_off_stream_estimate_required_bytes` — returns the expected
  value for known `(stream_length, tuple_size, descriptor_pad)` triples;
  verify the formula against an actual small PUT's block count.

### Integration tests

- **Pre-flight rejection:** configure `max_capacity_bytes` to 1 MiB, PUT a
  10 MiB file. Expect `CLIENT_API_ERROR` with `"cache full: configure
  larger max_capacity_bytes"` before any `PUT_DATA` is accepted. Verify
  no blocks were stored (`block_cache->current_bytes` unchanged).
- **tuple_size bound:** PUT with `--tuple-size 100` against
  `max_tuple_size = 5`. Expect `CLIENT_API_ERROR` with
  `"tuple_size 100 exceeds max_tuple_size 5"`.
- **Mid-stream failure:** configure `max_capacity_bytes` to a value that
  passes the pre-flight but fails partway through (e.g. by corrupting one
  section's `free_map` mid-PUT). Expect `CLIENT_API_ERROR` with
  `"cache full during put: ..."` and the client exits 1.
- **Backward-compatible wire:** an 8-element `PUT_REQUEST` (no `tuple_size`)
  from an old client still works; server uses default 3.
- **No regression:** existing PUT/GET round-trip tests pass; existing
  non-streaming commands (`config show`, `peer info`, `status`, `health`)
  pass.
- **Concurrency:** two concurrent PUTs of the same file. The first gets
  `CACHE_PUT_NEW` for its blocks; the second gets `CACHE_PUT_EXISTS`. If
  the second fails and the first succeeds, the first's ORI must still
  resolve (no cleanup-on-failure → no corruption).

### Manual test

- Start daemon with `--cache-dir /tmp/offs-cache --data-dir /tmp/offs-data
  --unix /tmp/offs.sock`.
- PUT a file larger than `max_capacity_bytes` (default 5 GiB): `offs put
  /path/to/big-file`. Expect `Error: cache full: configure larger
  max_capacity_bytes` and exit 1.
- PUT a small file: `offs put /etc/hostname`. Expect ORI printed, exit 0.
- GET the small file's ORI: `offs get <ori> --output /tmp/out`. Expect
  files identical.

## Out of scope

- No on-disk section cleanup (defragmentation handles reclamation).
- No cleanup of orphaned blocks on PUT failure (content-addressed blocks
  may be shared; unsafe to delete without per-PUT ownership tracking).
- No change to `block_type` (stays `standard` in OFFS).
- No change to `descriptor_pad` (stays server-side).
- No resumable uploads.
- No change to other CLI commands.
- No change to the GET path.