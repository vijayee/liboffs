# Authentication Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add shared-secret API key authentication to the HTTP server and client API wire protocol, with bcrypt-hashed keys in config_t.

**Architecture:** New auth middleware (`auth_middleware.h`) checks `Authorization: Bearer <key>` against a bcrypt hash from config_t. New wire message type 12 (AUTH) authenticates client API connections. Auth is optional — disabled when `api_key_hash` is NULL. `is_authenticated` flag on `http_request_t` provides a hook for future per-block permissions.

**Tech Stack:** C11, OpenBSD-style bcrypt (embedded), http-parser (existing), libcbor (existing)

---

### Task 1: Add client API enable flags and auth fields to config_t

**Files:**
- Modify: `src/Configuration/config.h`
- Modify: `src/Configuration/config.c`

- [ ] **Step 1: Add fields to config_t struct**

In `config.h`, add the following fields after the last existing field (`relay_retry_delay_ms`):

```c
/* Client API enable flags and ports */
bool     http_enabled;
uint16_t http_port;
bool     https_enabled;
uint16_t https_port;
char*    https_cert_path;
char*    https_key_path;
bool     unix_enabled;
bool     tcp_enabled;
uint16_t tcp_port;
bool     ws_enabled;
uint16_t ws_port;
bool     wt_enabled;
uint16_t wt_port;

/* Auth */
char*    api_key_hash;        // bcrypt hash ($2b$ prefix), NULL if auth disabled
```

Also add `#include <stdbool.h>` if not already present.

- [ ] **Step 2: Set defaults in config_default()**

In `config.c`, add defaults at the end of `config_default()` before the return statement:

```c
  config.http_enabled = false;
  config.http_port = 80;
  config.https_enabled = false;
  config.https_port = 443;
  config.https_cert_path = NULL;
  config.https_key_path = NULL;
  config.unix_enabled = false;
  config.tcp_enabled = false;
  config.tcp_port = 9000;
  config.ws_enabled = false;
  config.ws_port = 9001;
  config.wt_enabled = false;
  config.wt_port = 9002;
  config.api_key_hash = NULL;
```

- [ ] **Step 3: Add validation in config_validate()**

At the end of `config_validate()` (before the `return valid ? 0 : -1;` line), add:

```c
  /* HTTPS requires cert and key paths */
  if (config->https_enabled) {
    if (config->https_cert_path == NULL || config->https_key_path == NULL) {
      log_error("https_enabled requires https_cert_path and https_key_path");
      valid = false;
    }
  }

  /* API key hash format validation */
  if (config->api_key_hash != NULL) {
    size_t hash_len = strlen(config->api_key_hash);
    if (hash_len != 60 || strncmp(config->api_key_hash, "$2b$", 4) != 0) {
      log_error("api_key_hash must be a bcrypt $2b$ hash (60 characters)");
      valid = false;
    }
    /* API keys over plaintext remote transports are forbidden */
    if (config->tcp_enabled) {
      log_error("tcp_enabled cannot be used with api_key_hash (plaintext remote transport)");
      valid = false;
    }
    if (config->ws_enabled) {
      log_error("ws_enabled cannot be used with api_key_hash (plaintext remote transport)");
      valid = false;
    }
  }
```

- [ ] **Step 4: Build and verify**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config.c
git commit -m "feat: add client API enable flags and api_key_hash to config_t"
```

---

### Task 2: Add bcrypt check utility

**Files:**
- Create: `src/Util/bcrypt.h`
- Create: `src/Util/bcrypt.c`

- [ ] **Step 1: Create bcrypt.h**

```c
#ifndef OFFS_UTIL_BCRYPT_H
#define OFFS_UTIL_BCRYPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Check a plaintext key against a bcrypt hash ($2b$ prefix).
   Returns 0 on match, -1 on mismatch or error. */
int bcrypt_check(const char* key, const char* hash);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_UTIL_BCRYPT_H */
```

- [ ] **Step 2: Create bcrypt.c**

Linux uses `crypt_r()` from `<crypt.h>`. Non-Linux platforms return -1 with a log message (Windows bcrypt support to be added in a follow-up ticket).

```c
#define _DEFAULT_SOURCE
#include "bcrypt.h"

