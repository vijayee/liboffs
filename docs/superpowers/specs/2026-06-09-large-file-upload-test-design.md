# Large-File Upload + Download Integration Test — Design

**Date:** 2026-06-09
**Status:** Draft
**Related:** `test/test_offs_client_integration.cpp`, `test/test_offs_client.cpp`, `test/test_file_transfer_integration.cpp`, `src/ClientLibs/c/offs_client.c`

## Problem

The existing `offs_client` integration test (`test_offs_client_integration.cpp`) only exercises round-trips up to 1 MB and only covers PUT + GET, never streaming PUT/GET at scale. The library supports both:

- Streaming PUT (`offs_client_put_stream_start` / `_data` / `_end`)
- Streaming GET (`offs_client_get` with `data` / `end` / `error` callbacks)

A 1.77 GB `.mp4` is available on the workstation, but no test verifies that:

- The streaming PUT path actually carries multi-gigabyte uploads end-to-end through `offs_client` and the server transport.
- The server-computed BLAKE3 `file_hash` (returned embedded in the ORI string) matches what the client sent.
- The streaming GET path actually carries multi-gigabyte downloads back through the client.
- All four non-HTTP transports (Unix socket, TCP, WebSocket, WebTransport/msquic) work end-to-end at this scale.

## Approach

A new C++ test executable `test/test_large_file_upload.cpp` that uses the same fork-self pattern as `test_offs_client_integration.cpp`, but covers the full PUT-then-GET round-trip across all four non-HTTP transports, each test case handling a 1.77 GB file end-to-end.

- **Child process** runs the chosen transport (Unix / TCP / WS / WT) on a temp socket/port, with a `block_cache_t`, `ofd_cache_t`, `tuple_cache_t`, `scheduler_pool_t`, and `timer_actor_t` set up identically to the existing integration test. The `--transport` flag in the node-mode `main` selects which transport to start.
- **Parent process** (the GoogleTest fixture) connects to the node, computes a local BLAKE3 of the source file in 64 KB `fread` chunks, performs a streaming PUT of the entire file, parses the BLAKE3 `file_hash` from the returned ORI, base58-decodes it back to 32 bytes, and asserts it matches. Then it performs a streaming GET of the same content, writing each `data` callback's bytes to a temp file on disk, and byte-compares the temp file against the source.
- **Test fixture variants**: four `TEST_F` cases, one per transport. Each uses the same `run_round_trip(url, source_path, expected_size, expected_hash)` helper, only the node-starting code differs.
- **WebTransport (WT)** needs TLS certs; the fixture generates them on the fly with `openssl req -x509 -newkey rsa:2048 -days 1 -nodes -subj /CN=liboffs-test` into the test temp dir, matching the pattern from `test/test_file_transfer_integration.cpp:129`.

The on-disk GET is used (rather than buffering the whole 1.77 GB in RAM) so the test doesn't allocate nearly 2 GB just for the GET buffer. Temp files are deleted at the end of each test case in `TearDown`.

## Architecture

```
+--------------+  unix / tcp / ws / wt  +--------------+
|  Parent      | <--------------------> | Child        |
|  (gtest)     |                        | (--mode=node)|
|              |   1. stat() the .mp4   |              |
|              |   2. fread 64KB ->     |  scheduler_  |
|              |      local BLAKE3      |  pool +      |
|              |   3. put_stream_start  |  timer       |
|              |   4. fread 64KB ->     |  block_cache |
|              |      put_stream_data   |  ofd_cache   |
|              |   5. put_stream_end    |  tuple_cache |
|              |   6. parse ORI hash    |  unix/       |
|              |   7. get() -> fwrite   |  tcp/ws/wt   |
|              |      to temp file      |  _transport_ |
|              |   8. memcmp files      |  _start      |
|              |   9. unlink temp file  |              |
+--------------+                        +--------------+
```

The parent's flow runs once per test case. The node child starts, the parent connects, and on test success the parent tears down the connection, kills the child, and removes the temp file.

## Data flow

### Test fixture `SetUp`

1. `mkdtemp("/tmp/largefile-upload-XXXXXX")` → `test_dir`.
2. `mkdir cache/`.
3. `stat(kSourceFile, &st)`. If missing → `GTEST_SKIP("Source file not present at <path>")`. If size 0 → `FAIL()`.
4. Record `file_size` from `st.st_size`.
5. If `--transport=wt`: `generate_test_certs()` runs `openssl req -x509 -newkey rsa:2048 -keyout <dir>/test_key.pem -out <dir>/test_cert.pem -days 1 -nodes -subj /CN=liboffs-test 2>/dev/null` and asserts the exit code is 0.

### Per-test-case `Mp4_RoundTrip_<Transport>`

Each test case is `TEST_F(LargeFileUploadTest, Mp4_RoundTrip_UnixSocket)` etc.

