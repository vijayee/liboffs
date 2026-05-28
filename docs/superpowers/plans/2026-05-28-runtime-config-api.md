# Runtime Config API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add HTTP endpoints to read/update node config and trigger graceful restart, using cJSON for all JSON handling.

**Architecture:** All config changes are validated and saved to a pending JSON file, then applied on restart. Three new endpoints (GET/PUT /config, POST /config/restart) follow the existing peer_routes pattern. cJSON replaces all manual snprintf JSON construction across the codebase. TCP TLS config fields are added to config_t.

**Tech Stack:** C11, cJSON (vendored), OpenSSL, CMake

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `deps/cJSON/cJSON.c` | Create | Vendored JSON parser/writer |
| `deps/cJSON/cJSON.h` | Create | Vendored JSON header |
| `src/Configuration/config.h` | Modify | Add tcp_tls_* fields |
| `src/Configuration/config.c` | Modify | Defaults + validation for tcp_tls_* |
| `src/Configuration/config_pending.h` | Create | Pending config save/load/merge API |
| `src/Configuration/config_pending.c` | Create | Pending config file I/O with cJSON |
| `src/ClientAPI/HTTP/config_routes.h` | Create | Config route registration declaration |
| `src/ClientAPI/HTTP/config_routes.c` | Create | GET/PUT /config, POST /config/restart handlers |
| `src/ClientAPI/HTTP/http_server.h` | Modify | Add is_local_binding field + accessor |
| `src/ClientAPI/HTTP/http_server.c` | Modify | Set is_local_binding at creation |
| `src/ClientAPI/health_handler.h` | Modify | Change return type to cJSON* |
| `src/ClientAPI/health_handler.c` | Modify | Rewrite with cJSON builder |
| `src/ClientAPI/HTTP/peer_routes.c` | Modify | Replace manual JSON with cJSON |
| `src/ClientAPI/HTTP/health_routes.c` | Modify | Adapt to new health_data_to_json signature |
| `src/ClientAPI/WS/ws_connection.c` | Modify | Adapt to new health_data_to_json signature |
| `src/ClientAPI/TCP/tcp_connection.c` | Modify | Adapt to new health_data_to_json signature |
| `src/ClientAPI/WT/wt_connection.c` | Modify | Adapt to new health_data_to_json signature |
| `src/ClientAPI/Unix/unix_connection.c` | Modify | Adapt to new health_data_to_json signature |
| `src/Node/node.h` | Modify | Add offs_node_restart declaration |
| `src/Node/node.c` | Modify | Implement offs_node_restart |
| `CMakeLists.txt` | Modify | Add cJSON library target |
| `examples/off_server/main.c` | Modify | Wire config_routes registration |

---

### Task 1: Vendor cJSON

**Files:**
- Create: `deps/cJSON/cJSON.c`
- Create: `deps/cJSON/cJSON.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Download cJSON 1.7.18**

Download the single-file cJSON release from GitHub and place in deps/cJSON/.

Run:
```bash
curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c -o deps/cJSON/cJSON.c
curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h -o deps/cJSON/cJSON.h
```

Verify:
```bash
wc -l deps/cJSON/cJSON.c deps/cJSON/cJSON.h
```

- [ ] **Step 2: Add cJSON to CMakeLists.txt**

Add after the http-parser block (after line 48):

```cmake
# cJSON
add_library(cjson STATIC deps/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC deps/cJSON)
```

And link it to the offs target. After `target_link_libraries(offs PRIVATE crypt)` (line 62), add:

```cmake
target_link_libraries(offs PRIVATE cjson)
```

Also add the include directory for offs (after line 59):

```cmake
target_include_directories(offs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/cJSON)
```

- [ ] **Step 3: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds with no cJSON-related errors.

- [ ] **Step 4: Commit**

```bash
git add deps/cJSON/cJSON.c deps/cJSON/cJSON.h CMakeLists.txt
git commit -m "feat: vendor cJSON 1.7.18 for JSON parsing and generation"
```

---

### Task 2: Add tcp_tls_* config fields

**Files:**
- Modify: `src/Configuration/config.h`
- Modify: `src/Configuration/config.c`

- [ ] **Step 1: Add fields to config_t struct**

In `src/Configuration/config.h`, add after the `wt_port` field (after line 53):

```c
  /* TCP TLS */
  bool     tcp_tls_enabled;
  char*    tcp_tls_cert_path;
  char*    tcp_tls_key_path;
```

- [ ] **Step 2: Add defaults in config_default()**

In `src/Configuration/config.c`, add after the `config.wt_port = 9002;` line (after line 52):

```c
  config.tcp_tls_enabled = false;
  config.tcp_tls_cert_path = NULL;
  config.tcp_tls_key_path = NULL;
```

- [ ] **Step 3: Add validation in config_validate()**

In `src/Configuration/config.c`, add after the `https_enabled` validation block (after line 193). Insert before the `api_key_hash` validation:

```c
  /* TCP TLS requires cert and key paths */
  if (config->tcp_tls_enabled) {
    if (config->tcp_tls_cert_path == NULL || config->tcp_tls_key_path == NULL) {
      log_error("tcp_tls_enabled requires tcp_tls_cert_path and tcp_tls_key_path");
      valid = false;
    }
  }
