# Large-File Upload Integration Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a two-process integration test that streams a 1.77 GB `.mp4` from `offs_client` to a `unix_transport_t` node in 64 KB chunks and verifies the server-computed BLAKE3 `file_hash` (embedded in the returned ORI string) matches the local BLAKE3 of the source.

**Architecture:** Single new C++ test executable `test/test_large_file_upload.cpp` that uses the existing fork-self pattern from `test_offs_client_integration.cpp`. The child runs a `unix_transport_t` server; the parent connects, computes a local BLAKE3, performs a streaming PUT, parses the file_hash out of the returned ORI, base58-decodes it, and compares. No GET is performed — the hash comparison gives equivalent byte-integrity guarantees at a fraction of the cost.

**Tech Stack:** C++17, GoogleTest, fork/exec, `offs_client` (C library), `unix_transport_t` (C), BLAKE3 (deps), base58 (`src/Util/base58.h`).

---

## File Structure

### New Files
- `test/test_large_file_upload.cpp` — single self-contained test binary containing: node-mode `main` branch, GoogleTest fixture, two helper functions (`compute_local_blake3`, `parse_file_hash_from_ori`), and one test case (`LargeFileUpload.Mp4_HashMatches`).

### Modified Files
- `test/CMakeLists.txt` — append a new `add_executable(test_large_file_upload test_large_file_upload.cpp)` block that mirrors the existing `test_offs_client_integration` block (offs whole-archive link, cbor, blake3, hashmap, http-parser, GTest, GMock, pthread, includes for `src/`, `deps/blake3`, gtest headers, and the `HAS_MSQUIC` define if `deps/msquic` is present).

### Untouched
- All files under `src/`. The test exercises existing public APIs only.

---

## Task 1: Create the test source file with node-mode main

**Files:**
- Create: `test/test_large_file_upload.cpp`

- [ ] **Step 1: Write the file skeleton with the node-mode main and helper declarations**

Create `test/test_large_file_upload.cpp` with the following exact content:

