# Cross-Platform Abstraction Layer Design

## Overview

Create `src/Platform/` — a family of `platform_*` headers that provide a single cross-platform API for every non-portable operation in the codebase. Target platforms: Linux, macOS, Windows (MSVC).

Each header uses `#ifdef _WIN32` internally to dispatch to Win32/MSVC APIs or POSIX. Callers never include platform-specific headers or use `#ifdef` — all branching lives inside `Platform/`.

poll-dancer already provides cross-platform event loops, watchers, timers, and async wake-up. This design covers everything else: sockets, threading, file I/O, time, RNG, process info, compiler attributes, and local transport.

## Design Rules

1. **Each header is self-contained** — includes only `<stdint.h>`/`<stddef.h>` plus the platform internals it needs.
2. **`platform.h` is the umbrella** — `#include "Platform/platform.h"` brings in everything.
3. **No `#ifdef` in callers** — all platform branching lives inside `Platform/`.
4. **Opaque types for platform handles** — `platform_socket_t`, `platform_file_t`, `platform_mutex_t`, etc. hide the underlying OS types.
5. **Extend existing patterns** — `platform_thread.h` merges and fixes `threadding.h`, `platform_file.h` wraps the file operations already partially guarded in `wal.c`/`section.c`.

## Files

### New: `src/Platform/`

| File | Purpose |
|------|---------|
| `platform.h` | Umbrella include |
| `platform_internal.h` | Shared WSAStartup init, AF_UNIX capability detection, platform name |
| `platform_compiler.h` | `PLATFORM_NORETURN`, `PLATFORM_UNUSED`, `PLATFORM_PACKED`, `PLATFORM_ALIGNED(n)`, `PLATFORM_PRINTF(f,a)`, `PLATFORM_FFS(x)`, `PLATFORM_DIAGNOSTIC_PUSH/POP/IGNORE` |
| `platform_time.h` | `platform_sleep_ms()`, `platform_monotonic_ns()` |
| `platform_random.h` | `platform_random_bytes()` |
| `platform_process.h` | `platform_getpid()`, `platform_core_count()` |
| `platform_file.h` | Opaque `platform_file_t`, open/close/read/write/pread/pwrite/seek, `platform_mkdir()`, `platform_file_unlink()`, `platform_file_exists()` |
| `platform_socket.h` | Opaque `platform_socket_t`, `platform_address_t` (tagged union: IPv4/IPv6/local), socket create/bind/listen/accept/connect, send/recv/shutdown, nonblocking, reuseaddr, address parse/stringify |
| `platform_socket.c` | Platform-specific socket implementations |
| `platform_thread.h` | Opaque `platform_thread_t`/`platform_mutex_t`/`platform_rwlock_t`/`platform_condvar_t`/`platform_barrier_t`, thread create/join/detach, `platform_thread_setup_stack()` |
| `platform_thread.c` | Platform-specific threading (mmap/sigaltstack on POSIX, no-op on Windows) |
| `platform_local.h` | `platform_local_listen/accept/connect/cleanup` — AF_UNIX on POSIX/Win10 17063+, named pipes on older Windows |
| `platform_local.c` | AF_UNIX detection + named pipe fallback |

### Deleted

- `src/Util/threadding.h` — merged into `platform_thread.h`
- `src/Util/threadding.c` — merged into `platform_thread.c`

## API Reference

### platform_compiler.h

```c
#define PLATFORM_NORETURN           /* __declspec(noreturn) vs __attribute__((noreturn)) */
#define PLATFORM_UNUSED             /* suppresses unused warnings */
#define PLATFORM_PACKED             /* tight struct packing */
#define PLATFORM_ALIGNED(n)         /* alignment specification */
#define PLATFORM_PRINTF(f, a)       /* printf format checking (GCC/Clang only) */
#define PLATFORM_FFS(x)             /* find first set bit: _BitScanForward vs __builtin_ffs */

#define PLATFORM_DIAGNOSTIC_PUSH
#define PLATFORM_DIAGNOSTIC_POP
#define PLATFORM_DIAGNOSTIC_IGNORE(warning)
```

### platform_time.h

```c
void platform_sleep_ms(unsigned int ms);
uint64_t platform_monotonic_ns(void);
```

### platform_random.h

```c
int platform_random_bytes(uint8_t* buf, size_t len);
/* Returns 0 on success, -1 on failure.
   POSIX: getentropy(). Windows: BCryptGenRandom(). */
```

### platform_process.h

```c
int platform_getpid(void);
int platform_core_count(void);
```

### platform_file.h