```

Also update the plaintext remote transport check for api_key_hash. Change the existing `tcp_enabled` check to allow tcp with TLS:

```c
    /* API keys over plaintext remote transports are forbidden */
    if (config->tcp_enabled && !config->tcp_tls_enabled) {
      log_error("tcp_enabled without TLS cannot be used with api_key_hash (plaintext remote transport)");
      valid = false;
    }
```

- [ ] **Step 4: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config.c
git commit -m "feat: add tcp_tls_enabled/cert_path/key_path config fields"
```

---

### Task 3: Add is_local_binding to http_server_t

**Files:**
- Modify: `src/ClientAPI/HTTP/http_server.h`
- Modify: `src/ClientAPI/HTTP/http_server.c`

- [ ] **Step 1: Add field and accessor to header**

In `src/ClientAPI/HTTP/http_server.h`, add to the `http_server_t` struct after the `draining` field (after line 52):

```c
  uint8_t is_local_binding;   /* 1 if bound to loopback/link-local */
```

Add the accessor declaration before `#endif`:

```c
uint8_t http_server_is_local_binding(const http_server_t* server);
```

- [ ] **Step 2: Set is_local_binding in http_server_create**

In `src/ClientAPI/HTTP/http_server.c`, after the bind succeeds and the address is known, add logic to detect local binding. After line 126 (`return server;` is at line 128), but before the return. Find the `return server;` at the end of `http_server_create` and add before it:

```c
  /* Detect if bound to loopback or link-local */
  if (host != NULL) {
    server->is_local_binding = (strcmp(host, "127.0.0.1") == 0 ||
                                strcmp(host, "localhost") == 0 ||
                                strcmp(host, "::1") == 0);
  } else {
    server->is_local_binding = 0;
  }
```

Need to add `#include <string.h>` at the top of http_server.c.

- [ ] **Step 3: Implement accessor in http_server.c**

Add at the end of http_server.c:

```c
uint8_t http_server_is_local_binding(const http_server_t* server) {
  return server != NULL ? server->is_local_binding : 0;
}
```

- [ ] **Step 4: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/HTTP/http_server.h src/ClientAPI/HTTP/http_server.c
git commit -m "feat: add is_local_binding field to http_server_t"
```

---

### Task 4: Create config_pending.c/h

**Files:**
- Create: `src/Configuration/config_pending.h`
- Create: `src/Configuration/config_pending.c`

- [ ] **Step 1: Write config_pending.h**

```c
#ifndef OFFS_CONFIG_PENDING_H
#define OFFS_CONFIG_PENDING_H

#include "config.h"
#include <stddef.h>

/* Save a partial config (only the fields present in updates) as JSON.
   Merged with any existing pending config on disk.
   Returns 0 on success, -1 on error. */
int config_pending_save(const char* data_dir, const char* json_body, size_t body_len);

/* Load pending config from disk and merge into defaults.
   Returns a new config_t (caller must free string fields and the struct itself).
   Returns NULL if no pending config exists. */
config_t* config_pending_load(const char* data_dir);

/* Mark pending config as applied (rename .json to .applied).
   Returns 0 on success, -1 if no pending config exists. */
int config_pending_mark_applied(const char* data_dir);

/* Check if a pending config file exists.
   Returns 1 if exists, 0 if not, -1 on error. */
int config_pending_exists(const char* data_dir);

#endif
```

- [ ] **Step 2: Write config_pending.c — save**

The save function parses the incoming JSON body, merges with existing pending config (if any), cross-validates, and writes.

```c
#include "config_pending.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <cJSON.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static char* _pending_path(const char* data_dir) {
  size_t len = strlen(data_dir) + 32;
  char* path = get_clear_memory(len);
  snprintf(path, len, "%s/pending_config.json", data_dir);
  return path;
}