1. `compute_local_blake3(kSourceFile, expected_hash[32])` — 64 KB `fread` loop, `blake3_hasher_update` per chunk, finalize.
2. Call the transport-specific node starter: `start_unix_node()`, `start_tcp_node()`, `start_ws_node()`, or `start_wt_node()`. Each forks a child that `execl`s the test binary itself with `--mode=node --transport=<x> --socket/port <addr> --cache-dir <dir> [--cert <p> --key <p> for WT]`. Each starter polls for readiness (socket file appears, or TCP connect succeeds).
3. Build the URL: `unix://<path>`, `tcp://127.0.0.1:<port>`, `ws://127.0.0.1:<port>`, or `wt://127.0.0.1:<port>`.
4. `offs_client_connect(url, NULL)`. On NULL → `FAIL()`.
5. **PUT**:
   - `offs_client_put_stream_start(client, "video/mp4", file_name, file_size)`.
   - Loop: `fread(buf, 1, 65536, fp)` → `offs_client_put_stream_data(client, buf, n)` until `n == 0`. `ASSERT_EQ` on each call's return value.
   - `offs_client_put_stream_end(client, on_put, &put_ctx)`.
   - `wait_sem(&put_ctx.sem, 600000)` (10-minute PUT timeout). `ASSERT_EQ` on the return.
6. **PUT hash verification**:
   - `parse_file_hash_from_ori(ori.c_str(), server_hash[32])`. On parse failure → `FAIL()`.
   - `EXPECT_EQ(memcmp(expected_hash, server_hash, 32), 0)` with both digests printed in hex on failure.
7. **GET**:
   - `mkstemp` a temp filename in `test_dir` (e.g. `test_dir/dl-XXXXXX.mp4`).
   - Call `offs_client_get(client, ori.c_str(), on_get_data_to_file, on_get_end, on_get_error, &get_ctx)`. `get_ctx` holds the `FILE*` to the temp file.
   - `on_get_data_to_file` calls `fwrite(data, 1, len, fp)`, tracking total bytes written.
   - `on_get_end` and `on_get_error` post the semaphore.
   - `wait_sem(&get_ctx.sem, 600000)`. If `error_called` → `FAIL()` with status.
8. **GET byte-comparison**:
   - `fflush(get_ctx.fp)`, `fclose(get_ctx.fp)`.
   - Open both the source file and the downloaded temp file, `fstat` them, assert equal sizes.
   - For efficiency, compare in 1 MB chunks: read 1 MB from each, `memcmp`. On any mismatch, print the byte offset and quit.
   - If both files match all the way through: `EXPECT_EQ` passes.
9. `offs_client_disconnect(client)`.
10. `TearDown` removes the temp file and the test_dir.

### Test fixture `TearDown`

- If `node_pid > 0`: `kill(SIGTERM)`, `waitpid` in 100 ms increments for up to 2 s, then `SIGKILL` and `waitpid` again. Set `node_pid = 0`.
- If `get_ctx.fp` is still open: `fclose` it.
- `unlink(download_path)` if it still exists.
- `rm_rf(test_dir)`.

## Components reused

| Component | Source | Purpose |
|---|---|---|
| `offs_client_t` + streaming PUT + GET | `src/ClientLibs/c/offs_client.{h,c}` | Connect, streaming PUT, streaming GET |
| `unix_transport_t` | `src/ClientAPI/Unix/unix_transport.{h,c}` | Unix socket transport |
| `tcp_transport_t` | `src/ClientAPI/TCP/tcp_transport.{h,c}` | TCP transport |
| `ws_transport_t` | `src/ClientAPI/WS/ws_transport.{h,c}` | WebSocket transport |
| `wt_transport_t` | `src/ClientAPI/WT/wt_transport.{h,c}` | WebTransport (QUIC) transport — gated on `HAS_MSQUIC` |
| `block_cache_t`, `ofd_cache_t`, `tuple_cache_t` | `src/BlockCache/`, `src/OFFStreams/` | Same node setup as the existing test |
| `scheduler_pool_t`, `timer_actor_t` | `src/Scheduler/`, `src/Timer/` | Same node setup as the existing test |
| `base58_encode` / `base58_decode` | `src/Util/base58.h` | Decode the ORI's file_hash segment |
| `blake3_hasher_*` | `deps/BLAKE3/c/blake3.h` | Compute the local BLAKE3 |
| `rm_rf` | `src/Util/rm_rf.h` | Temp-dir cleanup |
| `openssl req` (via `system()`) | test fixture | Self-signed cert for WT, only when the WT test case runs |

No new infrastructure is introduced. No source files in `src/` are modified. The WT test case is gated on `HAS_MSQUIC` so a build without msquic compiles and skips the WT case.

## New code

- `test/test_large_file_upload.cpp` — single self-contained test file with helpers and a `main` that branches on `--mode=node` and `--transport=...` exactly like `test_offs_client_integration.cpp` does, but extended to all four transports.

### Helper functions (local to the test file)

