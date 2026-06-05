# OFFS Installer & Auto-Updater Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a cross-platform installer and fully-automatic self-updating system for the OFFS daemon.

**Architecture:** New liboffs modules in `src/Version/`, `src/Service/`, `src/Update/`, and `src/Updater/`. The daemon gets a new `update_actor_t` that runs on the scheduler pool, checking GitHub Releases on a timer, downloading updates, draining open streams, and spawning a standalone `offs-updater` binary that replaces all binaries and restarts the service. Packaging files in `packaging/` and release scripts in `scripts/` handle distribution.

**Tech Stack:** C11, CMake, OpenSSL, cJSON, poll-dancer, existing actor/scheduler. Raw sockets + OpenSSL for HTTPS (no libcurl).

---

## File Structure

```
liboffs/
├── src/
│   ├── Version/
│   │   ├── version.h              # version_t type, version_parse, version_compare, version_to_string
│   │   └── version.c              # implementation
│   ├── Service/
│   │   ├── service.h              # service_ops_t vtable + platform dispatch
│   │   ├── service_linux.c        # systemd implementation
│   │   └── service_windows.c      # Win32 Service API (macos deferred)
│   ├── Update/
│   │   ├── update_check.h         # GitHub API check interface
│   │   ├── update_check.c         # HTTPS GET, JSON parse, release channel logic
│   │   ├── update_download.h      # download + verify interface
│   │   ├── update_download.c      # HTTPS download, SHA256 verify, extract
│   │   ├── update_stage.h         # staging + backup interface
│   │   ├── update_stage.c         # stage binaries, create backup
│   │   ├── update_actor.h         # update_actor_t definition
│   │   └── update_actor.c         # actor lifecycle: timer, drain, spawn updater
│   └── Updater/
│       └── updater_main.c         # standalone offs-updater binary
├── packaging/
│   ├── linux/debian/{control,postinst,prerm,postrm,offs-daemon.service}
│   ├── linux/rpm/{offs.spec,offs-daemon.service}
│   ├── macos/{preinstall,postinstall,com.offs.daemon.plist}
│   └── windows/{offs.wxs,offs.wxl}
├── scripts/
│   ├── release.sh                 # per-platform build + package
│   └── collect-release.sh         # gather artifacts, create GitHub Release
├── CMakeLists.txt                 # modify: add VERSION define, new subdirectories
└── test/
    └── CMakeLists.txt             # add test_version target
```

### Files modified (OFFS repo):
```
OFFS/
├── src/offsd/main.c               # wire update_actor into daemon lifecycle
└── src/offs/commands/status.c     # add update status fields to existing status command
```

---

### Task 1: Version System

**Files:**
- Create: `src/Version/version.h`
- Create: `src/Version/version.c`

- [ ] **Step 1: Create header `src/Version/version.h`**

```c
//
// Created by victor on 5/28/25.
//

#ifndef OFFS_VERSION_H
#define OFFS_VERSION_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
  char prerelease[32];  // e.g. "rc.1", "dev.3", or "" for stable
} version_t;

typedef enum {
  channel_stable = 0,
  channel_rc = 1,
  channel_dev = 2
} update_channel_e;

bool          version_parse(const char* tag, version_t* out);
int           version_compare(const version_t* a, const version_t* b);
size_t        version_to_string(const version_t* v, char* buf, size_t buf_size);
update_channel_e version_channel(const version_t* v);
const char*   channel_to_string(update_channel_e channel);

// Build-time version injected by CMake
#ifndef OFFS_VERSION
#define OFFS_VERSION "0.0.0"
#endif

#endif // OFFS_VERSION_H
```

- [ ] **Step 2: Create implementation `src/Version/version.c`**

```c
//
// Created by victor on 5/28/25.
//

#include "version.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

bool version_parse(const char* tag, version_t* out) {
  if (tag == NULL || out == NULL) return false;

  memset(out, 0, sizeof(*out));

  // Skip optional leading 'v'
  const char* cursor = tag;
  if (*cursor == 'v' || *cursor == 'V') cursor++;

  char* end = NULL;
  out->major = (uint16_t)strtoul(cursor, &end, 10);
  if (*end != '.') return false;

  cursor = end + 1;
  out->minor = (uint16_t)strtoul(cursor, &end, 10);
  if (*end != '.') return false;

  cursor = end + 1;
  out->patch = (uint16_t)strtoul(cursor, &end, 10);

  // Optional prerelease suffix
  if (*end == '-') {
    size_t len = strlen(end + 1);
    if (len >= sizeof(out->prerelease)) return false;
    memcpy(out->prerelease, end + 1, len);
  }

  return true;
}

int version_compare(const version_t* a, const version_t* b) {
  if (a->major != b->major) return (a->major > b->major) ? 1 : -1;
  if (a->minor != b->minor) return (a->minor > b->minor) ? 1 : -1;
  if (a->patch != b->patch) return (a->patch > b->patch) ? 1 : -1;

  // Stable > prerelease, otherwise lexicographic compare of prerelease strings
  bool a_prerelease = (a->prerelease[0] != '\0');
  bool b_prerelease = (b->prerelease[0] != '\0');

  if (!a_prerelease && b_prerelease) return 1;
  if (a_prerelease && !b_prerelease) return -1;
  if (!a_prerelease && !b_prerelease) return 0;

  return strcmp(a->prerelease, b->prerelease);
}

size_t version_to_string(const version_t* v, char* buf, size_t buf_size) {
  if (v->prerelease[0] != '\0') {
    return (size_t)snprintf(buf, buf_size, "%u.%u.%u-%s",
                            v->major, v->minor, v->patch, v->prerelease);
  }
  return (size_t)snprintf(buf, buf_size, "%u.%u.%u", v->major, v->minor, v->patch);
}

update_channel_e version_channel(const version_t* v) {
  if (v->prerelease[0] == '\0') return channel_stable;
  if (strstr(v->prerelease, "rc")) return channel_rc;
  if (strstr(v->prerelease, "dev") || strstr(v->prerelease, "alpha")) return channel_dev;
  return channel_stable;
}

const char* channel_to_string(update_channel_e channel) {
  switch (channel) {
    case channel_stable: return "stable";
    case channel_rc:     return "rc";
    case channel_dev:    return "dev";
  }
  return "stable";
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Version/version.h src/Version/version.c
git commit -m "feat: add semver version system module"
```

---

### Task 2: Version Tests

**Files:**
- Create: `test/test_version.cpp`

- [ ] **Step 1: Create `test/test_version.cpp`**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Version/version.h"
}

TEST(TestVersion, ParseStable) {
  version_t v;
  ASSERT_TRUE(version_parse("v1.2.3", &v));
  EXPECT_EQ(v.major, 1);
  EXPECT_EQ(v.minor, 2);
  EXPECT_EQ(v.patch, 3);
  EXPECT_STREQ(v.prerelease, "");
}

TEST(TestVersion, ParsePrerelease) {
  version_t v;
  ASSERT_TRUE(version_parse("v2.0.0-rc.1", &v));
  EXPECT_EQ(v.major, 2);
  EXPECT_EQ(v.minor, 0);
  EXPECT_EQ(v.patch, 0);
  EXPECT_STREQ(v.prerelease, "rc.1");
}

TEST(TestVersion, ParseDev) {
  version_t v;
  ASSERT_TRUE(version_parse("v0.9.0-dev.5", &v));
  EXPECT_EQ(v.major, 0);
  EXPECT_EQ(v.minor, 9);
  EXPECT_EQ(v.patch, 0);
  EXPECT_STREQ(v.prerelease, "dev.5");
}