int config_pending_save(const char* data_dir, const char* json_body, size_t body_len) {
  /* Parse incoming JSON */
  cJSON* incoming = cJSON_ParseWithLength(json_body, body_len);
  if (incoming == NULL) {
    log_error("config_pending_save: failed to parse JSON body");
    return -1;
  }

  /* Load existing pending config if any */
  cJSON* existing = NULL;
  char* pending_path = _pending_path(data_dir);
  FILE* f = fopen(pending_path, "r");
  if (f != NULL) {
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize > 0) {
      char* existing_json = get_clear_memory((size_t)(fsize + 1));
      fread(existing_json, 1, (size_t)fsize, f);
      existing = cJSON_Parse(existing_json);
      free(existing_json);
    }
    fclose(f);
  }

  /* Merge: copy existing, then overlay incoming */
  cJSON* merged = existing ? cJSON_Duplicate(existing, 1) : cJSON_CreateObject();
  if (existing) cJSON_Delete(existing);

  /* For each field in incoming, set or delete in merged */
  cJSON* item = incoming->child;
  while (item != NULL) {
    if (cJSON_IsNull(item)) {
      /* Null means revert to default — remove from pending */
      cJSON_DeleteItemFromObject(merged, item->string);
    } else {
      /* Remove old key if present, then add new value */
      cJSON* old = cJSON_DetachItemFromObject(merged, item->string);
      if (old) cJSON_Delete(old);
      cJSON_AddItemToObject(merged, item->string, cJSON_Duplicate(item, 1));
    }
    item = item->next;
  }
  cJSON_Delete(incoming);

  /* Serialize and write */
  char* merged_str = cJSON_Print(merged);
  cJSON_Delete(merged);
  if (merged_str == NULL) {
    free(pending_path);
    return -1;
  }

  f = fopen(pending_path, "w");
  if (f == NULL) {
    log_error("config_pending_save: failed to open %s for writing", pending_path);
    free(merged_str);
    free(pending_path);
    return -1;
  }
  fputs(merged_str, f);
  fclose(f);
  free(merged_str);
  free(pending_path);
  return 0;
}
```

- [ ] **Step 3: Write config_pending.c — load**

```c
config_t* config_pending_load(const char* data_dir) {
  char* pending_path = _pending_path(data_dir);
  FILE* f = fopen(pending_path, "r");
  free(pending_path);
  if (f == NULL) return NULL;

  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (fsize <= 0) { fclose(f); return NULL; }

  char* json_str = get_clear_memory((size_t)(fsize + 1));
  fread(json_str, 1, (size_t)fsize, f);
  fclose(f);

  cJSON* root = cJSON_Parse(json_str);
  free(json_str);
  if (root == NULL) {
    log_error("config_pending_load: failed to parse pending config");
    return NULL;
  }

  config_t* config = get_clear_memory(sizeof(config_t));
  *config = config_default();

  /* Apply overrides from JSON — each key maps to config_t field */
  cJSON* item = root->child;
  while (item != NULL) {
    /* size_t fields */
    if (strcmp(item->string, "cache_size") == 0 && cJSON_IsNumber(item))
      config->cache_size = (size_t)item->valuedouble;
    else if (strcmp(item->string, "max_snapshots") == 0 && cJSON_IsNumber(item))
      config->max_snapshots = (size_t)item->valuedouble;
    else if (strcmp(item->string, "max_wals") == 0 && cJSON_IsNumber(item))
      config->max_wals = (size_t)item->valuedouble;
    else if (strcmp(item->string, "scheduler_thread_count") == 0 && cJSON_IsNumber(item))
      config->scheduler_thread_count = (size_t)item->valuedouble;
    else if (strcmp(item->string, "max_capacity_bytes") == 0 && cJSON_IsNumber(item))
      config->max_capacity_bytes = (size_t)item->valuedouble;
    /* bool fields */
    else if (strcmp(item->string, "http_enabled") == 0 && cJSON_IsBool(item))
      config->http_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "https_enabled") == 0 && cJSON_IsBool(item))
      config->https_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "unix_enabled") == 0 && cJSON_IsBool(item))
      config->unix_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "tcp_enabled") == 0 && cJSON_IsBool(item))
      config->tcp_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "ws_enabled") == 0 && cJSON_IsBool(item))
      config->ws_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "wt_enabled") == 0 && cJSON_IsBool(item))
      config->wt_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "tcp_tls_enabled") == 0 && cJSON_IsBool(item))
      config->tcp_tls_enabled = cJSON_IsTrue(item);
    /* uint16_t fields */
    else if (strcmp(item->string, "http_port") == 0 && cJSON_IsNumber(item))
      config->http_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "https_port") == 0 && cJSON_IsNumber(item))
      config->https_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "tcp_port") == 0 && cJSON_IsNumber(item))
      config->tcp_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "ws_port") == 0 && cJSON_IsNumber(item))
      config->ws_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "wt_port") == 0 && cJSON_IsNumber(item))
      config->wt_port = (uint16_t)item->valuedouble;
    /* string fields */
    else if (strcmp(item->string, "api_key_hash") == 0 && cJSON_IsString(item))
      config->api_key_hash = strdup(item->valuestring);
    else if (strcmp(item->string, "https_cert_path") == 0 && cJSON_IsString(item))
      config->https_cert_path = strdup(item->valuestring);
    else if (strcmp(item->string, "https_key_path") == 0 && cJSON_IsString(item))
      config->https_key_path = strdup(item->valuestring);
    else if (strcmp(item->string, "tcp_tls_cert_path") == 0 && cJSON_IsString(item))
      config->tcp_tls_cert_path = strdup(item->valuestring);
    else if (strcmp(item->string, "tcp_tls_key_path") == 0 && cJSON_IsString(item))
      config->tcp_tls_key_path = strdup(item->valuestring);

    item = item->next;
  }

  cJSON_Delete(root);

  if (config_validate(config) != 0) {
    log_error("config_pending_load: pending config failed validation");
    /* Free allocated strings */
    free(config->api_key_hash);
    free(config->https_cert_path);
    free(config->https_key_path);
    free(config->tcp_tls_cert_path);
    free(config->tcp_tls_key_path);
    free(config);
    return NULL;
  }

  return config;
}
```

- [ ] **Step 4: Write config_pending.c — mark_applied and exists**

```c
int config_pending_mark_applied(const char* data_dir) {
  char* pending_path = _pending_path(data_dir);
  size_t len = strlen(data_dir) + 48;
  char* applied_path = get_clear_memory(len);
  snprintf(applied_path, len, "%s/pending_config.applied", data_dir);

  int rc = rename(pending_path, applied_path);
  free(pending_path);
  free(applied_path);
  return rc;
}

int config_pending_exists(const char* data_dir) {
  char* pending_path = _pending_path(data_dir);
  FILE* f = fopen(pending_path, "r");
  free(pending_path);
  if (f == NULL) return 0;
  fclose(f);
  return 1;
}
```

- [ ] **Step 5: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/Configuration/config_pending.h src/Configuration/config_pending.c
git commit -m "feat: add config_pending save/load/merge for pending config file"
```

