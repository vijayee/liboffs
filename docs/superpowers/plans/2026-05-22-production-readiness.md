# Production Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write production blockers document, create harmony epics/tickets, and implement Windows Platform layer (socket, local, file).

**Architecture:** Three-phase plan. Phase 1 writes the audit document. Phase 2 creates harmony epics and tickets. Phase 3 implements the Windows `#ifdef _WIN32` branches for the three stubbed platform files, each following the existing MSVC patterns from `platform_thread.c`.

**Tech Stack:** C11 (MSVC/Winsock2/Win32 API for Windows, POSIX for existing), harmony CLI for ticket management.

---

### Task 1: Write Production Blockers Document

**Files:**
- Create: `docs/PRODUCTION_BLOCKERS.md`

- [ ] **Step 1: Write the document**

Write `docs/PRODUCTION_BLOCKERS.md` with the full audit findings:

```markdown
# Production Blockers

Audit of the `ClientAPI` branch, 2026-05-22.

## Summary

The core engine (P2P protocol, block cache, streaming, actor model) is well-built and
memory-clean. Gaps are concentrated in security and operational layers. The library is
suitable for trusted-LAN or research use; it is not ready for public-internet deployment.

## Critical (Production Blockers)

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| 1 | TLS certificate validation disabled globally | `quic_listener.c`, `relay_client.c`, `relay_server.c`, `wt_transport.c`, `offs_client.c` | MITM trivial on all QUIC connections |
| 2 | No authentication or authorization | Entire codebase | Any client can PUT/GET arbitrary blocks |
| 3 | Windows Platform layer incomplete | `platform_socket.c`, `platform_local.c`, `platform_file.c` | Cannot build on Windows |

### 1. TLS Certificate Validation Disabled

Every QUIC connection uses `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`
unconditionally. Affected files:
- `src/Network/quic_listener.c:578-581` — P2P listener
- `src/Network/relay_client.c:508-510` — relay client
- `src/Network/Relay/relay_server.c:615-617` — relay server
- `src/ClientAPI/WT/wt_transport.c:375` — WT transport server
- `src/ClientLibs/c/offs_client.c:872` — WT client (non-secure path only, but secure path
  has no CA configuration either)

TLS encrypts but does not authenticate peers.

### 2. No Authentication

The HTTP server has a middleware system (`http_middleware_t`) but no auth middleware
exists. The client API (`offs_client.h`) has no token, key, or credential parameters.
The P2P layer has no peer identity verification beyond the optional `public_key` field
in `authority_t` (used in salutation, but not enforced).

### 3. Windows Platform Layer Incomplete

Three modules have only stub implementations for `#ifdef _WIN32`:
- `platform_socket.c:216` — `/* Windows implementation deferred */`
- `platform_local.c:56` — `/* Windows implementation deferred */`
- `platform_file.c:76` — `/* Windows implementation deferred */`

Threading, time, process, and random have full Windows support.

## High (Significant Concerns)

| # | Issue | Impact |
|---|-------|--------|
| 4 | No monitoring, metrics, or health checks | Cannot observe runtime behavior |
| 5 | No graceful shutdown / connection draining | In-flight operations may be lost |
| 6 | No client-side retry or backoff | Transient failures become permanent |
| 7 | No input validation | Malformed data may cause undefined behavior |
| 8 | No wire protocol versioning | Protocol changes break all clients silently |

### 4. No Monitoring or Health Checks

Logging uses `rxi/log.c` (basic printf-style, no structured output, no log level
configuration per module). No Prometheus/StatsD metrics export. `topology_metrics_t`
is referenced in `authority.h` but has no implementation for health endpoints.

### 5. No Graceful Shutdown

`offs_node_stop()` in `node.c:66` sets `running=0` and tears down components in
sequence. No waiting for in-flight operations, no connection draining, no graceful
client disconnect. `scheduler_pool_stop()` is called immediately.

### 6. No Client Retry or Backoff

