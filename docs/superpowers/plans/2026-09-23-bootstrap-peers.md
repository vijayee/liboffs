# Bootstrap Peers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make bootstrap peers reachable end-to-end: config seed + persisted operator-managed list, engaged at startup and on partition heal, managed through CLI, client-API wire ops on all four transports (Unix/TCP/WS/WT), and HTTP — with the friend ops wired into TCP/WS/WT for parity.

**Architecture:** Two separate lists on `authority_t` (config-seeded read-only `bootstrap_peers`, persisted `managed_bootstrap_peers`). A shared bracket-aware endpoint parser feeds the connect loop. The 5s friend reconnect timer gains a partition-heal branch (zero connected peers → re-connect bootstrap lists with exponential backoff). Wire ops 52–55 mirror the friend ops 27–30; handlers live in `peer_handlers.c` and are dispatched from all four transport loops. Spec: `docs/superpowers/specs/2026-09-23-bootstrap-peers-design.md`.

**Tech Stack:** C (C11, libcbor, cJSON), GTest C++ test binary `test/testliboffs`, JS client (vite/vitest), sibling repo `../OFFS` (symlinked as `./OFFS`) for offsd flag + CLI.

**Conventions (from CLAUDE.md / STYLE_GUIDE.md):** no TODOs in completed work; no Co-Authored-By lines; conventional commits; check tests for memory leaks after each implementation; new `src/**/*.c` files are picked up automatically by `file(GLOB_RECURSE C_SRC "src/*/*.c")` in the root CMakeLists; test files must be added to `test/CMakeLists.txt` explicitly.

**Build/test commands:**
- Build tests: `cmake --build build-test -j$(nproc)`
- Run one test file: `./build-test/test/testliboffs --gtest_filter='EndpointTest.*'`
- Run all tests: `./build-test/test/testliboffs`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/Network/endpoint.c/.h` (new) | Bracket-aware `host:port` endpoint parser |
| `src/ClientAPI/client_api_wire.h/.c` (modify) | Bootstrap op codes 52–55, message structs, CBOR encode/decode |
| `src/Network/authority.h/.c` (modify) | `managed_bootstrap_peers` list, add/remove helpers, peer-store index 6 |
| `src/Configuration/config.h`, `config.c`, `config_pending.c`, `config_json.c` (modify) | `bootstrap_peers` CSV config field |
| `src/Network/network.c` (modify) | Connect both lists at startup + partition heal with backoff |
| `src/ClientAPI/peer_handlers.h/.c` (modify) | Bootstrap handlers + friend-save dirty fix |
| `src/ClientAPI/Unix/unix_connection.c`, `TCP/tcp_connection.c`, `WS/ws_connection.c`, `WT/wt_connection.c` (modify) | Dispatch FRIEND_* (TCP/WS/WT only) + BOOTSTRAP_* |
| `src/ClientAPI/HTTP/peer_routes.c` (modify) | `/bootstrap` POST/DELETE/GET routes |
| `src/ClientLibs/c/offs_client.h/.c` (modify) | C client bootstrap functions |
| `src/ClientLibs/js/offs-client/src/*.js` (modify) | JS client wire/methods/HTTP transport + dist rebuild |
| `../OFFS/src/offsd/main.c` (modify) | `--bootstrap` flag + config-file key |
| `../OFFS/src/offs/commands/bootstrap.c` (new), `cli_util.c`, `l10n/en.h` (modify) | `offs bootstrap add|remove|list` |

---

### Task 1: Endpoint parser

**Files:**
- Create: `src/Network/endpoint.h`, `src/Network/endpoint.c`
- Modify: `test/CMakeLists.txt` (add `test/test_endpoint.cpp` to the `testliboffs` source list near line 34)
- Test: `test/test_endpoint.cpp`

- [ ] **Step 1: Write the failing test**

Create `test/test_endpoint.cpp`:

```cpp
// Tests for the bracket-aware endpoint parser (src/Network/endpoint.c).
#include <gtest/gtest.h>

extern "C" {
#include "Network/endpoint.h"
}

TEST(EndpointTest, ParsesHostPort) {
  char host[64];
  uint16_t port = 0;
  ASSERT_EQ(0, parse_endpoint("10.0.0.1:8080", host, sizeof(host), &port));
  EXPECT_STREQ("10.0.0.1", host);
  EXPECT_EQ(8080, port);
}

TEST(EndpointTest, ParsesHostnamePort) {
  char host[256];
  uint16_t port = 0;
  ASSERT_EQ(0, parse_endpoint("bootstrap.example.com:443", host, sizeof(host), &port));
  EXPECT_STREQ("bootstrap.example.com", host);
  EXPECT_EQ(443, port);
}

TEST(EndpointTest, ParsesBracketedIpv6) {
  char host[64];
  uint16_t port = 0;
  ASSERT_EQ(0, parse_endpoint("[2001:db8::1]:8080", host, sizeof(host), &port));
  EXPECT_STREQ("2001:db8::1", host);
  EXPECT_EQ(8080, port);
}

TEST(EndpointTest, RejectsBareIpv6Literal) {
  // Unbracketed IPv6 literals stay unsupported until the IPv6 cycle.
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, parse_endpoint("2001:db8::1:8080", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsMissingPort) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, parse_endpoint("10.0.0.1", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsBracketWithoutPort) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, parse_endpoint("[2001:db8::1]", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsUnclosedBracket) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, parse_endpoint("[2001:db8::1:8080", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsEmptyInput) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, parse_endpoint("", host, sizeof(host), &port));
  EXPECT_NE(0, parse_endpoint(NULL, host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsBadPort) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, parse_endpoint("10.0.0.1:notaport", host, sizeof(host), &port));
  EXPECT_NE(0, parse_endpoint("10.0.0.1:70000", host, sizeof(host), &port));
  EXPECT_NE(0, parse_endpoint("10.0.0.1:0", host, sizeof(host), &port));
}
```

Add `test/test_endpoint.cpp` to the `testliboffs` source list in `test/CMakeLists.txt` (the `add_executable(testliboffs test_main.cpp ...)` block starting at line 34).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-test -j$(nproc) 2>&1 | tail -5`
Expected: build FAILS — `Network/endpoint.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `src/Network/endpoint.h`:

```c
#ifndef OFFS_ENDPOINT_H
#define OFFS_ENDPOINT_H

#include <stddef.h>
#include <stdint.h>

/* Parse a "host:port" or "[ipv6-literal]:port" endpoint string.
 *
 * Accepts IPv4 or hostname with any port separator, and IPv6 literals only
 * in brackets (bare unbracketed IPv6 is ambiguous and stays unsupported
 * until the IPv6 cycle). Port must be 1..65535.
 *
 * Returns 0 on success and fills host_out (NUL-terminated) + port_out.
 * Returns -1 on malformed input. */
int parse_endpoint(const char* input, char* host_out, size_t host_len,
                   uint16_t* port_out);

