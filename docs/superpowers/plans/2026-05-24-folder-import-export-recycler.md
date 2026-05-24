# Folder Import/Export & Recycler Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add recycler/temporary support to the C wire protocol and server PUT handler, then build folder import/export in the Dart example app.

**Architecture:** Extend `client_api_put_request_t` with recycler_urls/temporary fields in CBOR array positions 7-8. Add `offs_put_options_t` struct to the C client for clean API. Update server PUT handler to parse `recycler`/`temporary` headers and prepend `recycler_recipe_t` before `new_blocks_recipe_t`. Add folder import/export to the Dart `OffApi` service.

**Tech Stack:** C11 (liboffs), C++17 (GoogleTest), Dart/Flutter (example app), CBOR (wire protocol)

---

## File Structure

| File | Action | Purpose |
|------|--------|---------|
| `src/ClientAPI/client_api_wire.h` | Modify | Add recycler/temporary fields to put_request_t |
| `src/ClientAPI/client_api_wire.c` | Modify | Encode/decode/destroy for new fields |
| `src/ClientLibs/c/offs_client.h` | Modify | Add `offs_put_options_t` and `_ex` functions |
| `src/ClientLibs/c/offs_client.c` | Modify | Implement `_ex` functions |
| `src/ClientAPI/HTTP/off_routes.c` | Modify | Parse recycler/temporary headers, create recycler_recipe |
| `test/test_recycler_wire.cpp` | Create | Wire protocol round-trip tests for new fields |
| `examples/off_client/lib/services/off_api.dart` | Modify | Add recycler/temporary params |
| `examples/off_client/lib/screens/import_screen.dart` | Modify | Add folder import UI |
| `examples/off_client/lib/screens/export_screen.dart` | Modify | Add folder export UI |

---

### Task 1: Extend PUT_REQUEST wire protocol with recycler/temporary

**Files:**
- Modify: `src/ClientAPI/client_api_wire.h:35-43`
- Modify: `src/ClientAPI/client_api_wire.c:60-161`

- [ ] **Step 1: Add fields to struct**

In `src/ClientAPI/client_api_wire.h`, update `client_api_put_request_t`:

```c
typedef struct {
  char* content_type;
  char* file_name;
  size_t stream_length;
  char* server_address;   // may be NULL
  uint8_t* data;          // may be NULL for streaming uploads
  size_t data_size;
  char** recycler_urls;   // NULL or array of URL strings
  size_t recycler_count;  // 0 if no recycler
  uint8_t temporary;      // 0 or 1
} client_api_put_request_t;
```

- [ ] **Step 2: Update encode to include optional recycler/temporary fields**

In `src/ClientAPI/client_api_wire.c`, replace `client_api_put_request_encode`:

```c
cbor_item_t* client_api_put_request_encode(const client_api_put_request_t* msg) {
  uint8_t has_recycler = (msg->recycler_urls != NULL && msg->recycler_count > 0);
  size_t array_size = 6 + (has_recycler ? 1 : 0) + (msg->temporary ? 1 : 0);
  cbor_item_t* array = cbor_new_definite_array(array_size);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PUT_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->content_type);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->file_name);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->stream_length);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->server_address);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->data != NULL && msg->data_size > 0) {
    item = cbor_build_bytestring(msg->data, msg->data_size);
  } else {
    item = cbor_build_bytestring(NULL, 0);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (has_recycler) {
    cbor_item_t* urls_array = cbor_new_definite_array(msg->recycler_count);
    for (size_t i = 0; i < msg->recycler_count; i++) {
      item = cbor_build_string(msg->recycler_urls[i]);
      (void)cbor_array_push(urls_array, item);
      cbor_decref(&item);
    }
    (void)cbor_array_push(array, urls_array);
    cbor_decref(&urls_array);
  }

  if (msg->temporary) {
    item = cbor_build_uint8(1);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }

  return array;
}
```

- [ ] **Step 3: Update decode to parse optional recycler/temporary fields**

In `src/ClientAPI/client_api_wire.c`, replace `client_api_put_request_decode`:

```c
int client_api_put_request_decode(cbor_item_t* item, client_api_put_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* content_type = cbor_array_get(item, 1);
  msg->content_type = _decode_string(content_type, OFFS_MAX_CONTENT_TYPE_LEN);
  cbor_decref(&content_type);

  cbor_item_t* file_name = cbor_array_get(item, 2);
  msg->file_name = _decode_string(file_name, OFFS_MAX_FILE_NAME_LEN);
  cbor_decref(&file_name);

  cbor_item_t* stream_length = cbor_array_get(item, 3);
  msg->stream_length = _decode_size(stream_length);
  cbor_decref(&stream_length);

  if (validate_content_type(msg->content_type) != 0) {
    client_api_put_request_destroy(msg);
    return -1;
  }
  if (validate_file_name(msg->file_name) != 0) {
    client_api_put_request_destroy(msg);
    return -1;
  }
  if (msg->stream_length == 0 || msg->stream_length > OFFS_MAX_CBOR_MESSAGE_SIZE) {
    client_api_put_request_destroy(msg);
    return -1;
  }

  if (cbor_array_size(item) >= 5) {
    cbor_item_t* server_address = cbor_array_get(item, 4);
    msg->server_address = _decode_string(server_address, OFFS_MAX_ORI_STRING_LEN);
    cbor_decref(&server_address);
  }

  if (cbor_array_size(item) >= 6) {
    cbor_item_t* data_item = cbor_array_get(item, 5);
    if (!cbor_is_null(data_item) && cbor_isa_bytestring(data_item)) {
      msg->data_size = cbor_bytestring_length(data_item);
      if (msg->data_size > OFFS_MAX_CBOR_MESSAGE_SIZE) {
        cbor_decref(&data_item);
        client_api_put_request_destroy(msg);
        return -1;
      }
      if (msg->data_size > 0) {
        msg->data = get_memory(msg->data_size);
        memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
      }
    }
    cbor_decref(&data_item);
  }

  /* Index 6: recycler_urls (optional array of strings) */
  if (cbor_array_size(item) >= 7) {
    cbor_item_t* urls_item = cbor_array_get(item, 6);
    if (cbor_isa_array(urls_item)) {
      size_t url_count = cbor_array_size(urls_item);
      if (url_count > 0 && url_count <= 256) {
        msg->recycler_urls = get_clear_memory(sizeof(char*) * url_count);
        msg->recycler_count = url_count;
        for (size_t i = 0; i < url_count; i++) {
          cbor_item_t* url_str = cbor_array_get(urls_item, i);
          msg->recycler_urls[i] = _decode_string(url_str, OFFS_MAX_ORI_STRING_LEN);
          cbor_decref(&url_str);
          if (msg->recycler_urls[i] == NULL) {
            client_api_put_request_destroy(msg);
            cbor_decref(&urls_item);
            return -1;
          }
        }
      }
    }
    cbor_decref(&urls_item);
  }

  /* Index 7: temporary (optional uint8) */
  if (cbor_array_size(item) >= 8) {
    cbor_item_t* temp_item = cbor_array_get(item, 7);
    if (cbor_isa_uint(temp_item)) {
      msg->temporary = cbor_get_uint8(temp_item);
    }
    cbor_decref(&temp_item);
  }

  return 0;
}
```

- [ ] **Step 4: Update destroy to free recycler_urls**

In `src/ClientAPI/client_api_wire.c`, replace `client_api_put_request_destroy`:

```c
void client_api_put_request_destroy(client_api_put_request_t* msg) {
  if (msg == NULL) return;
  free(msg->content_type);
  free(msg->file_name);
  free(msg->server_address);
  free(msg->data);
  if (msg->recycler_urls != NULL) {
    for (size_t i = 0; i < msg->recycler_count; i++) {
      free(msg->recycler_urls[i]);
    }
    free(msg->recycler_urls);
  }
}
```

- [ ] **Step 5: Build and verify compilation**