`offs_client.c` operations fail immediately on error. No exponential backoff, no
retry policy. Hardcoded timeouts: 100ms poll interval, 1s WS connect timeout (100
loops × 10ms), 2s WT connect timeout (200 loops × 10ms). No user-configurable
timeouts.

### 7. No Input Validation

ORI strings, content types, file names, and URLs are passed directly to internal
functions. The `client_api_wire.c` decoder trusts CBOR input without bounds
checking on decoded values.

### 8. No Wire Protocol Versioning

`client_api_wire.h` defines message type constants (1-11) but has no version field
in the protocol. A protocol change would require flag-day cutover or break all
existing clients.

## Medium (Should Fix Before Broad Deployment)

| # | Issue | Impact |
|---|-------|--------|
| 9 | No HTTP-level rate limiting | HTTP endpoints have no request throttling |
| 10 | No encryption at rest | Block cache stores raw files on disk |
| 11 | Fragile build paths | `libpoll_dancer.a` at relative path |
| 12 | No configuration validation | Invalid config values pass silently |
| 13 | Bootstrap relies on static peer lists | No DNS seed, DHT, or mDNS discovery |

### 9. No HTTP Rate Limiting

Rate limiting exists at the P2P network layer (`rate_limit.h` with token buckets
per RPC type). The HTTP server (`http_server.h`) has no per-connection or
per-endpoint rate limiting.

### 10. No Encryption at Rest

Block cache (`block_cache.c`, `section.c`, `wal.c`) stores data as raw files.
No encryption layer on disk I/O.

### 11. Fragile Build Paths

`CMakeLists.txt` references `${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer/build/libpoll_dancer.a`
as a relative path. This breaks if the build directory is outside the source tree
or poll-dancer is installed elsewhere.

### 12. No Configuration Validation

`config_t` in `config.h` has no validation. Invalid values (zero bucket size,
max_tuple_size < min_tuple_size) pass through silently.

### 13. Static Peer Bootstrap Only

`authority_t` stores bootstrap peers as a static string array. No DNS seeding,
no DHT bootstrap, no mDNS local discovery. Nodes must be manually configured
with peer addresses.
```

- [ ] **Step 2: Commit**

```bash
git add docs/PRODUCTION_BLOCKERS.md
git commit -m "docs: add production blockers audit document"
```

---

### Task 2: Create Harmony Epics

- [ ] **Step 1: Verify harmony connectivity**

```bash
H="/home/victor/.claude/skills/harmony/harmony"; $H context
```

Expected: Shows Mycroft identity, no in-progress tickets.

- [ ] **Step 2: Create Security Hardening epic**

```bash
$H epic create "Security Hardening" --desc "TLS certificate validation, authentication framework, and encryption at rest for production security readiness"
```

- [ ] **Step 3: Create Platform Maturity epic**

```bash
$H epic create "Platform Maturity" --desc "Complete Windows Platform layer (socket, local, file), reproducible builds, and cross-platform CI validation"
```

- [ ] **Step 4: Create Observability epic**

```bash
$H epic create "Observability" --desc "Structured logging, metrics export, and health check endpoints for production monitoring"
```

- [ ] **Step 5: Create Reliability epic**

```bash
$H epic create "Reliability" --desc "Graceful shutdown, client retry/backoff, input validation, and configuration validation"
```

- [ ] **Step 6: Create Protocol Maturity epic**

```bash
$H epic create "Protocol Maturity" --desc "Wire protocol versioning, HTTP rate limiting, and DNS/mDNS peer bootstrap discovery"
```

- [ ] **Step 7: Verify epics**

```bash
$H epic list
```

Expected: Shows all 5 epics.

---

### Task 3: Create Harmony Tickets

- [ ] **Step 1: Create Security Hardening tickets**

```bash
$H ticket create "Enable TLS certificate validation on all QUIC connections" --priority high --epic "Security Hardening" --desc "Remove QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION from quic_listener.c, relay_client.c, relay_server.c, wt_transport.c. Add CA certificate configuration to authority_t. Add proper cert validation to offs_client.c WSS path."

