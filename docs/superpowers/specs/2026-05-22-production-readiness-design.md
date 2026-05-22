# Production Readiness — Design

Date: 2026-05-22

## Overview

Audit of the `ClientAPI` branch identified 14 gaps spanning security, platform maturity,
observability, reliability, and protocol maturity. This document defines the remediation
plan: a blockers document, harmony epics/tickets for tracking, and immediate implementation
of the Windows Platform layer.

## Deliverable 1: Production Blockers Document

**File:** `docs/PRODUCTION_BLOCKERS.md`

Three severity tiers:

**Critical (production blockers):**
- TLS certificate validation disabled globally (all QUIC connections use `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`)
- No authentication or authorization mechanism
- Windows Platform layer incomplete (socket, local, file modules stubbed)

**High (significant concerns):**
- No monitoring, metrics, or health checks
- No graceful shutdown / connection draining
- No client-side retry or backoff
- No input validation on external data
- No wire protocol versioning

**Medium (should fix before broad deployment):**
- No HTTP-level rate limiting
- No encryption at rest for block cache
- Fragile build paths (relative path to poll-dancer)
- No configuration validation
- Bootstrap relies on static peer lists only

## Deliverable 2: Harmony Epics and Tickets

| Epic | Tickets |
|------|---------|
| Security Hardening | TLS cert validation, Auth framework, Encryption at rest |
| Platform Maturity | Windows socket port, Windows local port, Windows file port, Reproducible builds |
| Observability | Structured logging/metrics, Health check endpoint |
| Reliability | Graceful shutdown, Client retry/backoff, Input validation, Config validation |
| Protocol Maturity | Wire protocol versioning, HTTP rate limiting, DNS/mDNS bootstrap |

## Deliverable 3: Windows Platform Port

Target: MSVC only, consistent with existing `platform_thread.c` and `platform_compiler.h`.

All three files follow the existing `#ifdef _WIN32` / `#else` / `#endif` pattern with
POSIX implementation in the `#else` branch.

### platform_socket.c (Winsock2)

Maps each `platform_socket_*` function to its Winsock2 equivalent:

| Function | Windows API |
|----------|------------|
| `platform_socket_create` | `socket()` after `_platform_winsock_init()` |
| `platform_socket_bind` | `bind()` |
| `platform_socket_listen` | `listen()` |
| `platform_socket_accept` | `accept()` |
| `platform_socket_connect` | `connect()` |
| `platform_socket_send` | `send()` |
| `platform_socket_recv` | `recv()` |
| `platform_socket_shutdown` | `shutdown()` |
| `platform_socket_destroy` | `closesocket()` |
| `platform_socket_fd` | cast `SOCKET` through `intptr_t` to `int` |
| `platform_socket_set_nonblocking` | `ioctlsocket()` with `FIONBIO` |
| `platform_socket_set_reuseaddr` | `setsockopt()` with `SO_REUSEADDR` |
| `platform_address_parse` | `inet_pton()` / `getaddrinfo()` |

### platform_local.c (AF_UNIX)

Windows 10 build 1803+ supports `AF_UNIX`. Uses the existing `_platform_has_af_unix()`
detector from `platform_internal.h`. Returns NULL with a log message if unavailable.

Single function: `platform_local_connect()` using `socket(AF_UNIX, SOCK_STREAM, 0)` +
`connect()` with `struct sockaddr_un`.

### platform_file.c (Win32 File API)

| Function | Windows API |
|----------|------------|
| `platform_file_open` | `CreateFileW()` with flag mapping |
| `platform_file_read` | `ReadFile()` |
| `platform_file_write` | `WriteFile()` |
| `platform_file_close` | `CloseHandle()` |
| `platform_file_seek` | `SetFilePointerEx()` |
| `platform_file_size` | `GetFileSizeEx()` |
| `platform_file_exists` | `GetFileAttributesW()` |
| `platform_file_delete` | `DeleteFileW()` |
| `mkdir_p` | `CreateDirectoryW()` recursive |
| `rm_rf` | `DeleteFileW()` / `RemoveDirectoryW()` recursive |