#endif
```

Create `src/Network/endpoint.c`:

```c
#include "endpoint.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int parse_endpoint(const char* input, char* host_out, size_t host_len,
                   uint16_t* port_out) {
  if (input == NULL || host_out == NULL || port_out == NULL) return -1;

  while (isspace((unsigned char)*input)) input++;
  if (*input == '\0') return -1;

  const char* host_start;
  size_t host_length;
  const char* port_start;

  if (*input == '[') {
    const char* closing = strchr(input, ']');
    if (closing == NULL) return -1;
    host_start = input + 1;
    host_length = (size_t)(closing - host_start);
    if (host_length == 0) return -1;
    if (closing[1] != ':') return -1;
    port_start = closing + 2;
  } else {
    /* Hostname/IPv4 cannot contain ':', so the last separator is the port. */
    const char* colon = strrchr(input, ':');
    if (colon == NULL) return -1;
    host_start = input;
    host_length = (size_t)(colon - input);
    if (host_length == 0) return -1;
    port_start = colon + 1;
  }

  if (port_start[0] == '\0') return -1;
  char* end = NULL;
  long port = strtol(port_start, &end, 10);
  if (end == port_start || *end != '\0' || port < 1 || port > 65535) return -1;

  if (host_length + 1 > host_len) return -1;
  memcpy(host_out, host_start, host_length);
  host_out[host_length] = '\0';
  *port_out = (uint16_t)port;
  return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs --gtest_filter='EndpointTest.*'`
Expected: PASS (9 tests).

- [ ] **Step 5: Commit**

```bash
git add src/Network/endpoint.h src/Network/endpoint.c test/test_endpoint.cpp test/CMakeLists.txt
git commit -m "feat: bracket-aware endpoint parser for bootstrap addresses"
```

---

### Task 2: Client API wire ops (codes 52–55)

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h` (op codes after line 51, structs after friend structs ~line 369, prototypes after line 521)
- Modify: `src/ClientAPI/client_api_wire.c` (encode/decode after friend block, ~line 1770)
- Test: `test/test_bootstrap_wire.cpp` (new, registered in `test/CMakeLists.txt`)

- [ ] **Step 1: Write the failing test**

Create `test/test_bootstrap_wire.cpp`:

```cpp
// Wire round-trip tests for the bootstrap peer client-API ops.
#include <gtest/gtest.h>
#include <cbor.h>
#include <cstring>

extern "C" {
#include "ClientAPI/client_api_wire.h"
}

TEST(BootstrapWire, AddRoundTrip) {
  client_api_bootstrap_add_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.endpoint = (char*)"[2001:db8::1]:8080";

  cbor_item_t* frame = client_api_bootstrap_add_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_bootstrap_add_t decoded;
  EXPECT_EQ(0, client_api_bootstrap_add_decode(frame, &decoded));
  EXPECT_STREQ("[2001:db8::1]:8080", decoded.endpoint);

  client_api_bootstrap_add_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(BootstrapWire, AddDecodeRejectsWrongTypeAndMissingField) {
  cbor_item_t* wrong = cbor_new_definite_array(1);
  cbor_item_t* t = cbor_build_uint8(CLIENT_API_FRIEND_ADD);
  cbor_array_push(wrong, t);
  cbor_decref(&t);
  client_api_bootstrap_add_t msg;
  memset(&msg, 0, sizeof(msg));
  EXPECT_NE(0, client_api_bootstrap_add_decode(wrong, &msg));
  cbor_decref(&wrong);

  cbor_item_t* short_frame = cbor_build_uint8(CLIENT_API_BOOTSTRAP_ADD);
  EXPECT_NE(0, client_api_bootstrap_add_decode(short_frame, &msg));
  cbor_decref(&short_frame);
}

TEST(BootstrapWire, RemoveRoundTrip) {
  client_api_bootstrap_remove_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.endpoint = (char*)"10.0.0.1:8080";

  cbor_item_t* frame = client_api_bootstrap_remove_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_bootstrap_remove_t decoded;
  EXPECT_EQ(0, client_api_bootstrap_remove_decode(frame, &decoded));
  EXPECT_STREQ("10.0.0.1:8080", decoded.endpoint);

  client_api_bootstrap_remove_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(BootstrapWire, ListResponseRoundTrip) {
  cbor_item_t* entries = cbor_new_definite_array(2);
  for (int iteration = 0; iteration < 2; iteration++) {
    cbor_item_t* entry = cbor_new_definite_array(3);
    cbor_item_t* host = cbor_build_string(iteration == 0 ? "10.0.0.1" : "2001:db8::1");
    cbor_item_t* port = cbor_build_uint16(iteration == 0 ? 8080 : 9090);
    cbor_item_t* source = cbor_build_uint8(iteration == 0
                                               ? CLIENT_API_BOOTSTRAP_SOURCE_CONFIG
                                               : CLIENT_API_BOOTSTRAP_SOURCE_MANAGED);
    cbor_array_push(entry, host);
    cbor_array_push(entry, port);
    cbor_array_push(entry, source);
    cbor_decref(&host);
    cbor_decref(&port);
    cbor_decref(&source);
    cbor_array_push(entries, entry);
    cbor_decref(&entry);
  }

  client_api_bootstrap_list_response_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.entries = entries;

  cbor_item_t* frame = client_api_bootstrap_list_response_encode(&msg);
  ASSERT_NE(frame, nullptr);

  client_api_bootstrap_list_response_t decoded;
  EXPECT_EQ(0, client_api_bootstrap_list_response_decode(frame, &decoded));
  ASSERT_TRUE(cbor_isa_array(decoded.entries));
  EXPECT_EQ(2, cbor_array_size(decoded.entries));

  client_api_bootstrap_list_response_destroy(&decoded);
  cbor_decref(&frame);
}

TEST(BootstrapWire, ListRequestEncodeHasTypeOnly) {
  cbor_item_t* frame = client_api_bootstrap_list_request_encode();
  ASSERT_NE(frame, nullptr);
  client_api_bootstrap_list_response_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  // A list request is not a response — decode must fail.
  EXPECT_NE(0, client_api_bootstrap_list_response_decode(frame, &decoded));
  cbor_decref(&frame);
}
```

Add `test/test_bootstrap_wire.cpp` to the `testliboffs` source list in `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-test -j$(nproc) 2>&1 | tail -5`
Expected: build FAILS — `client_api_bootstrap_add_encode` undeclared.

- [ ] **Step 3: Write the implementation**

In `src/ClientAPI/client_api_wire.h`, after line 51 (`CLIENT_API_EPHEMERAL_LIST_RESPONSE 51`) add:

```c
#define CLIENT_API_BOOTSTRAP_ADD             52
#define CLIENT_API_BOOTSTRAP_REMOVE          53
#define CLIENT_API_BOOTSTRAP_LIST            54
#define CLIENT_API_BOOTSTRAP_LIST_RESPONSE   55
```

After the friend structs (after `client_api_friend_list_response_t`, ~line 369) add:

```c
// --- Bootstrap Add ---
// [type, endpoint: string]  e.g. "10.0.0.1:8080" or "[2001:db8::1]:8080"
typedef struct {
  char* endpoint;  // caller frees via client_api_bootstrap_add_destroy
} client_api_bootstrap_add_t;

// --- Bootstrap Remove ---
// [type, endpoint: string]
typedef struct {
  char* endpoint;
} client_api_bootstrap_remove_t;

// --- Bootstrap List Request ---
// [type] — no payload

// --- Bootstrap List Response ---
// [type, entries: [ [host: string, port: uint, source: uint], ... ]]
// source: 0 = config-seeded (immutable), 1 = operator-managed (persisted)
#define CLIENT_API_BOOTSTRAP_SOURCE_CONFIG  0
#define CLIENT_API_BOOTSTRAP_SOURCE_MANAGED 1

typedef struct {
  cbor_item_t* entries;  // owned by struct, freed by _destroy
} client_api_bootstrap_list_response_t;
```

After the friend prototypes (~line 521) add:

```c
cbor_item_t* client_api_bootstrap_add_encode(const client_api_bootstrap_add_t* msg);
int client_api_bootstrap_add_decode(cbor_item_t* item, client_api_bootstrap_add_t* msg);
void client_api_bootstrap_add_destroy(client_api_bootstrap_add_t* msg);

cbor_item_t* client_api_bootstrap_remove_encode(const client_api_bootstrap_remove_t* msg);
int client_api_bootstrap_remove_decode(cbor_item_t* item, client_api_bootstrap_remove_t* msg);
void client_api_bootstrap_remove_destroy(client_api_bootstrap_remove_t* msg);

cbor_item_t* client_api_bootstrap_list_request_encode(void);
cbor_item_t* client_api_bootstrap_list_response_encode(const client_api_bootstrap_list_response_t* msg);
int client_api_bootstrap_list_response_decode(cbor_item_t* item, client_api_bootstrap_list_response_t* msg);
void client_api_bootstrap_list_response_destroy(client_api_bootstrap_list_response_t* msg);
```

In `src/ClientAPI/client_api_wire.c`, after the friend block (after `client_api_friend_list_response_decode`, ~line 1780) add:

```c
// --- Bootstrap Add ---
// [type, endpoint: string]

cbor_item_t* client_api_bootstrap_add_encode(const client_api_bootstrap_add_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_ADD);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  item = cbor_build_string(msg->endpoint);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_bootstrap_add_decode(cbor_item_t* item, client_api_bootstrap_add_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_BOOTSTRAP_ADD) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* endpoint_item = cbor_array_get(item, 1);
  if (!cbor_isa_string(endpoint_item) || cbor_string_length(endpoint_item) == 0 ||
      cbor_string_length(endpoint_item) > 256) {
    cbor_decref(&endpoint_item);
    return -1;
  }
  msg->endpoint = strndup((const char*)cbor_string_handle(endpoint_item),
                          cbor_string_length(endpoint_item));
  cbor_decref(&endpoint_item);
  return msg->endpoint == NULL ? -1 : 0;
}

void client_api_bootstrap_add_destroy(client_api_bootstrap_add_t* msg) {
  if (msg == NULL) return;
  free(msg->endpoint);
}

// --- Bootstrap Remove ---
// [type, endpoint: string]

cbor_item_t* client_api_bootstrap_remove_encode(const client_api_bootstrap_remove_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_REMOVE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  item = cbor_build_string(msg->endpoint);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_bootstrap_remove_decode(cbor_item_t* item, client_api_bootstrap_remove_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_BOOTSTRAP_REMOVE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* endpoint_item = cbor_array_get(item, 1);
  if (!cbor_isa_string(endpoint_item) || cbor_string_length(endpoint_item) == 0 ||
      cbor_string_length(endpoint_item) > 256) {
    cbor_decref(&endpoint_item);
    return -1;
  }
  msg->endpoint = strndup((const char*)cbor_string_handle(endpoint_item),
                          cbor_string_length(endpoint_item));
  cbor_decref(&endpoint_item);
  return msg->endpoint == NULL ? -1 : 0;
}

void client_api_bootstrap_remove_destroy(client_api_bootstrap_remove_t* msg) {
  if (msg == NULL) return;
  free(msg->endpoint);
}

// --- Bootstrap List Request ---
// [type] — no payload

cbor_item_t* client_api_bootstrap_list_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_LIST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Bootstrap List Response ---
// [type, entries: cbor_array of [host, port, source]]

cbor_item_t* client_api_bootstrap_list_response_encode(const client_api_bootstrap_list_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_LIST_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->entries != NULL) {
    (void)cbor_array_push(array, msg->entries);
  } else {
    item = cbor_new_definite_array(0);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }
  return array;
}

void client_api_bootstrap_list_response_destroy(client_api_bootstrap_list_response_t* msg) {
  if (msg == NULL) return;
  if (msg->entries != NULL) {
    cbor_decref(&msg->entries);
  }
}

int client_api_bootstrap_list_response_decode(cbor_item_t* item, client_api_bootstrap_list_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_BOOTSTRAP_LIST_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);
  msg->entries = cbor_array_get(item, 1);
  return 0;
}
```

Note: `strndup` requires `<string.h>` (already included) and POSIX; if the platform header check fails, use the existing `_decode_bytestring`-style copy helper pattern with `get_memory` + `memcpy` + explicit NUL — but `strndup` is already used in `src/Network/authority.c`, so it is safe.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs --gtest_filter='BootstrapWire.*'`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c test/test_bootstrap_wire.cpp test/CMakeLists.txt
git commit -m "feat: bootstrap peer client-API wire ops (52-55)"
```

---

### Task 3: Authority managed list + peer-store index 6

**Files:**
- Modify: `src/Network/authority.h` (field after line 27, helpers after line 88)
- Modify: `src/Network/authority.c` (destroy ~line 46, save ~line 396, load ~line 622)
- Test: `test/test_authority_bootstrap.cpp` (new, registered in `test/CMakeLists.txt`)

- [ ] **Step 1: Write the failing test**

Create `test/test_authority_bootstrap.cpp`:

```cpp
// Bootstrap list helpers and peer-store round trip (index 6).
#include <gtest/gtest.h>
#include <cstdio>

extern "C" {
#include "Configuration/config.h"
#include "Network/authority.h"
}

static authority_t* make_authority(const char* path) {
  config_t config;
  memset(&config, 0, sizeof(config));
  authority_t* authority = authority_create(&config);
  authority->peer_store_path = strdup(path);
  return authority;
}

TEST(AuthorityBootstrap, AddValidatesAndDedups) {
  authority_t* authority = make_authority("/tmp/test_bootstrap_store.cbor");
  ASSERT_NE(authority, nullptr);

  EXPECT_EQ(0, authority_bootstrap_add(authority, "10.0.0.1:8080"));
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  EXPECT_EQ(-2, authority_bootstrap_add(authority, "10.0.0.1:8080"));  // dup
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  EXPECT_EQ(-1, authority_bootstrap_add(authority, "not-an-endpoint"));
  EXPECT_EQ(-1, authority_bootstrap_add(authority, "2001:db8::1:9090"));  // bare v6
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  authority->bootstrap_peers = strdup("10.0.0.1:8080");
  authority->bootstrap_peer_count = 1;
  EXPECT_EQ(-2, authority_bootstrap_add(authority, "10.0.0.1:8080"));  // dup vs config
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  authority_destroy(authority);
}

TEST(AuthorityBootstrap, RemoveKnownAndUnknown) {
  authority_t* authority = make_authority("/tmp/test_bootstrap_store.cbor");
  authority->bootstrap_peers = strdup("10.0.0.1:8080");
  authority->bootstrap_peer_count = 1;

  ASSERT_EQ(0, authority_bootstrap_add(authority, "10.0.0.2:8080"));
  EXPECT_EQ(-2, authority_bootstrap_remove(authority, "10.0.0.1:8080"));   // config source → conflict
  EXPECT_EQ(0, authority_bootstrap_remove(authority, "10.0.0.1:8080"));    // removes managed copy of same host:port
  EXPECT_EQ(-1, authority_bootstrap_remove(authority, "10.0.0.1:8080"));   // now unknown
  EXPECT_EQ(-1, authority_bootstrap_remove(authority, "999.1.1.1:1"));     // unknown

  authority_destroy(authority);
}

TEST(AuthorityBootstrap, PeerStoreRoundTripIndex6) {
  const char* path = "/tmp/test_authority_bootstrap_roundtrip.cbor";
  authority_t* authority = make_authority(path);
  // authority_save_peers requires a network; it writes hebbian/rings state.
  // Use the two-arg helper guarded below; see Step 3 for the internal writer.
  authority_destroy(authority);
  (void)path;
}
```

Note for the implementer: `authority_save_peers` takes a `network_t*` and reads `network->hebbian` and `network->rings`. If constructing a minimal `network_t` in the test is impractical, factor the managed-entries CBOR build into a helper `authority_managed_bootstrap_to_cbor(const authority_t*)` in authority.c (declared in authority.h) and unit-test the add/remove helpers plus a save/load round trip through a real `offs_node_t` in the integration task instead — but do NOT skip the round-trip coverage. If a minimal `network_t` with an empty hebbian table and NULL rings can be constructed (check how `test_peer_state.cpp` builds one and reuse its fixture), write the round-trip test there.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-test -j$(nproc) 2>&1 | tail -5`
Expected: FAIL — `managed_bootstrap_peer_count` / `authority_bootstrap_add` not declared.

- [ ] **Step 3: Write the implementation**

In `src/Network/authority.h`, after lines 26–27 add:

```c
  char** managed_bootstrap_peers;
  size_t managed_bootstrap_peer_count;
```

After the `authority_save_peers` declaration (~line 88) add:

```c
/* Bootstrap peers. bootstrap_peers is config-seeded and immutable at runtime;
 * managed_bootstrap_peers is operator-added and persisted in peer-store
 * index 6. Endpoints are "host:port" or "[ipv6]:port" strings. */
int authority_bootstrap_add(authority_t* authority, const char* endpoint);
int authority_bootstrap_remove(authority_t* authority, const char* endpoint);
int authority_set_bootstrap_peers(authority_t* authority, const char* csv);
```

Return codes for `authority_bootstrap_add`: 0 = added, -1 = invalid endpoint or OOM, -2 = duplicate (in either list).
Return codes for `authority_bootstrap_remove`: 0 = removed, -1 = not found, -2 = config-source entry (conflict).

In `src/Network/authority.c`:

1. Extend `authority_destroy` after the `bootstrap_peers` free block (~line 51):

```c
  if (authority->managed_bootstrap_peers != NULL) {
    for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
      free(authority->managed_bootstrap_peers[index]);
    }
    free(authority->managed_bootstrap_peers);
  }