```c
typedef struct platform_file_t platform_file_t;

/* Open flags */
#define PLATFORM_O_RDONLY  ...
#define PLATFORM_O_RDWR    ...
#define PLATFORM_O_CREAT   ...
#define PLATFORM_O_BINARY  ...   /* no-op on POSIX, _O_BINARY on Windows */

platform_file_t* platform_file_open(const char* path, int flags, int mode);
void platform_file_close(platform_file_t* file);
ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count);
ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count);
ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset);
ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset);
int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence);

int platform_file_exists(const char* path);
int platform_file_unlink(const char* path);
int platform_mkdir(const char* path);  /* recursive */
```

On Windows, `pread`/`pwrite` are emulated via `_lseeki64` + `_read`/`_write` since Windows has no positional I/O. The opaque `platform_file_t` wraps `HANDLE` on Windows and `int fd` on POSIX.

### platform_socket.h

```c
typedef enum { PLATFORM_AF_INET, PLATFORM_AF_INET6, PLATFORM_AF_LOCAL } platform_address_family_e;

typedef struct {
    platform_address_family_e family;
    union {
        struct { uint32_t addr; uint16_t port; } inet;
        struct { uint8_t addr[16]; uint16_t port; } inet6;
        struct { char path[108]; } local;
    };
} platform_address_t;

typedef struct platform_socket_t platform_socket_t;

/* Lifecycle */
platform_socket_t* platform_socket_create(platform_address_family_e family, int stream);
void platform_socket_destroy(platform_socket_t* sock);
int platform_socket_fd(platform_socket_t* sock);  /* for poll-dancer */

/* Server */
int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr);
int platform_socket_listen(platform_socket_t* sock, int backlog);
platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote);

/* Client */
int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr);

/* Configuration */
int platform_socket_set_nonblocking(platform_socket_t* sock);
int platform_socket_set_reuseaddr(platform_socket_t* sock);

/* I/O */
ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len);
ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len);
int platform_socket_shutdown(platform_socket_t* sock, int how);

/* Address helpers */
int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port);
int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len);
```

Key behaviors:
- `platform_socket_fd()` returns `int` for poll-dancer. On Windows, casts `SOCKET` to `int` — poll-dancer's IOCP backend handles this.
- `platform_socket_send()` handles `MSG_NOSIGNAL` (Linux) / `SO_NOSIGPIPE` (macOS) internally. Windows needs neither.
- `platform_socket_set_nonblocking()` uses `ioctlsocket(FIONBIO)` on Windows, `fcntl(O_NONBLOCK)` on POSIX.
- WSAStartup is a one-shot init internal to the implementation.
- `PLATFORM_SHUT_RD`/`PLATFORM_SHUT_WR`/`PLATFORM_SHUT_RDWR` constants hide `SHUT_RD` vs `SD_RECEIVE` etc.

### platform_thread.h

```c
typedef struct platform_thread_t platform_thread_t;
typedef struct platform_mutex_t platform_mutex_t;
typedef struct platform_rwlock_t platform_rwlock_t;
typedef struct platform_condvar_t platform_condvar_t;
typedef struct platform_barrier_t platform_barrier_t;

typedef void* (*platform_thread_fn_t)(void* arg);

/* Thread */
platform_thread_t* platform_thread_create(platform_thread_fn_t fn, void* arg);
void* platform_thread_join(platform_thread_t* thread);
void platform_thread_detach(platform_thread_t* thread);
uint64_t platform_thread_self(void);
int platform_thread_setup_stack(void);

/* Mutex */
platform_mutex_t* platform_mutex_create(void);
void platform_mutex_destroy(platform_mutex_t* m);
void platform_mutex_lock(platform_mutex_t* m);
void platform_mutex_unlock(platform_mutex_t* m);

/* RWLock */
platform_rwlock_t* platform_rwlock_create(void);
void platform_rwlock_destroy(platform_rwlock_t* rw);
void platform_rwlock_read_lock(platform_rwlock_t* rw);
void platform_rwlock_read_unlock(platform_rwlock_t* rw);
void platform_rwlock_write_lock(platform_rwlock_t* rw);
void platform_rwlock_write_unlock(platform_rwlock_t* rw);

/* Condition variable */
platform_condvar_t* platform_condvar_create(void);
void platform_condvar_destroy(platform_condvar_t* cv);
void platform_condvar_wait(platform_condvar_t* cv, platform_mutex_t* m);
void platform_condvar_signal(platform_condvar_t* cv);
void platform_condvar_broadcast(platform_condvar_t* cv);

/* Barrier */
platform_barrier_t* platform_barrier_create(unsigned int count);
void platform_barrier_destroy(platform_barrier_t* b);
int platform_barrier_wait(platform_barrier_t* b);
```

