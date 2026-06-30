# Replace openssl CLI with libcrypto in Integration Tests — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate all `system("openssl req ...")` and `system("rm -rf ...")` shell-outs from the POSIX integration tests by using the existing libcrypto-backed `ca_generate()` helper and `std::filesystem::remove_all`, so the suite runs on hosts without a working `openssl` CLI.

**Architecture:** `tools/offs-ca/ca_ops.h` already exposes `ca_generate(subject, days, key_type, cert_path, key_path)` — a libcrypto-backed function that produces a self-signed cert and key, equivalent to `openssl req -x509 -newkey rsa:2048 -keyout K -out C -days 1 -nodes -subj '/CN=...'`. `test_offs_client.cpp:1110` already migrated to it; this plan applies the same pattern to the two remaining integration test binaries and converts one stray `rm -rf` shell-out to `std::filesystem::remove_all`.

**Tech Stack:** C++17 (`std::filesystem`), GoogleTest, CMake, libcrypto via `OpenSSL::Crypto` (already linked), `tools/offs-ca/ca_ops.{c,h}`.

---

## File Structure

- **Modify** `test/CMakeLists.txt` — add `ca_ops.c` to `test_file_transfer_integration` and `test_rpc_integration` source lists + include directories.
- **Modify** `test/test_file_transfer_integration.cpp` — replace 1 openssl shell-out at line 136.
- **Modify** `test/test_rpc_integration.cpp` — replace 3 openssl shell-outs at lines 202, 228, 276; add `ca_ops.h` include.
- **Modify** `test/test_offs_ca.cpp` — replace 1 `rm -rf` shell-out at line 29 with `std::filesystem::remove_all`.

No new files. No production (`src/`) changes.

---

## Task 1: Wire `ca_ops.c` into the integration test binaries

**Files:**
- Modify: `test/CMakeLists.txt:182` (test_file_transfer_integration `add_executable`)
- Modify: `test/CMakeLists.txt:249` (test_rpc_integration `add_executable`)

- [ ] **Step 1: Add `ca_ops.c` to `test_file_transfer_integration` source list**

In `test/CMakeLists.txt`, change line 182 from:

```cmake
add_executable(test_file_transfer_integration test_file_transfer_integration.cpp test_node_main.c)
```

to:

```cmake
add_executable(test_file_transfer_integration test_file_transfer_integration.cpp test_node_main.c ${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca/ca_ops.c)
```

- [ ] **Step 2: Add `ca_ops.h` include directory to `test_file_transfer_integration`**

Find the `target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)` line (the last `target_include_directories` for this target, currently around line 222). Immediately after it, add:

```cmake
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca)
```

- [ ] **Step 3: Add `ca_ops.c` to `test_rpc_integration` source list**

Change line 249 from:

```cmake
add_executable(test_rpc_integration test_rpc_integration.cpp test_node_main.c)
```

to:

```cmake
add_executable(test_rpc_integration test_rpc_integration.cpp test_node_main.c ${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca/ca_ops.c)
```

- [ ] **Step 4: Add `ca_ops.h` include directory to `test_rpc_integration`**

Find the `target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)` line (currently around line 289). Immediately after it, add:

```cmake
    target_include_directories(test_rpc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca)
```

- [ ] **Step 5: Reconfigure and build to confirm no breakage**

Run from repo root:

```bash
cmake --build build-noasan -j$(nproc) 2>&1 | tail -10
```

Expected: builds successfully with exit 0. (The new `ca_ops.c` is added to the source list but not yet included by any test file, so behavior is unchanged — this just verifies the source file compiles and links in.)

- [ ] **Step 6: Commit**

```bash
git add test/CMakeLists.txt
git commit -m "build(test): compile ca_ops.c into integration test binaries"
```

---

## Task 2: Replace openssl shell-out in `test_file_transfer_integration.cpp`

**Files:**
- Modify: `test/test_file_transfer_integration.cpp:136` (in `generate_test_certs()`)
- Modify: `test/test_file_transfer_integration.cpp` top (add include)