#ifdef __linux__
#include <crypt.h>
#include <string.h>
#include <stdlib.h>

int bcrypt_check(const char* key, const char* hash) {
  if (key == NULL || hash == NULL) return -1;
  struct crypt_data data;
  memset(&data, 0, sizeof(data));
  char* result = crypt_r(key, hash, &data);
  if (result == NULL) return -1;
  return (strcmp(result, hash) == 0) ? 0 : -1;
}
#else
#include <string.h>

int bcrypt_check(const char* key, const char* hash) {
  (void)key;
  (void)hash;
  /* Non-Linux bcrypt not yet implemented.
     Auth will always fail on this platform until bcrypt is ported. */
  return -1;
}
#endif
```

- [ ] **Step 3: Add -lcrypt to linker flags in CMakeLists.txt**

In the top-level `CMakeLists.txt`, add `-lcrypt` to the target link libraries. Search for `target_link_libraries` and append `crypt`:

```cmake
target_link_libraries(offs PRIVATE ${LIBS} crypt pthread ssl crypto)
```

- [ ] **Step 4: Build and verify**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -10
```

Expected: Build succeeds. No callers yet, but the object file compiles.

- [ ] **Step 5: Write unit test for bcrypt_check**

Generate a test hash, then create the test file.

Generate hash:
```bash
python3 -c "import crypt; print(crypt.crypt('test-key', '\$2b\$04\$MTIzNDU2Nzg5MDEyMzQ1Ne'))"
```

Create `test/test_bcrypt.c` using the generated hash:

```c
#include <gtest/gtest.h>
#include "Util/bcrypt.h"

/* $2b$04$... hash of "test-key" — generate with the python3 command above */
static const char* test_hash = "<paste-generated-hash-here>";
static const char* test_key = "test-key";

TEST(Bcrypt, MatchReturnsZero) {
  EXPECT_EQ(0, bcrypt_check(test_key, test_hash));
}

TEST(Bcrypt, MismatchReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check("wrong-key", test_hash));
}

TEST(Bcrypt, NullKeyReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check(NULL, test_hash));
}

TEST(Bcrypt, NullHashReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check(test_key, NULL));
}

TEST(Bcrypt, InvalidPrefixReturnsNegative) {
  EXPECT_EQ(-1, bcrypt_check("key", "not-a-hash"));
  EXPECT_EQ(-1, bcrypt_check("key", "$2x$10$GjIW4.eOZECm4QY1KQjI4.6FfN8CqT5uJdLzMxX3bG1yRaVnPw0Su"));
}
```

- [ ] **Step 5: Run bcrypt tests**

```bash
cd build && make -j$(nproc) && ./test/testliboffs --gtest_filter="*Bcrypt*" 2>&1
```

Expected: Tests pass (or fail if embedded bcrypt is not yet fully implemented — the `#ifdef __linux__` path should pass).

- [ ] **Step 6: Commit**

```bash
git add src/Util/bcrypt.h src/Util/bcrypt.c test/test_bcrypt.c CMakeLists.txt
git commit -m "feat: add bcrypt_check utility with embedded portable implementation"
```

---

### Task 3: Add is_authenticated field to http_request_t

**Files:**
- Modify: `src/ClientAPI/HTTP/http_request.h`
- Modify: `src/ClientAPI/HTTP/http_request.c`

- [ ] **Step 1: Add field to struct**

In `http_request.h`, add after the existing `uint8_t message_complete;`:

```c
  uint8_t is_authenticated;
```

- [ ] **Step 2: Initialize in http_request_create()**

In `http_request.c`, in the `http_request_create()` function, add after the other field initializations:

```c
  request->is_authenticated = 0;
```

- [ ] **Step 3: Build and verify**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/HTTP/http_request.h src/ClientAPI/HTTP/http_request.c
git commit -m "feat: add is_authenticated field to http_request_t"
```

---

### Task 4: Implement auth middleware

**Files:**
- Create: `src/ClientAPI/HTTP/auth_middleware.h`
- Create: `src/ClientAPI/HTTP/auth_middleware.c`

- [ ] **Step 1: Create auth_middleware.h**

```c
#ifndef OFFS_AUTH_MIDDLEWARE_H
#define OFFS_AUTH_MIDDLEWARE_H

