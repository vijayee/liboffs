# OFD-Aware Recycler Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make both client libraries (C and Flutter) OFD-aware so they resolve recycler OFDs into per-file recycler lists using name-matching and greedy block-count donor selection.

**Architecture:** New files `offs_ofd_resolver.h/.c` in the C client library and `recycler_resolver.dart` in the Flutter client. A new `offs_http_get()` function in the C library for raw HTTP GET (needed to fetch OFD CBOR). The existing `_uploadDirectory()` in the Flutter client gains OFD resolution. No server-side changes.

**Tech Stack:** C11 (libcbor, base58), Dart (cbor, http packages)

---

## File Map

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ClientLibs/c/offs_ofd_resolver.h` | Donor types, function declarations |
| Create | `src/ClientLibs/c/offs_ofd_resolver.c` | Resolve, build_recyclers, total_blocks |
| Modify | `src/ClientLibs/c/offs_client.h` | Add `offs_http_get()` decl |
| Modify | `src/ClientLibs/c/offs_client.c` | Add `offs_http_get()` impl |
| Create | `src/ClientLibs/c/test_offs_ofd_resolver.c` | C unit tests |
| Modify | `examples/off_client/lib/services/off_api.dart` | Add `fetchRawOfd()` |
| Create | `examples/off_client/lib/services/recycler_resolver.dart` | Resolver + donor algorithm |
| Create | `examples/off_client/test/recycler_resolver_test.dart` | Dart unit tests |
| Modify | `examples/off_client/lib/screens/import_screen.dart` | Integrate resolver in `_uploadDirectory()` |

---

### Task 1: C Library — `offs_http_get()`

**Files:**
- Modify: `src/ClientLibs/c/offs_client.h`
- Modify: `src/ClientLibs/c/offs_client.c`

- [ ] **Step 1: Add declaration to `offs_client.h`**

After the `offs_client_health` declaration (line 113), add:

```c
/* Raw HTTP GET — opens a temporary TCP connection to fetch data from a URL.
   Returns a buffer_t* with the response body, or NULL on error.
   Caller must DESTROY the returned buffer. */
buffer_t* offs_http_get(const char* url);
```

The header needs a forward include of `<Buffer/buffer.h>`. Add at the top after the existing includes:

```c
#include "../Buffer/buffer.h"
```

- [ ] **Step 2: Add implementation to `offs_client.c`**

Add this function at the end of `offs_client.c` (before the final `#endif` guard if any):

```c
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>

buffer_t* offs_http_get(const char* url) {
  if (!url) return NULL;

  /* Parse URL: http://host[:port]/path */
  const char* prefix = "http://";
  if (strncmp(url, prefix, 7) != 0) return NULL;
  const char* rest = url + 7;

  /* Extract host */
  const char* host_start = rest;
  const char* host_end = strchr(host_start, ':');
  const char* port_end = strchr(host_start, '/');
  const char* path_start = NULL;

  char host[256];
  int port = 80;

  if (host_end && host_end < (port_end ? port_end : host_start + strlen(host_start))) {
    /* Has port */
    size_t host_len = host_end - host_start;
    if (host_len >= sizeof(host)) return NULL;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    port = (int)strtol(host_end + 1, NULL, 10);
    path_start = strchr(host_end, '/');
  } else if (port_end) {
    /* No port, has path */
    size_t host_len = port_end - host_start;
    if (host_len >= sizeof(host)) return NULL;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    path_start = port_end;
  } else {
    /* No port, no path */
    size_t host_len = strlen(host_start);
    if (host_len >= sizeof(host)) return NULL;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    path_start = "/";
  }

  if (port <= 0 || port > 65535) return NULL;

  /* Resolve host */
  struct hostent* he = gethostbyname(host);
  if (!he) return NULL;

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return NULL;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
  addr.sin_port = htons((uint16_t)port);

  /* Set connect timeout */
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(sock);
    return NULL;
  }

  /* Build and send HTTP GET request */
  char request[4096];
  int req_len = snprintf(request, sizeof(request),
    "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
    path_start, host);
  if (req_len < 0 || req_len >= (int)sizeof(request)) {
    close(sock);
    return NULL;
  }

  if (send(sock, request, (size_t)req_len, 0) < 0) {
    close(sock);
    return NULL;
  }

  /* Read response */
  char resp_buf[65536];
  ssize_t total = 0;
  ssize_t n;
  while ((n = recv(sock, resp_buf + total, sizeof(resp_buf) - (size_t)total - 1, 0)) > 0) {
    total += n;
    if ((size_t)total >= sizeof(resp_buf) - 1) break;
  }
  close(sock);

  if (total <= 0) return NULL;
  resp_buf[total] = '\0';

  /* Find body (after \r\n\r\n) */
  char* body = strstr(resp_buf, "\r\n\r\n");
  if (!body) return NULL;
  body += 4;

  size_t body_len = (size_t)(resp_buf + total - body);

  /* Parse Content-Length if present */
  char* cl_header = strstr(resp_buf, "Content-Length:");
  if (!cl_header) cl_header = strstr(resp_buf, "content-length:");
  if (cl_header) {
    cl_header += 15;
    while (*cl_header == ' ') cl_header++;
    size_t cl = (size_t)strtol(cl_header, NULL, 10);
    if (cl < body_len) body_len = cl;
  }

  if (body_len == 0) return NULL;

  buffer_t* result = buffer_create(body_len);
  if (!result) return NULL;
  memcpy(result->data, body, body_len);
  return result;
}
```

Note: On Windows this will need `winsock2.h` instead of `<arpa/inet.h>` / `<unistd.h>`. Since the project currently targets Linux (per CLAUDE.md), we use POSIX sockets.

- [ ] **Step 3: Verify compilation**