- [ ] **Step 1: Add the `ca_ops.h` include**

In `test/test_file_transfer_integration.cpp`, find line 24:

```cpp
extern "C" int node_main(int argc, char* argv[]);
```

Immediately before it (after `#include "test_control_protocol.h"` on line 22), add:

```cpp
extern "C" {
#include "../tools/offs-ca/ca_ops.h"
}

extern "C" int node_main(int argc, char* argv[]);
```

- [ ] **Step 2: Replace the openssl shell-out**

In `test/test_file_transfer_integration.cpp`, find the `generate_test_certs()` method (line ~133):

```cpp
  void generate_test_certs() {
    cert_path = test_dir + "/test_cert.pem";
    key_path = test_dir + "/test_key.pem";
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path +
                      " -out " + cert_path +
                      " -days 1 -nodes -subj '/CN=liboffs-test' 2>/dev/null";
    ASSERT_EQ(system(cmd.c_str()), 0) << "Failed to generate test certificates";
  }
```

Replace with:

```cpp
  void generate_test_certs() {
    cert_path = test_dir + "/test_cert.pem";
    key_path = test_dir + "/test_key.pem";
    int rc = ca_generate("/CN=liboffs-test", 1, "rsa",
                         cert_path.c_str(), key_path.c_str());
    ASSERT_EQ(rc, 0) << "Failed to generate test certificates";
  }
```

- [ ] **Step 3: Rebuild**

```bash
cmake --build build-noasan -j$(nproc) 2>&1 | tail -5
```

Expected: exit 0.

- [ ] **Step 4: Run the test binary from its working directory**

```bash
cd build-noasan
./test/test_file_transfer_integration 2>&1 | tail -15
```

Expected: previously all 11 tests failed at `Failed to generate test certificates`. Now they should pass or fail for reasons unrelated to cert generation. If any test still fails specifically with `Failed to generate test certificates`, the replacement is wrong — re-check the `ca_generate` signature in `tools/offs-ca/ca_ops.h` and the `ca_ops.c` implementation.

- [ ] **Step 5: Commit**

```bash
git add test/test_file_transfer_integration.cpp
git commit -m "refactor(test): use ca_generate instead of openssl CLI in file_transfer tests"
```

---

## Task 3: Replace openssl shell-outs in `test_rpc_integration.cpp`

**Files:**
- Modify: `test/test_rpc_integration.cpp:202` (`generate_test_certs`)
- Modify: `test/test_rpc_integration.cpp:228` (`start_node`)
- Modify: `test/test_rpc_integration.cpp:276` (`start_node_no_relay`)
- Modify: `test/test_rpc_integration.cpp` top (add include)

- [ ] **Step 1: Add the `ca_ops.h` include**

In `test/test_rpc_integration.cpp`, find line 24:

```cpp
extern "C" int node_main(int argc, char* argv[]);
```

Immediately before it, add:

```cpp
extern "C" {
#include "../tools/offs-ca/ca_ops.h"
}

extern "C" int node_main(int argc, char* argv[]);
```

- [ ] **Step 2: Replace the relay-cert shell-out (line 202)**

Find `generate_test_certs()` (line ~199):

```cpp
  void generate_test_certs() {
    cert_path = test_dir + "/test_cert.pem";
    key_path = test_dir + "/test_key.pem";
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path +
                      " -out " + cert_path +
                      " -days 1 -nodes -subj '/CN=liboffs-test' 2>/dev/null";
    ASSERT_EQ(system(cmd.c_str()), 0) << "Failed to generate test certificates";
  }
```

Replace with:

```cpp
  void generate_test_certs() {
    cert_path = test_dir + "/test_cert.pem";
    key_path = test_dir + "/test_key.pem";
    int rc = ca_generate("/CN=liboffs-test", 1, "rsa",
                         cert_path.c_str(), key_path.c_str());
    ASSERT_EQ(rc, 0) << "Failed to generate test certificates";
  }
```