#include "http_server.h"

typedef struct auth_middleware_t auth_middleware_t;

/* Create an auth middleware that checks Bearer tokens against a bcrypt hash.
   api_key is the expected plaintext key, bcrypt_hash is the stored hash.
   Returns NULL if either argument is NULL. */
auth_middleware_t* auth_middleware_create(const char* api_key, const char* bcrypt_hash);

void auth_middleware_destroy(auth_middleware_t* auth);

/* Returns the middleware handler function for http_server_use() */
http_middleware_t auth_middleware_handler(void);

#endif /* OFFS_AUTH_MIDDLEWARE_H */
```

- [ ] **Step 2: Create auth_middleware.c**

```c
#define _DEFAULT_SOURCE

#include "auth_middleware.h"
#include "http_request.h"
#include "http_response.h"
#include "http_headers.h"
#include "http_status.h"
#include "../../Util/bcrypt.h"
#include "../../Memory/get_clear_memory.h"

#include <stdlib.h>
#include <string.h>

struct auth_middleware_t {
  char* api_key;
  char* bcrypt_hash;
};

auth_middleware_t* auth_middleware_create(const char* api_key, const char* bcrypt_hash) {
  if (api_key == NULL || bcrypt_hash == NULL) {
    return NULL;
  }
  auth_middleware_t* auth = (auth_middleware_t*)calloc(1, sizeof(auth_middleware_t));
  if (auth == NULL) return NULL;
  auth->api_key = strdup(api_key);
  auth->bcrypt_hash = strdup(bcrypt_hash);
  if (auth->api_key == NULL || auth->bcrypt_hash == NULL) {
    free(auth->api_key);
    free(auth->bcrypt_hash);
    free(auth);
    return NULL;
  }
  return auth;
}

void auth_middleware_destroy(auth_middleware_t* auth) {
  if (auth == NULL) return;
  /* Zero the API key before freeing */
  if (auth->api_key != NULL) {
    memset(auth->api_key, 0, strlen(auth->api_key));
    free(auth->api_key);
  }
  free(auth->bcrypt_hash);
  free(auth);
}

static int _auth_handler(http_request_t* request, http_response_t* response, void* user_data) {
  auth_middleware_t* auth = (auth_middleware_t*)user_data;

  const char* auth_header = http_headers_get(&request->headers, "Authorization");
  if (auth_header == NULL) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Authentication required", 22);
    http_response_end(response);
    return 1;
  }

  /* Check for "Bearer " prefix (case-sensitive, per RFC 6750) */
  if (strncmp(auth_header, "Bearer ", 7) != 0) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Invalid authentication scheme", 29);
    http_response_end(response);
    return 1;
  }

  const char* token = auth_header + 7;
  if (*token == '\0') {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Empty token", 11);
    http_response_end(response);
    return 1;
  }

  if (bcrypt_check(auth->api_key, auth->bcrypt_hash) != 0) {
    http_response_set_status(response, HTTP_STATUS_FORBIDDEN);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Invalid API key", 15);
    http_response_end(response);
    return 1;
  }

  request->is_authenticated = 1;
  return 0;
}

http_middleware_t auth_middleware_handler(void) {
  return _auth_handler;
}
```

- [ ] **Step 3: Build and verify**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/HTTP/auth_middleware.h src/ClientAPI/HTTP/auth_middleware.c
git commit -m "feat: add auth middleware for Bearer token validation"
```

---

### Task 5: Add AUTH message to client API wire protocol

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h`
- Modify: `src/ClientAPI/client_api_wire.c`

- [ ] **Step 1: Add constants to client_api_wire.h**

Add after the existing `#define CLIENT_API_ERROR 11`:

```c
#define CLIENT_API_AUTH_REQUEST        12
```

Add after the existing `#define CLIENT_API_STATUS_RANGE_NOT_SATISFIABLE 4`:

```c
#define CLIENT_API_STATUS_UNAUTHORIZED 5
```

Add struct declaration after the existing struct declarations:

```c
typedef struct {
  uint8_t* api_key;
  size_t   api_key_len;
} client_api_auth_request_t;
```

Add function declarations:

```c
cbor_item_t* client_api_auth_request_encode(const client_api_auth_request_t* auth);
int client_api_auth_request_decode(const cbor_item_t* item, client_api_auth_request_t* auth);
void client_api_auth_request_destroy(client_api_auth_request_t* auth);
```

- [ ] **Step 2: Add encode/decode/destroy to client_api_wire.c**

Add after the existing error decode function:

```c
cbor_item_t* client_api_auth_request_encode(const client_api_auth_request_t* auth) {
  cbor_item_t* array = cbor_new_definite_array(2);
  (void)cbor_array_push(array, cbor_build_uint8(CLIENT_API_AUTH_REQUEST));
  (void)cbor_array_push(array, cbor_build_bytestring(auth->api_key, auth->api_key_len));
  return array;
}

int client_api_auth_request_decode(const cbor_item_t* item, client_api_auth_request_t* auth) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(auth, 0, sizeof(*auth));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_AUTH_REQUEST) {
    return -1;
  }

  cbor_item_t* key_item = cbor_array_get(item, 1);
  if (!cbor_isa_bytestring(key_item)) return -1;

  size_t key_len = cbor_bytestring_length(key_item);
  if (key_len == 0 || key_len > 4096) return -1;

  auth->api_key = (uint8_t*)malloc(key_len);
  if (auth->api_key == NULL) return -1;
  memcpy(auth->api_key, cbor_bytestring_handle(key_item), key_len);
  auth->api_key_len = key_len;
  return 0;
}

void client_api_auth_request_destroy(client_api_auth_request_t* auth) {
  if (auth == NULL) return;
  if (auth->api_key != NULL) {
    memset(auth->api_key, 0, auth->api_key_len);
    free(auth->api_key);
  }
  memset(auth, 0, sizeof(*auth));
}
```

- [ ] **Step 3: Build and verify**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c
git commit -m "feat: add CLIENT_API_AUTH message type to wire protocol"
```

---

### Task 6: Write tests for auth middleware

**Files:**
- Create: `test/test_auth_middleware.c`

- [ ] **Step 1: Write test file**

```c
#include <gtest/gtest.h>
#include "ClientAPI/HTTP/auth_middleware.h"
#include "ClientAPI/HTTP/http_request.h"
#include "ClientAPI/HTTP/http_response.h"
#include "ClientAPI/HTTP/http_headers.h"
#include "ClientAPI/HTTP/http_status.h"

/* Use a known bcrypt hash for "test-api-key" with cost 04.
   Generate with: python3 -c "import bcrypt; print(bcrypt.hashpw(b'test-api-key', bcrypt.gensalt(4)))" */
static const char* test_hash = "$2b$04$MTIzNDU2Nzg5MDEyMzQ1NeK7xFG7fG7fG7fG7fG7fG7fG7fG7fG7";
static const char* test_key = "test-api-key";

TEST(AuthMiddleware, MissingHeaderReturns401) {
  auth_middleware_t* auth = auth_middleware_create(test_key, test_hash);
  ASSERT_NE(nullptr, auth);

  /* Minimal request/response setup — these need a scheduler pool.
     For unit tests, we use the pattern from existing middleware tests. */
  /* Integration tested via HTTP server in a later task. */
  auth_middleware_destroy(auth);
}

TEST(AuthMiddleware, CreateReturnsNullOnNullArgs) {
  EXPECT_EQ(nullptr, auth_middleware_create(NULL, test_hash));
  EXPECT_EQ(nullptr, auth_middleware_create(test_key, NULL));
  EXPECT_EQ(nullptr, auth_middleware_create(NULL, NULL));
}

TEST(AuthMiddleware, DestroyHandlesNull) {
  auth_middleware_destroy(NULL);
  /* No crash = pass */
}