---

### Task 5: Retrofit health_handler to use cJSON

**Files:**
- Modify: `src/ClientAPI/health_handler.h`
- Modify: `src/ClientAPI/health_handler.c`
- Modify: `src/ClientAPI/HTTP/health_routes.c`
- Modify: `src/ClientAPI/WS/ws_connection.c`
- Modify: `src/ClientAPI/TCP/tcp_connection.c`
- Modify: `src/ClientAPI/WT/wt_connection.c`
- Modify: `src/ClientAPI/Unix/unix_connection.c`

- [ ] **Step 1: Update health_handler.h**

Change `health_data_to_json` signature from writing to a buffer to returning a `cJSON*` tree:

Add `#include <cJSON.h>` at the top.

Change line 41:
```c
health_data_t health_data_collect(const health_context_t* ctx);
cJSON* health_data_to_json(const health_data_t* data);
```

Add `#include <cJSON.h>` include directive.

- [ ] **Step 2: Rewrite health_data_to_json in health_handler.c**

Add `#include <cJSON.h>` at the top. Replace the entire `health_data_to_json` function body (lines 95-165) with:

```c
cJSON* health_data_to_json(const health_data_t* data) {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddStringToObject(root, "status", data->status);
  cJSON_AddNumberToObject(root, "uptime_seconds", (double)data->uptime_seconds);

  if (data->node_id_str[0] != '\0') {
    cJSON_AddStringToObject(root, "node_id", data->node_id_str);
  }

  cJSON_AddNumberToObject(root, "peer_count", (double)data->peer_count);
  cJSON_AddNumberToObject(root, "total_connections", (double)data->total_connections);
  cJSON_AddNumberToObject(root, "avg_hebbian_weight", (double)data->avg_hebbian_weight);

  /* Block cache stats */
  cJSON* block_cache = cJSON_CreateObject();
  cJSON_AddNumberToObject(block_cache, "current_bytes", (double)data->block_cache_current_bytes);
  cJSON_AddNumberToObject(block_cache, "max_bytes", (double)data->block_cache_max_bytes);
  cJSON_AddNumberToObject(block_cache, "block_count", (double)data->block_cache_block_count);
  cJSON_AddItemToObject(root, "block_cache", block_cache);

  /* Rate limit stats */
  cJSON* rate_limits = cJSON_CreateArray();
  for (size_t i = 0; i < RPC_TYPE_COUNT; i++) {
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "type", rate_limit_names[i]);
    cJSON_AddNumberToObject(entry, "accepted", (double)data->rate_limit_accepted[i]);
    cJSON_AddNumberToObject(entry, "rejected", (double)data->rate_limit_rejected[i]);
    cJSON_AddNumberToObject(entry, "avg_tokens", (double)data->avg_rate_limit_tokens[i]);
    cJSON_AddNumberToObject(entry, "effective_rate", (double)data->effective_rate[i]);
    cJSON_AddItemToArray(rate_limits, entry);
  }
  cJSON_AddItemToObject(root, "rate_limits", rate_limits);

  /* RPC call stats (non-zero entries only) */
  cJSON* rpc_calls = cJSON_CreateArray();
  for (size_t i = 0; i < PEER_RPC_TYPE_COUNT; i++) {
    if (data->total_rpc_calls[i] > 0) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "name", rpc_names[i]);
      cJSON_AddNumberToObject(entry, "count", (double)data->total_rpc_calls[i]);
      cJSON_AddItemToArray(rpc_calls, entry);
    }
  }
  cJSON_AddItemToObject(root, "rpc_calls", rpc_calls);

  return root;
}
```

Remove the APPEND macro and all snprintf calls. Keep the `rate_limit_names` and `rpc_names` static arrays (lines 11-41).

- [ ] **Step 3: Update health_routes.c**

In `src/ClientAPI/HTTP/health_routes.c`, update `_health_middleware`:

Remove the stack buffer (`char json_buf[8192]`). Change to use cJSON:

```c
  health_data_t data = health_data_collect(&health_ctx);
  cJSON* json = health_data_to_json(&data);
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  if (json_str != NULL) {
    http_response_write(response, json_str, strlen(json_str));
    free(json_str);
  }
  http_response_end(response);
```

Add `#include <cJSON.h>` at the top.

- [ ] **Step 4: Update four transport connection files**

In each of `ws_connection.c`, `tcp_connection.c`, `wt_connection.c`, `unix_connection.c`, find the health request handler that calls `health_data_to_json`. The pattern is:

```c
char json_buf[8192];
health_data_t data = health_data_collect(&ctx);
size_t json_len = health_data_to_json(&data, json_buf, sizeof(json_buf));
```

Replace with:

```c
health_data_t data = health_data_collect(&ctx);
cJSON* json = health_data_to_json(&data);
char* json_str = cJSON_Print(json);
cJSON_Delete(json);
```

Then pass `json_str` (and `strlen(json_str)`) instead of `json_buf`/`json_len` to the `client_api_health_response_encode` call. Add `free(json_str)` after the encode call.

Add `#include <cJSON.h>` at the top of each file.

