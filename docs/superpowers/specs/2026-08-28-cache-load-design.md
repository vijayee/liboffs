# Cache Load Command — Design

Date: 2026-08-28
Status: Approved design (brainstorm session 2026-08-28)

## Goal

A **load** command on every client surface (HTTP, CBOR socket transports, C
client library, JS client library, Dart/Flutter binding, `offs` CLI) that
works like a GET but transfers no file data: the daemon resolves each of the
file's reconstructed data chunks (tuples) into its block cache — from local
cache or via the network wanted-list — without sending file data to the
client, and instead reports **tuple-level progress**
(`tuples_loaded / tuples_total`) as it goes.

Use case: "pin a file to a node without downloading it locally" — the
download-manager counterpart to GET.

Decisions made during brainstorming:

1. Progress metric is **tuple-level only**. Tuples are the meaningful unit
   (a tuple is reconstructable only when all `tuple_size` of its blocks are
   present); a per-block progress event was considered and dropped as
   unnecessary — the existing per-tuple render event already fires once per
   reconstructed tuple.
2. **Continue-and-report**: a tuple whose blocks never arrive (30 s
   network deadline) is *skipped*; the walk continues. Terminal status
   reflects the full outcome.
3. HTTP delivers progress as a **streaming `application/x-ndjson`**
   response; socket transports get dedicated frames.
4. HTTP reuses the **existing OFF URL with `?load=1`** (precedent:
   `?ofd=raw`) — no new route pattern, auth or CORS.
5. The Dart/Flutter binding is included in scope, **plus a catch-up of the
   QR peer surfaces it never received** (the QR feature shipped without
   binding updates).

## 1. Semantics

- `total_tuples = ceil(final_byte / block_size)` — computed from the parsed
  ORI with zero I/O (`readable_descriptor.c:327-328` already does this).
- A tuple is *loaded* when all `tuple_size` of its blocks resolve: either
  present in the block cache (`CACHE_GET_RESULT` with block) or fetched from
  the network (`NETWORK_FIND_BLOCK_RESULT` with found=1 — this path already
  verifies + `block_cache_put`, so arrival means cached).
- "Loaded into cache" is the persistent-index state; in-memory LRU eviction
  never loses a block.
- A tuple whose blocks don't resolve within the wanted-list deadline
  (default 30 s/block) is skipped; the dispatcher continues to the next
  tuple. Load-mode metrics tally skipped tuples.
- **Descriptor blocks are not skippable**: a missing *descriptor tree* block
  stops enumeration entirely (nothing downstream can be discovered), so a
  descriptor miss ends the load as `failed` with the tallies so far.
- Terminal statuses: `loaded` (every tuple loaded), `partial` (≥1 tuple
  skipped), `failed` (0 tuples loaded, or descriptor unrecoverable).
- Range requests are honored for partial loads (`[file_offset, final_byte)`
  restricts the enumerated tuple range exactly as GET does).

## 2. Internal design

Approach: **load mode on the existing GET pipeline** (approach chosen over a
standalone prober, which would fork the descriptor-walk + wanted-list
plumbing into a second codepath).

Changes to `src/OFFStreams/readable_off_stream.*`:

1. A `load_mode` construction/dispatch flag with two effects:
   - Block-resolution failure for a **data** tuple (network `found=0`) does
     not `stream_deactivate`; it aborts the current tuple, counts one
     skipped tuple, and continues with the next (`OFF_STREAM_WRITE` resume).
     Descriptor-node misses keep the existing teardown (see §1).
   - Each skipped/aborted tuple is tallied on the stream so the consumer can
     report it in the terminal event.
2. Tuple-completion signal: the plan must verify that the stream's existing
   `data_event` fires exactly once per reconstructed tuple (including at
   range-offset boundaries, where rendering trims bytes). If trimming
   batches or splits events, the fallback is one optional load-mode
   `tuple_done` event emitted from `_finish_decode_and_render` (the same
   place tuple completion is tallied today) — cheap and unambiguous. The
   consumer discards any payload and emits progress on the signal;
   `close_event` (stream walked to completion) and `error_event` (fatal)
   are reused as-is.
3. Normal GETs are untouched: default mode keeps `stream_deactivate` on
   miss.

New shared consumer (`src/ClientAPI/load_helpers.c` or similar, shared by
HTTP and socket transports so they cannot drift): given the pipeline
subscription, forwards per-tuple progress to the client and produces the
terminal tallies (`tuples_loaded`, `tuples_total`, status).

Concurrency note (accepted v1 limitation, same as GET): one tuple in flight
at a time. A file with many permanently-missing blocks completes in
`skipped × 30 s` worst case; wanted-list coalescing mitigates duplicates.
The progress stream makes this *visible*. Parallel multi-tuple prefetch is
v2, out of scope.