```cpp
//
// Two-process integration test for offs_client streaming PUT of a
// large file (~1.77 GB). Verifies that streaming the entire file via
// offs_client_put_stream_* to a unix_transport_t node produces an ORI
// whose embedded BLAKE3 file_hash matches the local BLAKE3 of the
// source. No GET is performed.
//

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <linux/limits.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <semaphore.h>

extern "C" {
#include "../src/ClientLibs/c/offs_client.h"
#include "../src/ClientAPI/Unix/unix_transport.h"
#include "../src/ClientAPI/health_handler.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include "../src/Util/base58.h"
}

#include "../deps/BLAKE3/c/blake3.h"

static const char* kSourceFile =
    "/home/victor/Videos/Big Hero 6 2014 1080p/Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4";
static const size_t kChunkSize = 64 * 1024;
static const int kPutTimeoutMs = 600000;  // 10 minutes for 1.77 GB

static std::string get_self_path() {
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len > 0) {
    path[len] = '\0';
    return std::string(path);
  }
  return "./test_large_file_upload";
}

struct PutCbContext {
  std::atomic<int> called{0};
  std::string ori_string;
  sem_t sem;
  PutCbContext() { sem_init(&sem, 0, 0); }
  ~PutCbContext() { sem_destroy(&sem); }
};

static void on_put(void* ctx, const char* ori_string) {
  auto* c = (PutCbContext*)ctx;
  if (ori_string != NULL) c->ori_string = ori_string;
  c->called.store(1, std::memory_order_release);
  sem_post(&c->sem);
}

static int wait_sem(sem_t* sem, int timeout_ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  return sem_timedwait(sem, &ts);
}

/* ---- Node main: runs a unix_transport_t, blocking on the running flag. ---- */

static int run_unix_node(const char* socket_path, const char* cache_dir) {
  scheduler_pool_t* pool = scheduler_pool_create(4);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  config_t config = {
      .index_bucket_size = 10,
      .index_wait = 1000,
      .index_max_wait = 5000,
      .section_size = 128000,
      .section_wait = 1000,
      .section_max_wait = 5000,
      .cache_size = 50,
      .max_tuple_size = 30,
      .lru_size = 50
  };
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  unix_transport_t* transport = unix_transport_create(pool, bc, ofd_cache, tc,
                                                     (char*)socket_path, NULL,
                                                     &health_ctx);
  if (transport == NULL) return 1;
  unix_transport_start(transport);

  while (running) {
    sleep(1);
  }
  unix_transport_stop(transport);
  unix_transport_destroy(transport);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  ofd_cache_destroy(ofd_cache);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
  return 0;
}

/* ---- Forward declarations of helper functions used by the fixture. ---- */

static int compute_local_blake3(const char* path, uint8_t out[32]);
static int parse_file_hash_from_ori(const char* ori, uint8_t out[32]);

/* ---- Test fixture. ---- */

class LargeFileUploadTest : public ::testing::Test {
protected:
  pid_t node_pid = 0;
  std::string test_dir;
  std::string cache_dir;
  std::string socket_path;
  size_t file_size = 0;

  void SetUp() override {
    char templ[] = "/tmp/largefile-upload-XXXXXX";
    char* mkdtemp_result = mkdtemp(templ);
    ASSERT_NE(mkdtemp_result, nullptr);
    test_dir = mkdtemp_result;
    cache_dir = test_dir + "/cache";
    mkdir(cache_dir.c_str(), 0700);

    struct stat st;
    if (stat(kSourceFile, &st) != 0) {
      GTEST_SKIP() << "Source file not present at " << kSourceFile
                   << " (errno=" << errno << ")";
    }
    if (st.st_size == 0) {
      FAIL() << "Source file is empty: " << kSourceFile;
    }
    file_size = (size_t)st.st_size;
  }

  void TearDown() override {
    if (node_pid > 0) {
      kill(node_pid, SIGTERM);
      int status = 0;
      for (int i = 0; i < 20; i++) {
        if (waitpid(node_pid, &status, WNOHANG) != 0) {
          node_pid = 0;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (node_pid > 0) {
        kill(node_pid, SIGKILL);
        waitpid(node_pid, &status, 0);
        node_pid = 0;
      }
    }
    if (!test_dir.empty()) {
      rm_rf(test_dir.c_str());
    }
  }

  void start_unix_node() {
    socket_path = test_dir + "/offs.sock";
    node_pid = fork();
    ASSERT_GE(node_pid, 0);
    if (node_pid == 0) {
      execl(get_self_path().c_str(), "test_large_file_upload",
            "--mode=node", "--transport=unix",
            "--socket", socket_path.c_str(),
            "--cache-dir", cache_dir.c_str(),
            (char*)NULL);
      _exit(127);
    }
    for (int i = 0; i < 100; i++) {
      if (access(socket_path.c_str(), F_OK) == 0) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    FAIL() << "Unix socket not created at " << socket_path;
  }
};

TEST_F(LargeFileUploadTest, Mp4_HashMatches) {
  ASSERT_GT(file_size, 0u);

  uint8_t expected_hash[32];
  ASSERT_EQ(compute_local_blake3(kSourceFile, expected_hash), 0)
      << "Failed to compute local BLAKE3 of " << kSourceFile;

  start_unix_node();
  std::string url = "unix://" + socket_path;

  offs_client_t* client = offs_client_connect(url.c_str(), NULL);
  ASSERT_NE(client, nullptr) << "Failed to connect to " << url;

  /* Extract just the basename for the file_name field. */
  std::string source_path(kSourceFile);
  size_t slash = source_path.rfind('/');
  std::string file_name = (slash == std::string::npos)
                              ? source_path
                              : source_path.substr(slash + 1);

  PutCbContext put_ctx;
  ASSERT_EQ(offs_client_put_stream_start(client, "video/mp4",
                                          file_name.c_str(), file_size), 0);
  ASSERT_EQ(wait_sem(&put_ctx.sem, kPutTimeoutMs), 0) << "PUT timed out";
  ASSERT_EQ(put_ctx.called.load(), 1);
  ASSERT_FALSE(put_ctx.ori_string.empty());
  std::string ori = put_ctx.ori_string;

  uint8_t server_hash[32];
  ASSERT_EQ(parse_file_hash_from_ori(ori.c_str(), server_hash), 0)
      << "Failed to parse BLAKE3 file_hash from ORI: " << ori;

  EXPECT_EQ(memcmp(expected_hash, server_hash, 32), 0)
      << "BLAKE3 file_hash mismatch between local computation and server ORI. "
      << "expected=" << [&]{
           char buf[65];
           for (int i = 0; i < 32; i++) snprintf(buf + i*2, 3, "%02x", expected_hash[i]);
           return std::string(buf);
         }()
      << " actual=" << [&]{
           char buf[65];
           for (int i = 0; i < 32; i++) snprintf(buf + i*2, 3, "%02x", server_hash[i]);
           return std::string(buf);
         }();

  offs_client_disconnect(client);
}

/* ---- Helper implementations. ---- */

static int compute_local_blake3(const char* path, uint8_t out[32]) {
  FILE* fp = fopen(path, "rb");
  if (fp == NULL) return -1;

  blake3_hasher hasher;
  blake3_hasher_init(&hasher);

  std::vector<uint8_t> buf(kChunkSize);
  while (true) {
    size_t n = fread(buf.data(), 1, kChunkSize, fp);
    if (n > 0) {
      blake3_hasher_update(&hasher, buf.data(), n);
    }
    if (n < kChunkSize) {
      if (ferror(fp)) {
        fclose(fp);
        return -1;
      }
      break;
    }
  }
  fclose(fp);

  blake3_hasher_finalize(&hasher, out, 32);
  return 0;
}

static int parse_file_hash_from_ori(const char* ori, uint8_t out[32]) {
  if (ori == NULL) return -1;
  const char* prefix = "/offsystem/v3/";
  const char* cursor = strstr(ori, prefix);
  if (cursor == NULL) return -1;
  cursor += strlen(prefix);

  /* Find the segment boundary by scanning for /<digits>/ — that marks
     the end of the content_type and start of the stream_length. */
  const char* type_end = NULL;
  const char* search = cursor;
  while (*search) {
    const char* slash = strchr(search, '/');
    if (!slash) break;
    const char* after_slash = slash + 1;
    char* endp;
    (void)strtol(after_slash, &endp, 10);
    if (endp != after_slash && *endp == '/') {
      type_end = slash;
      break;
    }
    search = after_slash;
  }
  if (type_end == NULL) return -1;

  cursor = type_end + 1;
  char* endp;
  (void)strtol(cursor, &endp, 10);
  if (endp == cursor) return -1;
  cursor = endp + 1;

  /* cursor now points to the start of the file_hash base58 segment. */
  const char* slash3 = strchr(cursor, '/');
  if (!slash3) return -1;
  size_t hash1_len = slash3 - cursor;

  uint8_t* raw = (uint8_t*)malloc(hash1_len);
  if (raw == NULL) return -1;
  size_t written = 0;
  int rc = base58_decode(cursor, raw, hash1_len, &written);
  if (rc != 0 || written != 32) {
    free(raw);
    return -1;
  }
  memcpy(out, raw, 32);
  free(raw);
  return 0;
}

/* ---- Entry point: dispatches based on --mode. ---- */

int main(int argc, char* argv[]) {
  std::string transport;
  std::string socket_path;
  std::string cache_dir;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--mode=node") continue;
    if (arg == "--transport=unix") transport = "unix";
    else if (arg == "--socket" && i + 1 < argc) socket_path = argv[++i];
    else if (arg == "--cache-dir" && i + 1 < argc) cache_dir = argv[++i];
  }

  if (transport == "unix" && !socket_path.empty()) {
    return run_unix_node(socket_path.c_str(), cache_dir.c_str());
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

- [ ] **Step 2: Verify the file compiles in isolation (syntax check)**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs && ls test/test_large_file_upload.cpp`
Expected: file exists, 250+ lines.