- [ ] **Step 5: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds with no JSON-related errors.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/health_handler.h src/ClientAPI/health_handler.c \
        src/ClientAPI/HTTP/health_routes.c src/ClientAPI/WS/ws_connection.c \
        src/ClientAPI/TCP/tcp_connection.c src/ClientAPI/WT/wt_connection.c \
        src/ClientAPI/Unix/unix_connection.c
git commit -m "refactor: replace manual JSON in health_handler with cJSON"
```

---

### Task 6: Retrofit peer_routes to use cJSON

**Files:**
- Modify: `src/ClientAPI/HTTP/peer_routes.c`

- [ ] **Step 1: Replace _peer_connect_handler JSON construction**

Replace lines 314-327 (the `asprintf` error JSON and the response JSON at lines 352-366):

Add `#include <cJSON.h>` at the top.

In the error path (invalid peer info, ~line 314):
```c
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "status", CONNECT_STATUS_INVALID_INFO);
    cJSON_AddStringToObject(json, "message", "Invalid peer info");
    char* json_str = cJSON_Print(json);
    cJSON_Delete(json);
    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, json_str, strlen(json_str));
    http_response_end(response);
    free(json_str);
    return;
```

In the success path (line 352), replace the `asprintf` block with:
```c
  cJSON* json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "status", status);
  cJSON_AddStringToObject(json, "message", message);
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
```

- [ ] **Step 2: Replace _peer_list_handler JSON construction**

Replace lines 384-413 (the manual snprintf with offset tracking):

```c
  cJSON* arr = cJSON_CreateArray();
  for (size_t index = 0; index < mgr->peer_count; index++) {
    peer_connection_t* peer = mgr->peers[index];
    if (!peer->connected) continue;
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "node_id", peer->remote_node_id.str);
    cJSON_AddBoolToObject(entry, "connected", true);
    cJSON_AddBoolToObject(entry, "is_friend", peer->is_friend);
    cJSON_AddItemToArray(arr, entry);
  }

  char* json_str = cJSON_Print(arr);
  cJSON_Delete(arr);
  if (json_str == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
```

Remove the `json_error` goto label. Remove the `_bool_str` helper. Remove the `JSON_BUF_SIZE` define.

- [ ] **Step 3: Replace _friend_add_handler JSON**

Replace the static string `"{\"status\":\"already_friend\"}"` block (line 467):
```c
      cJSON* json = cJSON_CreateObject();
      cJSON_AddStringToObject(json, "status", "already_friend");
      char* json_str = cJSON_Print(json);
      cJSON_Delete(json);
      http_response_set_status(response, HTTP_STATUS_CONFLICT);
      http_response_set_header(response, "Content-Type", "application/json");
      http_response_write(response, json_str, strlen(json_str));
      http_response_end(response);
      free(json_str);
```

Replace the `"{\"status\":\"added\"}"` block (line 488):
```c
  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "added");
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
```

- [ ] **Step 4: Replace _friend_remove_handler JSON**

Replace the `"{\"status\":\"removed\"}"` block (line 566):
```c
  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "removed");
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
```

- [ ] **Step 5: Replace _friend_list_handler JSON construction**

Replace the manual snprintf block (lines 585-619):
```c
  cJSON* arr = cJSON_CreateArray();
  for (size_t index = 0; index < authority->friend_peer_count; index++) {
    peer_info_t* friend_peer = authority->friend_peers[index];
    peer_connection_t* conn = connection_manager_lookup(mgr, &friend_peer->node_id);
    bool connected = (conn != NULL && conn->connected);
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "node_id", friend_peer->node_id.str);
    cJSON_AddBoolToObject(entry, "connected", connected);
    cJSON_AddItemToArray(arr, entry);
  }

  char* json_str = cJSON_Print(arr);
  cJSON_Delete(arr);
  if (json_str == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
```

Remove the `friend_json_error` goto label.

- [ ] **Step 6: Clean up unused helpers**

Remove the `_bool_str` helper function. Remove `#define JSON_BUF_SIZE 65536`. Remove `#include <stdio.h>` (no longer need snprintf/asprintf for JSON).

- [ ] **Step 7: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds.

- [ ] **Step 8: Commit**

```bash
git add src/ClientAPI/HTTP/peer_routes.c
git commit -m "refactor: replace manual JSON in peer_routes with cJSON"
```

---

### Task 7: Create config_routes.c/h

**Files:**
- Create: `src/ClientAPI/HTTP/config_routes.h`
- Create: `src/ClientAPI/HTTP/config_routes.c`

- [ ] **Step 1: Write config_routes.h**

```c
#ifndef OFFS_CONFIG_ROUTES_H
#define OFFS_CONFIG_ROUTES_H

#include "http_server.h"
#include "../../Scheduler/scheduler.h"
#include "../../Configuration/config.h"
#include "../../Node/node.h"

/* Register config routes on the HTTP server.
   node pointer is used for current config state and restart.
   data_dir is where pending_config.json is stored (e.g. "."). */
void config_routes_register(http_server_t* server, offs_node_t* node,
                            const config_t* config, const char* data_dir);

#endif
```

- [ ] **Step 2: Write config_routes.c — context and helpers**