## 3. Wire protocol (`src/ClientAPI/client_api_wire.h/.c`)

| Frame | Shape |
|---|---|
| `LOAD_REQUEST 39` | `[39, ori_string, has_range?, range_start?, range_end?]` — same optional-range shape as `GET_REQUEST` |
| `LOAD_PROGRESS 40` | `[40, tuples_loaded: uint, tuples_total: uint]` (repeated, one per tuple resolution) |
| `LOAD_END 41` | `[41, status: uint, tuples_loaded: uint, tuples_total: uint]` — status 0=loaded, 1=partial, 2=failed |

Request-level failures (unparseable ORI, unauthorized) use the existing
`ERROR` frame `[11, status, message]`. Encode/decode functions follow the
existing naming convention
(`client_api_load_request_encode/decode`, etc.). All transports that
dispatch GET (unix, TCP, WS) dispatch LOAD frames the same way; WebTransport
follows its GET support status.

## 4. HTTP

`GET /offsystem/v3/{type}/{stream-length}/{file-hash}/{descriptor-hash}/{file-name}?load=1`

- Query flag `?load` switches the handler from data streaming to load mode
  (`?ofd=raw` precedent for query behavior on this route). Auth, CORS
  registration, and `Range` handling unchanged.
- Response: `Content-Type: application/x-ndjson` piped through the existing
  streaming response machinery. One progress line per tuple:
  `{"tuples_loaded":n,"tuples_total":m}\n`, then a terminal line:
  `{"status":"loaded|partial|failed","tuples_loaded":n,"tuples_total":m}`
  followed by end-of-body.
- Request-level failures keep normal HTTP statuses (400 bad URL, 404
  descriptor unrecoverable is expressed in-band as `failed` terminal event
  where possible; a URL that cannot even parse is 400 before the stream
  starts).

## 5. Client bindings (all kept in lockstep)

- **C client** (`src/ClientLibs/c/offs_client.h/.c`):
  `offs_client_load(ori_string, on_progress_cb, on_end_cb, on_error_cb, ctx)`
  — progress callback `(ctx, tuples_loaded, tuples_total)`, end callback
  `(ctx, status, tuples_loaded, tuples_total)`. Same lock/snapshot/dispatch
  pattern as the peer ops.
- **JS client** (`src/ClientLibs/js/offs-client/`):
  `load(ori, { onProgress(tuplesLoaded, tuplesTotal), onEnd(status, tuplesLoaded, tuplesTotal), onError })`
  — HTTP transport reads the ndjson stream via the existing chunked
  body machinery; CBOR transports use frames 39–41. `dist/` rebuilt.
- **Dart/Flutter binding** (`examples/off_client/lib/services/off_api.dart`):
  `load(String offUrl, {void Function(int, int)? onProgress})` returning the
  terminal result — via the HTTP ndjson stream. **Catch-up for QR** (never
  added in the QR feature): `connectPeerImage(Uint8List ppmBytes)` and
  `addFriendImage(Uint8List ppmBytes)` posting `image/x-portable-pixmap`
  bodies; `getPeerInfo(format: 'qrcode')` already works via the existing
  `format` param.
- **CLI** (`OFFS/src/offs/commands/load.c`): `offs load <ori>` printing
  `Loading <ori>: n/m tuples (pct)` on stderr (same pattern as `put`),
  exit 0 on loaded/partial (partial prints a warning histogram line),
  exit 1 on failed. HTTP-only option is unnecessary — the CLI ships over
  the existing unix socket.
- **GUI note**: the ndjson stream maps directly to a progress bar
  (`tuples_loaded/tuples_total`); terminal status distinguishes
  "available on this node" from "partially available".

## 6. Testing

- `readable_off_stream` load-mode unit tests: skip-on-tuple-timeout,
  descriptor-miss → failed, tallies correct, normal (non-load) mode
  unchanged.
- Wire tests: 39/40/41 encode/decode round-trips, optional-range variants,
  backward-compat (frames < 39 unaffected).
- HTTP route test: `?load=1` ndjson shape (progress lines + terminal line),
  content type, and normal GET unchanged on the same URL without the flag.
- Binding tests: JS unit for `load()` over a mock transport; Dart analyzer
  run on the binding change.
- E2E against a real daemon: load of a previously-uploaded file →
  `loaded` with full tallies; load of an ORI referencing garbage hashes →
  `failed`/`partial` behavior confirmed within the 30 s deadline window;
  CLI progress output.
- Valgrind with `-gdwarf-4` on new suites; ASAN if harnesses allow.

## 7. Out of scope

- Parallel multi-tuple prefetch (v2; wanted-list coalescing and progress
  streaming make sequential acceptable for v1).
- Block-level progress reporting (rejected in brainstorm — tuple-level only).
- Load-job persistence/retry across daemon restarts.
- A pollable job-id API (the streaming design removes the need).