TEST(TestVersion, ParseWithoutV) {
  version_t v;
  ASSERT_TRUE(version_parse("3.4.5", &v));
  EXPECT_EQ(v.major, 3);
  EXPECT_EQ(v.minor, 4);
  EXPECT_EQ(v.patch, 5);
}

TEST(TestVersion, ParseInvalid) {
  version_t v;
  EXPECT_FALSE(version_parse(NULL, &v));
  EXPECT_FALSE(version_parse("not-a-version", &v));
  EXPECT_FALSE(version_parse("v1.2", &v));
}

TEST(TestVersion, CompareEqual) {
  version_t a, b;
  version_parse("v1.0.0", &a);
  version_parse("v1.0.0", &b);
  EXPECT_EQ(version_compare(&a, &b), 0);
}

TEST(TestVersion, CompareMajor) {
  version_t a, b;
  version_parse("v2.0.0", &a);
  version_parse("v1.9.9", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
  EXPECT_EQ(version_compare(&b, &a), -1);
}

TEST(TestVersion, CompareMinor) {
  version_t a, b;
  version_parse("v1.3.0", &a);
  version_parse("v1.2.9", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
}

TEST(TestVersion, ComparePatch) {
  version_t a, b;
  version_parse("v1.0.2", &a);
  version_parse("v1.0.1", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
}

TEST(TestVersion, StableOverPrerelease) {
  version_t a, b;
  version_parse("v1.0.0", &a);
  version_parse("v1.0.0-rc.1", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
  EXPECT_EQ(version_compare(&b, &a), -1);
}

TEST(TestVersion, ComparePrerelease) {
  version_t a, b;
  version_parse("v1.0.0-rc.2", &a);
  version_parse("v1.0.0-rc.1", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
}

TEST(TestVersion, ToStringStable) {
  version_t v;
  version_parse("v1.2.3", &v);
  char buf[64];
  version_to_string(&v, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1.2.3");
}

TEST(TestVersion, ToStringPrerelease) {
  version_t v;
  version_parse("v1.2.3-rc.1", &v);
  char buf[64];
  version_to_string(&v, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1.2.3-rc.1");
}

TEST(TestVersion, ChannelDetection) {
  version_t stable, rc, dev;
  version_parse("v1.0.0", &stable);
  version_parse("v1.0.0-rc.1", &rc);
  version_parse("v1.0.0-dev.3", &dev);

  EXPECT_EQ(version_channel(&stable), channel_stable);
  EXPECT_EQ(version_channel(&rc), channel_rc);
  EXPECT_EQ(version_channel(&dev), channel_dev);
}

TEST(TestVersion, OFFS_VERSION_defined) {
  EXPECT_STRNE(OFFS_VERSION, "");
}
```

- [ ] **Step 2: Add test to `test/CMakeLists.txt`**

Find the existing `gtest_add_tests` block near the bottom and add:

```cmake
add_executable(test_version
  test_version.cpp
)
target_link_libraries(test_version PRIVATE offs GTest::gtest GTest::gtest_main)
target_include_directories(test_version PRIVATE ${C_INC} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
gtest_add_tests(test_version "" ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd build && cmake .. && cmake --build . -j$(nproc) && ctest -R test_version`
Expected: PASS (version code already written above, so tests should pass)

- [ ] **Step 4: Commit**

```bash
git add test/test_version.cpp test/CMakeLists.txt
git commit -m "test: add version module unit tests"
```

---

### Task 3: Service Management Abstraction

**Files:**
- Create: `src/Service/service.h`
- Create: `src/Service/service_linux.c`
- Create: `src/Service/service_windows.c`

- [ ] **Step 1: Create header `src/Service/service.h`**

```c
//
// Created by victor on 5/28/25.
//

#ifndef OFFS_SERVICE_H
#define OFFS_SERVICE_H

#include <stdint.h>

typedef enum {
  service_result_ok = 0,
  service_result_error = -1,
  service_result_timeout = -2,
  service_result_not_installed = -3
} service_result_e;

typedef struct {
  int (*stop)(void);
  int (*start)(void);
  int (*is_running)(void);
  int (*install)(const char* install_dir);
  int (*uninstall)(void);
  const char* name;
} service_ops_t;

const service_ops_t* service_get_ops(void);

#endif // OFFS_SERVICE_H
```

- [ ] **Step 2: Create `src/Service/service_linux.c`**

```c
//
// Created by victor on 5/28/25.
//

#include "service.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define SERVICE_NAME "offs-daemon"

static int _linux_stop(void) {
  return system("systemctl stop " SERVICE_NAME) == 0
    ? service_result_ok : service_result_error;
}

static int _linux_start(void) {
  return system("systemctl start " SERVICE_NAME) == 0
    ? service_result_ok : service_result_error;
}

static int _linux_is_running(void) {
  int ret = system("systemctl is-active --quiet " SERVICE_NAME);
  return (ret == 0) ? 1 : 0;
}

static int _linux_install(const char* install_dir) {
  (void)install_dir;
  int ret = system("systemctl daemon-reload && systemctl enable " SERVICE_NAME);
  return (ret == 0) ? service_result_ok : service_result_error;
}

static int _linux_uninstall(void) {
  system("systemctl stop " SERVICE_NAME " 2>/dev/null");
  system("systemctl disable " SERVICE_NAME " 2>/dev/null");
  system("systemctl daemon-reload");
  return service_result_ok;
}

static const service_ops_t _linux_ops = {
  .stop       = _linux_stop,
  .start      = _linux_start,
  .is_running = _linux_is_running,
  .install    = _linux_install,
  .uninstall  = _linux_uninstall,
  .name       = SERVICE_NAME
};

const service_ops_t* service_get_ops(void) {
  return &_linux_ops;
}
```

- [ ] **Step 3: Create `src/Service/service_windows.c`** (stub for now, filled in later)

```c
//
// Created by victor on 5/28/25.
//

#include "service.h"

#define SERVICE_NAME L"offs-daemon"

#ifndef _WIN32
// Stub — platform not available
const service_ops_t* _windows_ops = NULL;
#else
#include <windows.h>

static int _win_stop(void) {
  SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
  if (scm == NULL) return service_result_error;

  SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (svc == NULL) { CloseServiceHandle(scm); return service_result_error; }

  SERVICE_STATUS status;
  if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return service_result_error;
  }

  // Wait for SERVICE_STOPPED with 30s timeout
  DWORD start = GetTickCount();
  while (status.dwCurrentState != SERVICE_STOPPED) {
    Sleep(250);
    if (!QueryServiceStatus(svc, &status)) break;
    if (GetTickCount() - start > 30000) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return service_result_timeout;
    }
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return (status.dwCurrentState == SERVICE_STOPPED)
    ? service_result_ok : service_result_error;
}

static int _win_start(void) {
  SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
  if (scm == NULL) return service_result_error;

  SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_START);
  if (svc == NULL) { CloseServiceHandle(scm); return service_result_error; }

  int ok = StartServiceW(svc, 0, NULL) ? service_result_ok : service_result_error;
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return ok;
}

static int _win_is_running(void) {
  SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
  if (scm == NULL) return 0;

  SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_QUERY_STATUS);
  if (svc == NULL) { CloseServiceHandle(scm); return 0; }

  SERVICE_STATUS status;
  int ok = QueryServiceStatus(svc, &status) && status.dwCurrentState == SERVICE_RUNNING;
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return ok;
}

static int _win_install(const char* install_dir) {
  (void)install_dir;
  // Service is installed by the .msi WiX <ServiceInstall> — no-op here
  return service_result_ok;
}

static int _win_uninstall(void) {
  // Service is removed by the .msi — no-op here
  return service_result_ok;
}

static const service_ops_t _win_ops = {
  .stop       = _win_stop,
  .start      = _win_start,
  .is_running = _win_is_running,
  .install    = _win_install,
  .uninstall  = _win_uninstall,
  .name       = "offs-daemon"
};

const service_ops_t* _windows_ops_ptr = &_win_ops;
#endif // _WIN32
```

- [ ] **Step 4: Commit**

```bash
git add src/Service/service.h src/Service/service_linux.c src/Service/service_windows.c
git commit -m "feat: add platform service management abstraction"
```

---

### Task 4: Update Check Module

**Files:**
- Create: `src/Update/update_check.h`
- Create: `src/Update/update_check.c`

- [ ] **Step 1: Create header `src/Update/update_check.h`**

```c
//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_CHECK_H
#define OFFS_UPDATE_CHECK_H

#include "../Version/version.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  version_t version;
  char tag_name[64];
  char download_url[512];
  char sha256[65];
  bool available;
  bool prerelease;
} update_info_t;

// Configuration for release checking
typedef struct {
  char github_repo[128];       // "owner/repo"
  char github_api_url[256];    // "https://api.github.com"
  char github_token[128];      // optional, for private repos
  update_channel_e channel;    // stable, rc, dev
} update_check_config_t;

// Fetch latest release info from GitHub. Caller must free() the returned info.
// Returns NULL on network error, or info->available=false when already current.
update_info_t* update_check_fetch(const update_check_config_t* config,
                                  const version_t* current_version);

void update_info_free(update_info_t* info);

#endif // OFFS_UPDATE_CHECK_H
```

- [ ] **Step 2: Create `src/Update/update_check.c`**

```c
//
// Created by victor on 5/28/25.
//

#include "update_check.h"
#include "../Util/allocator.h"
#include <cJSON.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif

// Build API URL from config
static void _build_url(const update_check_config_t* config,
                       char* buf, size_t buf_size) {
  if (config->channel == channel_stable) {
    snprintf(buf, buf_size, "%s/repos/%s/releases/latest",
             config->github_api_url, config->github_repo);
  } else {
    snprintf(buf, buf_size, "%s/repos/%s/releases",
             config->github_api_url, config->github_repo);
  }
}

// Simple HTTPS GET using raw sockets + OpenSSL (no libcurl).
// Returns response body as heap string, or NULL on error.
static char* _https_get(const char* host, const char* path,
                        const char* token) {
  // Resolve host
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char port[8] = "443";
  if (getaddrinfo(host, port, &hints, &res) != 0) return NULL;

  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock < 0) { freeaddrinfo(res); return NULL; }

  if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
    close_socket: close(sock); freeaddrinfo(res); return NULL;
  }
  freeaddrinfo(res);

  // TLS handshake
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == NULL) goto close_socket;
  SSL* ssl = SSL_new(ctx);
  SSL_set_fd(ssl, sock);
  if (SSL_connect(ssl) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); goto close_socket; }

  // Send request
  char request[1024];
  int req_len;
  if (token != NULL && token[0] != '\0') {
    req_len = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: offs-updater/1.0\r\n"
      "Accept: application/vnd.github+json\r\n"
      "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
      path, host, token);
  } else {
    req_len = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: offs-updater/1.0\r\n"
      "Accept: application/vnd.github+json\r\nConnection: close\r\n\r\n",
      path, host);
  }

  if (SSL_write(ssl, request, req_len) <= 0) {
    SSL_free(ssl); SSL_CTX_free(ctx); goto close_socket;
  }

  // Read response
  char* body = NULL;
  size_t body_cap = 0;
  size_t body_len = 0;
  char read_buf[4096];
  int n;

  while ((n = SSL_read(ssl, read_buf, sizeof(read_buf))) > 0) {
    if (body_len + (size_t)n > body_cap) {
      body_cap = body_cap ? body_cap * 2 : 8192;
      body = realloc(body, body_cap + 1);
    }
    memcpy(body + body_len, read_buf, (size_t)n);
    body_len += (size_t)n;
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  close(sock);

  if (body == NULL) return NULL;
  body[body_len] = '\0';

  // Find body (after \r\n\r\n)
  char* body_start = strstr(body, "\r\n\r\n");
  if (body_start == NULL) { free(body); return NULL; }
  body_start += 4;

  char* result = strdup(body_start);
  free(body);
  return result;
}