```c
#include "config_routes.h"
#include "http_response.h"
#include "http_request.h"
#include "http_headers.h"
#include "../../Configuration/config_pending.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
  offs_node_t* node;
  char* data_dir;
} config_routes_ctx_t;

/* Auth: check Bearer token OR local binding */
static int _config_check_auth(http_request_t* request, http_response_t* response,
                               http_server_t* server) {
  /* Local binding — skip auth */
  if (http_server_is_local_binding(server)) return 0;

  /* Remote — require Bearer token */
  if (!request->is_authenticated) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_end(response);
    return -1;
  }
  return 0;
}

/* Known config field names for validation */
static bool _is_known_field(const char* name) {
  static const char* known[] = {
    "api_key_hash", "rate_limits", "max_capacity_bytes",
    "cache_size", "max_snapshots", "max_wals", "scheduler_thread_count",
    "http_enabled", "http_port", "https_enabled", "https_port",
    "https_cert_path", "https_key_path", "unix_enabled",
    "tcp_enabled", "tcp_port", "ws_enabled", "ws_port",
    "wt_enabled", "wt_port",
    "tcp_tls_enabled", "tcp_tls_cert_path", "tcp_tls_key_path",
    NULL
  };
  for (size_t i = 0; known[i] != NULL; i++) {
    if (strcmp(name, known[i]) == 0) return true;
  }
  return false;
}
```

- [ ] **Step 3: Write GET /config handler**

```c
/* Serialize current config_t to cJSON */
static cJSON* _config_to_json(const config_t* config) {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddNumberToObject(root, "cache_size", (double)config->cache_size);
  cJSON_AddNumberToObject(root, "max_snapshots", (double)config->max_snapshots);
  cJSON_AddNumberToObject(root, "max_wals", (double)config->max_wals);
  cJSON_AddNumberToObject(root, "max_capacity_bytes", (double)config->max_capacity_bytes);
  cJSON_AddNumberToObject(root, "scheduler_thread_count", (double)config->scheduler_thread_count);

  cJSON_AddBoolToObject(root, "http_enabled", config->http_enabled);
  cJSON_AddNumberToObject(root, "http_port", (double)config->http_port);
  cJSON_AddBoolToObject(root, "https_enabled", config->https_enabled);
  cJSON_AddNumberToObject(root, "https_port", (double)config->https_port);
  if (config->https_cert_path)
    cJSON_AddStringToObject(root, "https_cert_path", config->https_cert_path);
  else
    cJSON_AddNullToObject(root, "https_cert_path");
  if (config->https_key_path)
    cJSON_AddStringToObject(root, "https_key_path", config->https_key_path);
  else
    cJSON_AddNullToObject(root, "https_key_path");

  cJSON_AddBoolToObject(root, "unix_enabled", config->unix_enabled);
  cJSON_AddBoolToObject(root, "tcp_enabled", config->tcp_enabled);
  cJSON_AddNumberToObject(root, "tcp_port", (double)config->tcp_port);
  cJSON_AddBoolToObject(root, "ws_enabled", config->ws_enabled);
  cJSON_AddNumberToObject(root, "ws_port", (double)config->ws_port);
  cJSON_AddBoolToObject(root, "wt_enabled", config->wt_enabled);
  cJSON_AddNumberToObject(root, "wt_port", (double)config->wt_port);

  cJSON_AddBoolToObject(root, "tcp_tls_enabled", config->tcp_tls_enabled);
  if (config->tcp_tls_cert_path)
    cJSON_AddStringToObject(root, "tcp_tls_cert_path", config->tcp_tls_cert_path);
  else
    cJSON_AddNullToObject(root, "tcp_tls_cert_path");
  if (config->tcp_tls_key_path)
    cJSON_AddStringToObject(root, "tcp_tls_key_path", config->tcp_tls_key_path);
  else
    cJSON_AddNullToObject(root, "tcp_tls_key_path");

  if (config->api_key_hash)
    cJSON_AddStringToObject(root, "api_key_hash", config->api_key_hash);
  else
    cJSON_AddNullToObject(root, "api_key_hash");

  return root;
}

static void _config_get_handler(http_request_t* request, http_response_t* response,
                                 void* user_data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)user_data;

  if (_config_check_auth(request, response, ctx->node->http_server) != 0) return;

  cJSON* json = _config_to_json(ctx->node->config);
  char* json_str = cJSON_Print(json);
  cJSON_Delete(json);

  if (json_str == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
}
```

- [ ] **Step 4: Write PUT /config handler**