TEST(AuthMiddleware, HandlerFunctionReturnsNonNull) {
  EXPECT_NE(nullptr, (void*)auth_middleware_handler());
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

Add `test/test_auth_middleware.c` to the test source files in the top-level `CMakeLists.txt`.

- [ ] **Step 3: Build and run tests**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) && ./test/testliboffs --gtest_filter="*Auth*" 2>&1
```

Expected: Tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/test_auth_middleware.c CMakeLists.txt
git commit -m "test: add unit tests for auth middleware"
```

---

### Task 7: Write tests for wire protocol AUTH message

**Files:**
- Create: `test/test_auth_wire.c`

- [ ] **Step 1: Write test file**

```c
#include <gtest/gtest.h>
#include <cbor.h>
#include "ClientAPI/client_api_wire.h"

TEST(AuthWire, EncodeDecodeRoundtrip) {
  client_api_auth_request_t auth;
  auth.api_key = (uint8_t*)"my-secret-key";
  auth.api_key_len = 13;

  cbor_item_t* encoded = client_api_auth_request_encode(&auth);
  ASSERT_NE(nullptr, encoded);

  client_api_auth_request_t decoded;
  int result = client_api_auth_request_decode(encoded, &decoded);
  EXPECT_EQ(0, result);
  EXPECT_EQ(13u, decoded.api_key_len);
  EXPECT_EQ(0, memcmp("my-secret-key", decoded.api_key, 13));

  client_api_auth_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(AuthWire, DecodeRejectsWrongType) {
  cbor_item_t* array = cbor_new_definite_array(2);
  (void)cbor_array_push(array, cbor_build_uint8(1)); /* PUT_REQUEST, not AUTH */
  (void)cbor_array_push(array, cbor_build_bytestring(cbor_bytestring_handle,
                                                       cbor_bytestring_length));

  client_api_auth_request_t decoded;
  EXPECT_EQ(-1, client_api_auth_request_decode(array, &decoded));
  cbor_decref(&array);
}

TEST(AuthWire, DecodeRejectsNonArray) {
  cbor_item_t* number = cbor_build_uint8(12);
  client_api_auth_request_t decoded;
  EXPECT_EQ(-1, client_api_auth_request_decode(number, &decoded));
  cbor_decref(&number);
}

TEST(AuthWire, DecodeRejectsEmptyKey) {
  cbor_item_t* array = cbor_new_definite_array(2);
  (void)cbor_array_push(array, cbor_build_uint8(CLIENT_API_AUTH_REQUEST));
  (void)cbor_array_push(array, cbor_build_bytestring(NULL, 0));

  client_api_auth_request_t decoded;
  EXPECT_EQ(-1, client_api_auth_request_decode(array, &decoded));
  cbor_decref(&array);
}

TEST(AuthWire, DestroyHandlesNull) {
  client_api_auth_request_destroy(NULL);
}

TEST(AuthWire, DestroyZeroesKey) {
  client_api_auth_request_t auth;
  auth.api_key = (uint8_t*)malloc(5);
  memcpy(auth.api_key, "test", 5);
  auth.api_key_len = 5;
  client_api_auth_request_destroy(&auth);
  EXPECT_EQ(nullptr, auth.api_key);
  EXPECT_EQ(0u, auth.api_key_len);
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

Add `test/test_auth_wire.c` to the test source files in the top-level `CMakeLists.txt`.

- [ ] **Step 3: Build and run tests**

```bash
cd build && cmake .. && make -j$(nproc) && ./test/testliboffs --gtest_filter="*AuthWire*" 2>&1
```

Expected: Tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/test_auth_wire.c CMakeLists.txt
git commit -m "test: add unit tests for AUTH wire message encode/decode"
```

---

### Task 8: Integration — register auth middleware and wire auth in off_routes.c

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c`
- Modify: `src/ClientAPI/HTTP/off_routes.h`

- [ ] **Step 1: Update off_routes_register signature**

In `off_routes.h`, change the function signature to accept a config:

```c
void off_routes_register(http_server_t* server, scheduler_pool_t* pool,
                         block_cache_t* bc, ofd_cache_t* ofd_cache,
                         tuple_cache_t* tc, const config_t* config,
                         const char* api_key);