```

2. Add helpers (place after `authority_init_local_id`):

```c
// --- Bootstrap peers (config-seeded + operator-managed) ---

static int authority_bootstrap_contains(authority_t* authority, const char* endpoint) {
  char host[256];
  uint16_t port = 0;
  if (parse_endpoint(endpoint, host, sizeof(host), &port) != 0) return 1;
  char canonical[320];
  snprintf(canonical, sizeof(canonical), "%s:%u", host, (unsigned)port);

  for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
    char config_host[256];
    uint16_t config_port = 0;
    if (parse_endpoint(authority->bootstrap_peers[index], config_host,
                       sizeof(config_host), &config_port) == 0 &&
        strcmp(config_host, host) == 0 && config_port == port) {
      return 1;
    }
  }
  for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
    char managed_host[256];
    uint16_t managed_port = 0;
    if (parse_endpoint(authority->managed_bootstrap_peers[index], managed_host,
                       sizeof(managed_host), &managed_port) == 0 &&
        strcmp(managed_host, host) == 0 && managed_port == port) {
      return 1;
    }
  }
  (void)canonical;
  return 0;
}

int authority_bootstrap_add(authority_t* authority, const char* endpoint) {
  if (authority == NULL || endpoint == NULL) return -1;
  char host[256];
  uint16_t port = 0;
  if (parse_endpoint(endpoint, host, sizeof(host), &port) != 0) return -1;
  if (authority_bootstrap_contains(authority, endpoint)) return -2;

  char* normalized = get_memory(strlen(host) + 8);
  if (normalized == NULL) return -1;
  if (strchr(host, ':') != NULL) {
    snprintf(normalized, strlen(host) + 8, "[%s]:%u", host, (unsigned)port);
  } else {
    snprintf(normalized, strlen(host) + 8, "%s:%u", host, (unsigned)port);
  }

  size_t new_count = authority->managed_bootstrap_peer_count + 1;
  char** expanded =
      realloc(authority->managed_bootstrap_peers, new_count * sizeof(char*));
  if (expanded == NULL) {
    free(normalized);
    return -1;
  }
  authority->managed_bootstrap_peers = expanded;
  authority->managed_bootstrap_peers[authority->managed_bootstrap_peer_count] = normalized;
  authority->managed_bootstrap_peer_count = new_count;
  return 0;
}

int authority_bootstrap_remove(authority_t* authority, const char* endpoint) {
  if (authority == NULL || endpoint == NULL) return -1;

  char host[256];
  uint16_t port = 0;
  if (parse_endpoint(endpoint, host, sizeof(host), &port) != 0) return -1;

  for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
    char config_host[256];
    uint16_t config_port = 0;
    if (parse_endpoint(authority->bootstrap_peers[index], config_host,
                       sizeof(config_host), &config_port) == 0 &&
        strcmp(config_host, host) == 0 && config_port == port) {
      return -2;  // config-seeded entries are immutable at runtime
    }
  }

  for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
    char managed_host[256];
    uint16_t managed_port = 0;
    if (parse_endpoint(authority->managed_bootstrap_peers[index], managed_host,
                       sizeof(managed_host), &managed_port) == 0 &&
        strcmp(managed_host, host) == 0 && managed_port == port) {
      free(authority->managed_bootstrap_peers[index]);
      for (size_t shift = index; shift + 1 < authority->managed_bootstrap_peer_count; shift++) {
        authority->managed_bootstrap_peers[shift] = authority->managed_bootstrap_peers[shift + 1];
      }
      authority->managed_bootstrap_peer_count--;
      return 0;
    }
  }
  return -1;
}

int authority_set_bootstrap_peers(authority_t* authority, const char* csv) {
  if (authority == NULL) return -1;
  if (authority->bootstrap_peers != NULL) {
    for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
      free(authority->bootstrap_peers[index]);
    }
    free(authority->bootstrap_peers);
    authority->bootstrap_peers = NULL;
    authority->bootstrap_peer_count = 0;
  }
  if (csv == NULL || csv[0] == '\0') return 0;

  char* copy = strdup(csv);
  if (copy == NULL) return -1;
  char* saveptr = NULL;
  for (char* token = strtok_r(copy, ",", &saveptr); token != NULL;
       token = strtok_r(NULL, ",", &saveptr)) {
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(token, host, sizeof(host), &port) != 0) {
      free(copy);
      return -1;
    }
    size_t length = strlen(host) + 8;
    char* stored = get_memory(length);
    if (stored == NULL) {
      free(copy);
      return -1;
    }
    if (strchr(host, ':') != NULL) {
      snprintf(stored, length, "[%s]:%u", host, (unsigned)port);
    } else {
      snprintf(stored, length, "%s:%u", host, (unsigned)port);
    }
    size_t new_count = authority->bootstrap_peer_count + 1;
    char** expanded = realloc(authority->bootstrap_peers, new_count * sizeof(char*));
    if (expanded == NULL) {
      free(stored);
      free(copy);
      return -1;
    }
    authority->bootstrap_peers = expanded;
    authority->bootstrap_peers[authority->bootstrap_peer_count] = stored;
    authority->bootstrap_peer_count = new_count;
  }
  free(copy);
  return 0;
}
```

Include `"endpoint.h"` at the top of `authority.c` (it lives in the same directory; use `"endpoint.h"`). Use `get_memory`/`get_clear_memory` from `Util/allocator.h` consistent with the file's existing usage (the file already uses `get_clear_memory`).

3. In `authority_save_peers`: change `cbor_new_definite_array(6)` (line ~295) to `cbor_new_definite_array(7)`, and after the index-5 friends block (after `cbor_decref(&friends_arr);` at line ~407) add:

```c
  // Index 6: operator-managed bootstrap entries as normalized strings
  cbor_item_t* managed_arr =
      cbor_new_definite_array(authority->managed_bootstrap_peer_count);
  for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
    cbor_item_t* str_item = cbor_build_string(authority->managed_bootstrap_peers[index]);
    (void)cbor_array_push(managed_arr, str_item);
    cbor_decref(&str_item);
  }
  (void)cbor_array_push(root, managed_arr);
  cbor_decref(&managed_arr);