Design decisions:
- **Opaque heap-allocated types** instead of the current `PLATFORMLOCKTYPE(name)` macro that declares stack variables. Struct members go from `PLATFORMLOCKTYPE(lock)` to `platform_mutex_t* lock`. Init goes from `platform_lock_init(&s->lock)` to `s->lock = platform_mutex_create()`.
- **`platform_thread_self` returns `uint64_t`** on all platforms. The current `threadding.h` returns `int` on Windows (`GetCurrentThreadId()`) but `pthread_t` on POSIX.
- **`platform_thread_create`** is new — currently every file duplicates `#ifdef _WIN32`/`CreateThread` vs `pthread_create`.
- **`platform_thread_setup_stack`** — the mmap/sigaltstack logic stays in the implementation. POSIX side uses `mmap(MAP_PRIVATE|MAP_ANONYMOUS)` + `sigaltstack()`. Windows is a no-op (returns 0).
- Fixes the known `threadding.h` bugs: `DeleteCriticalSection` extra argument, wrong `pthread_rwlock_t*` in Win32 branch, `PSRW*` typo, missing `platform_condition_*` on Windows.

### platform_local.h

```c
platform_socket_t* platform_local_listen(const char* path);
platform_socket_t* platform_local_accept(platform_socket_t* listener);
platform_socket_t* platform_local_connect(const char* path);
void platform_local_cleanup(const char* path);
```

The returned `platform_socket_t*` supports the full `platform_socket_*` API. Backend selection:
- **POSIX**: `AF_UNIX` / `SOCK_STREAM` always.
- **Windows**: runtime detection of AF_UNIX support (Win10 build 17063+). If available, uses `socket(AF_UNIX, SOCK_STREAM, 0)` via Winsock. If not, uses named pipes (`CreateNamedPipe`/`ConnectNamedPipe`/`ReadFile`/`WriteFile`).
- Detection result is cached for the process lifetime.
- `platform_local_cleanup()` calls `unlink()` on POSIX, `DeleteFile()` on Windows (named pipe path).

## Migration Map

### Per-file changes

| Current pattern | Replacement |
|-----------------|-------------|
| `#include <sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<sys/un.h>`, `<unistd.h>`, `<fcntl.h>` | `#include "Platform/platform.h"` |
| `PLATFORMLOCKTYPE(name)` (struct member) | `platform_mutex_t* name` |
| `platform_lock_init(&s->lock)` | `s->lock = platform_mutex_create()` |
| `platform_lock_destroy(&s->lock)` | `platform_mutex_destroy(s->lock)` |
| `platform_lock(&s->lock)` | `platform_mutex_lock(s->lock)` |
| `platform_unlock(&s->lock)` | `platform_mutex_unlock(s->lock)` |
| `pthread_create(&t, NULL, fn, arg)` | `platform_thread_create(fn, arg)` |
| `pthread_join(t, NULL)` | `platform_thread_join(thread)` |
| `pthread_mutex_lock/unlock/init/destroy` (direct) | `platform_mutex_*()` |
| `pthread_cond_wait/signal/init/destroy` (direct) | `platform_condvar_*()` |
| `int fd = socket(AF_INET, SOCK_STREAM, 0)` | `platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1)` |
| `bind(fd, ...)` / `listen(fd, ...)` | `platform_socket_bind(sock, ...)` / `platform_socket_listen(sock, ...)` |
| `accept(fd, ...)` | `platform_socket_accept(sock, ...)` |
| `close(fd)` (on sockets) | `platform_socket_destroy(sock)` |
| `send(fd, buf, len, MSG_NOSIGNAL)` | `platform_socket_send(sock, buf, len)` |
| `recv(fd, buf, len, 0)` | `platform_socket_recv(sock, buf, len)` |
| `shutdown(fd, SHUT_RDWR)` | `platform_socket_shutdown(sock, PLATFORM_SHUT_RDWR)` |
| `fcntl(fd, F_SETFL, O_NONBLOCK)` | `platform_socket_set_nonblocking(sock)` |
| `setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, ...)` | `platform_socket_set_reuseaddr(sock)` |
| `struct sockaddr_in addr; addr.sin_family = AF_INET; ...` | `platform_address_t addr; platform_address_parse(&addr, host, port)` |
| `connect(fd, ...)` (TCP/Unix) | `platform_socket_connect(sock, &addr)` or `platform_local_connect(path)` |
| `usleep(us)` | `platform_sleep_ms(ms)` |
| `nanosleep(...)` | `platform_sleep_ms(ms)` |
| `clock_gettime(CLOCK_MONOTONIC, &ts)` | `platform_monotonic_ns()` |
| `fopen("/dev/urandom", "rb")` / `getentropy()` | `platform_random_bytes(buf, len)` |
| `getpid()` | `platform_getpid()` |
| `sysconf(_SC_NPROCESSORS_ONLN)` | `platform_core_count()` |
| `open(path, O_RDWR\|O_CREAT, 0644)` | `platform_file_open(path, PLATFORM_O_RDWR\|PLATFORM_O_CREAT, 0644)` |
| `close(fd)` (on files) | `platform_file_close(file)` |
| `read(fd, ...)` / `write(fd, ...)` | `platform_file_read(file, ...)` / `platform_file_write(file, ...)` |
| `pread(fd, ...)` / `pwrite(fd, ...)` | `platform_file_pread(file, ...)` / `platform_file_pwrite(file, ...)` |
| `lseek(fd, ...)` | `platform_file_seek(file, ...)` |
| `unlink(path)` | `platform_file_unlink(path)` |
| `mkdir(path, S_IRWXU)` | `platform_mkdir(path)` |
| `__attribute__((unused))` | `PLATFORM_UNUSED` |
| `__builtin_ffs(x)` | `PLATFORM_FFS(x)` |
| `#pragma GCC diagnostic push/pop/ignored` | `PLATFORM_DIAGNOSTIC_PUSH/POP/IGNORE` |
| `signal(SIGPIPE, SIG_IGN)` | Remove — `platform_socket_send()` handles this internally |
| `#include "threadding.h"` | `#include "Platform/platform.h"` |
| `#include <poll.h>` | `#include <poll-dancer/poll-dancer.h>` |