Run: `cd build && cmake .. && make offs_client 2>&1 | head -20`
Expected: compilation succeeds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/ClientLibs/c/offs_client.h src/ClientLibs/c/offs_client.c
git commit -m "feat: add offs_http_get() for raw HTTP GET in C client library"
```

---

### Task 2: C Library — `offs_ofd_resolver.h`

**Files:**
- Create: `src/ClientLibs/c/offs_ofd_resolver.h`

- [ ] **Step 1: Write the header file**

```c
//
// Created by victor on 5/26/26.
//

#ifndef OFFS_OFD_RESOLVER_H
#define OFFS_OFD_RESOLVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A parsed OFD file entry used for donor matching */
typedef struct {
  char* name;
  char* url;            /* pre-built offs URL for this ORI */
  size_t final_byte;
  int block_type;       /* block_size_e value */
  size_t tuple_size;
  uint8_t* file_hash;   /* raw hash bytes for dedup */
  size_t hash_len;
} offs_ofd_donor_t;

/* Fetch and parse an OFD from a URL, recursively resolving sub-OFDs.
   Returns an array of donor entries (all file ORIs from the OFD tree).
   out_count receives the array length. Returns NULL on error.
   Caller frees with offs_ofd_free_donors(). */
offs_ofd_donor_t* offs_ofd_resolve(const char* ofd_url, size_t* out_count);

/* Build a per-file recycler URL list using the greedy donor algorithm.
   matched: the matching OFD entry for this file, or NULL if no match.
   file_size: size of the local file being uploaded.
   fallback_urls/fallback_count: original non-OFD recycler URLs.
   out_count: receives the number of URLs in the returned array.
   Returns a malloc'd array of malloc'd URL strings. Caller frees each string
   then the array. */
char** offs_ofd_build_recyclers(
    const offs_ofd_donor_t* donors, size_t donor_count,
    const offs_ofd_donor_t* matched,
    size_t file_size,
    const char** fallback_urls, size_t fallback_count,
    size_t* out_count);

/* Free an array of donor entries returned by offs_ofd_resolve(). */
void offs_ofd_free_donors(offs_ofd_donor_t* donors, size_t count);

/* Calculate total blocks (data + descriptor) for a file/ORI.
   Used internally and exposed for testing. */
size_t offs_ofd_total_blocks(size_t final_byte, int block_type, size_t tuple_size);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_OFD_RESOLVER_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/ClientLibs/c/offs_ofd_resolver.h
git commit -m "feat: add offs_ofd_resolver.h with donor types and API declarations"
```

---

### Task 3: C Library — `offs_ofd_resolver.c`

**Files:**
- Create: `src/ClientLibs/c/offs_ofd_resolver.c`

- [ ] **Step 1: Write the implementation**

```c
//
// Created by victor on 5/26/26.
//

#include "offs_ofd_resolver.h"
#include "offs_client.h"
#include "../../OFFStreams/ofd.h"
#include "../../OFFStreams/off_url.h"
#include "../../OFFStreams/ori.h"
#include "../../Buffer/buffer.h"
#include "../../Util/base58.h"
#include "../../Util/get_clear_memory.h"
#include "../../RefCounter/refcounter.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- Block sizing ---- */