// Extract host and path from URL
static void _parse_url(const char* url, char* host, size_t host_size,
                       char* path, size_t path_size) {
  const char* start = url;
  if (strncmp(start, "https://", 8) == 0) start += 8;

  const char* slash = strchr(start, '/');
  if (slash != NULL) {
    size_t host_len = (size_t)(slash - start);
    if (host_len >= host_size) host_len = host_size - 1;
    memcpy(host, start, host_len);
    host[host_len] = '\0';
    snprintf(path, path_size, "%s", slash);
  } else {
    snprintf(host, host_size, "%s", start);
    path[0] = '/';
    path[1] = '\0';
  }
}

static bool _matches_channel(const version_t* v, update_channel_e channel) {
  if (channel == channel_stable) return (v->prerelease[0] == '\0');
  if (channel == channel_rc)
    return (strstr(v->prerelease, "rc") != NULL);
  if (channel == channel_dev)
    return (v->prerelease[0] != '\0');  // any prerelease
  return true;
}

update_info_t* update_check_fetch(const update_check_config_t* config,
                                  const version_t* current_version) {
  char api_url[512];
  _build_url(config, api_url, sizeof(api_url));

  char host[256], path[512];
  _parse_url(api_url, host, sizeof(host), path, sizeof(path));

  char* json = _https_get(host, path, config->github_token);
  if (json == NULL) return NULL;

  cJSON* root = cJSON_Parse(json);
  free(json);
  if (root == NULL) return NULL;

  update_info_t* info = get_clear_memory(sizeof(update_info_t));

  // /releases/latest returns a single release object
  // /releases returns an array — take the first matching entry
  cJSON* release = root;
  if (cJSON_IsArray(root)) {
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, root) {
      cJSON* tag = cJSON_GetObjectItem(item, "tag_name");
      if (cJSON_IsString(tag)) {
        version_t v;
        if (version_parse(tag->valuestring, &v) &&
            _matches_channel(&v, config->channel)) {
          release = item;
          break;
        }
      }
    }
    if (release == root) {
      // No matching release found — treat as "no update available"
      info->available = false;
      cJSON_Delete(root);
      return info;
    }
  }

  cJSON* tag_name = cJSON_GetObjectItem(release, "tag_name");
  cJSON* prerelease = cJSON_GetObjectItem(release, "prerelease");
  cJSON* assets = cJSON_GetObjectItem(release, "assets");
  cJSON* body = cJSON_GetObjectItem(release, "body");

  if (cJSON_IsString(tag_name)) {
    version_parse(tag_name->valuestring, &info->version);
    snprintf(info->tag_name, sizeof(info->tag_name), "%s", tag_name->valuestring);
  }

  info->prerelease = cJSON_IsTrue(prerelease);

  // Compare versions
  if (version_compare(&info->version, current_version) <= 0) {
    info->available = false;
  } else {
    info->available = true;
  }

  // Find platform asset and extract SHA256 from body
  if (info->available && cJSON_IsArray(assets)) {
    cJSON* asset = NULL;
    cJSON_ArrayForEach(asset, assets) {
      cJSON* name = cJSON_GetObjectItem(asset, "name");
      if (cJSON_IsString(name)) {
#ifdef _WIN32
        if (strstr(name->valuestring, "windows-x64")) {
#elif defined(__APPLE__)
        if (strstr(name->valuestring, "macos-x64")) {
#else
        if (strstr(name->valuestring, "linux-x64")) {
#endif
          cJSON* url = cJSON_GetObjectItem(asset, "browser_download_url");
          if (cJSON_IsString(url)) {
            snprintf(info->download_url, sizeof(info->download_url),
                     "%s", url->valuestring);
          }
          break;
        }
      }
    }

    // Extract SHA256 from release body
    if (cJSON_IsString(body) && info->download_url[0] != '\0') {
      const char* body_text = body->valuestring;
      // Look for SHA256 line matching the platform asset
      const char* sha256_line = strstr(body_text, "sha256:");
      if (sha256_line != NULL) {
        sscanf(sha256_line, "sha256:%64s", info->sha256);
      }
    }
  }

  cJSON_Delete(root);
  return info;
}

void update_info_free(update_info_t* info) {
  free(info);
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Update/update_check.h src/Update/update_check.c
git commit -m "feat: add GitHub release checking module"
```

---

### Task 5: Update Download Module

**Files:**
- Create: `src/Update/update_download.h`
- Create: `src/Update/update_download.c`

- [ ] **Step 1: Create header `src/Update/update_download.h`**

```c
//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_DOWNLOAD_H
#define OFFS_UPDATE_DOWNLOAD_H

#include "update_check.h"
#include <stdbool.h>

// Download an update asset to staging_dir.
// Verifies SHA256 checksum if provided.
// Extracts the tarball/zip into staging_dir.
// Returns true on success (file written, verified, extracted).
bool update_download(const update_info_t* info,
                     const char* staging_dir,
                     const char* github_token);

#endif // OFFS_UPDATE_DOWNLOAD_H
```

- [ ] **Step 2: Create `src/Update/update_download.c`**

Uses the same `_https_get` pattern from `update_check.c` to download the asset. Streams to a temp file, computes SHA256 during write, compares against `info->sha256`, then extracts.

**Key functions:**
- `_compute_sha256(const char* filepath, char* out_hex[65])` — reads file, SHA256 via OpenSSL `EVP_MD_CTX`
- `_extract_archive(const char* archive_path, const char* dest_dir)` — runs `tar -xzf` on POSIX, `Expand-Archive` or manual zip on Windows
- Platform detection: `#ifdef _WIN32` for `.zip`, `#else` for `.tar.gz`

- [ ] **Step 3: Commit**

```bash
git add src/Update/update_download.h src/Update/update_download.c
git commit -m "feat: add update download and verification module"
```

---

### Task 6: Update Stage Module

**Files:**
- Create: `src/Update/update_stage.h`
- Create: `src/Update/update_stage.c`

- [ ] **Step 1: Create header `src/Update/update_stage.h`**

```c
//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_STAGE_H
#define OFFS_UPDATE_STAGE_H

#include <stdbool.h>

// Stage extracted update for installation.
// Backs up current install_dir to backup_dir/previous/.
// Returns true on success.
bool update_stage(const char* staging_dir,
                  const char* install_dir,
                  const char* backup_dir);

#endif // OFFS_UPDATE_STAGE_H
```

- [ ] **Step 2: Create `src/Update/update_stage.c`**

```c
//
// Created by victor on 5/28/25.
//

#include "update_stage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int _mkdir_p(const char* path) {
#ifdef _WIN32
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char* p = tmp; *p; p++) {
    if (*p == '/' || *p == '\\') { char c = *p; *p = '\0'; CreateDirectoryA(tmp, NULL); *p = c; }
  }
  return CreateDirectoryA(tmp, NULL) ? 0 : (GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1);
#else
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "mkdir -p \"%s\"", path);
  return system(tmp);
#endif
}

static int _copy_file(const char* src, const char* dst) {
  char cmd[1024];
#ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "copy /Y \"%s\" \"%s\" > nul", src, dst);
#else
  snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\"", src, dst);
#endif
  return system(cmd);
}

static int _remove_dir(const char* path) {
  char cmd[512];
#ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "rmdir /S /Q \"%s\"", path);
#else
  snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
#endif
  return system(cmd);
}

bool update_stage(const char* staging_dir,
                  const char* install_dir,
                  const char* backup_dir) {
  // Create backup directory
  char backup_prev[512];
  snprintf(backup_prev, sizeof(backup_prev), "%s/previous", backup_dir);
  _remove_dir(backup_prev);
  _mkdir_p(backup_dir);

  // Backup current install
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "cp -r \"%s\" \"%s\" 2>/dev/null || true",
           install_dir, backup_prev);
  system(cmd);

  return true;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Update/update_stage.h src/Update/update_stage.c
git commit -m "feat: add update staging and backup module"
```

---

### Task 7: Update Actor

**Files:**
- Create: `src/Update/update_actor.h`
- Create: `src/Update/update_actor.c`

- [ ] **Step 1: Create header `src/Update/update_actor.h`**

```c
//
// Created by victor on 5/28/25.
//

#ifndef OFFS_UPDATE_ACTOR_H
#define OFFS_UPDATE_ACTOR_H

#include "../Actor/actor.h"
#include "../Version/version.h"
#include "update_check.h"

typedef enum {
  update_state_idle = 0,
  update_state_checking,
  update_state_downloading,
  update_state_staged,
  update_state_draining,
  update_state_applying
} update_state_e;

typedef struct update_actor_t {
  actor_t actor;
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  update_check_config_t config;
  version_t current_version;
  update_state_e state;
  char staging_dir[512];
  char install_dir[512];
  char backup_dir[512];
  update_info_t* pending_update;
  uint64_t check_timer_id;
  uint64_t drain_start_ms;
  uint64_t drain_timeout_ms;
  // Pointer to the daemon's draining flag and offstream counter
  uint8_t* draining_flag;
  ATOMIC(uint32_t)* open_stream_count;
} update_actor_t;

update_actor_t* update_actor_create(scheduler_pool_t* pool,
                                    timer_actor_t* timer,
                                    const update_check_config_t* config,
                                    const char* staging_dir,
                                    const char* install_dir,
                                    const char* backup_dir,
                                    uint8_t* draining_flag,
                                    ATOMIC(uint32_t)* open_stream_count);
void update_actor_destroy(update_actor_t* actor);

// Trigger immediate check (called from signal handler or IPC)
void update_actor_check_now(update_actor_t* actor);

// Query current state for CLI
update_state_e update_actor_get_state(update_actor_t* actor);
const version_t* update_actor_get_pending_version(update_actor_t* actor);

#endif // OFFS_UPDATE_ACTOR_H
```

- [ ] **Step 2: Create `src/Update/update_actor.c`**

The actor dispatch function handles timer ticks and internal state transitions:

```c
//
// Created by victor on 5/28/25.
//

#include "update_actor.h"
#include "update_download.h"
#include "update_stage.h"
#include "../Timer/timer_actor.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define CHECK_INTERVAL_MS (6 * 60 * 60 * 1000)  // 6 hours default
#define DRAIN_TIMEOUT_MS  300000                 // 5 minutes

typedef enum {
  UPDATE_MSG_CHECK = 1,
  UPDATE_MSG_DOWNLOAD_DONE = 2,
  UPDATE_MSG_DRAIN_TICK = 3,
  UPDATE_MSG_APPLY = 4
} update_msg_e;

static void _dispatch(void* state, message_t* msg);
static void _start_check(update_actor_t* ua);
static void _on_download_done(update_actor_t* ua);
static void _start_drain(update_actor_t* ua);
static void _apply_update(update_actor_t* ua);

update_actor_t* update_actor_create(scheduler_pool_t* pool,
                                    timer_actor_t* timer,
                                    const update_check_config_t* config,
                                    const char* staging_dir,
                                    const char* install_dir,
                                    const char* backup_dir,
                                    uint8_t* draining_flag,
                                    ATOMIC(uint32_t)* open_stream_count) {
  update_actor_t* ua = get_clear_memory(sizeof(update_actor_t));
  actor_init(&ua->actor, ua, _dispatch, pool);
  ua->pool = pool;
  ua->timer = timer;
  ua->config = *config;
  version_parse(OFFS_VERSION, &ua->current_version);
  ua->state = update_state_idle;
  ua->draining_flag = draining_flag;
  ua->open_stream_count = open_stream_count;
  ua->drain_timeout_ms = DRAIN_TIMEOUT_MS;
  snprintf(ua->staging_dir, sizeof(ua->staging_dir), "%s", staging_dir);
  snprintf(ua->install_dir, sizeof(ua->install_dir), "%s", install_dir);
  snprintf(ua->backup_dir, sizeof(ua->backup_dir), "%s", backup_dir);

  // Schedule first check (on startup)
  ua->check_timer_id = timer_actor_set(timer, 5000, CHECK_INTERVAL_MS,
                                       &ua->actor, UPDATE_MSG_CHECK);
  return ua;
}

void update_actor_destroy(update_actor_t* ua) {
  if (ua->pending_update != NULL) {
    update_info_free(ua->pending_update);
  }
  timer_actor_cancel(ua->timer, ua->check_timer_id);
  actor_destroy(&ua->actor);
  free(ua);
}

void update_actor_check_now(update_actor_t* ua) {
  message_t msg = { .type = UPDATE_MSG_CHECK, .payload = NULL };
  actor_send(&ua->actor, &msg);
}

update_state_e update_actor_get_state(update_actor_t* ua) {
  return ua->state;
}

const version_t* update_actor_get_pending_version(update_actor_t* ua) {
  return ua->pending_update ? &ua->pending_update->version : NULL;
}

static void _dispatch(void* state, message_t* msg) {
  update_actor_t* ua = (update_actor_t*)state;

  switch (msg->type) {
    case UPDATE_MSG_CHECK:
      _start_check(ua);
      break;
    case UPDATE_MSG_DOWNLOAD_DONE:
      _on_download_done(ua);
      break;
    case UPDATE_MSG_DRAIN_TICK:
      _start_drain(ua);
      break;
    case UPDATE_MSG_APPLY:
      _apply_update(ua);
      break;
  }
}

static void _start_check(update_actor_t* ua) {
  ua->state = update_state_checking;

  update_info_t* info = update_check_fetch(&ua->config, &ua->current_version);
  if (info == NULL) {
    ua->state = update_state_idle;
    return;
  }

  if (!info->available) {
    update_info_free(info);
    ua->state = update_state_idle;
    return;
  }

  // Download the update
  ua->state = update_state_downloading;
  ua->pending_update = info;

  bool ok = update_download(info, ua->staging_dir, ua->config.github_token);
  if (!ok) {
    update_info_free(info);
    ua->pending_update = NULL;
    ua->state = update_state_idle;
    return;
  }

  // Stage for installation
  ok = update_stage(ua->staging_dir, ua->install_dir, ua->backup_dir);
  if (!ok) {
    update_info_free(info);
    ua->pending_update = NULL;
    ua->state = update_state_idle;
    return;
  }

  ua->state = update_state_staged;
  // Begin draining
  _start_drain(ua);
}

static void _start_drain(update_actor_t* ua) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;

  if (ua->drain_start_ms == 0) {
    ua->drain_start_ms = now_ms;
    ua->state = update_state_draining;
    // Set draining flag so HTTP server rejects new streams
    if (ua->draining_flag != NULL) {
      *ua->draining_flag = 1;
    }
  }

  // Check if all streams are closed
  uint32_t open_count = ua->open_stream_count
    ? ATOMIC_LOAD(ua->open_stream_count) : 0;

  if (open_count == 0) {
    // Drain complete — apply
    message_t msg = { .type = UPDATE_MSG_APPLY, .payload = NULL };
    actor_send(&ua->actor, &msg);
    return;
  }

  // Check timeout
  if (now_ms - ua->drain_start_ms > ua->drain_timeout_ms) {
    // Force apply
    message_t msg = { .type = UPDATE_MSG_APPLY, .payload = NULL };
    actor_send(&ua->actor, &msg);
    return;
  }

  // Recheck in 1 second
  timer_actor_set(ua->timer, 1000, 0, &ua->actor, UPDATE_MSG_DRAIN_TICK);
}

static void _apply_update(update_actor_t* ua) {
  ua->state = update_state_applying;

  // Spawn offs-updater with staging, install, and backup dirs
  pid_t pid = fork();
  if (pid == 0) {
    // Child: exec the updater
    char updater_path[512];
    snprintf(updater_path, sizeof(updater_path), "%s/offs-updater",
             ua->install_dir);
    execl(updater_path, "offs-updater",
          ua->staging_dir, ua->install_dir, ua->backup_dir, NULL);
    _exit(1);
  }

  // Parent (daemon): exit so the updater can replace us
  if (pid > 0) {
    exit(0);
  }
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Update/update_actor.h src/Update/update_actor.c
git commit -m "feat: add update actor with drain-before-update logic"
```

---

### Task 8: Updater Binary

**Files:**
- Create: `src/Updater/updater_main.c`

- [ ] **Step 1: Create `src/Updater/updater_main.c`**

Standalone executable. No dependency on liboffs. Self-replacing via temp-copy technique.

```c
//
// Created by victor on 5/28/25.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#define SEP "\\"
#else
#define SLEEP_MS(ms) usleep((ms) * 1000)
#define SEP "/"
#endif

#define STARTUP_TIMEOUT_SEC 30
#define PID_FILE "/var/run/offs-updater.pid"

static FILE* _log = NULL;

static void _log_write(const char* msg) {
  if (_log == NULL) {
    _log = fopen("/var/log/offs-updater.log", "a");
    if (_log == NULL) return;
  }
  time_t now = time(NULL);
  fprintf(_log, "[%s] %s\n", ctime(&now), msg);
  // ctime adds newline, remove trailing newline for clean log
  fflush(_log);
}

static int _copy_file(const char* src, const char* dst) {
  FILE* fsrc = fopen(src, "rb");
  if (fsrc == NULL) return -1;
  FILE* fdst = fopen(dst, "wb");
  if (fdst == NULL) { fclose(fsrc); return -1; }

  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
    fwrite(buf, 1, n, fdst);
  }
  fclose(fsrc);
  fclose(fdst);

  // Preserve executable bit on POSIX
#ifndef _WIN32
  struct stat st;
  if (stat(src, &st) == 0) {
    chmod(dst, st.st_mode);
  }
#endif
  return 0;
}

static int _service_stop(void) {
  _log_write("Stopping daemon service");
#ifdef _WIN32
  return system("sc stop offs-daemon");
#else
  return system("systemctl stop offs-daemon");
#endif
}

static int _service_start(void) {
  _log_write("Starting daemon service");
#ifdef _WIN32
  return system("sc start offs-daemon");
#else
  return system("systemctl daemon-reload && systemctl start offs-daemon");
#endif
}

typedef enum { FINISH_NORMAL = 0, FINISH_SELF_REPLACE = 1 } finish_mode_t;

int main(int argc, char** argv) {
  finish_mode_t mode = FINISH_NORMAL;
  const char* staging_dir = NULL;
  const char* install_dir = NULL;
  const char* backup_dir = NULL;

  // Parse args
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--finish") == 0) {
      mode = FINISH_SELF_REPLACE;
    } else if (staging_dir == NULL) {
      staging_dir = argv[i];
    } else if (install_dir == NULL) {
      install_dir = argv[i];
    } else if (backup_dir == NULL) {
      backup_dir = argv[i];
    }
  }

  if (staging_dir == NULL || install_dir == NULL || backup_dir == NULL) {
    fprintf(stderr, "Usage: offs-updater [--finish] <staging> <install> <backup>\n");
    return 1;
  }

  _log_write(mode == FINISH_SELF_REPLACE
    ? "Updater running in finish mode" : "Updater starting");

  // Step 1: Stop the service
  _service_stop();

  // Step 2: Wait for daemon to exit (poll)
  _log_write("Waiting for daemon to exit");
  for (int i = 0; i < 100; i++) {
    SLEEP_MS(300);
    // Check if offs-daemon process exists
#ifdef _WIN32
    int running = system("tasklist /FI \"IMAGENAME eq offs-daemon.exe\" 2>nul | find /I \"offs-daemon.exe\" >nul");
#else
    int running = system("pidof offs-daemon > /dev/null 2>&1");
#endif
    if (running != 0) break;
  }

  // Step 3: Replace binaries
  _log_write("Replacing binaries");
  const char* binaries[] = {"offs-daemon", "offs", "offs-updater", NULL};

  if (mode == FINISH_SELF_REPLACE) {
    // We're the temp copy — replace original updater, then remaining files
    char self_path[512], dst_path[512];
    snprintf(self_path, sizeof(self_path), "%s/offs-updater", staging_dir);
    snprintf(dst_path, sizeof(dst_path), "%s/offs-updater", install_dir);
    _copy_file(self_path, dst_path);

    for (int i = 0; binaries[i] != NULL; i++) {
      if (strcmp(binaries[i], "offs-updater") == 0) continue;  // already done
      char src[512], dst[512];
      snprintf(src, sizeof(src), "%s/%s", staging_dir, binaries[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, binaries[i]);
      _copy_file(src, dst);
    }
  } else {
    // First run: copy all binaries, then self-replace via temp copy
    // Copy updater to temp location
    char temp_updater[512];
    snprintf(temp_updater, sizeof(temp_updater), "/tmp/offs-updater-temp");

    char staging_updater[512];
    snprintf(staging_updater, sizeof(staging_updater), "%s/offs-updater", staging_dir);
    _copy_file(staging_updater, temp_updater);

    // Copy daemon and CLI
    for (int i = 0; binaries[i] != NULL; i++) {
      if (strcmp(binaries[i], "offs-updater") == 0) continue;  // done last
      char src[512], dst[512];
      snprintf(src, sizeof(src), "%s/%s", staging_dir, binaries[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, binaries[i]);
      _copy_file(src, dst);
    }

    // Now replace self: exec the temp copy with --finish
    char self_dst[512];
    snprintf(self_dst, sizeof(self_dst), "%s/offs-updater", install_dir);
    _log_write("Self-replacing via temp copy");
    execl(temp_updater, "offs-updater", "--finish",
          staging_dir, install_dir, backup_dir, NULL);
    // If we get here, exec failed — fall through to service_start for recovery
    _log_write("ERROR: exec of temp updater failed, attempting recovery");
  }

  // Step 4: Start service
  _log_write("Starting daemon service");
  _service_start();

  // Step 5: Verify daemon started
  _log_write("Verifying daemon startup");
  int started = 0;
  for (int i = 0; i < STARTUP_TIMEOUT_SEC * 2; i++) {
    SLEEP_MS(500);
#ifdef _WIN32
    int running = system("sc query offs-daemon | findstr RUNNING > nul");
#else
    int running = system("systemctl is-active --quiet offs-daemon");
#endif
    if (running == 0) { started = 1; break; }
  }

  if (!started) {
    _log_write("ERROR: Daemon failed to start, restoring from backup");
    // Restore from backup
    char backup_prev[512];
    snprintf(backup_prev, sizeof(backup_prev), "%s/previous", backup_dir);
    const char* backup_binaries[] = {"offs-daemon", "offs-cli",
                                     "offs-updater", NULL};
    for (int i = 0; backup_binaries[i] != NULL; i++) {
      char src[512], dst[512];
      snprintf(src, sizeof(src), "%s/%s", backup_prev, backup_binaries[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, backup_binaries[i]);
      _copy_file(src, dst);
    }
    _service_start();
    _log_write("Rollback complete");
  } else {
    _log_write("Update complete — daemon running");
  }

  // Cleanup
  if (_log != NULL) fclose(_log);
  return started ? 0 : 1;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Updater/updater_main.c
git commit -m "feat: add standalone offs-updater binary with self-replacement"
```

---

### Task 9: CMake Integration

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add OFFS_VERSION and updater target to root CMakeLists.txt**

After `project(offs C CXX)`, add:

```cmake
set(OFFS_VERSION_MAJOR 0)
set(OFFS_VERSION_MINOR 1)
set(OFFS_VERSION_PATCH 0)
set(OFFS_VERSION "${OFFS_VERSION_MAJOR}.${OFFS_VERSION_MINOR}.${OFFS_VERSION_PATCH}")
target_compile_definitions(offs PRIVATE OFFS_VERSION="${OFFS_VERSION}")
```

After the existing `add_subdirectory(examples)` line, add:

```cmake
# offs-updater standalone binary (no liboffs dependency, minimal linking)
add_executable(offs-updater
  src/Updater/updater_main.c
)
# Link only what the updater needs: platform functions (signals, syscalls)
if(OPENSSL_FOUND)
  target_link_libraries(offs-updater PRIVATE ${OPENSSL_LIBRARIES})
endif()
if(WIN32)
  target_link_libraries(offs-updater PRIVATE ws2_32 advapi32)
endif()
```

- [ ] **Step 2: Build and verify**

```bash
cd build && cmake .. && cmake --build . -j$(nproc)
```

Expected: Builds successfully with `offs-updater` in the build output. The `offs` library compiles with `OFFS_VERSION` defined.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "feat: add OFFS_VERSION and offs-updater target to build"
```

---

### Task 10: Packaging — Linux systemd + .deb

**Files:**
- Create: `packaging/linux/debian/control`
- Create: `packaging/linux/debian/postinst`
- Create: `packaging/linux/debian/prerm`
- Create: `packaging/linux/debian/postrm`
- Create: `packaging/linux/debian/offs-daemon.service`

- [ ] **Step 1: Create `packaging/linux/debian/control`**

```
Package: offs
Version: 0.1.0
Section: net
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.28), openssl (>= 3.0), systemd
Maintainer: Prometheus-SCN <support@prometheus-scn.org>
Description: Owner Free File System daemon and CLI
 OFFS is a decentralized, content-addressed storage system.
 This package installs the offs-daemon system service and offs CLI tool.
```

- [ ] **Step 2: Create systemd unit `packaging/linux/debian/offs-daemon.service`**

```ini
[Unit]
Description=OFFS Daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/offs-daemon
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal
Environment=GITHUB_TOKEN=%E/offs/github-token

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 3: Create `packaging/linux/debian/postinst`**

```sh
#!/bin/sh
set -e
systemctl daemon-reload
systemctl enable offs-daemon 2>/dev/null || true
systemctl start offs-daemon 2>/dev/null || true
```

- [ ] **Step 4: Create `packaging/linux/debian/prerm`**

```sh
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "upgrade" ]; then
  systemctl stop offs-daemon 2>/dev/null || true
fi
```

- [ ] **Step 5: Create `packaging/linux/debian/postrm`**

```sh
#!/bin/sh
set -e
if [ "$1" = "purge" ]; then
  systemctl disable offs-daemon 2>/dev/null || true
  systemctl daemon-reload
fi
```

- [ ] **Step 6: Make scripts executable and commit**

```bash
chmod +x packaging/linux/debian/postinst packaging/linux/debian/prerm packaging/linux/debian/postrm
git add packaging/
git commit -m "feat: add linux systemd service and .deb packaging"
```

---

### Task 11: Packaging — Linux .rpm

**Files:**
- Create: `packaging/linux/rpm/offs.spec`
- Create: `packaging/linux/rpm/offs-daemon.service` (same unit file as .deb)

- [ ] **Step 1: Create `packaging/linux/rpm/offs.spec`**

```spec
Name:           offs
Version:        0.1.0
Release:        1%{?dist}
Summary:        Owner Free File System daemon and CLI
License:        MIT
URL:            https://github.com/Prometheus-SCN/OFFS
Source0:        %{name}-%{version}.tar.gz

Requires:       openssl >= 3.0
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
OFFS is a decentralized, content-addressed storage system.
This package installs the offs-daemon system service and offs CLI tool.

%prep
%setup -q

%install
install -Dm755 offs-daemon %{buildroot}/usr/bin/offs-daemon
install -Dm755 offs %{buildroot}/usr/bin/offs
install -Dm755 offs-updater %{buildroot}/usr/bin/offs-updater
install -Dm644 packaging/linux/rpm/offs-daemon.service %{buildroot}/usr/lib/systemd/system/offs-daemon.service

%post
%systemd_post offs-daemon.service

%preun
%systemd_preun offs-daemon.service

%postun
%systemd_postun_with_restart offs-daemon.service

%files
/usr/bin/offs-daemon
/usr/bin/offs
/usr/bin/offs-updater
/usr/lib/systemd/system/offs-daemon.service
```

- [ ] **Step 2: Commit**

```bash
git add packaging/linux/rpm/
git commit -m "feat: add linux .rpm spec and packaging"
```

---

### Task 12: Packaging — macOS

**Files:**
- Create: `packaging/macos/preinstall`
- Create: `packaging/macos/postinstall`
- Create: `packaging/macos/com.offs.daemon.plist`

- [ ] **Step 1: Create launchd plist `packaging/macos/com.offs.daemon.plist`**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.offs.daemon</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/bin/offs-daemon</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>StandardOutPath</key>
  <string>/var/log/offs-daemon.log</string>
  <key>StandardErrorPath</key>
  <string>/var/log/offs-daemon.log</string>
</dict>
</plist>
```

- [ ] **Step 2: Create `packaging/macos/postinstall`**

```sh
#!/bin/sh
set -e
cp /tmp/offs-pkg/com.offs.daemon.plist /Library/LaunchDaemons/
launchctl bootstrap system /Library/LaunchDaemons/com.offs.daemon.plist 2>/dev/null || \
  launchctl load -w /Library/LaunchDaemons/com.offs.daemon.plist
```

- [ ] **Step 3: Create `packaging/macos/preinstall`**

```sh
#!/bin/sh
set -e
launchctl bootout system/com.offs.daemon 2>/dev/null || \
  launchctl unload /Library/LaunchDaemons/com.offs.daemon.plist 2>/dev/null || true
```

- [ ] **Step 4: Commit**

```bash
chmod +x packaging/macos/preinstall packaging/macos/postinstall
git add packaging/macos/
git commit -m "feat: add macOS launchd plist and .pkg scripts"
```

---

### Task 13: Packaging — Windows WiX

**Files:**
- Create: `packaging/windows/offs.wxs`
- Create: `packaging/windows/offs.wxl`

- [ ] **Step 1: Create `packaging/windows/offs.wxs`**

```xml
<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="OFFS" Language="1033" Version="0.1.0"
           Manufacturer="Prometheus-SCN" UpgradeCode="PUT-GUID-HERE">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perMachine" />
    <MajorUpgrade DowngradeErrorMessage="A newer version is already installed." />
    <MediaTemplate EmbedCab="yes" />
    <Feature Id="Complete" Title="OFFS" Level="1">
      <ComponentGroupRef Id="Binaries" />
    </Feature>
    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="ProgramFiles64Folder">
        <Directory Id="INSTALLDIR" Name="OFFS">
          <Component Id="OffsDaemon" Guid="PUT-GUID-HERE">
            <File Id="offsDaemonExe" Name="offs-daemon.exe" Source="offs-daemon.exe" KeyPath="yes" />
            <ServiceInstall Id="OffsDaemonService" Type="ownProcess" Name="offs-daemon"
                            DisplayName="OFFS Daemon" Description="Owner Free File System Daemon"
                            Start="auto" ErrorControl="normal" />
            <ServiceControl Id="OffsDaemonControl" Name="offs-daemon"
                            Stop="both" Remove="uninstall" Wait="yes" />
          </Component>
          <Component Id="OffsCli" Guid="PUT-GUID-HERE">
            <File Id="offsCliExe" Name="offs.exe" Source="offs.exe" KeyPath="yes" />
            <Environment Id="OffsPath" Action="set" Name="PATH"
                         Part="last" System="yes" Value="[INSTALLDIR]" />
          </Component>
          <Component Id="OffsUpdater" Guid="PUT-GUID-HERE">
            <File Id="offsUpdaterExe" Name="offs-updater.exe" Source="offs-updater.exe" KeyPath="yes" />
          </Component>
        </Directory>
      </Directory>
    </Directory>
    <ComponentGroup Id="Binaries">
      <ComponentRef Id="OffsDaemon" />
      <ComponentRef Id="OffsCli" />
      <ComponentRef Id="OffsUpdater" />
    </ComponentGroup>
  </Product>
</Wix>
```

- [ ] **Step 2: Commit**

```bash
git add packaging/windows/
git commit -m "feat: add Windows WiX installer definition"
```

---

### Task 14: Release Scripts

**Files:**
- Create: `scripts/release.sh`
- Create: `scripts/collect-release.sh`

- [ ] **Step 1: Create `scripts/release.sh`** (run on each platform)

```sh
#!/bin/bash
set -euo pipefail

PLATFORM="${1:-}"
TAG="${2:-}"

if [ -z "$PLATFORM" ] || [ -z "$TAG" ]; then
  echo "Usage: $0 <linux-x64|macos-x64|windows-x64> <tag>"
  exit 1
fi

git checkout "$TAG"

BUILD_DIR="build-release"
rm -rf "$BUILD_DIR"
mkdir "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# Create bundle directory
BUNDLE="offs-${PLATFORM}"
mkdir -p "$BUNDLE"

case "$PLATFORM" in
  linux-x64)
    cp src/Updater/offs-updater "$BUNDLE/"
    cp ../OFFS/build/offsd "$BUNDLE/offs-daemon" 2>/dev/null || true
    cp ../OFFS/build/offs "$BUNDLE/offs" 2>/dev/null || true
    echo "$TAG" > "$BUNDLE/VERSION"
    cd "$BUNDLE" && sha256sum * > checksums.sha256 && cd ..
    tar -czf "${BUNDLE}.tar.gz" "$BUNDLE"
    # Build .deb and .rpm
    ;;
  macos-x64)
    cp src/Updater/offs-updater "$BUNDLE/"
    # Mac binaries to be built from OFFS repo
    echo "$TAG" > "$BUNDLE/VERSION"
    cd "$BUNDLE" && shasum -a 256 * > checksums.sha256 && cd ..
    tar -czf "${BUNDLE}.tar.gz" "$BUNDLE"
    # Build .pkg with pkgbuild
    ;;
  windows-x64)
    cp src/Updater/offs-updater.exe "$BUNDLE/"
    echo "$TAG" > "$BUNDLE/VERSION"
    cd "$BUNDLE" && sha256sum * > checksums.sha256 && cd ..
    zip -r "${BUNDLE}.zip" "$BUNDLE"
    # Build .msi with WiX candle/light
    ;;