$H ticket create "Add authentication framework" --priority high --epic "Security Hardening" --desc "Implement auth middleware for HTTP server. Add API key/token support to client API wire protocol. Add peer identity verification enforcement in P2P salutation handshake."

$H ticket create "Add encryption at rest for block cache" --priority medium --epic "Security Hardening" --desc "Add optional AES encryption layer to section/WAL file I/O in block cache. Key management via authority_t configuration."
```

- [ ] **Step 2: Create Platform Maturity tickets**

```bash
$H ticket create "Implement Windows platform_socket.c" --priority high --epic "Platform Maturity" --desc "Implement Winsock2 backend for all platform_socket functions: create, bind, listen, accept, connect, send, recv, shutdown, destroy, fd, set_nonblocking, set_reuseaddr, address_parse, address_to_string."

$H ticket create "Implement Windows platform_local.c" --priority high --epic "Platform Maturity" --desc "Implement AF_UNIX support for Windows 10 1803+ in platform_local.c: listen, accept, connect, cleanup. Use _platform_has_af_unix() from platform_internal.h for feature detection. Return NULL with log message if AF_UNIX unavailable."

$H ticket create "Implement Windows platform_file.c" --priority high --epic "Platform Maturity" --desc "Implement Win32 CreateFileW/ReadFile/WriteFile backend for all platform_file functions: open, close, read, write, pread, pwrite, seek, exists, unlink, mkdir (recursive)."

$H ticket create "Make build paths relocatable" --priority medium --epic "Platform Maturity" --desc "Replace hardcoded relative paths to poll-dancer in CMakeLists.txt with a cache variable or find_package pattern."
```

- [ ] **Step 3: Create Observability tickets**

```bash
$H ticket create "Add structured logging and metrics" --priority medium --epic "Observability" --desc "Replace or augment rxi/log.c with structured logging (JSON or key=value). Add per-module log level configuration. Add Prometheus-style counter/gauge/histogram metrics for key operations."

$H ticket create "Add health check endpoint" --priority medium --epic "Observability" --desc "Implement HTTP GET /health endpoint returning node status, peer count, block cache stats, capacity, and uptime. Wire up topology_metrics_t."
```

- [ ] **Step 4: Create Reliability tickets**

```bash
$H ticket create "Implement graceful shutdown" --priority medium --epic "Reliability" --desc "Add connection draining to offs_node_stop(). Wait for in-flight operations to complete. Add shutdown timeout. Drain HTTP and P2P connections before stopping scheduler."

$H ticket create "Add client retry with exponential backoff" --priority medium --epic "Reliability" --desc "Add configurable retry policy to offs_client_t. Implement exponential backoff with jitter. Add connect/read/write timeout configuration. Remove hardcoded magic-number timeouts."

$H ticket create "Add input validation throughout" --priority medium --epic "Reliability" --desc "Validate ORI strings, content types, file names, and URLs at API boundaries. Add bounds checking in client_api_wire.c decoder. Reject malformed CBOR frames gracefully."

$H ticket create "Add configuration validation" --priority medium --epic "Reliability" --desc "Add config_validate() function that checks all config_t fields for sane values. Call at node startup. Reject zero/negative values where inappropriate."
```

- [ ] **Step 5: Create Protocol Maturity tickets**

```bash
$H ticket create "Add wire protocol version negotiation" --priority medium --epic "Protocol Maturity" --desc "Add version field to client API wire protocol. Implement version handshake on connect. Server advertises supported versions; client picks highest compatible. Reject incompatible versions with clear error."

$H ticket create "Add HTTP-level rate limiting" --priority medium --epic "Protocol Maturity" --desc "Add per-IP and per-endpoint rate limiting to HTTP server using token bucket algorithm. Integrate with existing rate_limit.h patterns. Return 429 with Retry-After header."