Run: `cd build && cmake .. && make -j$(nproc)`
Expected: Compiles without errors.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/client_api_wire.h src/ClientAPI/client_api_wire.c
git commit -m "feat: add recycler_urls and temporary fields to PUT_REQUEST wire protocol"
```

---

### Task 2: Write wire protocol round-trip tests

**Files:**
- Create: `test/test_recycler_wire.cpp`

- [ ] **Step 1: Write the test file**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include <cbor.h>
}

TEST(TestRecyclerWire, TestPutRequestEncodeDecode_WithRecycler) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"text/plain";
  msg.file_name = (char*)"test.txt";
  msg.stream_length = 1024;
  msg.server_address = (char*)"http://localhost:23402";

  const char* urls[] = {
    "http://localhost:23402/offsystem/v3/text/plain/100/abc123/def456/file.txt",
    "http://localhost:23402/offsystem/v3/image/png/200/ghi789/jkl012/photo.png"
  };
  msg.recycler_urls = (char**)urls;
  msg.recycler_count = 2;
  msg.temporary = 1;

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  ASSERT_GE(cbor_array_size(encoded), 8);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int result = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(result, 0);

  EXPECT_STREQ(decoded.content_type, "text/plain");
  EXPECT_STREQ(decoded.file_name, "test.txt");
  EXPECT_EQ(decoded.stream_length, (size_t)1024);
  EXPECT_STREQ(decoded.server_address, "http://localhost:23402");
  EXPECT_EQ(decoded.recycler_count, (size_t)2);
  EXPECT_STREQ(decoded.recycler_urls[0], urls[0]);
  EXPECT_STREQ(decoded.recycler_urls[1], urls[1]);
  EXPECT_EQ(decoded.temporary, 1);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(TestRecyclerWire, TestPutRequestEncodeDecode_NoRecycler) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"text/plain";
  msg.file_name = (char*)"test.txt";
  msg.stream_length = 1024;

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(cbor_array_size(encoded), (size_t)6);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int result = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(result, 0);

  EXPECT_EQ(decoded.recycler_count, (size_t)0);
  EXPECT_EQ(decoded.recycler_urls, nullptr);
  EXPECT_EQ(decoded.temporary, 0);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}

TEST(TestRecyclerWire, TestPutRequestEncodeDecode_RecyclerOnly) {
  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)"application/octet-stream";
  msg.file_name = (char*)"data.bin";
  msg.stream_length = 4096;

  const char* urls[] = { "http://localhost:23402/offsystem/v3/app/octet-stream/500/aaa111/bbb222/old.bin" };
  msg.recycler_urls = (char**)urls;
  msg.recycler_count = 1;
  msg.temporary = 0;

  cbor_item_t* encoded = client_api_put_request_encode(&msg);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(cbor_array_size(encoded), (size_t)7);

  client_api_put_request_t decoded;
  memset(&decoded, 0, sizeof(decoded));
  int result = client_api_put_request_decode(encoded, &decoded);
  ASSERT_EQ(result, 0);

  EXPECT_EQ(decoded.recycler_count, (size_t)1);
  EXPECT_STREQ(decoded.recycler_urls[0], urls[0]);
  EXPECT_EQ(decoded.temporary, 0);

  client_api_put_request_destroy(&decoded);
  cbor_decref(&encoded);
}
```

- [ ] **Step 2: Register test in CMakeLists.txt**

In `test/CMakeLists.txt`, add `test_recycler_wire.cpp` to the test sources list.

- [ ] **Step 3: Build and run tests**

Run: `cd build && cmake .. && make -j$(nproc) && ctest -R TestRecyclerWire -V`
Expected: All 3 tests pass.

- [ ] **Step 4: Run valgrind to verify no leaks**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/test_runner --gtest_filter=TestRecyclerWire*`
Expected: No leaks detected (ignore pre-existing leaks from valgrind known list).

- [ ] **Step 5: Commit**

```bash
git add test/test_recycler_wire.cpp test/CMakeLists.txt
git commit -m "test: add wire protocol round-trip tests for recycler and temporary fields"
```

---

### Task 3: Add offs_put_options_t and _ex functions to C client

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h:14-21,40-48`
- Modify: `src/ClientLibs/c/offs_client.c:1175-1221`