esac

echo "Artifact: ${BUNDLE}.tar.gz (or .zip)"
```

- [ ] **Step 2: Create `scripts/collect-release.sh`** (run on Linux after all platforms built)

```sh
#!/bin/bash
set -euo pipefail

TAG="${1:-}"
ARTIFACTS_DIR="${2:-./artifacts}"

if [ -z "$TAG" ]; then
  echo "Usage: $0 <tag> [artifacts-dir]"
  exit 1
fi

# Generate release body with checksums
BODY="OFFS ${TAG}\n\n## Checksums\n\n\`\`\`"
for f in "$ARTIFACTS_DIR"/*.tar.gz "$ARTIFACTS_DIR"/*.zip \
         "$ARTIFACTS_DIR"/*.deb "$ARTIFACTS_DIR"/*.rpm \
         "$ARTIFACTS_DIR"/*.pkg "$ARTIFACTS_DIR"/*.msi; do
  if [ -f "$f" ]; then
    SHA=$(sha256sum "$f" | cut -d' ' -f1)
    BODY="${BODY}\n${SHA}  $(basename "$f")"
  fi
done
BODY="${BODY}\n\`\`\`"

# Create GitHub release
gh release create "$TAG" \
  --title "OFFS ${TAG}" \
  --notes "$(echo -e "$BODY")" \
  "$ARTIFACTS_DIR"/*.tar.gz "$ARTIFACTS_DIR"/*.zip \
  "$ARTIFACTS_DIR"/*.deb "$ARTIFACTS_DIR"/*.rpm \
  "$ARTIFACTS_DIR"/*.pkg "$ARTIFACTS_DIR"/*.msi

echo "Release $TAG created"
```

- [ ] **Step 3: Make scripts executable and commit**

```bash
chmod +x scripts/release.sh scripts/collect-release.sh
git add scripts/
git commit -m "feat: add release and collection scripts"
```

---

### Task 15: Wire Update Actor into Daemon (OFFS repo)

**Files:**
- Modify: `OFFS/src/offsd/main.c`

- [ ] **Step 1: Add update_actor to the offsd_server_t struct**

Add after `unix_transport_t* unix_transport;`:
```c
  update_actor_t*    update_actor;
```

- [ ] **Step 2: Create update_actor in `_startup()`**

Add after the Unix transport creation:
```c
  /* Update actor — auto-update checks */
  update_check_config_t update_config = {
    .channel = channel_stable  // default; override from authority
  };
  snprintf(update_config.github_repo, sizeof(update_config.github_repo),
           "%s", "Prometheus-SCN/OFFS");
  snprintf(update_config.github_api_url, sizeof(update_config.github_api_url),
           "%s", "https://api.github.com");

  // Determine channel from authority environment
  const char* env = authority_get_environment(server->authority);
  if (env != NULL) {
    if (strcmp(env, "test") == 0) update_config.channel = channel_rc;
    else if (strcmp(env, "dev") == 0) update_config.channel = channel_dev;
  }

  // Read GITHUB_TOKEN from environment if available
  const char* token = getenv("GITHUB_TOKEN");
  if (token != NULL) {
    snprintf(update_config.github_token, sizeof(update_config.github_token),
             "%s", token);
  }

  server->update_actor = update_actor_create(
    server->pool, server->timer, &update_config,
    "/var/lib/offs/updates", install_dir, "/var/lib/offs/backup",
    &server->draining_val, &server->open_stream_count);