$H ticket create "Add DNS/mDNS peer bootstrap" --priority medium --epic "Protocol Maturity" --desc "Add DNS seed node discovery as alternative to static bootstrap peers. Add mDNS local network peer discovery. Keep static peer list as fallback."
```

- [ ] **Step 6: Verify tickets**

```bash
$H epic get "Security Hardening"
$H epic get "Platform Maturity"
```

Expected: Shows all tickets under each epic.

---

### Task 4: Implement Windows platform_socket.c

**Files:**
- Modify: `src/Platform/platform_socket.c:215-217`

- [ ] **Step 1: Replace the Windows stub with Winsock2 implementation**

Replace lines 215-217 (`#else` / `/* Windows implementation deferred */` / `#endif`) with:

```c
#else
  /* Windows implementation — Winsock2 */
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <stdlib.h>
  #include <string.h>

  #include "platform_internal.h"

  struct platform_socket_t {
    SOCKET fd;
    int family; /* platform_address_family_e */
  };

  static int _family_to_native(platform_address_family_e family) {
    switch (family) {
      case PLATFORM_AF_INET:  return AF_INET;
      case PLATFORM_AF_INET6: return AF_INET6;
      case PLATFORM_AF_LOCAL: return _platform_has_af_unix() ? AF_UNIX : -1;
      default: return -1;
    }
  }

  platform_socket_t* platform_socket_create(platform_address_family_e family, int stream) {
    if (_platform_winsock_init() != 0) {
      return NULL;
    }
    int native_family = _family_to_native(family);
    if (native_family < 0) {
      return NULL;
    }
    int type = stream ? SOCK_STREAM : SOCK_DGRAM;
    SOCKET fd = socket(native_family, type, 0);
    if (fd == INVALID_SOCKET) {
      return NULL;
    }
    platform_socket_t* sock = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (sock == NULL) {
      closesocket(fd);
      return NULL;
    }
    sock->fd = fd;
    sock->family = family;
    return sock;
  }

  void platform_socket_destroy(platform_socket_t* sock) {
    if (sock == NULL) {
      return;
    }
    closesocket(sock->fd);
    free(sock);
  }

  int platform_socket_fd(platform_socket_t* sock) {
    if (sock == NULL) return -1;
    return (int)(intptr_t)sock->fd;
  }

  static int _address_to_native(const platform_address_t* addr,
                                 struct sockaddr_storage* storage, socklen_t* addrlen) {
    memset(storage, 0, sizeof(*storage));
    switch (addr->family) {
      case PLATFORM_AF_INET: {
        struct sockaddr_in* sin = (struct sockaddr_in*)storage;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->inet.port);
        sin->sin_addr.s_addr = addr->inet.addr;
        *addrlen = sizeof(*sin);
        return 0;
      }
      case PLATFORM_AF_INET6: {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)storage;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(addr->inet6.port);
        memcpy(&sin6->sin6_addr, addr->inet6.addr, 16);
        *addrlen = sizeof(*sin6);
        return 0;
      }
      case PLATFORM_AF_LOCAL: {
        if (!_platform_has_af_unix()) {
          return -1;
        }
        struct sockaddr_un* sun = (struct sockaddr_un*)storage;
        sun->sun_family = AF_UNIX;
        strncpy(sun->sun_path, addr->local.path, sizeof(sun->sun_path) - 1);
        sun->sun_path[sizeof(sun->sun_path) - 1] = '\0';
        *addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                               + strlen(sun->sun_path) + 1);
        return 0;
      }
    }
    return -1;
  }

  int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return bind(sock->fd, (struct sockaddr*)&storage, addrlen) == 0 ? 0 : -1;
  }

  int platform_socket_listen(platform_socket_t* sock, int backlog) {
    return listen(sock->fd, backlog) == 0 ? 0 : -1;
  }

  platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote) {
    struct sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    SOCKET client_fd = accept(sock->fd, (struct sockaddr*)&storage, &addrlen);
    if (client_fd == INVALID_SOCKET) {
      return NULL;
    }
    platform_socket_t* client = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (client == NULL) {
      closesocket(client_fd);
      return NULL;
    }
    client->fd = client_fd;
    client->family = sock->family;
    if (remote != NULL) {
      memset(remote, 0, sizeof(*remote));
      remote->family = sock->family;
    }
    return client;
  }

  int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return connect(sock->fd, (struct sockaddr*)&storage, addrlen) == 0 ? 0 : -1;
  }

  int platform_socket_set_nonblocking(platform_socket_t* sock) {
    unsigned long mode = 1;
    return ioctlsocket(sock->fd, FIONBIO, &mode) == 0 ? 0 : -1;
  }

  int platform_socket_set_reuseaddr(platform_socket_t* sock) {
    int opt = 1;
    return setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR,
                      (const char*)&opt, sizeof(opt)) == 0 ? 0 : -1;
  }

  ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len) {
    int result = send(sock->fd, (const char*)buf, (int)len, 0);
    return (ssize_t)result;
  }

  ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len) {
    int result = recv(sock->fd, (char*)buf, (int)len, 0);
    return (ssize_t)result;
  }

  int platform_socket_shutdown(platform_socket_t* sock, int how) {
    int native_how;
    switch (how) {
      case PLATFORM_SHUT_RD:   native_how = SD_RECEIVE; break;
      case PLATFORM_SHUT_WR:   native_how = SD_SEND; break;
      case PLATFORM_SHUT_RDWR: native_how = SD_BOTH; break;
      default: return -1;
    }
    return shutdown(sock->fd, native_how) == 0 ? 0 : -1;
  }

  int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port) {
    memset(addr, 0, sizeof(*addr));
    struct in_addr in4;
    if (inet_pton(AF_INET, host, &in4) == 1) {
      addr->family = PLATFORM_AF_INET;
      addr->inet.addr = in4.s_addr;
      addr->inet.port = port;
      return 0;
    }
    struct in6_addr in6;
    if (inet_pton(AF_INET6, host, &in6) == 1) {
      addr->family = PLATFORM_AF_INET6;
      memcpy(addr->inet6.addr, &in6, 16);
      addr->inet6.port = port;
      return 0;
    }
    return -1;
  }

  int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len) {
    switch (addr->family) {
      case PLATFORM_AF_INET:
        if (inet_ntop(AF_INET, &addr->inet.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_INET6:
        if (inet_ntop(AF_INET6, addr->inet6.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_LOCAL:
        strncpy(buf, addr->local.path, len);
        if (len > 0) {
          buf[len - 1] = '\0';
        }
        return 0;
    }
    return -1;
  }
#endif
```