- [ ] **Step 1: Add options struct and _ex function declarations to header**

In `src/ClientLibs/c/offs_client.h`, add after `offs_client_config_t`:

```c
/* PUT options struct for extended parameters */
typedef struct {
  const char* content_type;
  const char* file_name;
  size_t stream_length;
  const char* server_address;
  const char** recycler_urls;
  size_t recycler_count;
  uint8_t temporary;
} offs_put_options_t;
```

Add function declarations after `offs_client_put_stream_end`:

```c
/* Extended PUT with recycler/temporary support */
int offs_client_put_ex(offs_client_t* client,
                       const offs_put_options_t* options,
                       const uint8_t* data,
                       size_t data_len,
                       offs_put_response_cb_t callback,
                       void* ctx);

int offs_client_put_stream_start_ex(offs_client_t* client,
                                     const offs_put_options_t* options);
```

- [ ] **Step 2: Implement _ex functions and update existing functions**

In `src/ClientLibs/c/offs_client.c`, add after `offs_client_put`:

```c
static void _fill_put_request(client_api_put_request_t* msg, const offs_put_options_t* options,
                               const uint8_t* data, size_t data_len) {
  msg->content_type = (char*)options->content_type;
  msg->file_name = (char*)options->file_name;
  msg->stream_length = options->stream_length;
  msg->server_address = (char*)options->server_address;
  msg->data = (uint8_t*)data;
  msg->data_size = data_len;
  msg->recycler_urls = (char**)options->recycler_urls;
  msg->recycler_count = options->recycler_count;
  msg->temporary = options->temporary;
}

int offs_client_put_ex(offs_client_t* client,
                       const offs_put_options_t* options,
                       const uint8_t* data,
                       size_t data_len,
                       offs_put_response_cb_t callback,
                       void* ctx) {
  if (client == NULL || !client->connected || options == NULL) return -1;

  platform_mutex_lock(client->lock);
  client->put_cb = callback;
  client->put_cb_ctx = ctx;
  platform_mutex_unlock(client->lock);

  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  _fill_put_request(&msg, options, data, data_len);

  cbor_item_t* frame = client_api_put_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_put_stream_start_ex(offs_client_t* client,
                                     const offs_put_options_t* options) {
  if (client == NULL || !client->connected || options == NULL) return -1;

  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  _fill_put_request(&msg, options, NULL, 0);

  cbor_item_t* frame = client_api_put_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}
```

Update `offs_client_put` to delegate to `_ex`:

```c
int offs_client_put(offs_client_t* client,
                    const char* content_type,
                    const char* file_name,
                    size_t stream_length,
                    const uint8_t* data,
                    size_t data_len,
                    offs_put_response_cb_t callback,
                    void* ctx) {
  offs_put_options_t options;
  memset(&options, 0, sizeof(options));
  options.content_type = content_type;
  options.file_name = file_name;
  options.stream_length = stream_length;
  return offs_client_put_ex(client, &options, data, data_len, callback, ctx);
}
```

Update `offs_client_put_stream_start` to delegate to `_ex`:

```c
int offs_client_put_stream_start(offs_client_t* client,
                                  const char* content_type,
                                  const char* file_name,
                                  size_t stream_length) {
  offs_put_options_t options;
  memset(&options, 0, sizeof(options));
  options.content_type = content_type;
  options.file_name = file_name;
  options.stream_length = stream_length;
  return offs_client_put_stream_start_ex(client, &options);
}
```

- [ ] **Step 3: Build and verify compilation**

Run: `cd build && cmake .. && make -j$(nproc)`
Expected: Compiles without errors.

- [ ] **Step 4: Commit**

```bash
git add src/ClientLibs/c/offs_client.h src/ClientLibs/c/offs_client.c
git commit -m "feat: add offs_put_options_t and _ex functions for recycler/temporary support"
```

---