```

Update the format comment above `authority_save_peers` (line ~278) to add:

```c
//   [ string, ... ]                     // managed bootstrap entries as "host:port" strings (index 6, v3 extension — optional)
```

4. In `authority_load_peers`, after the index-5 friends block (after `cbor_decref(&friends_item);`, ~line 654) add:

```c
    // Index 6: managed bootstrap entries (absent in older stores = empty)
    if (arr_size >= 7) {
      cbor_item_t* managed_item = cbor_array_get(root, 6);
      if (cbor_isa_array(managed_item)) {
        if (authority->managed_bootstrap_peers != NULL) {
          for (size_t idx = 0; idx < authority->managed_bootstrap_peer_count; idx++) {
            free(authority->managed_bootstrap_peers[idx]);
          }
          free(authority->managed_bootstrap_peers);
          authority->managed_bootstrap_peers = NULL;
          authority->managed_bootstrap_peer_count = 0;
        }
        size_t managed_count = cbor_array_size(managed_item);
        if (managed_count > 0) {
          authority->managed_bootstrap_peers =
              get_clear_memory(managed_count * sizeof(char*));
          for (size_t index = 0; index < managed_count; index++) {
            cbor_item_t* str_item = cbor_array_get(managed_item, index);
            if (cbor_isa_string(str_item)) {
              char* stored =
                  strndup((char*)cbor_string_handle(str_item), cbor_string_length(str_item));
              if (stored != NULL &&
                  parse_endpoint(stored, (char[256]){0}, 256, &(uint16_t){0}) == 0) {
                authority->managed_bootstrap_peers[authority->managed_bootstrap_peer_count++] = stored;
              } else {
                free(stored);
              }
            }
            cbor_decref(&str_item);
          }
        }
      }
      cbor_decref(&managed_item);
    }
```

(If the compound-literal `&(uint16_t){0}` pattern offends the style guide, declare `char discarded_host[256]; uint16_t discarded_port = 0;` above the loop and use those.)

Backward compatibility: old loaders read only indexes 0–5, so a 7-element v3 store stays readable by older daemons. New loader accepts `arr_size >= 6` stores (index 6 simply absent).

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs --gtest_filter='AuthorityBootstrap.*'`
Expected: PASS. Also run `./build-test/test/testliboffs --gtest_filter='*PeerState*'` to confirm no regression in the existing peer-store tests.

- [ ] **Step 5: Commit**

```bash
git add src/Network/authority.h src/Network/authority.c test/test_authority_bootstrap.cpp test/CMakeLists.txt
git commit -m "feat: operator-managed bootstrap peer list with peer-store index 6"
```

---

### Task 4: Config field + CSV seeding

**Files:**
- Modify: `src/Configuration/config.h` (add field in the client-API flags section, ~line 65)
- Modify: `src/Configuration/config_json.c` (line 15 `_string_fields[]`)
- Modify: `src/Configuration/config_pending.c` (parse chain ~line 153)
- Modify: `src/Configuration/config.c` (`config_free_members` ~line 307)
- Modify: `docs/CONFIG_FIELDS.md`
- Test: extend `test/test_config_json.cpp`

- [ ] **Step 1: Write the failing test**

Extend `test/test_config_json.cpp` (follow its existing test style — find a test that writes a JSON doc, calls the pending-config parse, and asserts a field; add):

```cpp
TEST(ConfigJsonTest, BootstrapPeersFieldParses) {
  const char* json = "{\"bootstrap_peers\": \"10.0.0.1:8080,[2001:db8::1]:9090\"}";
  config_t config;
  ASSERT_EQ(0, config_parse_json(&config, json));  // match the file's actual parse entry point
  ASSERT_NE(config.bootstrap_peers, nullptr);
  EXPECT_STREQ("10.0.0.1:8080,[2001:db8::1]:9090", config.bootstrap_peers);
  config_free_members(&config);
}
```

Match the actual parse entry point / fixture style used by the existing tests in `test_config_json.cpp` (they already exercise `http_enabled`-style fields — mirror one).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs --gtest_filter='*ConfigJson*bootstrap*'`
Expected: FAIL — no `bootstrap_peers` member.

- [ ] **Step 3: Write the implementation**

In `src/Configuration/config.h`, after `char* https_key_path;` (line ~65):

```c
  char*    bootstrap_peers;             // CSV of "host:port" network entry points (config-seeded, immutable at runtime)
```

In `src/Configuration/config_json.c` line 15 `_string_fields[]`, add `"bootstrap_peers"` to the list.

In `src/Configuration/config_pending.c`, in the string-field chain (~line 153, after `https_key_path`):

```c
    else if (strcmp(item->string, "bootstrap_peers") == 0 && cJSON_IsString(item))
      config->bootstrap_peers = strdup(item->valuestring);
```

In `src/Configuration/config.c` `config_free_members` (~line 307), add alongside the other string frees:

```c
  if (config->bootstrap_peers != NULL) {
    free(config->bootstrap_peers);
    config->bootstrap_peers = NULL;
  }
```

Also document the field in `docs/CONFIG_FIELDS.md` (one row: `bootstrap_peers`, string, CSV of `host:port` entry points seeded at startup).

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs --gtest_filter='ConfigJsonTest.*'`
Expected: PASS including the new test.

- [ ] **Step 5: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config_json.c src/Configuration/config_pending.c src/Configuration/config.c docs/CONFIG_FIELDS.md test/test_config_json.cpp
git commit -m "feat: bootstrap_peers CSV config field"
```

---

### Task 5: Connect loop + partition heal with backoff

**Files:**
- Modify: `src/Network/network.c` (`network_start_connections` ~line 4493; `network_handle_friend_reconnect_tick` ~line 4420; network struct fields in `src/Network/network.h`)

- [ ] **Step 1: Add heal/backoff state to `network_t`**

In `src/Network/network.h`, near the `friend_reconnect_timer_id` field (~line 328 area — grep `friend_reconnect_timer_id`):

```c
  /* Partition heal: reconnect to bootstrap peers when the node has zero
     connected peers. Exponential backoff in ms (1s doubling to 60s cap). */
  uint64_t bootstrap_next_attempt_ms;
  uint32_t bootstrap_backoff_ms;
```

- [ ] **Step 2: Rewrite `network_start_connections` to use the parser and both lists**

Replace the bootstrap block in `network_start_connections` (network.c:4498–4512) with:

```c
// --- Start connections to bootstrap and friend peers ---
static void network_connect_bootstrap_lists(network_t* network) {
  authority_t* authority = network->authority;
  if (authority == NULL) return;

  for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(authority->bootstrap_peers[index], host, sizeof(host), &port) == 0) {
      network_connect_peer(network, host, port);
    } else {
      /* Config-seeded endpoint failed validation — log and skip. */
    }
  }
  for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(authority->managed_bootstrap_peers[index], host, sizeof(host), &port) == 0) {
      network_connect_peer(network, host, port);
    }
  }
}