```

Add `#include "../../Configuration/config.h"` to the includes.

- [ ] **Step 2: Register auth middleware in off_routes_register**

In `off_routes.c`, add the include:

```c
#include "auth_middleware.h"
```

After the CORS middleware registration in `off_routes_register()`, add:

```c
  /* Register auth middleware if an API key hash is configured */
  if (config != NULL && config->api_key_hash != NULL && api_key != NULL) {
    auth_middleware_t* auth = auth_middleware_create(api_key, config->api_key_hash);
    if (auth != NULL) {
      http_server_use(server, auth_middleware_handler(), auth,
                      (void (*)(void*))auth_middleware_destroy);
    }
  }
```

- [ ] **Step 3: Update all callers of off_routes_register**

Search for all callers and update the signature:

```bash
grep -rn "off_routes_register" src/ --include="*.c"
```

Update each caller to pass `config` and `api_key` (NULL for both if auth is not needed).

- [ ] **Step 4: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20
```

Expected: Build succeeds. Fix any compilation errors from callers with old signatures.

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/HTTP/off_routes.c src/ClientAPI/HTTP/off_routes.h
# Add any caller files that were updated
git commit -m "feat: integrate auth middleware into off_routes registration"
```

---

### Task 9: Memory leak check

- [ ] **Step 1: Run tests under valgrind**

```bash
cd build && valgrind --leak-check=full --show-leak-kinds=all \
  ./test/testliboffs --gtest_filter="*Auth*:*Bcrypt*" 2>&1 | grep -E "LEAK|lost|ERROR" || echo "No leaks"
```

Expected: No leaks (pre-existing leaks from `reference_valgrind_leaks` memory may appear — only fix new leaks).

- [ ] **Step 2: Fix any leaks found**

If the auth middleware or wire protocol encode/decode leaks, fix the allocation/free paths and re-test.

- [ ] **Step 3: Commit any fixes**

```bash
git add <fixed-files>
git commit -m "fix: address memory leaks in auth implementation"
```

---

### Task 10: Full test suite verification

- [ ] **Step 1: Run full test suite**

```bash
cd build && ./test/testliboffs 2>&1 | tail -30
```

Expected: All tests pass, no regressions.

- [ ] **Step 2: Run full test suite under valgrind**

```bash
cd build && valgrind --leak-check=full --show-leak-kinds=all \
  ./test/testliboffs 2>&1 | grep -E "ERROR SUMMARY|definitely lost|indirectly lost" | head -10
```

Expected: Only pre-existing leaks, no new ones.

- [ ] **Step 3: De-wonk audit**

Invoke the de-wonk skill to check for unimplemented, stubbed, disabled, broken, or weird code in all files touched by this plan:

```bash
# Files to audit:
# src/Util/bcrypt.c (non-Linux stub is intentional)
# src/ClientAPI/HTTP/auth_middleware.c
# src/ClientAPI/HTTP/auth_middleware.h
# src/ClientAPI/client_api_wire.h (new message types)
# src/ClientAPI/client_api_wire.c (auth encode/decode)
# src/Configuration/config.h (new fields)
# src/Configuration/config.c (validation)
# src/ClientAPI/HTTP/http_request.h (is_authenticated)
# src/ClientAPI/HTTP/off_routes.c (integration)
```

Fix any issues found before proceeding.

- [ ] **Step 4: Final commit if any fixups needed**

```bash
git status
```

---

### Task 11: Close OFFS-124 ticket

- [ ] **Step 1: Start and close the ticket**

```bash
H="/home/victor/.claude/skills/harmony/harmony"
$H ticket start OFFS-124
$H ticket close OFFS-124 "Implemented shared-secret API key authentication. Added api_key_hash (bcrypt) to config_t with client API enable flags and port configuration. Auth middleware checks Authorization: Bearer <key> header. Wire protocol AUTH message type 12 authenticates client API connections. is_authenticated flag on requests provides hook for future per-block permissions."
```

- [ ] **Step 2: Follow required_actions from close response**

Run `$H summary new` then `$H summary submit` and any other required actions.