### Task 4: Add recycler/temporary header parsing to server PUT handler

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c:638-724,738-811`

- [ ] **Step 1: Add include and helper for recycler header parsing**

In `src/ClientAPI/HTTP/off_routes.c`, add include at top:

```c
#include "../../OFFStreams/ori.h"
```

Add a helper function before `_off_put_handler` to parse the recycler header into a `vec_ori_t`:

```c
static void _parse_recycler_header(const char* recycler_header, vec_ori_t* oris) {
  vec_init(oris);
  if (recycler_header == NULL || recycler_header[0] == '\0') return;

  /* recycler header is a JSON array: ["url1", "url2", ...] */
  const char* cursor = recycler_header;
  while (*cursor) {
    /* Find opening quote of a URL string */
    const char* start = strchr(cursor, '"');
    if (start == NULL) break;
    start++;
    const char* end = strchr(start, '"');
    if (end == NULL) break;

    size_t url_len = (size_t)(end - start);
    if (url_len > 0 && url_len < OFFS_MAX_ORI_STRING_LEN) {
      char* url_str = get_memory(url_len + 1);
      memcpy(url_str, start, url_len);
      url_str[url_len] = '\0';

      off_url_t* parsed = off_url_parse(url_str);
      if (parsed != NULL) {
        ori_t* ori = ori_create(parsed->stream_length);
        ori->descriptor_hash = buffer_copy(parsed->descriptor_hash);
        ori->file_hash = buffer_copy(parsed->file_hash);
        ori->file_name = strdup(parsed->file_name);
        ori->block_type = standard;
        ori->tuple_size = 3;
        vec_push(oris, ori);
        off_url_destroy(parsed);
      }
      free(url_str);
    }
    cursor = end + 1;
  }
}
```

- [ ] **Step 2: Update `_off_put_handler` to use recycler recipe**

In `_off_put_handler`, after parsing existing headers (before the recipe creation), add:

```c
  const char* recycler_header = http_request_header(request, "recycler");
  const char* temporary_header = http_request_header(request, "temporary");
  uint8_t is_temporary = (temporary_header != NULL && strcmp(temporary_header, "true") == 0);
```

Replace the recipe creation block:

```c
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(ctx->pool, ctx->bc, standard);
  vec_block_recipe_t recipes;
  vec_init(&recipes);
```

With:

```c
  vec_block_recipe_t recipes;
  vec_init(&recipes);

  vec_ori_t recycler_oris;
  _parse_recycler_header(recycler_header, &recycler_oris);
  if (recycler_oris.length > 0) {
    recycler_recipe_t* recycler = recycler_recipe_create(ctx->pool, ctx->bc, standard, recycler_oris, NULL);
    vec_push(&recipes, (block_recipe_t*)recycler);
    /* recycler_recipe took ownership of ori references; vec cleanup is handled by recipe destroy */
  }

  new_blocks_recipe_t* recipe = new_blocks_recipe_create(ctx->pool, ctx->bc, standard);
  vec_push(&recipes, (block_recipe_t*)recipe);
```

- [ ] **Step 3: Apply same changes to `_off_put_headers_complete`**

Make the identical recycler header parsing and recipe ordering change in `_off_put_headers_complete` (lines 738-811), after the existing header parsing but before recipe creation.

- [ ] **Step 4: Build and verify compilation**

Run: `cd build && cmake .. && make -j$(nproc)`
Expected: Compiles without errors.

- [ ] **Step 5: Run existing tests to check for regressions**

Run: `cd build && ctest -R TestOffRoutes -V`
Expected: All existing tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/HTTP/off_routes.c
git commit -m "feat: add recycler and temporary header parsing to server PUT handler"
```

---

### Task 5: Populate OFD cache on directory PUT

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c`

**Why:** When a directory OFD is uploaded (content-type `offsystem/directory`), the cache should be populated immediately so that subsequent GET requests for paths within the directory hit the cache. Currently the cache is only populated lazily on the first GET via `ofd_cache_resolve()`.

- [ ] **Step 1: Add OFD cache populator actor type**

In `src/ClientAPI/HTTP/off_routes.c`, add after the `put_context_t` struct:

```c
typedef struct {
  actor_t actor;
  ofd_cache_t* ofd_cache;
  buffer_t* file_hash;
  scheduler_pool_t* pool;
} ofd_cache_populator_t;