void network_start_connections(network_t* network) {
  if (network == NULL) return;

  // Connect to bootstrap peers (fire-and-forget)
  network_connect_bootstrap_lists(network);
```

(keep the existing friend-peers block below unchanged), and add the include `"endpoint.h"` at the top of network.c's includes.

- [ ] **Step 3: Add the partition-heal branch to the reconnect tick**

At the top of `network_handle_friend_reconnect_tick` (network.c:4420), before the friend loop:

```c
static void network_handle_friend_reconnect_tick(network_t* network, message_t* msg) {
  (void)msg;
  if (network->authority == NULL) return;

  /* Partition heal: if nothing is connected, re-enter the network via the
     bootstrap lists. Exponential backoff (1s doubling, 60s cap) so a dead
     bootstrap peer cannot cause a hot connect loop. */
  size_t connected_count = 0;
  for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
    peer_connection_t* peer = network->conn_mgr.peers[index];
    if (peer != NULL && peer->connected) connected_count++;
  }
  if (connected_count == 0 &&
      (authority->bootstrap_peer_count > 0 ||
       authority->managed_bootstrap_peer_count > 0)) {
    uint64_t now_ms = platform_monotonic_ns() / 1000000u;
    if (now_ms >= network->bootstrap_next_attempt_ms) {
      network_connect_bootstrap_lists(network);
      network->bootstrap_backoff_ms =
          network->bootstrap_backoff_ms == 0 ? 1000u
                                             : network->bootstrap_backoff_ms * 2u;
      if (network->bootstrap_backoff_ms > 60000u) {
        network->bootstrap_backoff_ms = 60000u;
      }
      network->bootstrap_next_attempt_ms = now_ms + network->bootstrap_backoff_ms;
    }
  }
```

Note: the original function's early-return `if (network->authority == NULL || network->authority->friend_peers == NULL) return;` must be relaxed to just the NULL check shown above (the friend loop below already tolerates NULL `friend_peers` via the count loop — verify; if it indexes `friend_peers` unguarded, keep a NULL check around the friend loop only).

Backoff reset on success: in the QUIC connected/salutation path, after a peer reaches `peer->connected == true`, set `network->bootstrap_backoff_ms = 0` and `network->bootstrap_next_attempt_ms = 0`. Find the connect-success site via `grep -n "NETWORK_QUIC_CONNECTED" src/Network/network.c` and add the reset in the connected-transition branch (same place the salutation handler marks the peer connected, near line 951–955 where authenticated peers are inserted).

`platform_monotonic_ns` is declared in `src/Platform/platform_time.h` — include it in network.c if not already included.

- [ ] **Step 4: Build and run the network test suite**

Run: `cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs --gtest_filter='*Network*'`
Expected: PASS (no regressions; the heal branch only fires with zero connected peers, which existing tests already tolerate).

- [ ] **Step 5: Commit**

```bash
git add src/Network/network.h src/Network/network.c
git commit -m "feat: bootstrap partition heal with exponential backoff"
```

---

### Task 6: Daemon handlers (bootstrap + friend-save dirty fix)

**Files:**
- Modify: `src/ClientAPI/peer_handlers.h` (declarations after `peer_handle_friend_list_request` ~line 33)
- Modify: `src/ClientAPI/peer_handlers.c` (bootstrap handlers after `peer_handle_friend_list_request` ~line 450; dirty-mark in friend add/remove)

- [ ] **Step 1: Add declarations to `peer_handlers.h`**

```c
void peer_handle_bootstrap_add(peer_handler_ctx_t* ctx, cbor_item_t* frame);
void peer_handle_bootstrap_remove(peer_handler_ctx_t* ctx, cbor_item_t* frame);
void peer_handle_bootstrap_list_request(peer_handler_ctx_t* ctx, cbor_item_t* frame);
```

- [ ] **Step 2: Implement the handlers in `peer_handlers.c`**

After `peer_handle_friend_list_request` add:

```c
// --- Bootstrap peer management (config list immutable; managed list persisted) ---

void peer_handle_bootstrap_add(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_bootstrap_add_t msg;
  if (client_api_bootstrap_add_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid bootstrap add message");
    return;
  }

  /* Keep the normalized endpoint (authority_bootstrap_add stores one), then
     fire-and-forget connect through the shared loop so endpoint parsing
     stays in one place. */
  char endpoint_copy[320];
  snprintf(endpoint_copy, sizeof(endpoint_copy), "%s", msg.endpoint);
  int result = authority_bootstrap_add(ctx->authority, endpoint_copy);
  client_api_bootstrap_add_destroy(&msg);

  uint8_t status = CLIENT_API_STATUS_OK;
  if (result == -1) {
    status = CLIENT_API_STATUS_BAD_REQUEST;
  } else if (result == -2) {
    status = CLIENT_API_STATUS_CONFLICT;  // duplicate entry
  } else if (result != 0) {
    status = CLIENT_API_STATUS_INTERNAL_ERROR;
  }

  if (status == CLIENT_API_STATUS_OK) {
    /* Mark dirty so the debounced save persists the managed list (the Unix
       path previously relied on the shutdown save — see friend-save fix). */
    network_mark_peer_state_dirty(ctx->network);
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(endpoint_copy, host, sizeof(host), &port) == 0) {
      network_connect_peer(ctx->network, host, port);
    }
  }

  client_api_peer_connect_result_t result_frame;
  memset(&result_frame, 0, sizeof(result_frame));
  result_frame.status = status;
  cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result_frame);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_bootstrap_remove(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  client_api_bootstrap_remove_t msg;
  if (client_api_bootstrap_remove_decode(frame, &msg) != 0) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_BAD_REQUEST, "Invalid bootstrap remove message");
    return;
  }

  int result = authority_bootstrap_remove(ctx->authority, msg.endpoint);
  client_api_bootstrap_remove_destroy(&msg);

  uint8_t status = CLIENT_API_STATUS_OK;
  if (result == -1) {
    status = CLIENT_API_STATUS_NOT_FOUND;
  } else if (result == -2) {
    status = CLIENT_API_STATUS_CONFLICT;
  } else if (result != 0) {
    status = CLIENT_API_STATUS_INTERNAL_ERROR;
  } else {
    network_mark_peer_state_dirty(ctx->network);
  }

  client_api_peer_connect_result_t result_frame;
  memset(&result_frame, 0, sizeof(result_frame));
  result_frame.status = status;
  cbor_item_t* out_frame = client_api_peer_connect_result_encode(&result_frame);
  ctx->send_frame(ctx->conn, out_frame);
}

void peer_handle_bootstrap_list_request(peer_handler_ctx_t* ctx, cbor_item_t* frame) {
  (void)frame; /* no payload */

  if (!ctx->is_authenticated) {
    ctx->send_error(ctx->conn, CLIENT_API_STATUS_UNAUTHORIZED, "Authentication required");
    return;
  }

  authority_t* auth = ctx->authority;
  cbor_item_t* entries = cbor_new_definite_array(
      auth->bootstrap_peer_count + auth->managed_bootstrap_peer_count);

  for (size_t index = 0; index < auth->bootstrap_peer_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(auth->bootstrap_peers[index], host, sizeof(host), &port) != 0) continue;
    cbor_item_t* entry = cbor_new_definite_array(3);
    cbor_item_t* host_item = cbor_build_string(host);
    cbor_item_t* port_item = cbor_build_uint16(port);
    cbor_item_t* source_item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_SOURCE_CONFIG);
    (void)cbor_array_push(entry, host_item);
    (void)cbor_array_push(entry, port_item);
    (void)cbor_array_push(entry, source_item);
    cbor_decref(&host_item);
    cbor_decref(&port_item);
    cbor_decref(&source_item);
    (void)cbor_array_push(entries, entry);
    cbor_decref(&entry);
  }
  for (size_t index = 0; index < auth->managed_bootstrap_peer_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(auth->managed_bootstrap_peers[index], host, sizeof(host), &port) != 0) continue;
    cbor_item_t* entry = cbor_new_definite_array(3);
    cbor_item_t* host_item = cbor_build_string(host);
    cbor_item_t* port_item = cbor_build_uint16(port);
    cbor_item_t* source_item = cbor_build_uint8(CLIENT_API_BOOTSTRAP_SOURCE_MANAGED);
    (void)cbor_array_push(entry, host_item);
    (void)cbor_array_push(entry, port_item);
    (void)cbor_array_push(entry, source_item);
    cbor_decref(&host_item);
    cbor_decref(&port_item);
    cbor_decref(&source_item);
    (void)cbor_array_push(entries, entry);
    cbor_decref(&entry);
  }

  client_api_bootstrap_list_response_t response;
  memset(&response, 0, sizeof(response));
  response.entries = entries;
  cbor_item_t* out_frame = client_api_bootstrap_list_response_encode(&response);
  client_api_bootstrap_list_response_destroy(&response);
  ctx->send_frame(ctx->conn, out_frame);
}
```

- [ ] **Step 3: Friend-save fix**

In `peer_handle_friend_add` (peer_handlers.c, after the `auth->friend_peer_count = new_count;` line ~329) and in `peer_handle_friend_remove` (after the shift-down loop, ~line 345), add:

```c
  /* Persist via the debounced dirty flag — previously only the HTTP handlers
     saved immediately, so a crash before shutdown could lose friend changes. */
  network_mark_peer_state_dirty(ctx->network);
```

Confirm `peer_handler_ctx_t` exposes `network` (it does — see `ctx->network->conn_mgr` usage in `peer_handle_friend_add`). Include `"../Network/endpoint.h"` and `"../Network/network.h"` if not already included (endpoint.h is needed by the bootstrap list handler).

- [ ] **Step 4: Build and run the transport/handler test surface**

Run: `cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs --gtest_filter='*UnixTransport*:*WireValidation*'`
Expected: PASS (handlers are exercised further in Task 7's dispatch tests and Task 13's integration run).

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/peer_handlers.h src/ClientAPI/peer_handlers.c
git commit -m "feat: bootstrap peer client-API handlers and friend-save dirty fix"
```

---

### Task 7: Dispatch wiring — all four transports

**Files:**
- Modify: `src/ClientAPI/Unix/unix_connection.c:926-934` (add bootstrap cases after the friend cases)
- Modify: `src/ClientAPI/TCP/tcp_connection.c:831+` (add FRIEND_* + BOOTSTRAP_* cases)
- Modify: `src/ClientAPI/WS/ws_connection.c:1112+` (same)
- Modify: `src/ClientAPI/WT/wt_connection.c:559+` (same)

- [ ] **Step 1: Wire the cases**

In each transport's message-type switch, add (Unix already has the FRIEND cases; add BOOTSTRAP after them):

```c
    case CLIENT_API_BOOTSTRAP_ADD:
      peer_handle_bootstrap_add(&conn->peer_ctx, frame);
      break;
    case CLIENT_API_BOOTSTRAP_REMOVE:
      peer_handle_bootstrap_remove(&conn->peer_ctx, frame);
      break;
    case CLIENT_API_BOOTSTRAP_LIST:
      peer_handle_bootstrap_list_request(&conn->peer_ctx, frame);
      break;
```

For TCP/WS/WT, add the identical FRIEND block first (copying the unix_connection.c:926–934 cases):

```c
    case CLIENT_API_FRIEND_ADD:
      peer_handle_friend_add(&conn->peer_ctx, frame);
      break;
    case CLIENT_API_FRIEND_REMOVE:
      peer_handle_friend_remove(&conn->peer_ctx, frame);
      break;
    case CLIENT_API_FRIEND_LIST:
      peer_handle_friend_list_request(&conn->peer_ctx, frame);
      break;
```

Each transport's connection struct must expose a `peer_ctx` of type `peer_handler_ctx_t`; the Unix connection already has it. For TCP/WS/WT, check the handler-context name with `grep -n "peer_ctx\|peer_handle" src/ClientAPI/TCP/tcp_connection.c` — if those transports don't carry a peer context, add the same `peer_handler_ctx_t` member they already use for `peer_handle_info_request`/`peer_handle_connect` (both of those handlers exist over every transport — confirm by grepping which cases call `peer_handle_*` in each file and reuse that context exactly).