- [ ] **Step 3: Replace the node-cert shell-out in `start_node` (line 228)**

Find `start_node()` body (line ~225):

```cpp
  void start_node(uint16_t node_port, uint16_t control_port,
                  uint16_t relay_port, const std::string& cache_dir) {
    /* Generate unique key pair per node so each has a distinct identity */
    std::string node_cert = cache_dir + "/node_cert.pem";
    std::string node_key = cache_dir + "/node_key.pem";
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + node_key +
                      " -out " + node_cert +
                      " -days 1 -nodes -subj '/CN=liboffs-test-node' 2>/dev/null";
    ASSERT_EQ(system(cmd.c_str()), 0) << "Failed to generate node certificate";
```

Replace the cmd+ASSERT block with:

```cpp
    std::string node_cert = cache_dir + "/node_cert.pem";
    std::string node_key = cache_dir + "/node_key.pem";
    int rc = ca_generate("/CN=liboffs-test-node", 1, "rsa",
                         node_cert.c_str(), node_key.c_str());
    ASSERT_EQ(rc, 0) << "Failed to generate node certificate";
```

(Preserve the leading `/* Generate unique key pair per node so each has a distinct identity */` comment if present.)

- [ ] **Step 4: Replace the node-cert shell-out in `start_node_no_relay` (line 276)**

Find `start_node_no_relay()` body (line ~273):

```cpp
  void start_node_no_relay(uint16_t node_port, uint16_t control_port,
                            const std::string& cache_dir) {
    /* Generate unique key pair per node so each has a distinct identity */
    std::string node_cert = cache_dir + "/node_cert.pem";
    std::string node_key = cache_dir + "/node_key.pem";
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + node_key +
                      " -out " + node_cert +
                      " -days 1 -nodes -subj '/CN=liboffs-test-node' 2>/dev/null";
    ASSERT_EQ(system(cmd.c_str()), 0) << "Failed to generate node certificate";
```

Replace the cmd+ASSERT block with:

```cpp
    std::string node_cert = cache_dir + "/node_cert.pem";
    std::string node_key = cache_dir + "/node_key.pem";
    int rc = ca_generate("/CN=liboffs-test-node", 1, "rsa",
                         node_cert.c_str(), node_key.c_str());
    ASSERT_EQ(rc, 0) << "Failed to generate node certificate";
```

- [ ] **Step 5: Rebuild**

```bash
cmake --build build-noasan -j$(nproc) 2>&1 | tail -5
```

Expected: exit 0.

- [ ] **Step 6: Run the RPC test binary**

```bash
cd build-noasan
./test/test_rpc_integration 2>&1 | tail -25
```

Expected: previously all 21 tests failed at `Failed to generate test certificates`. Now they should pass or fail for reasons unrelated to cert generation.

- [ ] **Step 7: Commit**

```bash
git add test/test_rpc_integration.cpp
git commit -m "refactor(test): use ca_generate instead of openssl CLI in rpc tests"
```

---

## Task 4: Replace `rm -rf` shell-out in `test_offs_ca.cpp`

**Files:**
- Modify: `test/test_offs_ca.cpp:29` (`TearDown`)

- [ ] **Step 1: Add the `<filesystem>` include**

In `test/test_offs_ca.cpp`, find the include block at the top (lines 1-13). After `#include <string>` on line 6, add:

```cpp
#include <filesystem>
```

(The `<system_error>` header is pulled in transitively by `<filesystem>` for `std::error_code`.)

- [ ] **Step 2: Replace the `rm -rf` shell-out**

In `test/test_offs_ca.cpp`, find `TearDown()` (line ~28):

```cpp
  void TearDown() override {
    std::string cmd = "rm -rf " + tmp_dir;
    (void)system(cmd.c_str());
  }
```

Replace with:

```cpp
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  }
```

`std::error_code` overload ignores errors to match the previous `(void)system(...)` best-effort semantics.

- [ ] **Step 3: Rebuild**

```bash
cmake --build build-noasan -j$(nproc) 2>&1 | tail -5
```