- [ ] **Step 2: Build verification**

Check that the code compiles on Linux (the POSIX `#ifndef _WIN32` branch is unchanged):

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -20
```

Expected: Build succeeds (compiles the `#ifndef _WIN32` POSIX branch, Windows branch is ifdef'd out).

- [ ] **Step 3: Run socket-related tests**

```bash
cd build && ./test/testliboffs --gtest_filter="*Platform*:*Socket*:*Transport*:*Unix*:*TCP*" 2>&1
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/Platform/platform_socket.c
git commit -m "feat: implement Windows Winsock2 backend for platform_socket"
```

---

### Task 5: Implement Windows platform_local.c

**Files:**
- Modify: `src/Platform/platform_local.c:55-57`

- [ ] **Step 1: Replace the Windows stub with AF_UNIX implementation**

Replace lines 55-57 (`#else` / `/* Windows implementation deferred */` / `#endif`) with:

```c
#else
  /* Windows implementation — AF_UNIX (Windows 10 1803+) */
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <afunix.h>
  #include <stdlib.h>
  #include <string.h>

  #include "platform_internal.h"
  #include "../../Util/log.h"

  platform_socket_t* platform_local_listen(const char* path) {
    if (!_platform_has_af_unix()) {
      log_warn("platform_local_listen: AF_UNIX not available on this Windows version");
      return NULL;
    }
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    platform_file_unlink(path); /* remove stale socket file */

    if (platform_socket_bind(sock, &addr) != 0 ||
        platform_socket_listen(sock, 128) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  platform_socket_t* platform_local_accept(platform_socket_t* listener) {
    return platform_socket_accept(listener, NULL);
  }

  platform_socket_t* platform_local_connect(const char* path) {
    if (!_platform_has_af_unix()) {
      log_warn("platform_local_connect: AF_UNIX not available on this Windows version");
      return NULL;
    }
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    if (platform_socket_connect(sock, &addr) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  void platform_local_cleanup(const char* path) {
    platform_file_unlink(path);
  }
#endif
```