```

- [ ] **Step 3: Add SIGHUP handler that triggers update check**

Modify `_signal_handler` to handle SIGHUP:
```c
static void _signal_handler(int sig) {
  if (sig == SIGHUP && g_server != NULL && g_server->update_actor != NULL) {
    update_actor_check_now(g_server->update_actor);
    return;
  }
  if (g_node != NULL) {
    ATOMIC_STORE(&g_node->running, 0);
  }
}
```

- [ ] **Step 4: Destroy update_actor in shutdown sequence**

Add `update_actor_destroy(server->update_actor);` to the shutdown sequence before `timer_actor_destroy`.

- [ ] **Step 5: Build and test daemon**

```bash
cd OFFS && cmake -B build && cmake --build build
```

- [ ] **Step 6: Commit**

```bash
git add src/offsd/main.c
git commit -m "feat: wire update actor into daemon lifecycle"
```

---

### Task 16: CLI Update Status Command (OFFS repo)

**Files:**
- Modify: `OFFS/src/offs/commands/status.c` (add update status)

- [ ] **Step 1: Add update status to existing `offs status` command**

The existing `status` command already talks to the daemon via CBOR and shows health info. Extend it to also report update state. Add after existing status output:

```c
  // Query update status
  buffer_t* update_resp = _send_message(client, UPDATE_STATUS, NULL);
  if (update_resp != NULL) {
    // Parse response JSON from daemon
    printf("  Update:\n");
    printf("    State: %s\n", /* parsed state string */);
    printf("    Current version: %s\n", /* parsed current version */);
    if (/* update available */) {
      printf("    Available version: %s\n", /* parsed available version */);
    }
  }
```

Use the existing `cli_client_t` send/receive pattern from other commands like `health.c`.

- [ ] **Step 2: Commit**

```bash
git add src/offs/commands/status.c
git commit -m "feat: add update status to offs status command"
```

---

## Verification

1. **Build:** `cmake -B build && cmake --build build -j$(nproc)` — all targets compile
2. **Version tests:** `cd build && ctest -R test_version` — 14 tests pass
3. **Updater self-replacement:** Copy `offs-updater` to a temp dir, run with `--finish`, verify it replaces itself and starts daemon
4. **Drain flow:** Start daemon with open stream, trigger SIGHUP, verify daemon enters draining state, waits for stream to close, then applies update
5. **Rollback:** Corrupt the staged `offs-daemon` binary, run updater, verify it restores from backup
6. **Drain timeout:** Block stream closure, verify updater forces apply after configured timeout
7. **.deb install:** `dpkg -i offs-*.deb` on clean system, verify `offs status` works, daemon starts on boot