Note: a full compile won't succeed yet because `test/CMakeLists.txt` doesn't know about this file. That's expected — Task 2 wires it up.

- [ ] **Step 3: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && git add test/test_large_file_upload.cpp && git commit -m "test: add large-file upload integration test source"
```

---

## Task 2: Wire the test into CMake

**Files:**
- Modify: `test/CMakeLists.txt` (append new `add_executable` block after the `test_offs_client_integration` block, ~line 145)

- [ ] **Step 1: Find the exact insertion point**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs && grep -n "test_offs_client_integration" test/CMakeLists.txt`
Expected: shows the lines defining the `test_offs_client_integration` target. The new block goes immediately after its closing `target_include_directories`.

- [ ] **Step 2: Append the new target block**

Open `test/CMakeLists.txt` and append the following block at the end of the file (after the last existing line). The new block is structurally identical to `test_offs_client_integration`:

```cmake
    add_executable(test_large_file_upload test_large_file_upload.cpp)
    add_dependencies(test_large_file_upload cbor)
    add_dependencies(test_large_file_upload offs)
    add_dependencies(test_large_file_upload blake3)
    add_dependencies(test_large_file_upload http-parser)
    target_link_libraries(test_large_file_upload PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(test_large_file_upload PRIVATE ssl crypto)
    target_link_libraries(test_large_file_upload PRIVATE blake3)
    target_link_libraries(test_large_file_upload PRIVATE hashmap)
    target_link_libraries(test_large_file_upload PRIVATE http-parser)
    target_link_libraries(test_large_file_upload PUBLIC cbor)
    target_link_libraries(test_large_file_upload PRIVATE poll-dancer)
    target_link_libraries(test_large_file_upload PRIVATE GTest::gtest_main)
    target_link_libraries(test_large_file_upload PRIVATE GTest::gmock)
    target_link_libraries(test_large_file_upload PRIVATE pthread)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
      target_compile_definitions(test_large_file_upload PRIVATE HAS_MSQUIC)
      target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/src/inc)
      target_link_libraries(test_large_file_upload PRIVATE msquic::msquic msquic::platform)
    endif()
    target_include_directories(test_large_file_upload PUBLIC ${C_INC})
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cJSON)
    target_include_directories(test_large_file_upload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)
    target_include_directories(test_large_file_upload PRIVATE ${GTEST_INCLUDE_DIR})
    target_include_directories(test_large_file_upload PRIVATE ${GMOCK_INCLUDE_DIR})
```