static void _ofd_cache_populator_dispatch(void* state, message_t* msg) {
  ofd_cache_populator_t* populator = (ofd_cache_populator_t*)state;

  switch (msg->type) {
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      block_t* block = result->block;

      if (block != NULL) {
        ofd_t* ofd = ofd_decode(block->data);
        if (ofd != NULL) {
          ofd_cache_put(populator->ofd_cache, populator->file_hash, ofd);
        }
        block_destroy(block);
      }
      DESTROY(result->hash, buffer);

      DESTROY(populator->file_hash, buffer);
      atomic_fetch_or(&populator->actor.flags, ACTOR_FLAG_DESTROY);
      actor_destroy(&populator->actor);
      scheduler_pool_defer_cleanup(populator->pool, populator, free);
      return;
    }
    default:
      break;
  }
}
```

- [ ] **Step 2: Add ofd_cache and bc to put_context_t; spawn populator in `_put_on_descriptor_close`**

Add two new fields to the `put_context_t` struct:

```c
typedef struct {
    http_response_t* response;
    http_connection_t* connection;
    buffer_t* file_hash;
    buffer_t* descriptor_hash;
    char* content_type;
    char* file_name;
    size_t stream_length;
    char* server_address;
    writeable_descriptor_t* desc;
    writeable_off_stream_t* ws;
    new_blocks_recipe_t* recipe;
    tuple_cache_t* tc;
    ofd_cache_t* ofd_cache;    // NEW
    block_cache_t* bc;         // NEW
} put_context_t;
```

In `_off_put_handler`, add after `put_ctx->tc = ctx->tc;`:

```c
    put_ctx->ofd_cache = ctx->ofd_cache;
    put_ctx->bc = ctx->bc;
```

Same additions in `_off_put_headers_complete`.

In `_put_on_descriptor_close`, add after `buffer_t* file_hash = buffer_copy(put_ctx->file_hash);`:

```c
  /* Populate OFD cache for directory content types on upload */
  if (put_ctx->content_type != NULL &&
      strstr(put_ctx->content_type, "offsystem/directory") != NULL &&
      put_ctx->ofd_cache != NULL) {
    ofd_cache_populator_t* populator = get_clear_memory(sizeof(ofd_cache_populator_t));
    actor_init(&populator->actor, populator, _ofd_cache_populator_dispatch,
               ((stream_t*)put_ctx->ws)->pool);
    populator->ofd_cache = put_ctx->ofd_cache;
    populator->file_hash = buffer_copy(file_hash);
    populator->pool = ((stream_t*)put_ctx->ws)->pool;

    buffer_t* hash_ref = buffer_copy(file_hash);
    block_cache_get(put_ctx->bc, hash_ref, &populator->actor);
    DESTROY(hash_ref, buffer);
  }
```

- [ ] **Step 3: Build and verify compilation**

Run: `cd build && cmake .. && make -j$(nproc)`
Expected: Compiles without errors.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/HTTP/off_routes.c
git commit -m "feat: populate OFD cache on directory PUT upload, shared across all transports"
```

---

### Task 7: Add recycler/temporary to Dart OffApi service

**Files:**
- Modify: `examples/off_client/lib/services/off_api.dart`

- [ ] **Step 1: Add recyclerUrls and temporary parameters to upload methods**

In `off_api.dart`, update `uploadFile` signature and body:

```dart
  Future<String> uploadFile({
    required String fileName,
    required int streamLength,
    String? contentType,
    String? serverAddress,
    List<String>? recyclerUrls,
    bool temporary = false,
    required String filePath,
    void Function(double progress)? onProgress,
  }) async {
    final type = contentType ?? mimeFromExtension(fileName);
    final uri = Uri.parse('$baseUrl/offsystem');
    final file = File(filePath);
    final fileStream = file.openRead();

    final request = http.StreamedRequest('PUT', uri);
    request.headers['type'] = type;
    request.headers['file-name'] = fileName;
    request.headers['stream-length'] = streamLength.toString();
    request.headers['Content-Type'] = 'application/octet-stream';
    if (serverAddress != null) {
      request.headers['server-address'] = serverAddress;
    }
    if (recyclerUrls != null && recyclerUrls.isNotEmpty) {
      request.headers['recycler'] = jsonEncode(recyclerUrls);
    }
    if (temporary) {
      request.headers['temporary'] = 'true';
    }

    // ... rest unchanged
  }
```

Update `uploadFileBuffered` similarly:

```dart
  Future<String> uploadFileBuffered({
    required String fileName,
    required int streamLength,
    String? contentType,
    String? serverAddress,
    List<String>? recyclerUrls,
    bool temporary = false,
    required List<int> bodyBytes,
  }) async {
    // ... same header additions as uploadFile
  }
```

Add `import 'dart:convert';` at the top if not already present.

- [ ] **Step 2: Verify Dart analysis**

Run: `cd examples/off_client && dart analyze lib/services/off_api.dart`
Expected: No errors.

- [ ] **Step 3: Commit**

```bash
git add examples/off_client/lib/services/off_api.dart
git commit -m "feat: add recyclerUrls and temporary parameters to OffApi upload methods"
```

---

### Task 8: Add folder import to Dart UI

**Files:**
- Modify: `examples/off_client/lib/screens/import_screen.dart`

- [ ] **Step 1: Add folder import method and UI**

In `_ImportScreenState`, add state variables:

```dart
  bool _isFolder = false;
  List<String>? _recyclerUrls;
  final TextEditingController _recyclerController = TextEditingController();
```

Add folder import method:

```dart
  Future<void> _importFolder() async {
    final dirResult = await FilePicker.platform.getDirectoryPath();
    if (dirResult == null) return;

    setState(() {
      _isUploading = true;
      _progress = 0;
      _error = null;
      _resultUrl = null;
    });

    try {
      final folder = Directory(dirResult);
      final folderName = folder.path.split(Platform.pathSeparator).last;
      final ofd = <String, String>{};

      final entries = folder.listSync(recursive: true);
      final files = entries.whereType<File>().toList();

      for (int i = 0; i < files.length; i++) {
        final file = files[i];
        final relativePath = file.path.substring(folder.path.length + 1);
        final filePath = relativePath.replaceAll(Platform.pathSeparator, '/');

        final length = await file.length();
        final url = await _api.uploadFile(
          fileName: filePath.split('/').last,
          streamLength: length,
          filePath: file.path,
          recyclerUrls: _recyclerUrls,
        );

        ofd[filePath] = url;
        setState(() => _progress = (i + 1) / (files.length + 1));
      }

      final ofdJson = jsonEncode(ofd);
      final ofdBytes = utf8.encode(ofdJson);
      final finalUrl = await _api.uploadFileBuffered(
        fileName: '$folderName.ofd',
        streamLength: ofdBytes.length,
        contentType: 'offsystem/directory',
        bodyBytes: ofdBytes,
        recyclerUrls: _recyclerUrls,
      );

      setState(() {
        _progress = 1.0;
        _resultUrl = finalUrl;
      });
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isUploading = false);
    }
  }
```

Add `import 'dart:convert';` and `import 'dart:io';` at the top.

- [ ] **Step 2: Add folder import button to the UI**

In the `build` method, add a button before the file pick button:

```dart
          ElevatedButton.icon(
            onPressed: _isUploading ? null : _importFolder,
            icon: const Icon(Icons.create_new_folder),
            label: const Text('Import Folder'),
          ),
```

- [ ] **Step 3: Verify Dart analysis**

Run: `cd examples/off_client && dart analyze lib/screens/import_screen.dart`
Expected: No errors.

- [ ] **Step 4: Commit**