static size_t _block_size_for_type(int type) {
  switch ((block_size_e)type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

size_t offs_ofd_total_blocks(size_t final_byte, int block_type, size_t tuple_size) {
  size_t bs = _block_size_for_type(block_type);
  size_t descriptor_pad = 32;  /* SHA-256 hash size */

  size_t data_blocks = (final_byte + bs - 1) / bs;
  size_t cut_point = (bs / descriptor_pad) * descriptor_pad;
  size_t desc_data_per_blk = cut_point - descriptor_pad;
  size_t desc_bytes = data_blocks * descriptor_pad * tuple_size;
  size_t desc_blocks = (desc_bytes + desc_data_per_blk - 1) / desc_data_per_blk;

  return data_blocks + desc_blocks;
}

/* ---- URL building from OFD entries ---- */

static char* _build_ori_url(const char* server_base, const char* content_type,
                             size_t stream_length, const buffer_t* file_hash,
                             const buffer_t* descriptor_hash, const char* file_name) {
  /* Build an off_url_t and serialize it */
  off_url_t url;
  memset(&url, 0, sizeof(url));
  url.server_address = (char*)server_base;
  url.content_type = (char*)content_type;
  url.stream_length = stream_length;
  url.stream_offset = 0;
  url.file_hash = (buffer_t*)file_hash;
  url.descriptor_hash = (buffer_t*)descriptor_hash;
  url.file_name = (char*)file_name;

  return off_url_to_string(&url);
}

/* Extract server base from an offs URL like "http://host:port/offsystem/v3/..."
   Returns "http://host:port"  */
static char* _extract_server_base(const char* url) {
  const char* prefix = "http://";
  if (strncmp(url, prefix, 7) != 0) return NULL;
  const char* host_start = url + 7;
  const char* slash = strchr(host_start, '/');
  size_t base_len;
  if (slash) {
    base_len = (size_t)(slash - url);
  } else {
    base_len = strlen(url);
  }
  char* base = get_clear_memory(base_len + 1);
  memcpy(base, url, base_len);
  return base;
}

/* Extract filename (base name) from an OFD URL */
static char* _extract_filename(const char* url) {
  const char* last_slash = strrchr(url, '/');
  if (!last_slash) return NULL;
  const char* name_start = last_slash + 1;
  /* Strip query params */
  const char* qmark = strchr(name_start, '?');
  size_t name_len;
  if (qmark) {
    name_len = (size_t)(qmark - name_start);
  } else {
    name_len = strlen(name_start);
  }
  char* name = get_clear_memory(name_len + 1);
  memcpy(name, name_start, name_len);
  return name;
}

/* ---- OFD resolution ---- */

/* Build a sub-OFD URL from a directory entry's hash */
static char* _build_sub_ofd_url(const char* server_base, const buffer_t* dir_hash,
                                 const char* entry_name) {
  /* Build filename: <entry_name>.ofd */
  size_t base_name_len = strlen(entry_name);
  char* ofd_name = get_clear_memory(base_name_len + 5);
  memcpy(ofd_name, entry_name, base_name_len);
  memcpy(ofd_name + base_name_len, ".ofd", 4);

  off_url_t url;
  memset(&url, 0, sizeof(url));
  url.server_address = (char*)server_base;
  url.content_type = "offsystem/directory";
  url.stream_length = 0;
  url.stream_offset = 0;
  url.file_hash = (buffer_t*)dir_hash;
  url.descriptor_hash = (buffer_t*)dir_hash;
  url.file_name = ofd_name;

  char* url_str = off_url_to_string(&url);
  free(ofd_name);
  return url_str;
}

/* Recursive helper: append file entries from an OFD to the donors array */
static int _resolve_ofd_entries(ofd_t* ofd, const char* server_base,
                                 offs_ofd_donor_t** donors, size_t* donor_count,
                                 size_t* donor_cap) {
  for (size_t i = 0; i < ofd->entries.length; i++) {
    ofd_entry_t* entry = &ofd->entries.data[i];

    if (entry->type == OFD_ENTRY_FILE && entry->file_ori) {
      ori_t* ori = entry->file_ori;

      /* Build ORI URL */
      char* ori_url = _build_ori_url(server_base, "application/octet-stream",
                                      ori->final_byte, ori->file_hash,
                                      ori->descriptor_hash, entry->name);
      if (!ori_url) continue;

      /* Grow donors array if needed */
      if (*donor_count >= *donor_cap) {
        size_t new_cap = *donor_cap ? *donor_cap * 2 : 16;
        offs_ofd_donor_t* new_donors = realloc(*donors, new_cap * sizeof(offs_ofd_donor_t));
        if (!new_donors) {
          free(ori_url);
          return -1;
        }
        *donors = new_donors;
        *donor_cap = new_cap;
      }

      offs_ofd_donor_t* donor = &(*donors)[*donor_count];
      memset(donor, 0, sizeof(*donor));
      donor->name = strdup(entry->name);
      donor->url = ori_url;
      donor->final_byte = ori->final_byte;
      donor->block_type = (int)ori->block_type;
      donor->tuple_size = ori->tuple_size;
      donor->hash_len = ori->file_hash->size;
      donor->file_hash = get_clear_memory(ori->file_hash->size);
      memcpy(donor->file_hash, ori->file_hash->data, ori->file_hash->size);
      (*donor_count)++;
    }
  }

  /* Recurse into subdirectories */
  for (size_t i = 0; i < ofd->entries.length; i++) {
    ofd_entry_t* entry = &ofd->entries.data[i];
    if (entry->type == OFD_ENTRY_DIRECTORY && entry->dir_hash) {
      char* sub_url = _build_sub_ofd_url(server_base, entry->dir_hash, entry->name);
      if (!sub_url) continue;

      /* Append ?ofd=raw */
      size_t sub_url_len = strlen(sub_url);
      char* raw_url = get_clear_memory(sub_url_len + 10);
      memcpy(raw_url, sub_url, sub_url_len);
      memcpy(raw_url + sub_url_len, "?ofd=raw", 9);
      free(sub_url);

      buffer_t* raw_data = offs_http_get(raw_url);
      free(raw_url);

      if (raw_data) {
        ofd_t* sub_ofd = ofd_decode(raw_data);
        if (sub_ofd) {
          _resolve_ofd_entries(sub_ofd, server_base, donors, donor_count, donor_cap);
          DESTROY(sub_ofd, ofd);
        }
        DESTROY(raw_data, buffer);
      }
    }
  }

  return 0;
}

offs_ofd_donor_t* offs_ofd_resolve(const char* ofd_url, size_t* out_count) {
  if (!ofd_url || !out_count) return NULL;
  *out_count = 0;

  /* Append ?ofd=raw */
  size_t url_len = strlen(ofd_url);
  char* raw_url = get_clear_memory(url_len + 10);
  memcpy(raw_url, ofd_url, url_len);
  if (strchr(ofd_url, '?')) {
    memcpy(raw_url + url_len, "&ofd=raw", 9);
  } else {
    memcpy(raw_url + url_len, "?ofd=raw", 9);
  }

  buffer_t* raw_data = offs_http_get(raw_url);
  free(raw_url);
  if (!raw_data) return NULL;

  ofd_t* ofd = ofd_decode(raw_data);
  DESTROY(raw_data, buffer);
  if (!ofd) return NULL;

  char* server_base = _extract_server_base(ofd_url);
  if (!server_base) {
    DESTROY(ofd, ofd);
    return NULL;
  }

  offs_ofd_donor_t* donors = NULL;
  size_t donor_count = 0;
  size_t donor_cap = 0;

  int rc = _resolve_ofd_entries(ofd, server_base, &donors, &donor_count, &donor_cap);
  free(server_base);
  DESTROY(ofd, ofd);

  if (rc != 0) {
    offs_ofd_free_donors(donors, donor_count);
    return NULL;
  }

  *out_count = donor_count;
  return donors;
}

/* ---- Donor selection ---- */

/* Compare donors by total_blocks descending for qsort */
static int _donor_block_cmp(const void* a, const void* b) {
  const offs_ofd_donor_t* da = (const offs_ofd_donor_t*)a;
  const offs_ofd_donor_t* db = (const offs_ofd_donor_t*)b;
  size_t ba = offs_ofd_total_blocks(da->final_byte, da->block_type, da->tuple_size);
  size_t bb = offs_ofd_total_blocks(db->final_byte, db->block_type, db->tuple_size);
  if (ba > bb) return -1;
  if (ba < bb) return 1;
  return 0;
}

/* Check if two raw hashes are equal */
static int _hash_eq(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
  if (a_len != b_len) return 0;
  return memcmp(a, b, a_len) == 0;
}

char** offs_ofd_build_recyclers(
    const offs_ofd_donor_t* donors, size_t donor_count,
    const offs_ofd_donor_t* matched,
    size_t file_size,
    const char** fallback_urls, size_t fallback_count,
    size_t* out_count)
{
  if (!out_count) return NULL;
  *out_count = 0;
  if (!donors && donor_count > 0) return NULL;

  /* Estimate max output size: matched + all donors + fallbacks */
  size_t max_out = (matched ? 1 : 0) + donor_count + fallback_count;
  char** result = get_clear_memory(max_out * sizeof(char*));
  if (!result) return NULL;
  size_t result_count = 0;

  /* Default block parameters for the file being uploaded */
  int file_block_type = matched ? matched->block_type : (int)standard;
  size_t file_tuple_size = matched ? matched->tuple_size : (size_t)3;

  size_t blocks_needed = offs_ofd_total_blocks(file_size, file_block_type, file_tuple_size);
  size_t blocks_covered = 0;

  /* 1. Matched ORI */
  if (matched) {
    result[result_count] = strdup(matched->url);
    if (!result[result_count]) goto oom;
    result_count++;
    blocks_covered += offs_ofd_total_blocks(matched->final_byte, matched->block_type,
                                             matched->tuple_size);
  }

  /* 2. Greedy donors sorted by block count descending */
  if (blocks_covered < blocks_needed && donor_count > 0) {
    /* Create a sortable copy of donor pointers */
    offs_ofd_donor_t* sorted = get_clear_memory(donor_count * sizeof(offs_ofd_donor_t));
    if (!sorted) goto oom;
    memcpy(sorted, donors, donor_count * sizeof(offs_ofd_donor_t));
    qsort(sorted, donor_count, sizeof(offs_ofd_donor_t), _donor_block_cmp);

    for (size_t i = 0; i < donor_count && blocks_covered < blocks_needed; i++) {
      /* Skip self (the matched donor) */
      if (matched && _hash_eq(sorted[i].file_hash, sorted[i].hash_len,
                               matched->file_hash, matched->hash_len)) {
        continue;
      }
      result[result_count] = strdup(sorted[i].url);
      if (!result[result_count]) {
        free(sorted);
        goto oom;
      }
      result_count++;
      blocks_covered += offs_ofd_total_blocks(sorted[i].final_byte, sorted[i].block_type,
                                               sorted[i].tuple_size);
    }
    free(sorted);
  }

  /* 3. Fallback recyclers */
  for (size_t i = 0; i < fallback_count; i++) {
    result[result_count] = strdup(fallback_urls[i]);
    if (!result[result_count]) goto oom;
    result_count++;
  }

  *out_count = result_count;
  return result;

oom:
  for (size_t i = 0; i < result_count; i++) free(result[i]);
  free(result);
  return NULL;
}

void offs_ofd_free_donors(offs_ofd_donor_t* donors, size_t count) {
  if (!donors) return;
  for (size_t i = 0; i < count; i++) {
    free(donors[i].name);
    free(donors[i].url);
    free(donors[i].file_hash);
  }
  free(donors);
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd build && cmake .. && make offs_ofd_resolver 2>&1 | head -20`
Expected: compilation succeeds. Fix any include path issues.

- [ ] **Step 3: Commit**

```bash
git add src/ClientLibs/c/offs_ofd_resolver.c
git commit -m "feat: add offs_ofd_resolver.c with OFD resolution and donor algorithm"
```

---

### Task 4: C Library — Unit Tests

**Files:**
- Create: `src/ClientLibs/c/test_offs_ofd_resolver.c`

- [ ] **Step 1: Write the tests**

```c
#include "offs_ofd_resolver.h"
#include "../../OFFStreams/ofd.h"
#include "../../OFFStreams/ori.h"
#include "../../Buffer/buffer.h"
#include "../../OFFStreams/off_url.h"
#include "../../Util/base58.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* ---- total_blocks tests ---- */

static void test_total_blocks_small_file(void) {
  TEST("total_blocks small file (2KB, standard)");
  size_t blocks = offs_ofd_total_blocks(2048, (int)standard, 3);
  /* data_blocks = ceil(2048/128000) = 1
     desc_blocks = ceil(1*32*3 / 127968) = ceil(96/127968) = 1 */
  assert(blocks == 2);
  PASS();
}

static void test_total_blocks_zero_byte(void) {
  TEST("total_blocks zero-byte file");
  size_t blocks = offs_ofd_total_blocks(0, (int)standard, 3);
  /* data_blocks = 0, desc_blocks = 0 */
  assert(blocks == 0);
  PASS();
}

static void test_total_blocks_large_file(void) {
  TEST("total_blocks large file (10MB, standard)");
  /* data_blocks = ceil(10000000/128000) = 79
     desc_bytes = 79 * 32 * 3 = 7584
     desc_blocks = ceil(7584 / 127968) = 1 */
  size_t blocks = offs_ofd_total_blocks(10000000, (int)standard, 3);
  assert(blocks == 80);
  PASS();
}

/* ---- build_recyclers tests ---- */

static void test_build_recyclers_exact_match(void) {
  TEST("build_recyclers exact match (no donors consumed)");
  /* File size == matched final_byte → no donors needed */
  uint8_t hash[32] = {1};
  offs_ofd_donor_t matched = {
    .name = "test.js", .url = "http://h/off/v3/app/js/100/h1/h2/test.js",
    .final_byte = 1000, .block_type = (int)standard, .tuple_size = 3,
    .file_hash = hash, .hash_len = 32
  };

  size_t out_count = 0;
  char** result = offs_ofd_build_recyclers(NULL, 0, &matched, 1000, NULL, 0, &out_count);
  assert(result != NULL);
  assert(out_count == 1);
  assert(strcmp(result[0], matched.url) == 0);
  free(result[0]);
  free(result);
  PASS();
}

static void test_build_recyclers_file_grew(void) {
  TEST("build_recyclers file grew (needs donors)");
  uint8_t hash1[32] = {1};
  uint8_t hash2[32] = {2};
  uint8_t hash3[32] = {3};

  offs_ofd_donor_t matched = {
    .name = "main.js", .url = "http://h/off/v3/app/js/2048/h1/h2/main.js",
    .final_byte = 2048, .block_type = (int)standard, .tuple_size = 3,
    .file_hash = hash1, .hash_len = 32
  };
  /* matched covers 2 blocks (data=1 + desc=1) */

  offs_ofd_donor_t donors[2] = {
    { .name = "old.js", .url = "http://h/off/v3/app/js/128000/h3/h4/old.js",
      .final_byte = 128000, .block_type = (int)standard, .tuple_size = 3,
      .file_hash = hash2, .hash_len = 32 },
    { .name = "lib.js", .url = "http://h/off/v3/app/js/256000/h5/h6/lib.js",
      .final_byte = 256000, .block_type = (int)standard, .tuple_size = 3,
      .file_hash = hash3, .hash_len = 32 },
  };
  /* file_size = 4KB → blocks_needed = 2 (data=1 + desc=1)
     matched covers 2 → no donors actually needed here.
     Let's use a bigger file: 256KB → blocks_needed = 2+1=3
     matched covers 2 → shortfall = 1
     donor old.js (128KB) covers 2 blocks → greedy takes it */

  size_t out_count = 0;
  char** result = offs_ofd_build_recyclers(donors, 2, &matched, 256000, NULL, 0, &out_count);
  assert(result != NULL);
  assert(out_count == 2);  /* matched + 1 donor */
  assert(strcmp(result[0], matched.url) == 0);
  free(result[0]);
  free(result[1]);
  free(result);
  PASS();
}

static void test_build_recyclers_no_match(void) {
  TEST("build_recyclers no match (uses donor pool only)");
  uint8_t hash1[32] = {10};
  offs_ofd_donor_t donors[1] = {
    { .name = "other.js", .url = "http://h/off/v3/app/js/128000/h1/h2/other.js",
      .final_byte = 128000, .block_type = (int)standard, .tuple_size = 3,
      .file_hash = hash1, .hash_len = 32 }
  };
  const char* fallback[] = {"http://backup/off/v3/app/js/0/h3/h4/backup.ofd"};

  size_t out_count = 0;
  char** result = offs_ofd_build_recyclers(donors, 1, NULL, 50000,
                                            fallback, 1, &out_count);
  assert(result != NULL);
  assert(out_count == 2);  /* 1 donor + 1 fallback */
  free(result[0]);
  free(result[1]);
  free(result);
  PASS();
}

static void test_build_recyclers_fallback_only(void) {
  TEST("build_recyclers null donors (fallback only)");
  const char* fallback[] = {"http://fb/off/v3/app/js/0/h1/h2/backup.ofd"};

  size_t out_count = 0;
  char** result = offs_ofd_build_recyclers(NULL, 0, NULL, 50000,
                                            fallback, 1, &out_count);
  assert(result != NULL);
  assert(out_count == 1);
  assert(strcmp(result[0], fallback[0]) == 0);
  free(result[0]);
  free(result);
  PASS();
}

/* ---- main ---- */

int main(void) {
  printf("C OFD Resolver Tests:\n");

  test_total_blocks_small_file();
  test_total_blocks_zero_byte();
  test_total_blocks_large_file();
  test_build_recyclers_exact_match();
  test_build_recyclers_file_grew();
  test_build_recyclers_no_match();
  test_build_recyclers_fallback_only();

  printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
```

- [ ] **Step 2: Verify compilation and run tests**

Run: `cd build && cmake .. && make test_offs_ofd_resolver && ./test_offs_ofd_resolver`
Expected: all 7 tests pass.

- [ ] **Step 3: Run valgrind**

Run: `valgrind --leak-check=full --error-exitcode=1 ./test_offs_ofd_resolver`
Expected: no leaks detected (pre-existing leaks from the reference list are acceptable).

- [ ] **Step 4: Commit**

```bash
git add src/ClientLibs/c/test_offs_ofd_resolver.c
git commit -m "test: add C OFD resolver unit tests for block calc and donor algorithm"
```

---

### Task 5: Flutter — `fetchRawOfd()` in off_api.dart

**Files:**
- Modify: `examples/off_client/lib/services/off_api.dart`

- [ ] **Step 1: Add `fetchRawOfd()` method**

Add to the `OffApi` class, after the `downloadFile()` method (line 170):

```dart
/// Fetch raw OFD CBOR bytes from a URL with ?ofd=raw.
Future<Uint8List> fetchRawOfd(String url) async {
  final separator = url.contains('?') ? '&' : '?';
  final rawUrl = '$url${separator}ofd=raw';
  final response = await http.get(Uri.parse(rawUrl));
  if (response.statusCode == 200) {
    return response.bodyBytes;
  } else {
    throw Exception('OFD fetch failed: ${response.statusCode}');
  }
}
```

Add the `dart:typed_data` import for `Uint8List` at the top of the file (it's currently not imported):

```dart
import 'dart:typed_data';
```

- [ ] **Step 2: Verify compilation**

Run: `cd examples/off_client && flutter analyze`
Expected: no new errors.

- [ ] **Step 3: Commit**

```bash
git add examples/off_client/lib/services/off_api.dart
git commit -m "feat: add fetchRawOfd() to Flutter OffApi client"
```

---

### Task 6: Flutter — `recycler_resolver.dart`

**Files:**
- Create: `examples/off_client/lib/services/recycler_resolver.dart`

- [ ] **Step 1: Write the resolver**

```dart
import 'dart:typed_data';
import 'package:cbor/cbor.dart';
import 'off_api.dart';
import 'ofd.dart';
import 'base58.dart';

/// Result from resolving an OFD recycler URL.
class ResolvedOfd {
  final String serverBase;
  final List<OfdEntry> entries;
  const ResolvedOfd({required this.serverBase, required this.entries});
}

/// Resolves OFD recycler URLs into per-file recycler lists.
class RecyclerResolver {
  final OffApi _api;

  RecyclerResolver(this._api);

  /// Fetch and parse an OFD, returning all file entries with the server base.
  /// Recursively resolves sub-directory OFDs.
  Future<ResolvedOfd> resolveOfd(String ofdUrl) async {
    final donorPool = <OfdEntry>[];
    final rawBytes = await _api.fetchRawOfd(ofdUrl);
    final entries = parseOfdCbor(rawBytes);

    // Extract server base from the OFD URL
    final uri = Uri.parse(ofdUrl);
    final serverBase = '${uri.scheme}://${uri.host}${uri.hasPort ? ':${uri.port}' : ''}';

    await _collectDonors(entries, serverBase, donorPool);
    return ResolvedOfd(serverBase: serverBase, entries: donorPool);
  }

  /// Recursively collect file ORIs from OFD entries into the donor pool.
  Future<void> _collectDonors(
    List<OfdEntry> entries,
    String serverBase,
    List<OfdEntry> donorPool,
  ) async {
    for (final entry in entries) {
      if (entry.isDirectory) {
        // Build sub-OFD URL and recurse
        if (entry.dirHash == null) continue;
        final dirHashB58 = base58Encode(entry.dirHash!);
        final subUrl = '$serverBase/offsystem/v3/offsystem/directory/0/'
            '$dirHashB58/$dirHashB58/${Uri.encodeComponent(entry.name)}.ofd';

        try {
          final subBytes = await _api.fetchRawOfd(subUrl);
          final subEntries = parseOfdCbor(subBytes);
          await _collectDonors(subEntries, serverBase, donorPool);
        } catch (_) {
          // Sub-OFD fetch failed — skip this subtree
        }
      } else {
        // File entry: add to donor pool
        if (entry.fileHash != null && entry.descriptorHash != null) {
          donorPool.add(entry);
        }
      }
    }
  }

  /// Calculate total blocks (data + descriptor) for a file size.
  static int totalBlocks(int finalByte, {int blockType = 128000, int tupleSize = 3, int descriptorPad = 32}) {
    if (finalByte == 0) return 0;
    final dataBlocks = (finalByte + blockType - 1) ~/ blockType; // ceil division
    final cutPoint = (blockType ~/ descriptorPad) * descriptorPad;
    final descDataPerBlock = cutPoint - descriptorPad;
    final descBytes = dataBlocks * descriptorPad * tupleSize;
    final descBlocks = (descBytes + descDataPerBlock - 1) ~/ descDataPerBlock;
    return dataBlocks + descBlocks;
  }

  /// Get the block count for an OfdEntry.
  static int entryBlocks(OfdEntry entry) {
    return totalBlocks(
      entry.finalByte ?? 0,
      blockType: entry.blockType ?? 128000,
      tupleSize: entry.tupleSize ?? 3,
    );
  }

  /// Build a per-file recycler URL list.
  ///
  /// [matched] is the OFD entry matching this file by name, or null.
  /// [fileSize] is the size of the local file being uploaded.
  /// [donorPool] contains all available donor entries from the OFD tree.
  /// [fallback] contains original non-OFD recycler URLs.
  /// [serverBase] is the base URL of the server hosting the OFD, e.g. "http://host:23402".
  static List<String> buildRecyclerList({
    required OfdEntry? matched,
    required int fileSize,
    required List<OfdEntry> donorPool,
    required List<String> fallback,
    required String serverBase,
  }) {
    final result = <String>[];
    final blockType = matched?.blockType ?? 128000;
    final tupleSize = matched?.tupleSize ?? 3;
    final blocksNeeded = totalBlocks(fileSize, blockType: blockType, tupleSize: tupleSize);
    int blocksCovered = 0;

    // 1. Matched ORI
    if (matched != null) {
      result.add(_entryToUrl(matched, serverBase));
      blocksCovered += entryBlocks(matched);
    }

    // 2. Greedy donors sorted by block count descending
    if (blocksCovered < blocksNeeded && donorPool.isNotEmpty) {
      final sorted = List<OfdEntry>.from(donorPool)
        ..sort((a, b) => entryBlocks(b).compareTo(entryBlocks(a)));

      for (final donor in sorted) {
        if (blocksCovered >= blocksNeeded) break;
        // Skip self
        if (matched != null && _sameHash(donor.fileHash, matched.fileHash)) continue;
        result.add(_entryToUrl(donor, serverBase));
        blocksCovered += entryBlocks(donor);
      }
    }

    // 3. Fallback recyclers
    result.addAll(fallback);
    return result;
  }

  /// Convert an OfdEntry to a full ORI URL string.
  static String _entryToUrl(OfdEntry entry, String serverBase) {
    final fileHashB58 = base58Encode(entry.fileHash!);
    final descHashB58 = base58Encode(entry.descriptorHash!);
    final contentType = mimeFromExtension(entry.name);
    return '$serverBase/offsystem/v3/$contentType/${entry.finalByte}/'
        '$fileHashB58/$descHashB58/${Uri.encodeComponent(entry.name)}';
  }

  static bool _sameHash(Uint8List? a, Uint8List? b) {
    if (a == null || b == null) return false;
    if (a.length != b.length) return false;
    for (int i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd examples/off_client && flutter analyze`
Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add examples/off_client/lib/services/recycler_resolver.dart
git commit -m "feat: add RecyclerResolver for OFD-aware per-file recycler lists"
```

---

### Task 7: Flutter — Unit Tests

**Files:**
- Create: `examples/off_client/test/recycler_resolver_test.dart`

- [ ] **Step 1: Write the tests**

```dart
import 'dart:typed_data';
import 'package:flutter_test/flutter_test.dart';
import 'package:off_client/services/recycler_resolver.dart';
import 'package:off_client/services/ofd.dart';

const serverBase = 'http://example.com:23402';

void main() {
  group('totalBlocks', () {
    test('small file (2KB, standard)', () {
      final blocks = RecyclerResolver.totalBlocks(2048);
      // data=1 + desc=1 = 2
      expect(blocks, 2);
    });

    test('zero byte file', () {
      final blocks = RecyclerResolver.totalBlocks(0);
      expect(blocks, 0);
    });

    test('large file (10MB, standard)', () {
      // data=ceil(10M/128K)=79, desc=ceil(79*96/127968)=1 → 80
      final blocks = RecyclerResolver.totalBlocks(10000000);
      expect(blocks, 80);
    });
  });

  group('buildRecyclerList', () {
    final hash1 = Uint8List(32)..[0] = 1;
    final hash2 = Uint8List(32)..[0] = 2;
    final hash3 = Uint8List(32)..[0] = 3;
    final descHash = Uint8List(32)..[0] = 0xFF;

    test('exact match — no donors consumed', () {
      final matched = OfdEntry.file(
        name: 'test.js', finalByte: 2048, fileHash: hash1, descriptorHash: descHash,
      );
      final result = RecyclerResolver.buildRecyclerList(
        matched: matched, fileSize: 2048, donorPool: [], fallback: [],
        serverBase: serverBase,
      );
      expect(result.length, 1);
      expect(result[0], contains(serverBase));
      expect(result[0], contains('/2048/'));
    });

    test('file grew — donors consumed', () {
      final matched = OfdEntry.file(
        name: 'main.js', finalByte: 2048, fileHash: hash1, descriptorHash: descHash,
      );
      final donors = [
        OfdEntry.file(name: 'old.js', finalByte: 128000, fileHash: hash2, descriptorHash: descHash),
        OfdEntry.file(name: 'lib.js', finalByte: 256000, fileHash: hash3, descriptorHash: descHash),
      ];
      // 256KB file → blocks_needed = 2+1=3, matched covers 2, need 1 more
      final result = RecyclerResolver.buildRecyclerList(
        matched: matched, fileSize: 256000, donorPool: donors, fallback: [],
        serverBase: serverBase,
      );
      expect(result.length, 2); // matched + 1 donor
      expect(result[0], contains('/2048/')); // matched is first
    });

    test('no match — uses donor pool', () {
      final donors = [
        OfdEntry.file(name: 'other.js', finalByte: 128000, fileHash: hash2, descriptorHash: descHash),
      ];
      final fallback = ['http://fallback/ofd'];
      final result = RecyclerResolver.buildRecyclerList(
        matched: null, fileSize: 50000, donorPool: donors, fallback: fallback,
        serverBase: serverBase,
      );
      expect(result.length, 2); // donor + fallback
      expect(result.last, 'http://fallback/ofd');
    });

    test('no donors — fallback only', () {
      final fallback = ['http://fb/ofd'];
      final result = RecyclerResolver.buildRecyclerList(
        matched: null, fileSize: 50000, donorPool: [], fallback: fallback,
        serverBase: serverBase,
      );
      expect(result.length, 1);
      expect(result[0], 'http://fb/ofd');
    });

    test('self-skip — matched donor not duplicated', () {
      final matched = OfdEntry.file(
        name: 'app.js', finalByte: 2048, fileHash: hash1, descriptorHash: descHash,
      );
      // Donor pool contains the same entry as the matched
      final donors = [
        OfdEntry.file(name: 'app.js', finalByte: 2048, fileHash: hash1, descriptorHash: descHash),
      ];
      final result = RecyclerResolver.buildRecyclerList(
        matched: matched, fileSize: 256000, donorPool: donors, fallback: [],
        serverBase: serverBase,
      );
      // matched + 0 donors (the one donor is self, skipped)
      // matched covers 2 blocks, needs 3 → shortfall, but no usable donors
      expect(result.length, 1);
    });
  });
}
```

- [ ] **Step 2: Run the tests**

Run: `cd examples/off_client && flutter test test/recycler_resolver_test.dart`
Expected: all 6 tests pass.

- [ ] **Step 3: Commit**

```bash
git add examples/off_client/test/recycler_resolver_test.dart
git commit -m "test: add RecyclerResolver unit tests for donor algorithm"
```

---

### Task 8: Flutter — Integrate into `import_screen.dart`

**Files:**
- Modify: `examples/off_client/lib/screens/import_screen.dart`

- [ ] **Step 1: Add imports**

Add to the existing imports (after line 9):

```dart
import '../services/recycler_resolver.dart';
```

- [ ] **Step 2: Add helper methods to `_ImportScreenState`**

After the `_recyclerController` field (line 27), no new fields needed. Add these methods after `_upload()` (line 73):

```dart
  /// Check if a URL is an OFD (off file descriptor) by URL pattern.
  static bool _isOfdUrl(String url) {
    return url.contains('offsystem/directory') || url.endsWith('.ofd');
  }

  /// Check if an OFD URL's filename matches a directory name.
  static bool _ofdMatchesDir(String url, String dirName) {
    final uri = Uri.parse(url);
    final segments = uri.pathSegments;
    if (segments.isEmpty) return false;
    final filename = segments.last;
    return filename == '$dirName.ofd' || filename == dirName;
  }

  /// Filter recycler URLs to those that are NOT OFDs.
  List<String> _nonOfdRecyclers() {
    if (_recyclerUrls == null) return [];
    return _recyclerUrls!.where((u) => !_isOfdUrl(u)).toList();
  }
```

- [ ] **Step 3: Modify `_uploadDirectory()`**

Replace the existing `_uploadDirectory` method (lines 75-137) with the enhanced version:

```dart
  /// Upload a directory recursively and return the URL of its top-level OFD.
  Future<String> _uploadDirectory(Directory dir, {List<OfdEntry>? parentDonorPool}) async {
    final entries = <OfdEntry>[];
    final dirEntries = dir.listSync();

    final files = dirEntries.whereType<File>().toList();
    final subdirs = dirEntries.whereType<Directory>().toList();
    final dirName = dir.path.split(Platform.pathSeparator).last;

    // Build donor pool: start from parent, add from matching OFD recyclers
    final donorPool = <OfdEntry>[...?parentDonorPool];
    final ofdEntryMap = <String, OfdEntry>{};
    String? recyclerServerBase;

    if (_recyclerUrls != null) {
      final resolver = RecyclerResolver(_api);
      for (final url in _recyclerUrls!) {
        if (_isOfdUrl(url) && _ofdMatchesDir(url, dirName)) {
          try {
            final resolved = await resolver.resolveOfd(url);
            recyclerServerBase = resolved.serverBase;
            for (final entry in resolved.entries) {
              donorPool.add(entry);
              ofdEntryMap[entry.name] = entry;
            }
          } catch (_) {
            // OFD resolution failed — continue without it
          }
        }
      }
    }

    final fallbackRecyclers = _nonOfdRecyclers();

    // Process subdirectories first: each gets its own OFD uploaded
    for (final subdir in subdirs) {
      final subDirName = subdir.path.split(Platform.pathSeparator).last;
      final subUrl = await _uploadDirectory(subdir, parentDonorPool: donorPool);
      final parsed = parseOffUrl(subUrl);
      if (parsed == null) {
        throw Exception('Failed to parse sub-OFD URL: $subUrl');
      }
      final dirHash = Uint8List.fromList(base58Decode(parsed.fileHashB58)!);
      entries.add(OfdEntry.directory(name: subDirName, dirHash: dirHash));
    }

    // Upload files in this directory with per-file recycler lists
    for (final file in files) {
      final fileName = file.path.split(Platform.pathSeparator).last;
      final length = await file.length();

      // Build per-file recycler list from OFD entries
      final matched = ofdEntryMap[fileName];
      final fileRecyclers = RecyclerResolver.buildRecyclerList(
        matched: matched,
        fileSize: length,
        donorPool: donorPool,
        fallback: fallbackRecyclers,
        serverBase: recyclerServerBase ?? '',
      );

      final url = await _api.uploadFile(
        fileName: fileName,
        streamLength: length,
        filePath: file.path,
        recyclerUrls: fileRecyclers.isNotEmpty ? fileRecyclers : null,
      );

      final parsed = parseOffUrl(url);
      if (parsed == null) {
        throw Exception('Failed to parse upload URL: $url');
      }

      final fileHash = Uint8List.fromList(base58Decode(parsed.fileHashB58)!);
      final descHash = Uint8List.fromList(base58Decode(parsed.descriptorHashB58)!);

      entries.add(OfdEntry.file(
        name: fileName,
        fileHash: fileHash,
        descriptorHash: descHash,
        finalByte: parsed.streamLength,
      ));
    }

    if (entries.isEmpty) {
      throw Exception('Empty directory: ${dir.path}');
    }

    // Build and upload this directory's OFD
    final ofdBytes = buildOfdCbor(entries);
    return _api.uploadFileBuffered(
      fileName: '$dirName.ofd',
      streamLength: ofdBytes.length,
      contentType: 'offsystem/directory',
      bodyBytes: ofdBytes,
      recyclerUrls: _recyclerUrls,
    );
  }
```

- [ ] **Step 4: Verify compilation**

Run: `cd examples/off_client && flutter analyze`
Expected: no errors.

- [ ] **Step 5: Manual smoke test**

Run the app and test:
1. Upload a directory with no recycler URLs → works as before
2. Upload a directory modified, with a previous OFD as recycler → upload succeeds

- [ ] **Step 6: Commit**

```bash
git add examples/off_client/lib/screens/import_screen.dart
git commit -m "feat: integrate OFD-aware recycler resolution into directory upload"
```

---

### Task 9: CMakeLists — Add test target

**Files:**
- Modify: `src/ClientLibs/c/CMakeLists.txt`

- [ ] **Step 1: Add test executable target**

Add after the existing library/test targets:

```cmake
add_executable(test_offs_ofd_resolver test_offs_ofd_resolver.c offs_ofd_resolver.c offs_client.c)
target_link_libraries(test_offs_ofd_resolver ${LIBCBOR_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(test_offs_ofd_resolver PRIVATE ${LIBCBOR_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
```

(Exact library names depend on existing CMake configuration — adjust to match.)

- [ ] **Step 2: Verify build**

Run: `cd build && cmake .. && make test_offs_ofd_resolver`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/ClientLibs/c/CMakeLists.txt
git commit -m "build: add test_offs_ofd_resolver target"
```
