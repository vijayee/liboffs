# Replace `openssl` CLI shell-outs with libcrypto in integration tests

## Problem

Two POSIX integration test binaries generate self-signed test certificates by
shelling out to the `openssl` CLI:

```
system("openssl req -x509 -newkey rsa:2048 -keyout K -out C "
       "-days 1 -nodes -subj '/CN=...' 2>/dev/null")
```

Sites:
- `test/test_rpc_integration.cpp:202` — relay cert
- `test/test_rpc_integration.cpp:228` — node cert (`start_node`)
- `test/test_rpc_integration.cpp:276` — node cert (`start_node_no_relay`)
- `test/test_file_transfer_integration.cpp:136` — relay cert

When the host has no `openssl` binary, or the binary is linked against a
different `libssl.so.3` than the system one (the failure observed on this
machine: `version OPENSSL_3.4.0 not found`), every test in the binary fails
at fixture setup with `Failed to generate test certificates`. This is
environmental, but it silently masks real regressions and prevents the
POSIX integration suite from running on hosts without a matching openssl
CLI build.

A separate non-openssl shell-out exists at `test/test_offs_ca.cpp:29`:

```
std::string cmd = "rm -rf " + tmp_dir;
(void)system(cmd.c_str());
```

`rm` is not present on Windows cmd, so this is also non-portable. The
other integration tests already use `std::filesystem::remove_all` for the
same purpose (`test_rpc_integration.cpp:169,192`,
`test_file_transfer_integration.cpp:100,126`).

## Precedent

`test/test_offs_client.cpp:1110` already migrated off the same
`system("openssl req ... 2>/dev/null")` pattern to the in-process
`ca_generate()` function from `tools/offs-ca/ca_ops.h`. The comment at
that site explains the migration was driven by Windows cmd not
understanding `/dev/null`, which silently broke cert generation on
Windows. `ca_generate` is libcrypto-backed (it uses `openssl/x509.h`,
`openssl/pem.h`, `openssl/evp.h` internally — see
`tools/offs-ca/ca_ops.c`).

`testliboffs` already compiles `ca_ops.c` into the test binary
(`test/CMakeLists.txt:95`) and exposes its include path
(`test/CMakeLists.txt:162`).

## Design

### Replace the 4 openssl shell-outs

In `test/test_rpc_integration.cpp` (3 sites) and
`test/test_file_transfer_integration.cpp` (1 site), replace:

```cpp
std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path +
                  " -out " + cert_path +
                  " -days 1 -nodes -subj '/CN=liboffs-test' 2>/dev/null";
ASSERT_EQ(system(cmd.c_str()), 0) << "Failed to generate test certificates";
```

with:

```cpp
int rc = ca_generate("/CN=liboffs-test", 1, "rsa",
                      cert_path.c_str(), key_path.c_str());
ASSERT_EQ(rc, 0) << "Failed to generate test certificates";
```

The node-cert sites use `/CN=liboffs-test-node` — preserve that subject
literal. Pass `"rsa"` (not the default `"ed25519"`) to match the old
`rsa:2048` behavior and to preserve the cross-platform Schannel
compatibility rationale documented in `test_offs_client.cpp:1108`.

Add `#include "../../tools/offs-ca/ca_ops.h"` to both files (inside the
existing `extern "C"` block, matching the `test_offs_client.cpp` style).

### Replace the rm shell-out

In `test/test_offs_ca.cpp:29`, replace:

```cpp
std::string cmd = "rm -rf " + tmp_dir;
(void)system(cmd.c_str());
```

with:

```cpp
std::error_code ec;
std::filesystem::remove_all(tmp_dir, ec);
```

`<filesystem>` is already included transitively; if not, add
`#include <filesystem>`. Ignore the error code to match the previous
`(void)system(...)` semantics — best-effort cleanup.

### CMake

`test_rpc_integration` and `test_file_transfer_integration` already link
`OpenSSL::SSL OpenSSL::Crypto` and `cbor`, `blake3`, `poll-dancer`, etc.
What they lack is `ca_ops.c` in their source lists and its include
directory.

In `test/CMakeLists.txt`:

1. `test_file_transfer_integration` `add_executable` (line 184): add
   `${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca/ca_ops.c` to the source
   list. Add
   `target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/offs-ca)`
   alongside the existing include directives.

2. `test_rpc_integration` `add_executable` (line 233): same two changes.

No new dependencies — `OpenSSL::Crypto` is already linked.

## Testing

1. `cmake --build build-noasan -j$(nproc)` — must succeed with 0 errors.
2. From `build-noasan/`:
   - `./test/test_rpc_integration` — all 21 currently-failing tests
     must pass.
   - `./test/test_file_transfer_integration` — all 11
     currently-failing tests must pass.
   - `./test/test_offs_ca` — must still pass (no behavioral change to
     cleanup).
   - `./test/testliboffs` — must still pass 682/682 (regression guard).
3. Confirm `openssl` is no longer invoked: `strace -f -e trace=execve
   ./test/test_rpc_integration 2>&1 | grep openssl` should return no
   hits. (Optional sanity check.)

## Out of scope

- No changes to production code under `src/`.
- No changes to `test_offs_client.cpp` — it already uses `ca_generate`.
- No changes to the cert material itself (subject, key type, validity
  period all preserved).
- No changes to other `system(...)` calls (e.g., `start_node`'s `execl`
  of the test binary, which is intentional process spawning).