Expected: exit 0.

- [ ] **Step 4: Run the offs_ca test binary**

```bash
cd build-noasan
./test/test_offs_ca 2>&1 | tail -10
```

Expected: all tests still pass (this is a behavioral no-op for tests that succeeded before; previously the `rm` silently no-op'd on Windows, now `remove_all` works cross-platform).

- [ ] **Step 5: Commit**

```bash
git add test/test_offs_ca.cpp
git commit -m "refactor(test): use std::filesystem::remove_all instead of rm -rf in offs_ca tests"
```

---

## Task 5: Final regression verification

**Files:**
- None (verification only)

- [ ] **Step 1: Full rebuild**

```bash
cmake --build build-noasan -j$(nproc) 2>&1 | tail -3
```

Expected: exit 0, `Built target testliboffs` and friends.

- [ ] **Step 2: Run the full `testliboffs` suite (regression guard)**

```bash
cd build-noasan
./test/testliboffs 2>&1 | tail -5
```

Expected: `682/682 PASS` (or the same count as the pre-change baseline — no new failures).

- [ ] **Step 3: Run the two previously-broken integration binaries**

```bash
cd build-noasan
./test/test_rpc_integration 2>&1 | tail -5
./test/test_file_transfer_integration 2>&1 | tail -5
./test/test_offs_client_integration 2>&1 | tail -5
./test/test_offs_ca 2>&1 | tail -5
```

Expected:
- `test_rpc_integration`: tests pass or fail for reasons unrelated to cert generation (no `Failed to generate test certificates`).
- `test_file_transfer_integration`: same.
- `test_offs_client_integration`: 5/5 pass (unchanged).
- `test_offs_ca`: all pass.

- [ ] **Step 4: Confirm no `openssl` CLI is invoked (optional sanity check)**

```bash
cd build-noasan
strace -f -e trace=execve ./test/test_rpc_integration 2>&1 | grep -c '"openssl"'
```

Expected: `0`. (If strace is unavailable, skip this step — it's a sanity check only, not a gate.)

- [ ] **Step 5: Run de-wonk audit**

Use the de-wonk skill to scan the changed files for unimplemented, stubbed, disabled, broken, or weird code.

- [ ] **Step 6: Check for memory leaks**

Per project convention (`CLAUDE.md`), run valgrind on the affected test binaries:

```bash
cd build-gdwarf4
cmake --build . -j$(nproc) 2>&1 | tail -3
valgrind --leak-check=full --error-exitcode=1 ./test/test_offs_ca 2>&1 | tail -15
```

Expected: `0 leaks`, exit 0. (The `ca_generate` path already runs under `test_offs_client.cpp` so it should be leak-clean; this verifies the new call sites don't introduce any.)

If `build-gdwarf4` is unavailable, build with `-gdwarf-4` first or skip this step and note it in the wrap-up.

---

## Notes for the implementer

- `ca_generate` is declared in `tools/offs-ca/ca_ops.h` as `int ca_generate(const char* subject_name, int days_valid, const char* key_type, const char* cert_path, const char* key_path);` — argument order is **cert_path before key_path** (matches `openssl req -x509 -keyout K -out C`, where `-keyout` comes first; be careful not to swap them).
- Pass `"rsa"` (not `NULL`) for `key_type` — see the rationale at `test/test_offs_client.cpp:1108`: Schannel on Windows accepts RSA but not ed25519 reliably.
- `<filesystem>` is already included in `test_rpc_integration.cpp` (line 19) and `test_file_transfer_integration.cpp` (line 19) — no need to add it there.
- If any test still fails with `Failed to generate test certificates` after the replacement, run `./test/test_offs_ca` — it exercises `ca_generate` directly and will surface any libcrypto environment issue.
- The two integration test binaries are gated behind `if(NOT WIN32)` in CMake, so this change is POSIX-only by construction. The `ca_ops.c` file itself is portable (already used by `testliboffs` on Windows), so no further platform guards are needed.