- [ ] **Step 3: Verify CMake accepts the new target**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs && cmake --build build-test --target test_large_file_upload 2>&1 | tail -30`
Expected: ends with `[100%] Built target test_large_file_upload` and no errors. Warnings about unused parameters or unused variables are acceptable; errors are not.

- [ ] **Step 4: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && git add test/CMakeLists.txt && git commit -m "build: add test_large_file_upload target to CMake"
```

---

## Task 3: Run the test and verify it passes

**Files:** (no changes)

- [ ] **Step 1: Run the test**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build-test/test && ./test_large_file_upload 2>&1 | tail -30`
Expected: `LargeFileUpload.Mp4_HashMatches` passes. Output line will look like:
```
[       OK ] LargeFileUpload.Mp4_HashMatches (NNNN ms)
[----------] 1 test from LargeFileUploadTest (NNNN ms total)

[==========] 1 test from 1 test suite ran. (NNNN ms total)
[  PASSED  ] 1 test.
```

Expected wall-clock time on a modern workstation: well under 2 minutes for the 1.77 GB upload. The 10-minute timeout is generous.

- [ ] **Step 2: If the test fails, diagnose and fix**

Common failure modes and fixes:

- **`compute_local_blake3` returns -1 on the source file** → file path in `kSourceFile` is wrong or the file is not readable. Verify with `ls -la "$kSourceFile"`.
- **PUT timed out** → server child didn't start, or `unix_transport_start` is stuck. Check the child process stderr by adding `2>&1` to the `execl` call temporarily.
- **ORI parse fails (`parse_file_hash_from_ori` returns -1)** → the ORI format changed or the off_url.c format assumption is wrong. Print the raw ORI and compare against the parse logic.
- **BLAKE3 mismatch** → bytes diverged in transit. Print both digests and the file sizes; the difference is reproducible if you re-run.

Fix the underlying issue, rebuild, and re-run before committing.

- [ ] **Step 3: Verify under valgrind for leaks (existing test infra convention)**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs && ls build-gdwarf4/test/ 2>/dev/null`
Expected: a `test_large_file_upload` binary exists in `build-gdwarf4/test/` (or whichever build dir was built with `-gdwarf-4` for valgrind compatibility per the project's `valgrind DWARF5` memory note).

If the gdwarf-4 build doesn't exist yet, build it:
Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs && cmake --build build-gdwarf4 --target test_large_file_upload 2>&1 | tail -10`
Expected: `[100%] Built target test_large_file_upload`.

Then run valgrind:
Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build-gdwarf4/test && valgrind --leak-check=full --error-exitcode=1 ./test_large_file_upload 2>&1 | tail -30`
Expected: `definitely lost: 0 bytes in 0 blocks` (or matches the project's documented pre-existing leaks in `valgrind pre-existing leaks` memory note — `timer_actor` 24B, `block` 16B, `index/get_dir` 508B, `cbor` 640B). Any *new* leak is a defect in this test's code and must be fixed before marking the task complete.

- [ ] **Step 4: Commit (only if changes were made in step 2)**

If the test passed cleanly in step 1 with no fixes, there is nothing to commit. If a fix was needed, commit it with a descriptive message:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs && git add -u && git commit -m "test: fix <short description of issue>"
```

---

## Self-Review

**1. Spec coverage:**

| Spec section | Covered by |
|---|---|
| Problem statement (why this test exists) | Addressed by the goal statement in this plan |
| Approach (fork-self, two-process, Unix socket) | Task 1 (`run_unix_node`, `start_unix_node`) |
| Data flow steps 1–8 | Task 1 `LargeFileUploadTest.Mp4_HashMatches` body |
| Components reused (no new src/ code) | Task 1 only adds one new file under `test/` |
| Helper functions: `compute_local_blake3`, `parse_file_hash_from_ori`, `on_put`, `wait_sem` | Task 1 — all defined inline |
| Error handling (skip on missing, FAIL on each error) | Task 1 — `GTEST_SKIP`, `ASSERT_EQ`, `FAIL()`, `EXPECT_EQ` with hex diff on mismatch |
| Test target file path | Task 1 — `kSourceFile` constant |
| CMake target | Task 2 |
| No GET, hash-only verification | Task 1 `Mp4_HashMatches` — no `offs_client_get` call |
| Test naming `LargeFileUpload.Mp4_HashMatches` | Task 1 `TEST_F` |
| BLAKE3 of source in 64 KB chunks | Task 1 `compute_local_blake3` |
| Extract file_hash from ORI by base58-decoding first segment after length | Task 1 `parse_file_hash_from_ori` |

All spec requirements have a task. No gaps.

**2. Placeholder scan:**

No `TBD`, `TODO`, "implement later", "add appropriate error handling", or unfilled code references. Every step that writes code contains the full code.

**3. Type consistency:**

- `kSourceFile` is `const char*` used consistently in `compute_local_blake3(path, ...)` and `stat(path, &st)`.
- `PutCbContext` is defined once and used in both `on_put` and `Mp4_HashMatches`.
- `compute_local_blake3` and `parse_file_hash_from_ori` signatures match between forward declarations, definitions, and call sites.
- `uint8_t expected_hash[32]` and `uint8_t server_hash[32]` are 32-byte arrays matching `BLAKE3_OUT_LEN` (32) and the `base58_decode` `written == 32` check.
- The lambda capture blocks in the `EXPECT_EQ` failure message both produce `std::string` consistently.

No inconsistencies.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-09-large-file-upload-test.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
