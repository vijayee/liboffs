# Large-File Upload Integration Test — Design

**Date:** 2026-06-09
**Status:** Draft
**Related:** `test/test_offs_client_integration.cpp`, `test/test_offs_client.cpp`, `src/ClientLibs/c/offs_client.c`

## Problem

The existing `offs_client` integration test (`test_offs_client_integration.cpp`) only exercises round-trips up to 1 MB. The library supports streaming PUT (`offs_client_put_stream_start` / `_data` / `_end`) and a real-world 1.77 GB `.mp4` is available on the workstation, but no test verifies that:

- The streaming PUT path actually carries multi-gigabyte uploads end-to-end through `offs_client` and the `unix_transport_t` server.
- The server-computed BLAKE3 `file_hash` (returned embedded in the ORI string) matches what the client sent.

The point is to exercise the real upload pipeline at a size that the buffered single-shot `offs_client_put` cannot handle (it would need the whole file in RAM), and to validate byte integrity without paying the cost of a full GET download.

## Approach

A new C++ test executable `test/test_large_file_upload.cpp` that uses the same fork-self pattern as `test_offs_client_integration.cpp`:

- **Child process** runs a `unix_transport_t` node on a temp socket, with a `block_cache_t`, `ofd_cache_t`, `tuple_cache_t`, `scheduler_pool_t`, and `timer_actor_t` set up identically to the existing integration test.
- **Parent process** (the GoogleTest fixture) connects to the node over the Unix socket, computes a local BLAKE3 of the source file in 64 KB `fread` chunks, then performs a streaming PUT of the entire file. After the PUT returns, it extracts the BLAKE3 `file_hash` from the returned ORI string, base58-decodes it back to 32 bytes, and asserts it matches the locally computed digest.

No GET is performed. The hash comparison gives the same byte-integrity guarantee as a full download-and-compare at a tiny fraction of the cost: any byte the server hashed differently from what the client sent would produce a divergent BLAKE3.

## Architecture

```
+----------------------------+        unix socket        +----------------------------+
|       Parent (gtest)       | <-----------------------> |   Child (--mode=node)      |
|                            |                            |                            |
| 1. stat() the .mp4         |                            |  scheduler_pool + timer    |
| 2. fread 64KB -> BLAKE3    |                            |  block_cache + ofd_cache   |
| 3. connect unix://...      |                            |  tuple_cache               |
| 4. put_stream_start        |                            |  unix_transport_start      |
| 5. fread 64KB -> data      |                            |     [server: writeable_    |
|    (loop)                  |                            |      off_stream per chunk] |
| 6. put_stream_end          |                            |  ...returns ORI with BLAKE3|
| 7. parse + decode ORI hash |                            |                            |
| 8. memcmp to local hash    |                            |                            |
+----------------------------+                            +----------------------------+
```

## Data flow

1. `SetUp()` calls `mkdtemp("/tmp/largefile-upload-XXXXXX")`, creates `cache/`, and `stat()`s the source file.
2. `fork()` and `execl(self_path, "--mode=node", "--transport=unix", "--socket", path, "--cache-dir", dir, NULL)` in the child.
3. Parent polls `access(socket_path, F_OK)` for up to 5 s, then `offs_client_connect("unix://<path>", NULL)`.
4. Local hash: `blake3_hasher_init`; loop `fread(buf, 1, 65536, fp)` and `blake3_hasher_update`; `blake3_hasher_finalize` to 32 bytes.
5. Streaming PUT:
   - `offs_client_put_stream_start(client, "video/mp4", file_name, file_size)`.
   - Loop: `fread(buf, 1, 65536, fp)` → `offs_client_put_stream_data(client, buf, n)` until `n == 0`.
   - `offs_client_put_stream_end(client, on_put, &put_ctx)`; `wait_sem(&put_ctx.sem, 600000)` (10 minute timeout).
6. Parse ORI string. The format is documented in `src/OFFStreams/off_url.c`:
   ```
   <server>/offsystem/v3/<content_type>/<stream_length>/<file_hash_b58>/<descriptor_hash_b58>/<file_name>
   ```
   Skip everything up to and including `/offsystem/v3/`. Read the content-type segment up to `/<digits>/`. The first base58 segment after that is the `file_hash` (BLAKE3, 32 bytes). Pass it to `base58_decode` with a 32-byte output buffer; assert it produced exactly 32 bytes.