### Files impacted (by category)

**Heavy changes (socket + threading):**
- `src/ClientAPI/HTTP/http_server.c`, `http_connection.c`
- `src/ClientAPI/WS/ws_transport.c`, `ws_connection.c`
- `src/ClientAPI/TCP/tcp_transport.c`, `tcp_connection.c`
- `src/ClientAPI/Unix/unix_transport.c`, `unix_connection.c`
- `src/ClientAPI/WT/wt_transport.c`
- `src/ClientLibs/c/offs_client.c`
- `src/Network/quic_listener.c`, `relay_client.c`
- `src/Network/Relay/relay_server.c`, `relay_server_main.c`
- `src/Timer/timer_actor.c`
- `src/Actor/pool.c`

**File I/O changes:**
- `src/BlockCache/wal.c`, `section.c`, `index.c`, `sections.c`
- `src/Streams/file-stream.c`
- `src/Util/mkdir_p.c`, `rm_rf.c`

**Threading-only changes:**
- `src/Scheduler/scheduler.c`

**Compiler attribute changes:**
- `src/Network/quic_listener.c`, `relay_client.c`, `quic_peer_send.c`
- `src/Network/Relay/relay_server.c`
- `src/BlockCache/section.c`
- `src/OFFStreams/tuple_cache.c`, `ofd_cache.c`
- `src/BlockCache/sections.c`, `block_cache.c`, `index.c`

**Test files:**
- `test/test_node_main.c`, `test/test_offs_client.cpp`, `test/test_ws_transport.cpp`, etc.

## Thread Safety

The `platform_*` APIs are not internally thread-safe unless documented. Callers are responsible for synchronization. The exception is `platform_socket_send()` on the same socket from multiple threads — platform behavior matches the underlying OS (undefined on Windows, atomic on Linux for small writes).

## Error Handling

All functions that can fail return an error indicator:
- Functions returning pointers: `NULL` on failure.
- Functions returning `int`: `-1` on failure.
- Functions returning `ssize_t`: `-1` on failure.
- `platform_address_parse`: returns `-1` on malformed input.

No `errno` inspection by callers — error details are internal to the platform implementation. Logging of platform errors happens inside `Platform/` via the existing `log.h`.

## Testing

Each `platform_*` module gets a corresponding test file:
- `test/test_platform_socket.cpp`
- `test/test_platform_thread.cpp`
- `test/test_platform_file.cpp`
- `test/test_platform_time.cpp`
- `test/test_platform_random.cpp`
- `test/test_platform_local.cpp`

Tests verify correctness on the current platform. Cross-platform verification happens via CI on Linux, macOS, and Windows runners.

## Out of Scope

- IPv6 support in transport servers (the address API supports it, but transports remain IPv4-only for now)
- SChannel TLS support (OpenSSL is assumed available on all platforms)
- poll-dancer changes (the library already supports all three platforms)
- MsQuic changes (MsQuic is already cross-platform)
- Porting `src/Util/get_dir.c` (uses `opendir`/`readdir`) — low-priority, used only in one place
- Windows CI/CD pipeline setup