```c
static void _config_put_handler(http_request_t* request, http_response_t* response,
                                 void* user_data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)user_data;

  if (_config_check_auth(request, response, ctx->node->http_server) != 0) return;

  /* Need a body */
  if (request->body == NULL || request->body->size == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_end(response);
    return;
  }

  /* Parse the incoming JSON */
  cJSON* incoming = cJSON_ParseWithLength((const char*)request->body->data,
                                           request->body->size);
  if (incoming == NULL) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "error", "invalid JSON body");
    char* err_str = cJSON_Print(err);
    cJSON_Delete(err);
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_set_header(response, "Content-Type", "application/json");
    http_response_write(response, err_str, strlen(err_str));
    http_response_end(response);
    free(err_str);
    return;
  }

  /* Validate field names and types */
  cJSON* rejected = cJSON_CreateArray();
  cJSON* staged = cJSON_CreateArray();
  bool has_valid = false;
  bool has_rejected = false;

  cJSON* item = incoming->child;
  while (item != NULL) {
    if (!_is_known_field(item->string)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "unknown field");
      cJSON_AddItemToArray(rejected, entry);
      has_rejected = true;
      item = item->next;
      continue;
    }

    /* Type check: strings for paths and api_key_hash */
    bool is_string_field = (strcmp(item->string, "api_key_hash") == 0 ||
                            strcmp(item->string, "https_cert_path") == 0 ||
                            strcmp(item->string, "https_key_path") == 0 ||
                            strcmp(item->string, "tcp_tls_cert_path") == 0 ||
                            strcmp(item->string, "tcp_tls_key_path") == 0);
    /* Accept null for string fields (means revert to default) */
    if (is_string_field && !cJSON_IsString(item) && !cJSON_IsNull(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected string or null");
      cJSON_AddItemToArray(rejected, entry);
      has_rejected = true;
      item = item->next;
      continue;
    }

    bool is_bool_field = (strcmp(item->string, "http_enabled") == 0 ||
                          strcmp(item->string, "https_enabled") == 0 ||
                          strcmp(item->string, "unix_enabled") == 0 ||
                          strcmp(item->string, "tcp_enabled") == 0 ||
                          strcmp(item->string, "ws_enabled") == 0 ||
                          strcmp(item->string, "wt_enabled") == 0 ||
                          strcmp(item->string, "tcp_tls_enabled") == 0);
    if (is_bool_field && !cJSON_IsBool(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected boolean");
      cJSON_AddItemToArray(rejected, entry);
      has_rejected = true;
      item = item->next;
      continue;
    }

    bool is_number_field = (!is_string_field && !is_bool_field &&
                             strcmp(item->string, "rate_limits") != 0);
    if (is_number_field && !cJSON_IsNumber(item)) {
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", item->string);
      cJSON_AddStringToObject(entry, "reason", "expected number");
      cJSON_AddItemToArray(rejected, entry);
      has_rejected = true;
      item = item->next;
      continue;
    }

    cJSON_AddItemToArray(staged,
      cJSON_CreateString(item->string));
    has_valid = true;
    item = item->next;
  }

  /* Save to pending config if any valid fields */
  bool restart_required = false;
  if (has_valid) {
    if (config_pending_save(ctx->data_dir,
                            (const char*)request->body->data,
                            request->body->size) == 0) {
      restart_required = true;
    } else {
      /* Save failed — move all staged to rejected */
      cJSON* entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "field", "*");
      cJSON_AddStringToObject(entry, "reason", "failed to write pending config");
      cJSON_AddItemToArray(rejected, entry);
      has_rejected = true;
      /* Clear staged */
      cJSON_Delete(staged);
      staged = cJSON_CreateArray();
      restart_required = false;
    }
  }

  cJSON_Delete(incoming);

  /* Build response */
  cJSON* result = cJSON_CreateObject();
  cJSON_AddItemToObject(result, "staged", staged);
  cJSON_AddItemToObject(result, "rejected", rejected);
  cJSON_AddBoolToObject(result, "restart_required", restart_required);

  char* json_str = cJSON_Print(result);
  cJSON_Delete(result);

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  if (json_str != NULL) {
    http_response_write(response, json_str, strlen(json_str));
    free(json_str);
  }
  http_response_end(response);
}
```

- [ ] **Step 5: Write POST /config/restart handler**

```c
static void _config_restart_handler(http_request_t* request, http_response_t* response,
                                     void* user_data) {
  config_routes_ctx_t* ctx = (config_routes_ctx_t*)user_data;

  /* Restart is only allowed on local transports */
  if (!http_server_is_local_binding(ctx->node->http_server)) {
    http_response_set_status(response, HTTP_STATUS_FORBIDDEN);
    http_response_set_header(response, "Content-Type", "application/json");
    const char* msg = "{\"error\":\"restart requires local transport\"}";
    http_response_write(response, msg, strlen(msg));
    http_response_end(response);
    return;
  }

  /* Verify pending config exists */
  if (config_pending_exists(ctx->data_dir) != 1) {
    http_response_set_status(response, 409);
    http_response_set_header(response, "Content-Type", "application/json");
    const char* msg = "{\"error\":\"no pending config to apply\"}";
    http_response_write(response, msg, strlen(msg));
    http_response_end(response);
    return;
  }

  /* Send 202 before restart begins */
  http_response_set_status(response, 202);
  http_response_set_header(response, "Content-Type", "application/json");
  const char* msg = "{\"message\":\"restarting\"}";
  http_response_write(response, msg, strlen(msg));
  http_response_end(response);

  /* Trigger restart */
  offs_node_restart(ctx->node, ctx->data_dir);
}
```

- [ ] **Step 6: Write config_routes_register**

```c
void config_routes_register(http_server_t* server, offs_node_t* node,
                            const config_t* config, const char* data_dir) {
  (void)config; /* unused — config is accessed via node->config */

  config_routes_ctx_t* ctx = get_clear_memory(sizeof(config_routes_ctx_t));
  ctx->node = node;
  ctx->data_dir = strdup(data_dir);

  http_server_get_with_data(server, "/config", _config_get_handler, ctx, free);
  http_server_put_with_data(server, "/config", _config_put_handler, ctx, NULL);
  http_server_post_with_data(server, "/config/restart", _config_restart_handler, ctx, NULL);
}
```

- [ ] **Step 7: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds.

- [ ] **Step 8: Commit**