- `compute_local_blake3(const char* path, uint8_t out[32])` — 64 KB `fread` loop, returns 0 on success, -1 on open/read error.
- `parse_file_hash_from_ori(const char* ori, uint8_t out[32])` — scans the ORI string for the first base58 segment after the content-type/length prefix, decodes it. Returns 0 on success, -1 on parse/decode failure.
- `on_put`, `wait_sem` — identical to the helpers in `test_offs_client_integration.cpp`.
- `on_get_data_to_file(void* ctx, const uint8_t* data, size_t len)` — `fwrite` into the `FILE*` carried in `ctx`, atomically increment the byte counter.
- `on_get_end(void* ctx)`, `on_get_error(void* ctx, uint8_t status, const char* msg)` — post the semaphore, set end/error flags.
- `generate_test_certs()` — runs `openssl req -x509 -newkey rsa:2048 ...` into `test_dir/test_cert.pem` and `test_dir/test_key.pem`, asserts exit 0.
- `run_unix_node(socket_path, cache_dir)`, `run_tcp_node(port, cache_dir)`, `run_ws_node(port, cache_dir)`, `run_wt_node(port, cache_dir, cert_path, key_path)` — mirror the `run_*_node` functions in `test_offs_client_integration.cpp:131-260` and the `wt_transport_create` signature in `src/ClientAPI/WT/wt_transport.h:62`.
- `start_unix_node()`, `start_tcp_node()`, `start_ws_node()`, `start_wt_node()` — fork + execl the test binary in `--mode=node`, wait for readiness, store the child pid.
- `run_round_trip(url, source_path, file_size, expected_hash)` — the actual test body: PUT, hash-verify, GET to temp file, byte-compare, cleanup. Identical across transports.

## Error handling

| Condition | Behavior |
|---|---|
| Source file missing | `GTEST_SKIP()` once in `SetUp`; no transport test runs |
| `stat` fails or size 0 | `FAIL()` in `SetUp` |
| `generate_test_certs` (`openssl`) returns non-zero | `FAIL()` in `SetUp` for the WT test case only |
| `offs_client_connect` returns NULL | `FAIL()` |
| `offs_client_put_stream_start/data/end` returns nonzero | `FAIL()` with the return code |
| PUT callback not called within 10 min | `FAIL()` "PUT timed out" |
| `parse_file_hash_from_ori` fails | `FAIL()` printing the raw ORI |
| BLAKE3 `memcmp` fails | `FAIL()` printing both digests in hex |
| GET `on_get_error` called | `FAIL()` with the error status |
| GET callback not called within 10 min | `FAIL()` "GET timed out" |
| Downloaded file size != source size | `FAIL()` printing both sizes |
| Byte mismatch at offset N | `FAIL()` printing the offset and the actual byte values from both files |
| Node child doesn't become ready in 5 s | `FAIL()` |

## Test target

- File: `/home/victor/Videos/Big Hero 6 2014 1080p/Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4`
- Size: 1,766,948,530 bytes (1.77 GB)
- Content type sent: `video/mp4`
- File name sent: `Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4`
- Skips cleanly when the file is absent.

## Test cases

Four `TEST_F` cases, one per transport. The body of each is two lines: call the matching `start_*_node()` and then call `run_round_trip(...)` with the matching URL.

- `LargeFileUploadTest.Mp4_RoundTrip_UnixSocket` — `unix://<path>`
- `LargeFileUploadTest.Mp4_RoundTrip_Tcp` — `tcp://127.0.0.1:<port>`
- `LargeFileUploadTest.Mp4_RoundTrip_WebSocket` — `ws://127.0.0.1:<port>`
- `LargeFileUploadTest.Mp4_RoundTrip_WebTransport` — `wt://127.0.0.1:<port>` — gated on `HAS_MSQUIC`; if `HAS_MSQUIC` is not defined, the case is `#if`-stripped so the file still compiles on non-msquic builds.

## Estimated runtime

Per transport: PUT 1.77 GB + GET 1.77 GB on localhost. On a modern workstation, streaming PUT sustains several hundred MB/s on a Unix socket; GET is similar. Rough per-transport time: 30-90 s. With four transports: 2-6 minutes total. The 10-minute timeout per PUT/GET is generous.

## CMake changes

Append a new executable block to `test/CMakeLists.txt`, mirroring the existing `test_offs_client_integration` block plus the `deps/blake3` include and the `HAS_MSQUIC` define + msquic includes/libs (already present in that block).

New target name: `test_large_file_upload`. The same `gtest_add_tests` discovery picks up the new tests automatically; no other CMake changes needed.

## Why GET to disk and not to memory

A 1.77 GB allocation in the test process would be expensive (allocator fragmentation, system pressure on CI) and would duplicate the source file's contents in RAM. Writing the GET callbacks to a temp file keeps the test memory footprint flat and follows how a real client would consume a large download (write to a file, compare later). The byte-compare is then done in 1 MB chunks so we never hold more than 1 MB in memory at a time.