7. `memcmp(local_blake3, decoded_file_hash, 32) == 0`.
8. `TearDown()` sends `SIGTERM` to the child, `waitpid`s, then `rm_rf` the temp dir.

## Components reused

| Component | Source | Purpose |
|---|---|---|
| `offs_client_t` + streaming API | `src/ClientLibs/c/offs_client.{h,c}` | Connect, PUT, streaming PUT |
| `unix_transport_t` | `src/ClientAPI/Unix/unix_transport.{h,c}` | Server-side transport |
| `block_cache_t`, `ofd_cache_t`, `tuple_cache_t` | `src/BlockCache/`, `src/OFFStreams/` | Same node setup as the existing test |
| `scheduler_pool_t`, `timer_actor_t` | `src/Scheduler/`, `src/Timer/` | Same node setup as the existing test |
| `base58_encode` / `base58_decode` | `src/Util/base58.h` | Decode the ORI's file_hash segment |
| `blake3_hasher_*` | `deps/BLAKE3/c/blake3.h` | Compute the local BLAKE3 |
| `rm_rf` | `src/Util/rm_rf.h` | Temp-dir cleanup |

No new infrastructure is introduced. No source files in `src/` are modified.

## New code

- `test/test_large_file_upload.cpp` — single self-contained test file with helpers and a `main` that branches on `--mode=node` exactly like `test_offs_client_integration.cpp`.

### Helper functions (local to the test file)

- `compute_local_blake3(const char* path, uint8_t out[32])` — opens the file, `fread`s in 64 KB chunks, calls `blake3_hasher_update` on each chunk, finalizes into `out`. Returns 0 on success, -1 on open failure.
- `parse_file_hash_from_ori(const char* ori, uint8_t out[32])` — scans the ORI string for the first base58 segment that follows the stream-length segment, decodes it with `base58_decode`. Returns 0 on success, -1 on parse/decode failure.
- `on_put`, `wait_sem` — identical to the helpers in `test_offs_client_integration.cpp`.

## Error handling

| Condition | Behavior |
|---|---|
| Source file missing | `GTEST_SKIP()` with a clear message ("Source file not present at <path>") |
| `stat` fails or size == 0 | `FAIL()` with errno |
| `offs_client_connect` returns NULL | `FAIL()` |
| `offs_client_put_stream_start/data/end` returns nonzero | `FAIL()` with the return code |
| PUT callback not called within 10 minutes | `FAIL()` "PUT timed out" |
| ORI string empty or unparseable | `FAIL()` "ORI malformed" |
| `base58_decode` returns nonzero or writes != 32 bytes | `FAIL()` "file_hash decode failed" |
| `memcmp` of the two digests fails | `FAIL()` printing both digests in hex |

When the digests mismatch, the test prints `expected:` and `actual:` lines with the 32-byte digests formatted as 64 hex chars. The `fread` loop is the only place that ever touches the source bytes on the client side, so any divergence is by construction a defect in the streaming PUT path on either the client (`offs_client_put_stream_data`) or the server (`unix_connection.c: _unix_put_on_stream_data` / `writeable_off_stream_write`).

## Test target

- File: `/home/victor/Videos/Big Hero 6 2014 1080p/Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4`
- Size: 1,766,948,530 bytes (verified with `ls -la`)
- Content type sent: `video/mp4`
- File name sent: `Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4`

The file path is hard-coded in the test. On machines without the file (CI, headless builds), the test skips cleanly rather than failing.

## CMake changes

Append a new executable block to `test/CMakeLists.txt`, mirroring the existing `test_offs_client_integration` block (whole-archive `offs` link, `cbor`, `blake3`, `hashmap`, `http-parser`, `GTest::gtest_main`, `GTest::gmock`, `pthread`, includes for `src/`, `deps/blake3`, gtest headers, and the `HAS_MSQUIC` define if `deps/msquic` is present).

New target name: `test_large_file_upload`. No CTest registration changes needed — `gtest_add_tests` already discovers tests in every executable, and the existing `test_offs_client_integration` shows the pattern works for self-forking binaries.

## Test naming

- File: `test/test_large_file_upload.cpp`
- Binary: `test_large_file_upload`
- Single test case: `LargeFileUpload.Mp4_HashMatches`

## Why no GET?

A full GET of 1.77 GB on localhost would add 3-5× runtime, allocate 1.77 GB of test memory, and provide no additional correctness signal: a BLAKE3 mismatch is mathematically equivalent to a single-byte difference. If a separate streaming-download test is later wanted, it can be added as a sibling case that reuses this same fixture.