```bash
git add src/ClientAPI/HTTP/config_routes.h src/ClientAPI/HTTP/config_routes.c
git commit -m "feat: add config routes (GET/PUT /config, POST /config/restart)"
```

---

### Task 8: Implement offs_node_restart()

**Files:**
- Modify: `src/Node/node.h`
- Modify: `src/Node/node.c`

- [ ] **Step 1: Add declaration to node.h**

Add after `void offs_node_destroy(offs_node_t* node);` (after line 40):

```c
/* Trigger a graceful restart using pending config.
   Must only be called from a local transport handler.
   Sends 202 before beginning restart on a background thread. */
void offs_node_restart(offs_node_t* node, const char* data_dir);
```

- [ ] **Step 2: Add includes in node.c**

Add at the top of node.c:
```c
#include "../Configuration/config_pending.h"
```

- [ ] **Step 3: Implement offs_node_restart in node.c**

Add at the end of node.c (before EOF):

```c
void offs_node_restart(offs_node_t* node, const char* data_dir) {
  if (node == NULL || data_dir == NULL) return;

  log_info("offs_node_restart: beginning graceful restart");

  /* Phase 1: Stop everything */
  offs_node_stop(node);

  /* Phase 2: Tear down subsystems */
  if (node->network != NULL) {
    network_destroy(node->network);
    node->network = NULL;
  }
  if (node->block_cache != NULL) {
    block_cache_destroy(node->block_cache);
    node->block_cache = NULL;
  }
  if (node->http_server != NULL) {
    http_server_destroy(node->http_server);
    node->http_server = NULL;
  }

  /* Phase 3: Load pending config */
  config_t* new_config = config_pending_load(data_dir);
  if (new_config == NULL) {
    log_error("offs_node_restart: failed to load pending config, restarting with current config");
    /* Deep-copy the current config */
    new_config = get_clear_memory(sizeof(config_t));
    *new_config = *node->config;
    /* Copy string fields */
    if (node->config->api_key_hash)
      new_config->api_key_hash = strdup(node->config->api_key_hash);
    if (node->config->https_cert_path)
      new_config->https_cert_path = strdup(node->config->https_cert_path);
    if (node->config->https_key_path)
      new_config->https_key_path = strdup(node->config->https_key_path);
    if (node->config->tcp_tls_cert_path)
      new_config->tcp_tls_cert_path = strdup(node->config->tcp_tls_cert_path);
    if (node->config->tcp_tls_key_path)
      new_config->tcp_tls_key_path = strdup(node->config->tcp_tls_key_path);
  }

  node->config = new_config;

  /* Phase 4: Re-create scheduler and timer (must happen before subsystems) */
  node->scheduler = scheduler_pool_create(new_config->scheduler_thread_count);
  if (node->scheduler == NULL) {
    log_error("offs_node_restart: failed to create scheduler pool");
    return;
  }

  node->timer = timer_actor_create();
  if (node->timer == NULL) {
    log_error("offs_node_restart: failed to create timer actor");
    scheduler_pool_destroy(node->scheduler);
    return;
  }

  /* Phase 5: Start with new config */
  if (offs_node_start(node) != 0) {
    log_error("offs_node_restart: offs_node_start failed");
    return;
  }

  /* Phase 6: Mark pending config as applied */
  config_pending_mark_applied(data_dir);

  log_info("offs_node_restart: restart complete");
}
```

Note: `strdup` may need `<string.h>` which is already included.

- [ ] **Step 4: Build to verify**

Run:
```bash
cd build && cmake .. && make offs -j$(nproc)
```
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/Node/node.h src/Node/node.c
git commit -m "feat: implement offs_node_restart for graceful config reload"
```

---

### Task 9: Wire config_routes into main.c

**Files:**
- Modify: `examples/off_server/main.c`

- [ ] **Step 1: Add include and registration call**

Add `#include "../../src/ClientAPI/HTTP/config_routes.h"` with the other route includes.

After the existing route registrations (`peer_routes_register` line), add:

```c
  /* Register config management routes */
  config_routes_register(server, &node_obj, &config, ".");
```

This registers /config routes on the HTTP server. The `"."` data_dir stores `pending_config.json` in the current working directory (same dir that contains `sections/`).

- [ ] **Step 2: Build the example to verify**

Run:
```bash
cd build && cmake .. && make off_server -j$(nproc)
```
Expected: Build succeeds with no link errors.

- [ ] **Step 3: Commit**

```bash
git add examples/off_server/main.c
git commit -m "feat: wire config_routes into example server"
```

---

### Task 10: Run tests and fix issues

- [ ] **Step 1: Build all targets**

Run:
```bash
cd build && cmake .. && make -j$(nproc)
```
Expected: All targets build.

- [ ] **Step 2: Run existing tests**

Run:
```bash
cd build && ctest --output-on-failure
```
Expected: All previously-passing tests still pass.

- [ ] **Step 3: Fix any test failures**

If tests fail, fix and re-run.

- [ ] **Step 4: Commit any fixes**

```bash
git add -A && git commit -m "fix: address test failures from config API changes"
```

---

### Task 11: De-wonk audit

- [ ] **Step 1: Run de-wonk skill**

Use the `de-wonk` skill to check for unimplemented, stubbed, disabled, broken, or weird code before declaring work finished. Fix any issues found.

- [ ] **Step 2: Final commit**

```bash
git add -A && git commit -m "chore: de-wonk fixes for config API"
```