```bash
git add examples/off_client/lib/screens/import_screen.dart
git commit -m "feat: add folder import to Dart UI with recycler support"
```

---

### Task 9: Add folder export to Dart UI

**Files:**
- Modify: `examples/off_client/lib/screens/export_screen.dart`

- [ ] **Step 1: Add folder export method**

In `_ExportScreenState`, add:

```dart
  Future<void> _exportFolder() async {
    final url = _urlController.text.trim();
    if (url.isEmpty || _saveDirectory == null) return;

    setState(() {
      _isDownloading = true;
      _error = null;
      _resultFile = null;
    });

    try {
      final rawUrl = url.contains('?') ? '$url&ofd=raw' : '$url?ofd=raw';
      final response = await http.get(Uri.parse(rawUrl));
      if (response.statusCode != 200) {
        throw Exception('Failed to fetch OFD: ${response.statusCode}');
      }

      final ofd = jsonDecode(response.body) as Map<String, dynamic>;
      final urlParsed = Uri.parse(url);
      final pathSegments = urlParsed.pathSegments;
      final rawFileName = pathSegments.isNotEmpty ? pathSegments.last : 'export';
      final dirName = rawFileName.endsWith('.ofd')
          ? rawFileName.substring(0, rawFileName.length - 4)
          : rawFileName;
      final localDir = Directory('${_saveDirectory!}${Platform.pathSeparator}$dirName');
      if (!await localDir.exists()) {
        await localDir.create(recursive: true);
      }

      final entries = ofd.entries.toList();
      for (int i = 0; i < entries.length; i++) {
        final path = entries[i].key;
        final fileUrl = entries[i].value as String;

        final fileResponse = await http.get(Uri.parse(fileUrl));
        if (fileResponse.statusCode != 200) {
          throw Exception('Failed to download $path: ${fileResponse.statusCode}');
        }

        final filePath = '${localDir.path}${Platform.pathSeparator}$path';
        final file = File(filePath);
        final parentDir = file.parent;
        if (!await parentDir.exists()) {
          await parentDir.create(recursive: true);
        }
        await file.writeAsBytes(fileResponse.bodyBytes);
      }

      setState(() => _resultFile = localDir.path);
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isDownloading = false);
    }
  }
```

Add `import 'dart:convert';` and `import 'dart:io';` at the top, and `import 'package:http/http.dart' as http;` if not present.

- [ ] **Step 2: Add folder export button to UI**

In the `build` method, add next to the download button:

```dart
          ElevatedButton(
            onPressed: _urlController.text.isNotEmpty && !_isDownloading ? _exportFolder : null,
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF313181),
              foregroundColor: Colors.white,
            ),
            child: const Text('Export Folder'),
          ),
```

- [ ] **Step 3: Verify Dart analysis**

Run: `cd examples/off_client && dart analyze lib/screens/export_screen.dart`
Expected: No errors.

- [ ] **Step 4: Commit**

```bash
git add examples/off_client/lib/screens/export_screen.dart
git commit -m "feat: add folder export to Dart UI with OFD parsing"
```

---

### Task 10: Run full test suite and valgrind

- [ ] **Step 1: Run full test suite**

Run: `cd build && ctest -V`
Expected: All tests pass.

- [ ] **Step 2: Run server tests under valgrind**

Run: `valgrind --leak-check=full --track-origins=yes ./build/test_runner --gtest_filter=TestOffRoutes* 2>&1 | grep -E "definitely|indirectly|possibly|ERROR SUMMARY"`
Expected: No new leaks beyond the pre-existing known list.

- [ ] **Step 3: Run recycler wire tests under valgrind**

Run: `valgrind --leak-check=full --track-origins=yes ./build/test_runner --gtest_filter=TestRecyclerWire* 2>&1 | grep -E "definitely|indirectly|possibly|ERROR SUMMARY"`
Expected: 0 bytes definitely/indirectly lost.

- [ ] **Step 4: Commit any final fixes**

```bash
git add -A
git commit -m "chore: verify tests pass and no new valgrind leaks"
```