- [ ] **Step 2: Build**

Run: `cmake --build build-test -j$(nproc)`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add src/ClientAPI/Unix/unix_connection.c src/ClientAPI/TCP/tcp_connection.c src/ClientAPI/WS/ws_connection.c src/ClientAPI/WT/wt_connection.c
git commit -m "feat: dispatch friend and bootstrap ops on TCP, WS, and WT transports"
```

---

### Task 8: HTTP `/bootstrap` routes

**Files:**
- Modify: `src/ClientAPI/HTTP/peer_routes.c` (handlers before `peer_routes_register` ~line 607; registration at line 607)

- [ ] **Step 1: Add the three handlers**

Insert before `peer_routes_register`:

```c
/* --- POST /bootstrap --- */

static void _bootstrap_add_handler(http_request_t* request, http_response_t* response,
                                    void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  cJSON* body = cJSON_Parse(request->body);
  if (body == NULL) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  cJSON* endpoint_item = cJSON_GetObjectItem(body, "endpoint");
  if (!cJSON_IsString(endpoint_item) || strlen(cJSON_GetStringValue(endpoint_item)) == 0) {
    cJSON_Delete(body);
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  const char* endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(body, "endpoint"));
  char endpoint_copy[320];
  snprintf(endpoint_copy, sizeof(endpoint_copy), "%s", endpoint);
  cJSON_Delete(body);

  int result = authority_bootstrap_add(ctx->node->authority, endpoint_copy);
  if (result == -2) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "already_bootstrap");
    char* json_str = cJSON_Print(json);
    cJSON_Delete(json);
    http_response_set_status(response, HTTP_STATUS_CONFLICT);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, json_str, strlen(json_str));
    http_response_end(response);
    free(json_str);
    return;
  }
  if (result != 0) {
    http_response_set_status(response, result == -1 ? HTTP_STATUS_BAD_REQUEST
                                                    : HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  network_mark_peer_state_dirty(ctx->node->network);
  /* Connect immediately (fire-and-forget), mirroring the friend add handler. */
  char host[256];
  uint16_t port = 0;
  if (parse_endpoint(endpoint_copy, host, sizeof(host), &port) == 0) {
    network_connect_peer(ctx->node->network, host, port);
  }

  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "added");
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
}

/* --- DELETE /bootstrap (body: {"endpoint": "..."}) --- */

static void _bootstrap_remove_handler(http_request_t* request, http_response_t* response,
                                       void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  cJSON* body = cJSON_Parse(request->body);
  if (body == NULL) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  cJSON* endpoint_item = cJSON_GetObjectItem(body, "endpoint");
  if (!cJSON_IsString(endpoint_item)) {
    cJSON_Delete(body);
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }
  char endpoint[256];
  snprintf(endpoint, sizeof(endpoint), "%s", endpoint_item->valuestring);
  cJSON_Delete(body);

  int result = authority_bootstrap_remove(ctx->node->authority, endpoint);
  if (result == -1) {
    http_response_set_status(response, HTTP_STATUS_NOT_FOUND);
    http_response_end(response);
    return;
  }
  if (result == -2) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "config_immutable");
    char* json_str = cJSON_Print(json);
    cJSON_Delete(json);
    http_response_set_status(response, HTTP_STATUS_CONFLICT);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, json_str, strlen(json_str));
    http_response_end(response);
    free(json_str);
    return;
  }

  network_mark_peer_state_dirty(ctx->node->network);

  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "removed");
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
}

/* --- GET /bootstrap --- */

static void _bootstrap_list_handler(http_request_t* request, http_response_t* response,
                                     void* user_data) {
  peer_routes_ctx_t* ctx = (peer_routes_ctx_t*)user_data;

  if (_check_auth(request, response) != 0) return;

  authority_t* authority = ctx->node->authority;
  cJSON* json = cJSON_CreateObject();
  cJSON* config_arr = cJSON_AddArrayToObject(json, "config");
  cJSON* managed = cJSON_AddArrayToObject(json, "managed");

  for (size_t index = 0; index < authority->bootstrap_peer_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(authority->bootstrap_peers[index], host, sizeof(host), &port) != 0) continue;
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "host", host);
    cJSON_AddNumberToObject(entry, "port", port);
    cJSON_AddItemToArray(config, entry);
  }
  for (size_t index = 0; index < authority->managed_bootstrap_peer_count; index++) {
    char host[256];
    uint16_t port = 0;
    if (parse_endpoint(authority->managed_bootstrap_peers[index], host, sizeof(host), &port) != 0) continue;
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "host", host);
    cJSON_AddNumberToObject(entry, "port", port);
    cJSON_AddItemToArray(managed, entry);
  }

  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
}
```

Add the includes at the top of peer_routes.c if missing: `"../../Network/endpoint.h"` and `"../../Network/network.h"` (already included).

- [ ] **Step 2: Register the routes**

In `peer_routes_register` (~line 607), add after the `/friends` lines:

```c
  http_server_post_with_data(server, "/bootstrap", _bootstrap_add_handler, ctx, NULL);
  http_server_delete_with_data(server, "/bootstrap", _bootstrap_delete_handler_ref, ctx, NULL);
  http_server_get_with_data(server, "/bootstrap", _bootstrap_list_handler, ctx, NULL);
```

(Use the exact delete-registration helper that `DELETE /friends/[^/]+` uses — `http_server_delete_with_data` — and confirm `request->body` is the field name for the raw body by checking `_decode_peer_info_body`'s body access in the same file; adapt if it is `request->data`.)

- [ ] **Step 3: Build**

Run: `cmake --build build-test -j$(nproc)`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/HTTP/peer_routes.c
git commit -m "feat: /bootstrap HTTP routes for peer entry points"
```

---

### Task 9: C client library functions

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h` (callback typedef after line ~104, function declarations after line ~264)
- Modify: `src/ClientLibs/c/offs_client.c` (client-struct fields ~line 192, callback snapshot ~line 618, response dispatch after the `CLIENT_API_FRIEND_LIST_RESPONSE` case ~line 1008, functions after `offs_client_friend_list` ~line 2548)

- [ ] **Step 1: Declarations in `offs_client.h`**

```c
/* Bootstrap list entry, delivered by offs_client_bootstrap_list. */
typedef struct {
  const char* host;   /* NUL-terminated, held via offs_client_release_payload */
  uint16_t port;
  int source;         /* 0 = config, 1 = managed */
} offs_bootstrap_entry_t;

typedef void (*offs_bootstrap_list_cb_t)(void* ctx, uint8_t status,
    const offs_bootstrap_entry_t* entries, size_t entry_count);

int offs_client_bootstrap_add(offs_client_t* client, const char* endpoint,
                              offs_peer_connect_cb_t callback, void* ctx);
int offs_client_bootstrap_remove(offs_client_t* client, const char* endpoint,
                                 offs_peer_connect_cb_t callback, void* ctx);
int offs_client_bootstrap_list(offs_client_t* client,
                               offs_bootstrap_list_cb_t callback, void* ctx);
```

- [ ] **Step 2: Client struct fields and dispatch in `offs_client.c`**

Add to the client struct near `friend_list_cb` (line ~192):

```c
  offs_peer_connect_cb_t bootstrap_result_cb;
  void* bootstrap_result_cb_ctx;
  offs_bootstrap_list_cb_t bootstrap_list_cb;
  void* bootstrap_list_cb_ctx;
```

Add to the callback snapshot block (~line 618, mirroring the friend_list lines):

```c
  offs_peer_connect_cb_t bootstrap_result_cb = client->bootstrap_result_cb;
  void* bootstrap_result_cb_ctx = client->bootstrap_result_cb_ctx;
  offs_bootstrap_list_cb_t bootstrap_list_cb = client->bootstrap_list_cb;
  void* bootstrap_list_cb_ctx = client->bootstrap_list_cb_ctx;
```

Add a dispatch case after the `CLIENT_API_FRIEND_LIST_RESPONSE` case (~line 1008):

```c
    case CLIENT_API_BOOTSTRAP_LIST_RESPONSE: {
      client_api_bootstrap_list_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_bootstrap_list_response_decode(frame, &msg) == 0) {
        if (cbor_isa_array(msg.entries)) {
          size_t array_size = cbor_array_size(msg.entries);
          /* Snapshot handed to the callback; held after the callback returns
             so the consumer can release each entry and the array via
             offs_client_release_payload. */
          offs_bootstrap_entry_t* entries =
              get_clear_memory(array_size * sizeof(offs_bootstrap_entry_t));
          if (array_size == 0 || entries != NULL) {
            size_t count = 0;
            for (size_t index = 0; index < array_size; index++) {
              cbor_item_t* entry = cbor_array_get(msg.entries, index);
              if (cbor_isa_array(entry) && cbor_array_size(entry) == 3) {
                cbor_item_t* host_item = cbor_array_get(entry, 0);
                cbor_item_t* port_item = cbor_array_get(entry, 1);
                cbor_item_t* source_item = cbor_array_get(entry, 2);
                if (cbor_isa_string(host_item) && cbor_isa_uint(port_item) &&
                    cbor_isa_uint(source_item)) {
                  offs_bootstrap_entry_t* slot = &entries[count++];
                  slot->host = strndup((char*)cbor_string_handle(host_item),
                                       cbor_string_length(host_item));
                  slot->port = (uint16_t)cbor_get_int(port_item);
                  slot->source = (int)cbor_get_int(source_item);
                }
                cbor_decref(&host_item);
                cbor_decref(&port_item);
                cbor_decref(&source_item);
              }
              cbor_decref(&entry);
            }
            if (bootstrap_list_cb != NULL) {
              bootstrap_list_cb(bootstrap_list_cb_ctx, CLIENT_API_STATUS_OK, entries, count);
              /* Each host string AND the array stay alive for the consumer. */
              for (size_t index = 0; index < count; index++) {
                _hold_payload(client, (void*)entries[index].host);
              }
              _hold_payload(client, entries);
              _clear_delivered_slot(client, bootstrap_list_cb);
            } else {
              for (size_t index = 0; index < count; index++) {
                free(entries[index].host);
              }
              free(entries);
            }
          }
        }
        client_api_bootstrap_list_response_destroy(&msg);
      }
      break;
    }