- [ ] **Step 2: Build verification**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

Expected: Build succeeds.

- [ ] **Step 3: Run local socket tests**

```bash
cd build && ./test/testliboffs --gtest_filter="*Unix*:*Local*:*Platform*" 2>&1
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/Platform/platform_local.c
git commit -m "feat: implement Windows AF_UNIX backend for platform_local"
```

---

### Task 6: Implement Windows platform_file.c

**Files:**
- Modify: `src/Platform/platform_file.c:75-77`

- [ ] **Step 1: Replace the Windows stub with Win32 implementation**

Replace lines 75-77 (`#else` / `/* Windows implementation deferred */` / `#endif`) with:

```c
#else
  /* Windows implementation — Win32 File API */
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <stdlib.h>
  #include <string.h>
  #include <errno.h>

  struct platform_file_t {
    HANDLE handle;
  };

  platform_file_t* platform_file_open(const char* path, int flags, int mode) {
    (void)mode;
    DWORD dwDesiredAccess = 0;
    DWORD dwCreationDisposition = OPEN_EXISTING;

    if (flags & PLATFORM_O_RDWR) {
      dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
    } else if (flags & PLATFORM_O_WRONLY) {
      dwDesiredAccess = GENERIC_WRITE;
    } else {
      dwDesiredAccess = GENERIC_READ;
    }

    if (flags & PLATFORM_O_CREAT) {
      if (flags & PLATFORM_O_TRUNC) {
        dwCreationDisposition = CREATE_ALWAYS;
      } else {
        dwCreationDisposition = OPEN_ALWAYS;
      }
    } else if (flags & PLATFORM_O_TRUNC) {
      dwCreationDisposition = TRUNCATE_EXISTING;
    }

    /* Convert UTF-8 path to wide string */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    HANDLE h = CreateFileW(wpath, dwDesiredAccess,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, dwCreationDisposition,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
      return NULL;
    }
    platform_file_t* file = (platform_file_t*)calloc(1, sizeof(platform_file_t));
    if (file == NULL) {
      CloseHandle(h);
      return NULL;
    }
    file->handle = h;
    return file;
  }

  void platform_file_close(platform_file_t* file) {
    if (file == NULL) return;
    CloseHandle(file->handle);
    free(file);
  }

  ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count) {
    DWORD bytes_read = 0;
    if (!ReadFile(file->handle, buf, (DWORD)count, &bytes_read, NULL)) {
      return -1;
    }
    return (ssize_t)bytes_read;
  }

  ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count) {
    DWORD bytes_written = 0;
    if (!WriteFile(file->handle, buf, (DWORD)count, &bytes_written, NULL)) {
      return -1;
    }
    return (ssize_t)bytes_written;
  }

  ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset) {
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = (DWORD)(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = (DWORD)(offset >> 32);
    DWORD bytes_read = 0;
    if (!ReadFile(file->handle, buf, (DWORD)count, &bytes_read, &overlapped)) {
      return -1;
    }
    return (ssize_t)bytes_read;
  }

  ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset) {
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = (DWORD)(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = (DWORD)(offset >> 32);
    DWORD bytes_written = 0;
    if (!WriteFile(file->handle, buf, (DWORD)count, &bytes_written, &overlapped)) {
      return -1;
    }
    return (ssize_t)bytes_written;
  }

  int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence) {
    DWORD move_method;
    switch (whence) {
      case PLATFORM_SEEK_SET: move_method = FILE_BEGIN; break;
      case PLATFORM_SEEK_CUR: move_method = FILE_CURRENT; break;
      case PLATFORM_SEEK_END: move_method = FILE_END; break;
      default: return -1;
    }
    LARGE_INTEGER li_offset;
    li_offset.QuadPart = offset;
    LARGE_INTEGER li_new;
    if (!SetFilePointerEx(file->handle, li_offset, &li_new, move_method)) {
      return -1;
    }
    return (int64_t)li_new.QuadPart;
  }

  int platform_file_exists(const char* path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return 0;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    DWORD attrs = GetFileAttributesW(wpath);
    free(wpath);
    return (attrs != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
  }

  int platform_file_unlink(const char* path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return -1;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return -1;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    int result = DeleteFileW(wpath) ? 0 : -1;
    free(wpath);
    return result;
  }

  /* Recursive mkdir using CreateDirectoryW */
  static int _mkdir_p_win(const char* path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return -1;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return -1;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    /* Walk the path creating each component */
    for (int i = 0; wpath[i] != L'\0'; i++) {
      if (wpath[i] == L'\\' || wpath[i] == L'/') {
        if (i == 0) continue; /* skip leading slash */
        WCHAR saved = wpath[i];
        wpath[i] = L'\0';
        CreateDirectoryW(wpath, NULL); /* ignore error if already exists */
        wpath[i] = saved;
      }
    }
    /* Create the final component */
    int result = CreateDirectoryW(wpath, NULL) ? 0 : -1;
    if (result != 0 && GetLastError() == ERROR_ALREADY_EXISTS) {
      result = 0;
    }
    free(wpath);
    return result;
  }

  int platform_mkdir(const char* path) {
    return _mkdir_p_win(path);
  }
#endif
```