```

(The `count` variable pattern: follow how the peer-list case above declares/uses its `count` local — mirror it exactly.)

Add the API functions after `offs_client_friend_list` (~line 2548):

```c
int offs_client_bootstrap_add(offs_client_t* client, const char* endpoint,
                              offs_peer_connect_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected || endpoint == NULL || endpoint[0] == '\0') return -1;

  client_api_bootstrap_add_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.endpoint = (char*)endpoint;

  cbor_item_t* frame = client_api_bootstrap_add_encode(&msg);
  if (frame == NULL) return -1;

  platform_mutex_lock(client->lock);
  client->bootstrap_result_cb = callback;
  client->bootstrap_result_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  _send_frame(client, frame);
  return 0;
}

int offs_client_bootstrap_remove(offs_client_t* client, const char* endpoint,
                                 offs_peer_connect_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected || endpoint == NULL || endpoint[0] == '\0') return -1;

  client_api_bootstrap_remove_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.endpoint = (char*)endpoint;

  cbor_item_t* frame = client_api_bootstrap_remove_encode(&msg);
  if (frame == NULL) return -1;

  platform_mutex_lock(client->lock);
  client->bootstrap_result_cb = callback;
  client->bootstrap_result_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  _send_frame(client, frame);
  return 0;
}

int offs_client_bootstrap_list(offs_client_t* client,
                               offs_bootstrap_list_cb_t callback, void* ctx) {
  if (client == NULL || !client->connected) return -1;

  platform_mutex_lock(client->lock);
  client->bootstrap_list_cb = callback;
  client->bootstrap_list_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  cbor_item_t* frame = client_api_bootstrap_list_request_encode();
  _send_frame(client, frame);
  return 0;
}
```

Also check the `_clear_delivered_slot` / delivered-slot bookkeeping used by the friend list path (lines ~192–193, ~618, ~957–1008) and mirror it for the two new slots so `offs_client_release_payload` stays consistent.

- [ ] **Step 3: Build**

Run: `cmake --build build-test -j$(nproc)`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/ClientLibs/c/offs_client.h src/ClientLibs/c/offs_client.c
git commit -m "feat: bootstrap functions in the C client library"
```

---

### Task 10: JS client + dist rebuild

**Files:**
- Modify: `src/ClientLibs/js/offs-client/src/wire.js` (MSG codes ~line 34; encode/decode after the friend block ~line 470)
- Modify: `src/ClientLibs/js/offs-client/src/index.js` (methods after `friendList` ~line 624)
- Modify: `src/ClientLibs/js/offs-client/src/transports/http-transport.js` (methods after `friendList` ~line 432)
- Test: `src/ClientLibs/js/offs-client/test/unit/wire.test.js` (new)
- Build: `dist/` via `npm run build`

- [ ] **Step 1: Write the failing unit test**

Create `src/ClientLibs/js/offs-client/test/unit/wire.test.js` (match the import style of `test/unit/client.test.js`):

```js
import { describe, it, expect } from 'vitest';
import * as wire from '../../src/wire.js';

describe('bootstrap wire', () => {
  it('encodes add with an endpoint string', () => {
    const bytes = wire.encodeBootstrapAdd('[2001:db8::1]:8080');
    const arr = wire.decode(bytes);
    expect(arr[0]).toBe(wire.MSG.BOOTSTRAP_ADD);
    expect(arr[1]).toBe('[2001:db8::1]:8080');
  });

  it('encodes remove with an endpoint string', () => {
    const bytes = wire.encodeBootstrapRemove('10.0.0.1:8080');
    const arr = wire.decode(bytes);
    expect(arr[0]).toBe(wire.MSG.BOOTSTRAP_REMOVE);
    expect(arr[1]).toBe('10.0.0.1:8080');
  });

  it('encodes a list request and decodes the response', () => {
    const requestBytes = wire.encodeBootstrapListRequest();
    expect(wire.decode(requestBytes)[0]).toBe(wire.MSG.BOOTSTRAP_LIST);

    const responseBytes = wire.encode([wire.MSG.BOOTSTRAP_LIST_RESPONSE, [
      ['10.0.0.1', 8080, 0],
      ['2001:db8::1', 9090, 1],
    ]]);
    const entries = wire.decodeBootstrapListResponse(responseBytes);
    expect(entries).toHaveLength(2);
    expect(entries[1][0]).toBe('2001:db8::1');
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd src/ClientLibs/js/offs-client && npm run test:unit`
Expected: FAIL — `wire.MSG.BOOTSTRAP_ADD` undefined.

- [ ] **Step 3: Implement**

In `src/wire.js` MSG block (after `EPHEMERAL_LIST_RESPONSE: 51,` ~line 34):

```js
  BOOTSTRAP_ADD: 52,
  BOOTSTRAP_REMOVE: 53,
  BOOTSTRAP_LIST: 54,
  BOOTSTRAP_LIST_RESPONSE: 55,
```

After the friend encode/decode block (~line 470):

```js
// --- Bootstrap ---

/**
 * @param {string} endpoint "host:port" or "[ipv6]:port"
 * @returns {Uint8Array}
 */
export function encodeBootstrapAdd(endpoint) {
  return encoder.encode([MSG.BOOTSTRAP_ADD, endpoint]);
}

/**
 * @param {string} endpoint
 * @returns {Uint8Array}
 */
export function encodeBootstrapRemove(endpoint) {
  return encoder.encode([MSG.BOOTSTRAP_REMOVE, endpoint]);
}

/**
 * @returns {Uint8Array}
 */
export function encodeBootstrapListRequest() {
  return encoder.encode([MSG.BOOTSTRAP_LIST]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {any[]} entries of [host, port, source]
 */
export function decodeBootstrapListResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.BOOTSTRAP_LIST_RESPONSE) throw new Error('Not a bootstrap list response');
  return arr[1];
}
```

In `src/index.js`, after `friendList()` (~line 624):

```js
  /**
   * Add a bootstrap endpoint ("host:port" or "[ipv6]:port").
   * @param {string} endpoint
   * @returns {Promise<void>}
   */
  async bootstrapAdd(endpoint) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.bootstrapAdd(endpoint);
    }
    const requestBytes = wire.encodeBootstrapAdd(endpoint);
    await this.transport.send(requestBytes);
  }

  /**
   * @param {string} endpoint
   * @returns {Promise<void>}
   */
  async bootstrapRemove(endpoint) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.bootstrapRemove(endpoint);
    }
    const requestBytes = wire.encodeBootstrapRemove(endpoint);
    await this.transport.send(requestBytes);
  }

  /**
   * @returns {Promise<any[]>} entries of [host, port, source]
   */
  async bootstrapList() {
    if (this.transport instanceof HttpTransport) {
      return this.transport.bootstrapList();
    }
    const requestBytes = wire.encodeBootstrapListRequest();
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.BOOTSTRAP_LIST_RESPONSE);
    return wire.decodeBootstrapListResponse(responseBytes);
  }
```

In `src/transports/http-transport.js` after `friendList()` (~line 432):

```js
  async bootstrapAdd(endpoint) {
    const response = await fetch(this.url('/bootstrap'), {
      method: 'POST',
      headers: { ...this.authHeaders(), 'Content-Type': 'application/json' },
      body: JSON.stringify({ endpoint }),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Bootstrap add failed: ${response.status}`);
  }

  async bootstrapRemove(endpoint) {
    const response = await fetch(this.url('/bootstrap'), {
      method: 'DELETE',
      headers: { ...this.authHeaders(), 'Content-Type': 'application/json' },
      body: JSON.stringify({ endpoint }),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Bootstrap remove failed: ${response.status}`);
  }

  async bootstrapList() {
    const response = await fetch(this.url('/bootstrap'), {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Bootstrap list failed: ${response.status}`);
    return response.json();
  }
```

- [ ] **Step 4: Run tests and lint, rebuild dist**

Run:
```bash
cd src/ClientLibs/js/offs-client
npm run test:unit
npm run lint
npm run build
```
Expected: unit tests PASS; `dist/offs-client.esm.js` and `dist/offs-client.umd.js` regenerated.

- [ ] **Step 5: Commit**

```bash
git add src/ClientLibs/js/offs-client
git commit -m "feat: bootstrap + friend methods in JS client with rebuilt dist"
```

---

### Task 11: offsd flag + config-file key (OFFS repo)

**Files:**
- Modify: `../OFFS/src/offsd/main.c` (args struct, `_parse_args` ~line 455, config-file parser ~line 252, `_startup` ~line 738)

- [ ] **Step 1: Add the flag**

In `offsd_args_t`, add `char* bootstrap_peers;` (near `api_key`). In `_parse_args` (main.c:455), add:

```c
    } else if (strcmp(argv[i], "--bootstrap") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->bootstrap_peers, argv[++i]) != 0) return -1;
```

In `_startup` (~line 742, mirroring the `--max-capacity-bytes` override):

```c
  /* CLI --bootstrap seeds the config-seeded (immutable) bootstrap list. */
  if (args->bootstrap_peers != NULL) {
    server->config.bootstrap_peers = strdup(args->bootstrap_peers);
  }
```

In `_parse_config_file` (~line 311 area), in the `network` section:

```c
    cJSON* bootstrap = cJSON_GetObjectItem(network, "bootstrap-peers");
    if (cJSON_IsString(bootstrap) && args->bootstrap_peers == NULL) {
      args->bootstrap_peers = strdup(bootstrap->valuestring);
    }
```

And add `"  --bootstrap <csv>  Comma-separated bootstrap entry points\n"` to `_print_usage`.

- [ ] **Step 2: Seed the authority at startup**

Where `server->authority` is initialized (near `server->authority->peer_store_path = path_join(args->data_dir, "peer_store.cbor");` at main.c:939), add:

```c
    if (server->config.bootstrap_peers != NULL) {
      authority_set_bootstrap_peers(server->authority, server->config.bootstrap_peers);
    }
```

- [ ] **Step 3: Build offsd**

Run: `cmake --build ../OFFS/build -j$(nproc) 2>&1 | tail -5` (or the OFFS repo's documented build command — check `../OFFS/README.md` / `../OFFS/scripts/`)
Expected: clean build.

- [ ] **Step 4: Commit (in the OFFS repo)**

```bash
git -C ../OFFS add src/offsd/main.c
git -C ../OFFS commit -m "feat: --bootstrap flag and config-file key for network entry points"
```

---

### Task 12: `offs bootstrap` CLI command (OFFS repo)

**Files:**
- Create: `../OFFS/src/offs/commands/bootstrap.c`
- Modify: `../OFFS/src/offs/cli_util.c` (forward decl ~line 21, table ~line 42)
- Modify: `../OFFS/src/offs/l10n/en.h` (strings after the L10N_FRIEND_* block ~line 100)
- Modify: `../OFFS/CMakeLists.txt` or the CLI source registration wherever `commands/friend.c` is listed (find with `grep -rn "friend.c" ../OFFS/src/offs`)

- [ ] **Step 1: l10n strings**

In `../OFFS/src/offs/l10n/en.h` after the friend strings (line ~100):

```c
#define L10N_BOOTSTRAP_DESC          "Bootstrap peer management"
#define L10N_BOOTSTRAP_ADD_USAGE     "Usage: offs bootstrap add <host:port | [ipv6]:port>"
#define L10N_BOOTSTRAP_REMOVE_USAGE  "Usage: offs bootstrap remove <host:port>"
#define L10N_BOOTSTRAP_LIST_PROMPT   "Bootstrap peers:"
```

- [ ] **Step 2: The command**

Create `../OFFS/src/offs/commands/bootstrap.c` (structure mirrors `cmd_friend`; no QR path):

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cbor.h>
#include "ClientAPI/client_api_wire.h"
#include "../client.h"
#include "../l10n/en.h"

int cmd_bootstrap(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    printf("Usage: offs bootstrap <add|remove|list> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "add") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_BOOTSTRAP_ADD_USAGE);
      return 1;
    }
    client_api_bootstrap_add_t req;
    memset(&req, 0, sizeof(req));
    req.endpoint = (char*)argv[1];

    cbor_item_t* request = client_api_bootstrap_add_encode(&req);
    if (request == NULL) {
      fprintf(stderr, "%s\n", L10N_ERROR);
      return 1;
    }
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);
    if (response == NULL) {
      fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
      return 1;
    }
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      if (client_api_error_decode(response, &err_msg) == 0) {
        fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
        client_api_error_destroy(&err_msg);
      }
      cbor_decref(&response);
      return 1;
    }
    cbor_decref(&response);
    printf("%s\n", L10N_OK);
    return 0;
  }

  if (strcmp(subcommand, "remove") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_BOOTSTRAP_REMOVE_USAGE);
      return 1;
    }
    client_api_bootstrap_remove_t remove_req;
    memset(&remove_req, 0, sizeof(remove_req));
    remove_req.endpoint = (char*)argv[1];

    cbor_item_t* request = client_api_bootstrap_remove_encode(&remove_req);
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);
    if (response == NULL) {
      fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
      return 1;
    }
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      if (client_api_error_decode(response, &err_msg) == 0) {
        fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
        client_api_error_destroy(&err_msg);
      }
      cbor_decref(&response);
      return 1;
    }
    cbor_decref(&response);
    printf("%s\n", L10N_OK);
    return 0;
  }

  if (strcmp(subcommand, "list") == 0) {
    cbor_item_t* request = client_api_bootstrap_list_request_encode();
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);
    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_BOOTSTRAP_LIST_RESPONSE) {
        client_api_bootstrap_list_response_t list;
        memset(&list, 0, sizeof(list));
        if (client_api_bootstrap_list_response_decode(response, &list) == 0) {
          printf("%s\n", L10N_BOOTSTRAP_LIST_PROMPT);
          if (list.entries != NULL) {
            size_t count = cbor_array_size(list.entries);
            for (size_t index = 0; index < count; index++) {
              cbor_item_t* entry = cbor_array_get(list.entries, index);
              if (cbor_isa_array(entry) && cbor_array_size(entry) == 3) {
                cbor_item_t* host_item = cbor_array_get(entry, 0);
                cbor_item_t* port_item = cbor_array_get(entry, 1);
                cbor_item_t* source_item = cbor_array_get(entry, 2);
                const char* source = cbor_get_uint8(source_item) == 0 ? "config" : "managed";
                printf("  %s:%u (%s)\n", cbor_string_handle(host_item),
                       (unsigned)cbor_get_int(port_item),
                       source);
                cbor_decref(&host_item);
                cbor_decref(&port_item);
                cbor_decref(&source_item);
              }
              cbor_decref(&entry);
            }
          }
          client_api_bootstrap_list_response_destroy(&list);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  printf("Usage: offs bootstrap <add|remove|list> ...\n");
  return 1;
}
```

(The `source` label prints `config` for `CLIENT_API_BOOTSTRAP_SOURCE_CONFIG` and `managed` otherwise.)

In `../OFFS/src/offs/cli_util.c`: add `int cmd_bootstrap(int argc, char** argv, cli_client_t* client);` near line 21 and `{"bootstrap", L10N_BOOTSTRAP_DESC, cmd_bootstrap},` before the `{"help", ...}` entry (line 46). Add `commands/bootstrap.c` to the CLI's source list wherever `commands/friend.c` is compiled.

- [ ] **Step 3: Build and exercise**

Run: build the offs CLI (documented OFFS build), then with a running offsd:
```bash
offs bootstrap add 10.0.0.1:8080      # expect: OK
offs bootstrap list                   # shows the entry with source marker
offs bootstrap remove 10.0.0.1:8080
offs bootstrap list                   # empty managed list
```
Expected: add/remove print OK; list prints host:port + source markers.

- [ ] **Step 4: Commit**

```bash
git -C ../OFFS add src/offs/commands/bootstrap.c src/offs/cli_util.c src/offs/l10n/en.h
git -C ../OFFS commit -m "feat: offs bootstrap add|remove|list CLI commands"
```

---

### Task 13: Integration verification + closure

**Files:** none (verification only). Touches: harmony tickets, submodule bump.

- [ ] **Step 1: Build both repos and run the full liboffs test suite**

```bash
cmake --build build-test -j$(nproc) && ./build-test/test/testliboffs
```
Expected: all tests PASS.

- [ ] **Step 2: Valgrind the touched suites (DWARF4 build — see valgrind DWARF5 quirk)**

```bash
cmake --build build-gdwarf4 -j$(nproc) 2>/dev/null || true
valgrind --leak-check=full ./build-gdwarf4/test/testliboffs --gtest_filter='EndpointTest.*:BootstrapWire.*:AuthorityBootstrap.*:ConfigJsonTest.*'
```
Expected: 0 leaks, no invalid reads. (Known pre-existing issues to ignore: scheduler `_scheduler_worker_loop` read at scheduler.c:119 during shutdown.)

- [ ] **Step 3: End-to-end run with offsd**

With node certs available (see `../OFFS` e2e pattern):
```bash
cd ../OFFS && ./build/offsd --bootstrap "127.0.0.1:<quic-port-2>" --data-dir /tmp/offs-e2e-a --foreground &
# second node or CLI:
offs bootstrap add 127.0.0.1:<quic-port>
offs bootstrap list           # shows config + managed entries
curl -H "Authorization: Bearer $KEY" http://127.0.0.1:<http-port>/bootstrap
```
Expected: both lists visible; `offs bootstrap list` marks sources; peer connects.

- [ ] **Step 4: De-wonk audit**

Invoke the de-wonk skill on the completed implementation; resolve every unimplemented/stubbed/disabled/broken/weird finding; verify no TODO/FIXME/HACK/XXX remain in touched files (`grep -rn "TODO\|FIXME\|HACK\|XXX" src/Network/endpoint.c src/Network/authority.c src/ClientAPI/ ../OFFS/src/offs/commands/bootstrap.c`).

- [ ] **Step 5: Bump the submodule in ../OFFS and close harmony tickets**

```bash
git -C ../OFFS/deps/liboffs fetch origin master 2>/dev/null || true
git -C ../OFFS add deps/liboffs
git -C ../OFFS commit -m "chore: bump liboffs submodule (bootstrap peers)"
```
Then close the epic tickets created for this feature in Harmony (`$H ticket close <N> "..."`) per CLAUDE.md.

- [ ] **Step 6: Final commit of any stragglers**

```bash
git add -A -- src test docs && git commit -m "chore: bootstrap peers verification pass"
```
(only if anything is uncommitted)

---

## Self-review notes

- Spec coverage: data model + index 6 (Task 3), config seed (Task 4), engagement + heal (Task 5), wire ops all four transports (Tasks 2, 6, 7), HTTP (Task 8), CLI/C/JS bindings (Tasks 9, 10, 11, 12), endpoint parser (Task 1), friend-save fix (Task 6), friend parity on TCP/WS/WT (Task 7), errors (Task 3/6/8 status mappings), testing (all tasks + Task 13).
- Type consistency: `parse_endpoint` (Task 1) is used by Tasks 3, 5, 6, 8; `authority_bootstrap_add/remove` (Task 3) is used by Tasks 6, 8; wire codes 52–55 are consistent across Tasks 2, 6, 7, 9, 10, 12 (JS `MSG` values match the C defines).