- [ ] **Step 2: Build verification**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

Expected: Build succeeds.

- [ ] **Step 3: Run file-related tests**

```bash
cd build && ./test/testliboffs --gtest_filter="*File*:*Block*:*Cache*:*Section*:*Stream*:*WAL*" 2>&1
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/Platform/platform_file.c
git commit -m "feat: implement Windows Win32 backend for platform_file"
```

---

### Task 7: Final Verification

- [ ] **Step 1: Run full test suite**

```bash
cd build && ./test/testliboffs 2>&1 | tail -30
```

Expected: All tests pass, no regressions.

- [ ] **Step 2: Verify no compiler warnings on Linux**

```bash
cd build && make -j$(nproc) 2>&1 | grep -i "warning\|error" || echo "No warnings"
```

Expected: No warnings or errors.

- [ ] **Step 3: Final commit (if any cleanup needed)**

```bash
git status
git log --oneline -5
```

---

### Task 8: Close Windows Port Harmony Tickets

- [ ] **Step 1: Start and close the three Windows port tickets**

```bash
H="/home/victor/.claude/skills/harmony/harmony"

# Find ticket numbers
$H ticket list --epic "Platform Maturity"

# Start and close each
$H ticket start <TICKET-NUM>
$H ticket close <TICKET-NUM> "Implemented Windows Winsock2 backend for platform_socket (all functions: create, bind, listen, accept, connect, send, recv, shutdown, destroy, fd, set_nonblocking, set_reuseaddr, address_parse, address_to_string)"

# Repeat for local and file tickets
```

Expected: Tickets closed, harmony required_actions followed.
```

Wait, let me also check the mkdir_p extern since platform_file.c uses it.</think>Let me check one more detail before writing the plan.

<｜DSML｜tool_calls>
<｜DSML｜invoke name="Read">
<｜DSML｜parameter name="file_path" string="true">/home/victor/Workspace/src/github.com/vijayee/liboffs/src/Util/mkdir_p.c