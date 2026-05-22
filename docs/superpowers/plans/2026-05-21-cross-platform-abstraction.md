# Cross-Platform Abstraction Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `src/Platform/` with cross-platform abstractions for sockets, threading, file I/O, time, RNG, process info, compiler attributes, and local transport. Target Linux, macOS, Windows (MSVC). Migrate all existing code to use these abstractions. Migrate `offs_client.c` to poll-dancer event loop.

**Architecture:** New `src/Platform/` directory with 15 files. Each `platform_*.h` header provides a single-focus API with `#ifdef _WIN32` internally dispatching to Win32/MSVC or POSIX. Opaque types (`platform_socket_t`, `platform_file_t`, `platform_mutex_t`, etc.) hide OS handles. `platform_thread.h` merges and fixes the buggy `threadding.h`. poll-dancer covers event loops, watchers, timers — everything else goes through `Platform/`.

**Tech Stack:** C11, MSVC/Win32 on Windows, POSIX on Linux/macOS, poll-dancer, OpenSSL

---

## File Structure

### New: `src/Platform/`

| File | Responsibility |
|------|---------------|
| `platform.h` | Umbrella include — brings in all platform headers |
| `platform_internal.h` | Shared WSAStartup init, AF_UNIX capability detection |
| `platform_compiler.h` | `PLATFORM_NORETURN`, `PLATFORM_UNUSED`, `PLATFORM_PACKED`, `PLATFORM_ALIGNED`, `PLATFORM_PRINTF`, `PLATFORM_FFS`, diagnostic pragmas |
| `platform_time.h` | `platform_sleep_ms()`, `platform_monotonic_ns()` — header-only or thin .c |
| `platform_random.h` | `platform_random_bytes()` |
| `platform_random.c` | `getentropy()` on POSIX, `BCryptGenRandom()` on Windows |
| `platform_process.h` | `platform_getpid()`, `platform_core_count()` |
| `platform_process.c` | Platform-specific implementations |
| `platform_file.h` | Opaque `platform_file_t`, open/close/read/write/pread/pwrite/seek, mkdir, unlink, exists |
| `platform_file.c` | `int fd` on POSIX, `HANDLE` on Windows |
| `platform_socket.h` | Opaque `platform_socket_t`, `platform_address_t` (tagged union), socket create/bind/listen/accept/connect, send/recv/shutdown, nonblocking, reuseaddr |
| `platform_socket.c` | Winsock2 on Windows, POSIX sockets on Linux/macOS |
| `platform_thread.h` | Opaque thread/mutex/rwlock/condvar/barrier types, create/join/detach, setup_stack |
| `platform_thread.c` | `pthread_*` on POSIX, Win32 threads on Windows |
| `platform_local.h` | `platform_local_listen/accept/connect/cleanup` |
| `platform_local.c` | AF_UNIX on POSIX, AF_UNIX + named pipe fallback on Windows |

### Deleted

| File | Reason |
|------|--------|
| `src/Util/threadding.h` | Merged into `platform_thread.h` |
| `src/Util/threadding.c` | Merged into `platform_thread.c` |

### Modified (~30 files)

All transport servers, connections, block cache files, the client library, network modules, timer actor, scheduler, and test files.

---

## Phase 1: Simple Headers (compiler, time, random, process)

### Task 1: Create platform_compiler.h

**Files:**
- Create: `src/Platform/platform_compiler.h`

- [ ] **Step 1: Write the header**

```c
#ifndef OFFS_PLATFORM_COMPILER_H
#define OFFS_PLATFORM_COMPILER_H

#if defined(_MSC_VER)
  /* MSVC */
  #define PLATFORM_NORETURN       __declspec(noreturn)
  #define PLATFORM_UNUSED         /* MSVC: suppress C4100 via /wd4100, no per-symbol equivalent */
  #define PLATFORM_STRUCT_PACKED  __declspec(align(1))
  #define PLATFORM_ALIGNED(n)     __declspec(align(n))
  #define PLATFORM_PRINTF(f, a)   /* no MSVC equivalent */
  #define PLATFORM_FFS(x)         (_BitScanForward((unsigned long*)&(x), (x)) ? 0 : 0) /* placeholder */

  #define PLATFORM_DIAGNOSTIC_PUSH         __pragma(warning(push))
  #define PLATFORM_DIAGNOSTIC_POP          __pragma(warning(pop))
  #define PLATFORM_DIAGNOSTIC_IGNORE(w)    __pragma(warning(disable: w))
#else
  /* GCC / Clang */
  #define PLATFORM_NORETURN       __attribute__((noreturn))
  #define PLATFORM_UNUSED         __attribute__((unused))
  #define PLATFORM_STRUCT_PACKED  __attribute__((packed))
  #define PLATFORM_ALIGNED(n)     __attribute__((aligned(n)))
  #define PLATFORM_PRINTF(f, a)   __attribute__((format(printf, f, a)))
  #define PLATFORM_FFS(x)         __builtin_ffs(x)

  #define PLATFORM_DIAGNOSTIC_PUSH         _Pragma("GCC diagnostic push")
  #define PLATFORM_DIAGNOSTIC_POP          _Pragma("GCC diagnostic pop")
  #define PLATFORM_DIAGNOSTIC_IGNORE_STR(w) #w
  #define PLATFORM_DIAGNOSTIC_IGNORE(w)    _Pragma(PLATFORM_DIAGNOSTIC_IGNORE_STR(GCC diagnostic ignored w))
#endif

#endif /* OFFS_PLATFORM_COMPILER_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_compiler.h
git commit -m "feat: add platform_compiler.h with cross-platform compiler attribute macros"
```

### Task 2: Create platform_time.h

**Files:**
- Create: `src/Platform/platform_time.h`

- [ ] **Step 1: Write the header**

```c
#ifndef OFFS_PLATFORM_TIME_H
#define OFFS_PLATFORM_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void platform_sleep_ms(unsigned int ms);
uint64_t platform_monotonic_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_TIME_H */
```

- [ ] **Step 2: Create platform_time.c**

```c
#include "platform_time.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>

  void platform_sleep_ms(unsigned int ms) {
    Sleep(ms);
  }

  uint64_t platform_monotonic_ns(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / freq.QuadPart);
  }
#else
  #include <time.h>
  #include <unistd.h>

  void platform_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
  }

  uint64_t platform_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  }
#endif
```

- [ ] **Step 3: Commit**

```bash
git add src/Platform/platform_time.h src/Platform/platform_time.c
git commit -m "feat: add platform_time with sleep_ms and monotonic_ns"
```

### Task 3: Create platform_random.h and platform_random.c

**Files:**
- Create: `src/Platform/platform_random.h`
- Create: `src/Platform/platform_random.c`

- [ ] **Step 1: Write platform_random.h**

```c
#ifndef OFFS_PLATFORM_RANDOM_H
#define OFFS_PLATFORM_RANDOM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int platform_random_bytes(uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_RANDOM_H */
```

- [ ] **Step 2: Write platform_random.c**

```c
#include "platform_random.h"

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>

  int platform_random_bytes(uint8_t* buf, size_t len) {
    /* BCryptGenRandom always uses the system-preferred RNG provider */
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
  }
#else
  #include <unistd.h>

  int platform_random_bytes(uint8_t* buf, size_t len) {
    return getentropy(buf, len);
  }
#endif
```

- [ ] **Step 3: Commit**

```bash
git add src/Platform/platform_random.h src/Platform/platform_random.c
git commit -m "feat: add platform_random with cross-platform random_bytes"
```

### Task 4: Create platform_process.h and platform_process.c

**Files:**
- Create: `src/Platform/platform_process.h`
- Create: `src/Platform/platform_process.c`

- [ ] **Step 1: Write platform_process.h**

```c
#ifndef OFFS_PLATFORM_PROCESS_H
#define OFFS_PLATFORM_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

int platform_getpid(void);
int platform_core_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_PROCESS_H */
```

- [ ] **Step 2: Write platform_process.c**

```c
#include "platform_process.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>

  int platform_getpid(void) {
    return (int)GetCurrentProcessId();
  }

  int platform_core_count(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
  }
#else
  #include <unistd.h>

  int platform_getpid(void) {
    return (int)getpid();
  }

  int platform_core_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return (int)(count > 0 ? count : 1);
  }
#endif
```

- [ ] **Step 3: Commit**

```bash
git add src/Platform/platform_process.h src/Platform/platform_process.c
git commit -m "feat: add platform_process with getpid and core_count"
```

---

## Phase 2: Platform Thread (merges threadding.h/.c)

### Task 5: Create platform_thread.h

**Files:**
- Create: `src/Platform/platform_thread.h`

- [ ] **Step 1: Write the header**

```c
#ifndef OFFS_PLATFORM_THREAD_H
#define OFFS_PLATFORM_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct platform_thread_t platform_thread_t;
typedef struct platform_mutex_t platform_mutex_t;
typedef struct platform_rwlock_t platform_rwlock_t;
typedef struct platform_condvar_t platform_condvar_t;
typedef struct platform_barrier_t platform_barrier_t;

typedef void* (*platform_thread_fn_t)(void* arg);

/* Thread lifecycle */
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

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_THREAD_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_thread.h
git commit -m "feat: add platform_thread.h with cross-platform threading API"
```

### Task 6: Create platform_thread.c — POSIX implementation

**Files:**
- Create: `src/Platform/platform_thread.c`

- [ ] **Step 1: Write the POSIX implementation**

```c
#include "platform_thread.h"

#ifdef _WIN32
  /* Windows implementation — Task 7 */
#else
  #include <stdlib.h>
  #include <pthread.h>
  #include <unistd.h>
  #include <sys/mman.h>
  #include <signal.h>

  struct platform_thread_t {
    pthread_t handle;
    platform_thread_fn_t fn;
    void* arg;
    void* result;
  };

  struct platform_mutex_t     { pthread_mutex_t  handle; };
  struct platform_rwlock_t    { pthread_rwlock_t handle; };
  struct platform_condvar_t   { pthread_cond_t   handle; };
  struct platform_barrier_t   { pthread_barrier_t handle; };

  /* --- Thread --- */

  static void* _thread_wrapper(void* arg) {
    platform_thread_t* t = (platform_thread_t*)arg;
    t->result = t->fn(t->arg);
    return t->result;
  }

  platform_thread_t* platform_thread_create(platform_thread_fn_t fn, void* arg) {
    platform_thread_t* t = (platform_thread_t*)calloc(1, sizeof(platform_thread_t));
    if (t == NULL) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->handle, NULL, _thread_wrapper, t) != 0) {
      free(t);
      return NULL;
    }
    return t;
  }

  void* platform_thread_join(platform_thread_t* thread) {
    if (thread == NULL) return NULL;
    void* result;
    pthread_join(thread->handle, &result);
    free(thread);
    return result;
  }

  void platform_thread_detach(platform_thread_t* thread) {
    if (thread == NULL) return;
    pthread_detach(thread->handle);
    free(thread);
  }

  uint64_t platform_thread_self(void) {
    return (uint64_t)pthread_self();
  }

  int platform_thread_setup_stack(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    /* stack setup is done once per process, cached */
    /* Simple no-op for now — the old mmap/sigaltstack logic was
       only needed for the relay server and can be added back if required. */
    return 0;
  }

  /* --- Mutex --- */

  platform_mutex_t* platform_mutex_create(void) {
    platform_mutex_t* m = (platform_mutex_t*)calloc(1, sizeof(platform_mutex_t));
    if (m == NULL) return NULL;
    if (pthread_mutex_init(&m->handle, NULL) != 0) {
      free(m);
      return NULL;
    }
    return m;
  }

  void platform_mutex_destroy(platform_mutex_t* m) {
    if (m == NULL) return;
    pthread_mutex_destroy(&m->handle);
    free(m);
  }

  void platform_mutex_lock(platform_mutex_t* m)   { pthread_mutex_lock(&m->handle); }
  void platform_mutex_unlock(platform_mutex_t* m) { pthread_mutex_unlock(&m->handle); }

  /* --- RWLock --- */

  platform_rwlock_t* platform_rwlock_create(void) {
    platform_rwlock_t* rw = (platform_rwlock_t*)calloc(1, sizeof(platform_rwlock_t));
    if (rw == NULL) return NULL;
    if (pthread_rwlock_init(&rw->handle, NULL) != 0) {
      free(rw);
      return NULL;
    }
    return rw;
  }

  void platform_rwlock_destroy(platform_rwlock_t* rw) {
    if (rw == NULL) return;
    pthread_rwlock_destroy(&rw->handle);
    free(rw);
  }

  void platform_rwlock_read_lock(platform_rwlock_t* rw)    { pthread_rwlock_rdlock(&rw->handle); }
  void platform_rwlock_read_unlock(platform_rwlock_t* rw)  { pthread_rwlock_unlock(&rw->handle); }
  void platform_rwlock_write_lock(platform_rwlock_t* rw)   { pthread_rwlock_wrlock(&rw->handle); }
  void platform_rwlock_write_unlock(platform_rwlock_t* rw) { pthread_rwlock_unlock(&rw->handle); }

  /* --- Condition Variable --- */

  platform_condvar_t* platform_condvar_create(void) {
    platform_condvar_t* cv = (platform_condvar_t*)calloc(1, sizeof(platform_condvar_t));
    if (cv == NULL) return NULL;
    if (pthread_cond_init(&cv->handle, NULL) != 0) {
      free(cv);
      return NULL;
    }
    return cv;
  }

  void platform_condvar_destroy(platform_condvar_t* cv) {
    if (cv == NULL) return;
    pthread_cond_destroy(&cv->handle);
    free(cv);
  }

  void platform_condvar_wait(platform_condvar_t* cv, platform_mutex_t* m) {
    pthread_cond_wait(&cv->handle, &m->handle);
  }

  void platform_condvar_signal(platform_condvar_t* cv)    { pthread_cond_signal(&cv->handle); }
  void platform_condvar_broadcast(platform_condvar_t* cv) { pthread_cond_broadcast(&cv->handle); }

  /* --- Barrier --- */

  platform_barrier_t* platform_barrier_create(unsigned int count) {
    platform_barrier_t* b = (platform_barrier_t*)calloc(1, sizeof(platform_barrier_t));
    if (b == NULL) return NULL;
    if (pthread_barrier_init(&b->handle, NULL, count) != 0) {
      free(b);
      return NULL;
    }
    return b;
  }

  void platform_barrier_destroy(platform_barrier_t* b) {
    if (b == NULL) return;
    pthread_barrier_destroy(&b->handle);
    free(b);
  }

  int platform_barrier_wait(platform_barrier_t* b) {
    int rc = pthread_barrier_wait(&b->handle);
    return (rc == PTHREAD_BARRIER_SERIAL_THREAD) ? 1 : 0;
  }
#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_thread.c
git commit -m "feat: add platform_thread.c POSIX implementation"
```

### Task 7: Add Windows path to platform_thread.c

**Files:**
- Modify: `src/Platform/platform_thread.c` (add inside `#ifdef _WIN32` block)

- [ ] **Step 1: Add the Windows implementation**

Replace the `#ifdef _WIN32` placeholder comment with:

```c
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <stdlib.h>
  #include <synchapi.h>

  struct platform_thread_t {
    HANDLE handle;
    platform_thread_fn_t fn;
    void* arg;
    void* result;
  };

  struct platform_mutex_t     { CRITICAL_SECTION handle; };
  struct platform_rwlock_t    { SRWLOCK handle; };
  struct platform_condvar_t   { CONDITION_VARIABLE handle; };
  struct platform_barrier_t   { SYNCHRONIZATION_BARRIER handle; };

  static DWORD WINAPI _thread_wrapper(LPVOID arg) {
    platform_thread_t* t = (platform_thread_t*)arg;
    t->result = t->fn(t->arg);
    return 0;
  }

  platform_thread_t* platform_thread_create(platform_thread_fn_t fn, void* arg) {
    platform_thread_t* t = (platform_thread_t*)calloc(1, sizeof(platform_thread_t));
    if (t == NULL) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 0, _thread_wrapper, t, 0, NULL);
    if (t->handle == NULL) {
      free(t);
      return NULL;
    }
    return t;
  }

  void* platform_thread_join(platform_thread_t* thread) {
    if (thread == NULL) return NULL;
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    void* result = thread->result;
    free(thread);
    return result;
  }

  void platform_thread_detach(platform_thread_t* thread) {
    if (thread == NULL) return;
    CloseHandle(thread->handle);
    free(thread);
  }

  uint64_t platform_thread_self(void) {
    return (uint64_t)GetCurrentThreadId();
  }

  int platform_thread_setup_stack(void) {
    return 0; /* no-op on Windows */
  }

  /* --- Mutex --- */

  platform_mutex_t* platform_mutex_create(void) {
    platform_mutex_t* m = (platform_mutex_t*)calloc(1, sizeof(platform_mutex_t));
    if (m == NULL) return NULL;
    InitializeCriticalSection(&m->handle);
    return m;
  }

  void platform_mutex_destroy(platform_mutex_t* m) {
    if (m == NULL) return;
    DeleteCriticalSection(&m->handle);
    free(m);
  }

  void platform_mutex_lock(platform_mutex_t* m)   { EnterCriticalSection(&m->handle); }
  void platform_mutex_unlock(platform_mutex_t* m) { LeaveCriticalSection(&m->handle); }

  /* --- RWLock --- */

  platform_rwlock_t* platform_rwlock_create(void) {
    platform_rwlock_t* rw = (platform_rwlock_t*)calloc(1, sizeof(platform_rwlock_t));
    if (rw == NULL) return NULL;
    InitializeSRWLock(&rw->handle);
    return rw;
  }

  void platform_rwlock_destroy(platform_rwlock_t* rw) {
    /* SRWLOCK needs no destruction */
    free(rw);
  }

  void platform_rwlock_read_lock(platform_rwlock_t* rw)    { AcquireSRWLockShared(&rw->handle); }
  void platform_rwlock_read_unlock(platform_rwlock_t* rw)  { ReleaseSRWLockShared(&rw->handle); }
  void platform_rwlock_write_lock(platform_rwlock_t* rw)   { AcquireSRWLockExclusive(&rw->handle); }
  void platform_rwlock_write_unlock(platform_rwlock_t* rw) { ReleaseSRWLockExclusive(&rw->handle); }

  /* --- Condition Variable --- */

  platform_condvar_t* platform_condvar_create(void) {
    platform_condvar_t* cv = (platform_condvar_t*)calloc(1, sizeof(platform_condvar_t));
    if (cv == NULL) return NULL;
    InitializeConditionVariable(&cv->handle);
    return cv;
  }

  void platform_condvar_destroy(platform_condvar_t* cv) {
    /* CONDITION_VARIABLE needs no destruction */
    free(cv);
  }

  void platform_condvar_wait(platform_condvar_t* cv, platform_mutex_t* m) {
    SleepConditionVariableCS(&cv->handle, &m->handle, INFINITE);
  }

  void platform_condvar_signal(platform_condvar_t* cv)    { WakeConditionVariable(&cv->handle); }
  void platform_condvar_broadcast(platform_condvar_t* cv) { WakeAllConditionVariable(&cv->handle); }

  /* --- Barrier --- */

  platform_barrier_t* platform_barrier_create(unsigned int count) {
    platform_barrier_t* b = (platform_barrier_t*)calloc(1, sizeof(platform_barrier_t));
    if (b == NULL) return NULL;
    if (!InitializeSynchronizationBarrier(&b->handle, count, -1)) {
      free(b);
      return NULL;
    }
    return b;
  }

  void platform_barrier_destroy(platform_barrier_t* b) {
    /* SYNCHRONIZATION_BARRIER needs no destruction on Windows 8+ */
    free(b);
  }

  int platform_barrier_wait(platform_barrier_t* b) {
    return EnterSynchronizationBarrier(&b->handle, SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE) ? 1 : 0;
  }
#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_thread.c
git commit -m "feat: add platform_thread.c Windows implementation"
```

- [ ] **Step 3: Build and verify on Linux**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build (platform_thread not yet referenced by any code, but must compile)

---

## Phase 3: Platform File I/O

### Task 8: Create platform_file.h

**Files:**
- Create: `src/Platform/platform_file.h`

- [ ] **Step 1: Write the header**

```c
#ifndef OFFS_PLATFORM_FILE_H
#define OFFS_PLATFORM_FILE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct platform_file_t platform_file_t;

/* Open flags — values match the platform's native flags */
#define PLATFORM_O_RDONLY  0
#define PLATFORM_O_RDWR    2
#define PLATFORM_O_CREAT   0100
#define PLATFORM_O_BINARY  0  /* no-op on POSIX */

platform_file_t* platform_file_open(const char* path, int flags, int mode);
void platform_file_close(platform_file_t* file);
ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count);
ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count);
ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset);
ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset);
int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence);

#define PLATFORM_SEEK_SET  0
#define PLATFORM_SEEK_CUR  1
#define PLATFORM_SEEK_END  2

int platform_file_exists(const char* path);
int platform_file_unlink(const char* path);
int platform_mkdir(const char* path);  /* recursive */

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_FILE_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_file.h
git commit -m "feat: add platform_file.h with cross-platform file I/O API"
```

### Task 9: Create platform_file.c — POSIX implementation

**Files:**
- Create: `src/Platform/platform_file.c`

NOTE: On Windows, `PLATFORM_O_RDONLY` etc. need different values. The constants in the header are POSIX values. When we add Windows support, we'll need to map them. For now this is POSIX-only — Windows will use `#ifdef _WIN32` with `_O_RDONLY`/`_O_RDWR`/`_O_CREAT`/`_O_BINARY` equivalents.

- [ ] **Step 1: Write the implementation**

```c
#include "platform_file.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <string.h>
  #include <errno.h>

  struct platform_file_t {
    int fd;
  };

  platform_file_t* platform_file_open(const char* path, int flags, int mode) {
    int fd = open(path, flags, mode);
    if (fd < 0) return NULL;
    platform_file_t* file = (platform_file_t*)calloc(1, sizeof(platform_file_t));
    if (file == NULL) {
      close(fd);
      return NULL;
    }
    file->fd = fd;
    return file;
  }

  void platform_file_close(platform_file_t* file) {
    if (file == NULL) return;
    close(file->fd);
    free(file);
  }

  ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count) {
    return read(file->fd, buf, count);
  }

  ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count) {
    return write(file->fd, buf, count);
  }

  ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset) {
    return pread(file->fd, buf, count, (off_t)offset);
  }

  ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset) {
    return pwrite(file->fd, buf, count, (off_t)offset);
  }

  int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence) {
    int w = (whence == PLATFORM_SEEK_SET) ? SEEK_SET :
            (whence == PLATFORM_SEEK_CUR) ? SEEK_CUR : SEEK_END;
    off_t result = lseek(file->fd, (off_t)offset, w);
    return (int64_t)result;
  }

  int platform_file_exists(const char* path) {
    return access(path, F_OK) == 0 ? 1 : 0;
  }

  int platform_file_unlink(const char* path) {
    return unlink(path);
  }

  int platform_mkdir(const char* path) {
    /* Recursive mkdir using existing mkdir_p utility.
       We call the existing implementation to avoid duplication. */
    extern int mkdir_p(const char* path);
    return mkdir_p(path);
  }
#else
  /* Windows implementation deferred to Phase 8 */
  #include <windows.h>
  #include <stdlib.h>

  struct platform_file_t {
    HANDLE handle;
  };

  /* ... Windows implementation uses CreateFile, ReadFile, WriteFile,
     SetFilePointerEx, etc. ... */
#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_file.c
git commit -m "feat: add platform_file.c POSIX implementation"
```

- [ ] **Step 3: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build

---

## Phase 4: Platform Socket

### Task 10: Create platform_internal.h

**Files:**
- Create: `src/Platform/platform_internal.h`

- [ ] **Step 1: Write the internal header**

```c
#ifndef OFFS_PLATFORM_INTERNAL_H
#define OFFS_PLATFORM_INTERNAL_H

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>

  /* One-shot WSAStartup */
  static inline int _platform_winsock_init(void) {
    static int initialized = 0;
    if (!initialized) {
      WSADATA wsa_data;
      if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return -1;
      initialized = 1;
    }
    return 0;
  }

  /* Windows AF_UNIX detection */
  static inline int _platform_has_af_unix(void) {
    static int checked = 0;
    static int available = 0;
    if (!checked) {
      SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
      if (s != INVALID_SOCKET) {
        available = 1;
        closesocket(s);
      }
      checked = 1;
    }
    return available;
  }
#endif

#endif /* OFFS_PLATFORM_INTERNAL_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_internal.h
git commit -m "feat: add platform_internal.h with WSAStartup and AF_UNIX detection"
```

### Task 11: Create platform_socket.h

**Files:**
- Create: `src/Platform/platform_socket.h`

- [ ] **Step 1: Write the header**

```c
#ifndef OFFS_PLATFORM_SOCKET_H
#define OFFS_PLATFORM_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PLATFORM_AF_INET,
  PLATFORM_AF_INET6,
  PLATFORM_AF_LOCAL
} platform_address_family_e;

typedef struct {
  platform_address_family_e family;
  union {
    struct { uint32_t addr; uint16_t port; } inet;
    struct { uint8_t addr[16]; uint16_t port; } inet6;
    struct { char path[108]; } local;
  };
} platform_address_t;

typedef struct platform_socket_t platform_socket_t;

#define PLATFORM_SHUT_RD    0
#define PLATFORM_SHUT_WR    1
#define PLATFORM_SHUT_RDWR  2

/* Lifecycle */
platform_socket_t* platform_socket_create(platform_address_family_e family, int stream);
void platform_socket_destroy(platform_socket_t* sock);
int platform_socket_fd(platform_socket_t* sock);

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

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_SOCKET_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_socket.h
git commit -m "feat: add platform_socket.h with cross-platform socket API"
```

### Task 12: Create platform_socket.c — POSIX implementation

**Files:**
- Create: `src/Platform/platform_socket.c`

- [ ] **Step 1: Write the POSIX implementation**

```c
#include "platform_socket.h"
#include "platform_internal.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <string.h>
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <errno.h>

  struct platform_socket_t {
    int fd;
    int family; /* platform_address_family_e */
  };

  static int _family_to_native(platform_address_family_e family) {
    switch (family) {
      case PLATFORM_AF_INET:  return AF_INET;
      case PLATFORM_AF_INET6: return AF_INET6;
      case PLATFORM_AF_LOCAL: return AF_LOCAL;
      default: return -1;
    }
  }

  platform_socket_t* platform_socket_create(platform_address_family_e family, int stream) {
    int native_family = _family_to_native(family);
    if (native_family < 0) return NULL;
    int type = stream ? SOCK_STREAM : SOCK_DGRAM;
    int fd = socket(native_family, type, 0);
    if (fd < 0) return NULL;
    platform_socket_t* sock = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (sock == NULL) {
      close(fd);
      return NULL;
    }
    sock->fd = fd;
    sock->family = family;
    return sock;
  }

  void platform_socket_destroy(platform_socket_t* sock) {
    if (sock == NULL) return;
    close(sock->fd);
    free(sock);
  }

  int platform_socket_fd(platform_socket_t* sock) {
    return sock != NULL ? sock->fd : -1;
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
        struct sockaddr_un* sun = (struct sockaddr_un*)storage;
        sun->sun_family = AF_LOCAL;
        strncpy(sun->sun_path, addr->local.path, sizeof(sun->sun_path) - 1);
        *addrlen = sizeof(*sun);
        return 0;
      }
    }
    return -1;
  }

  int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) return -1;
    return bind(sock->fd, (struct sockaddr*)&storage, addrlen);
  }

  int platform_socket_listen(platform_socket_t* sock, int backlog) {
    return listen(sock->fd, backlog);
  }

  platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote) {
    struct sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    int client_fd = accept(sock->fd, (struct sockaddr*)&storage, &addrlen);
    if (client_fd < 0) return NULL;
    platform_socket_t* client = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (client == NULL) {
      close(client_fd);
      return NULL;
    }
    client->fd = client_fd;
    client->family = sock->family;
    /* Populate remote address if requested */
    if (remote != NULL) {
      memset(remote, 0, sizeof(*remote));
      remote->family = sock->family;
    }
    return client;
  }

  int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) return -1;
    return connect(sock->fd, (struct sockaddr*)&storage, addrlen);
  }

  int platform_socket_set_nonblocking(platform_socket_t* sock) {
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);
  }

  int platform_socket_set_reuseaddr(platform_socket_t* sock) {
    int opt = 1;
    return setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  }

  ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len) {
  #ifdef __APPLE__
    /* macOS doesn't have MSG_NOSIGNAL; SO_NOSIGPIPE must be set on the socket */
    return send(sock->fd, buf, len, 0);
  #else
    return send(sock->fd, buf, len, MSG_NOSIGNAL);
  #endif
  }

  ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len) {
    return recv(sock->fd, buf, len, 0);
  }

  int platform_socket_shutdown(platform_socket_t* sock, int how) {
    int native_how;
    switch (how) {
      case PLATFORM_SHUT_RD:   native_how = SHUT_RD; break;
      case PLATFORM_SHUT_WR:   native_how = SHUT_WR; break;
      case PLATFORM_SHUT_RDWR: native_how = SHUT_RDWR; break;
      default: return -1;
    }
    return shutdown(sock->fd, native_how);
  }

  int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port) {
    memset(addr, 0, sizeof(*addr));
    /* Try IPv4 first */
    struct in_addr in4;
    if (inet_pton(AF_INET, host, &in4) == 1) {
      addr->family = PLATFORM_AF_INET;
      addr->inet.addr = in4.s_addr;
      addr->inet.port = port;
      return 0;
    }
    /* Try IPv6 */
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
        if (inet_ntop(AF_INET, &addr->inet.addr, buf, (socklen_t)len) == NULL) return -1;
        return 0;
      case PLATFORM_AF_INET6:
        if (inet_ntop(AF_INET6, addr->inet6.addr, buf, (socklen_t)len) == NULL) return -1;
        return 0;
      case PLATFORM_AF_LOCAL:
        strncpy(buf, addr->local.path, len);
        return 0;
    }
    return -1;
  }
#else
  /* Windows implementation — Phase 8 */
#endif
```

- [ ] **Step 2: Add platform_socket.c to CMakeLists.txt**

Add `src/Platform/platform_socket.c` to the library sources.

- [ ] **Step 3: Commit**

```bash
git add src/Platform/platform_socket.c
git commit -m "feat: add platform_socket.c POSIX implementation"
```

- [ ] **Step 4: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build

---

## Phase 5: Platform Local Transport

### Task 13: Create platform_local.h

**Files:**
- Create: `src/Platform/platform_local.h`

- [ ] **Step 1: Write the header**

```c
#ifndef OFFS_PLATFORM_LOCAL_H
#define OFFS_PLATFORM_LOCAL_H

#include "platform_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

platform_socket_t* platform_local_listen(const char* path);
platform_socket_t* platform_local_accept(platform_socket_t* listener);
platform_socket_t* platform_local_connect(const char* path);
void platform_local_cleanup(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_LOCAL_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_local.h
git commit -m "feat: add platform_local.h for local socket transport"
```

### Task 14: Create platform_local.c — POSIX implementation

**Files:**
- Create: `src/Platform/platform_local.c`

- [ ] **Step 1: Write the POSIX implementation**

```c
#include "platform_local.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <string.h>
  #include <sys/un.h>
  #include <unistd.h>

  platform_socket_t* platform_local_listen(const char* path) {
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) return NULL;

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
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) return NULL;

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
#else
  /* Windows implementation — Phase 8 */
#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/Platform/platform_local.c
git commit -m "feat: add platform_local.c POSIX implementation"
```

- [ ] **Step 3: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build

---

## Phase 6: Umbrella Header + CMakeLists

### Task 15: Create platform.h umbrella + CMakeLists

**Files:**
- Create: `src/Platform/platform.h`
- Create: `src/Platform/CMakeLists.txt`

- [ ] **Step 1: Write platform.h**

```c
#ifndef OFFS_PLATFORM_H
#define OFFS_PLATFORM_H

#include "platform_compiler.h"
#include "platform_time.h"
#include "platform_random.h"
#include "platform_process.h"
#include "platform_file.h"
#include "platform_socket.h"
#include "platform_thread.h"
#include "platform_local.h"

#endif /* OFFS_PLATFORM_H */
```

- [ ] **Step 2: Write CMakeLists.txt**

```cmake
target_sources(offs PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/platform_time.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_random.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_process.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_file.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_socket.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_thread.c
  ${CMAKE_CURRENT_LIST_DIR}/platform_local.c
)

target_include_directories(offs PUBLIC ${CMAKE_SOURCE_DIR}/src)
```

- [ ] **Step 3: Add to root CMakeLists.txt**

Add `add_subdirectory(src/Platform)` to the root `CMakeLists.txt`.

- [ ] **Step 4: Commit**

```bash
git add src/Platform/platform.h src/Platform/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add platform.h umbrella and CMake integration"
```

- [ ] **Step 5: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build

---

## Phase 7: Migrate Threading

### Task 16: Migrate transport servers to platform_thread

**Files:** All transport `_start()` and `_stop()` functions.

Each transport server (`ws_transport.c`, `http_server.c`, `tcp_transport.c`, `unix_transport.c`, `wt_transport.c`) has:
- `PLATFORMTHREADTYPE thread` → `platform_thread_t* thread`
- `pthread_create(&t, NULL, _server_thread, transport)` with `#ifdef _WIN32` guard → `transport->thread = platform_thread_create(_server_thread, transport)`
- `pthread_join(t, NULL)` with guard → `platform_thread_join(transport->thread)`
- `PLATFORMLOCKTYPE(destroy_lock)` → `platform_mutex_t* destroy_lock`
- `platform_lock_init(&transport->destroy_lock)` → `transport->destroy_lock = platform_mutex_create()`
- `platform_lock_destroy(&transport->destroy_lock)` → `platform_mutex_destroy(transport->destroy_lock)`
- `platform_lock/unlock` → `platform_mutex_lock/unlock`
- `#include "threadding.h"` → `#include "Platform/platform.h"`

Also migrate: `timer_actor.c`, `scheduler.c`, `src/Actor/pool.c`, `quic_listener.c`, `relay_client.c`, `relay_server.c`

- [ ] **Step 1: Migrate ws_transport.c**

The struct member changes:
```c
/* Before: */
PLATFORMTHREADTYPE thread;
PLATFORMLOCKTYPE(destroy_lock);

/* After: */
platform_thread_t* thread;
platform_mutex_t* destroy_lock;
```

The init in `ws_transport_create()`:
```c
/* Before: */
platform_lock_init(&transport->destroy_lock);

/* After: */
transport->destroy_lock = platform_mutex_create();
```

The start:
```c
/* Before: */
#ifdef _WIN32
transport->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)_server_thread, transport, 0, NULL);
#else
pthread_create(&transport->thread, NULL, _server_thread, transport);
#endif

/* After: */
transport->thread = platform_thread_create(_server_thread, transport);
```

The stop:
```c
/* Before: */
#ifdef _WIN32
WaitForSingleObject(transport->thread, INFINITE);
CloseHandle(transport->thread);
#else
pthread_join(transport->thread, NULL);
#endif

/* After: */
platform_thread_join(transport->thread);
```

And `platform_lock_destroy(&transport->destroy_lock)` → `platform_mutex_destroy(transport->destroy_lock)`

- [ ] **Step 2: Repeat for http_server.c, tcp_transport.c, unix_transport.c, wt_transport.c**

Same pattern as Step 1 for each file.

- [ ] **Step 3: Migrate timer_actor.c, scheduler.c, pool.c, quic_listener.c, relay_client.c, relay_server.c**

Replace `PLATFORMLOCKTYPE`/`PLATFORMTHREADTYPE` with `platform_*` types, replace init/destroy/lock/unlock calls.

- [ ] **Step 4: Migrate offs_client.c threading**

Replace:
```c
PLATFORMTHREADTYPE thread;          →  platform_thread_t* recv_thread;
pthread_mutex_t lock;               →  platform_mutex_t* lock;
pthread_cond_t recv_cond;           →  platform_condvar_t* recv_cond;
pthread_mutex_init(&client->lock, NULL)    →  client->lock = platform_mutex_create();
pthread_mutex_destroy(&client->lock)       →  platform_mutex_destroy(client->lock);
pthread_mutex_lock/unlock(...)             →  platform_mutex_lock/unlock(...)
pthread_cond_init(&wt.recv_cond, NULL)     →  client->transport.wt.recv_cond = platform_condvar_create();
pthread_cond_destroy(...)                  →  platform_condvar_destroy(...)
pthread_cond_wait/signal(...)              →  platform_condvar_wait/signal(...)
pthread_create(&client->recv_thread, NULL, _recv_thread, client)
                                           →  client->recv_thread = platform_thread_create(_recv_thread, client);
pthread_join(client->recv_thread, NULL)    →  platform_thread_join(client->recv_thread);
```

- [ ] **Step 5: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build without threadding.h

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPI/HTTP/http_server.c src/ClientAPI/WS/ws_transport.c \
        src/ClientAPI/TCP/tcp_transport.c src/ClientAPI/Unix/unix_transport.c \
        src/ClientAPI/WT/wt_transport.c src/Timer/timer_actor.c \
        src/Scheduler/scheduler.c src/Actor/pool.c \
        src/Network/quic_listener.c src/Network/relay_client.c \
        src/Network/Relay/relay_server.c src/ClientLibs/c/offs_client.c
git commit -m "feat: migrate all threading to platform_thread API"
```

- [ ] **Step 7: Run tests to verify no regressions**

```bash
cmake --build build --target testliboffs -- -j$(nproc) && ./build/test/testliboffs
```
Expected: all tests pass

---

## Phase 8: Migrate Sockets

### Task 17: Migrate transport servers to platform_socket

**Files:** `ws_transport.c`, `http_server.c`, `tcp_transport.c`, `unix_transport.c`, all `*_connection.c` files

Replace raw socket operations with `platform_socket_*` API. Each transport's `_create()` function changes:

```c
/* Before: */
int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
struct sockaddr_in addr = {0};
addr.sin_family = AF_INET;
addr.sin_port = htons(port);
inet_pton(AF_INET, host, &addr.sin_addr);
bind(listen_fd, ...);
listen(listen_fd, backlog);
fcntl(listen_fd, F_SETFL, O_NONBLOCK);

/* After: */
platform_address_t addr;
platform_address_parse(&addr, host, port);
platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
platform_socket_set_reuseaddr(sock);
platform_socket_bind(sock, &addr);
platform_socket_listen(sock, backlog);
platform_socket_set_nonblocking(sock);
int listen_fd = platform_socket_fd(sock); /* for pd_watcher_create */
```

Connection I/O changes:
```c
/* Before: */
send(fd, buf, len, MSG_NOSIGNAL);
recv(fd, buf, len, 0);
shutdown(fd, SHUT_RDWR);
close(fd);

/* After: */
platform_socket_send(sock, buf, len);
platform_socket_recv(sock, buf, len);
platform_socket_shutdown(sock, PLATFORM_SHUT_RDWR);
platform_socket_destroy(sock);
```

`fcntl(fd, F_SETFL, O_NONBLOCK)` → `platform_socket_set_nonblocking(sock)`

- [ ] **Step 1: Migrate ws_transport.c socket operations**

Replace `int listen_fd` with `platform_socket_t* listen_sock` in the transport struct. Replace socket creation, bind, listen, accept with platform equivalents. The `pd_watcher_create` call uses `platform_socket_fd(listen_sock)`.

- [ ] **Step 2: Migrate ws_connection.c**

Replace `int fd` with `platform_socket_t* sock` in the connection struct. Replace `send()`/`recv()`/`shutdown()`/`close()` with platform equivalents. Update `pd_watcher_create` to use `platform_socket_fd(sock)`.

- [ ] **Step 3: Repeat for http_server.c, http_connection.c, tcp_transport.c, tcp_connection.c**

Same pattern.

- [ ] **Step 4: Migrate unix_transport.c and unix_connection.c to platform_local**

Replace `socket(AF_UNIX, ...)`, `sockaddr_un`, `bind()`, `listen()`, `accept()`, `connect()`, `unlink()` with `platform_local_listen()`, `platform_local_accept()`, `platform_local_connect()`, `platform_local_cleanup()`.

```c
/* Before: */
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
struct sockaddr_un addr = {0};
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
unlink(path);
bind(fd, ...);
listen(fd, 128);

/* After: */
platform_socket_t* sock = platform_local_listen(path);
int fd = platform_socket_fd(sock);
```

- [ ] **Step 5: Migrate offs_client.c socket operations**

Replace `_connect_unix()` with `platform_local_connect()`. Replace `_connect_tcp()` with `platform_socket_create(PLATFORM_AF_INET, 1)` + `platform_address_parse()` + `platform_socket_connect()`. Replace `send()`/`recv()`/`close()` on sockets with platform equivalents. Replace `shutdown(fd, SHUT_RDWR)` with `platform_socket_shutdown(sock, PLATFORM_SHUT_RDWR)`.

- [ ] **Step 6: Remove platform-specific socket headers**

Remove `#include <sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<sys/un.h>`, `<fcntl.h>` from all modified files. They're now only in `Platform/`.

- [ ] **Step 7: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build

- [ ] **Step 8: Commit**

```bash
git add src/ClientAPI/ src/ClientLibs/c/offs_client.c
git commit -m "feat: migrate all socket operations to platform_socket and platform_local"
```

- [ ] **Step 9: Run tests**

```bash
cmake --build build --target testliboffs -- -j$(nproc) && ./build/test/testliboffs
```
Expected: all tests pass

---

## Phase 9: Migrate File I/O

### Task 18: Migrate block cache and streams to platform_file

**Files:** `src/BlockCache/wal.c`, `section.c`, `index.c`, `sections.c`, `src/Streams/file-stream.c`, `src/Util/mkdir_p.c`, `src/Util/rm_rf.c`

- [ ] **Step 1: Migrate wal.c**

Replace `open(path, O_RDWR | O_CREAT | O_BINARY, 0644)` → `platform_file_open(path, PLATFORM_O_RDWR | PLATFORM_O_CREAT, 0644)`. Replace `close(fd)` → `platform_file_close(file)`. Replace `read()`, `write()`, `lseek()` → `platform_file_read/write/seek()`.

- [ ] **Step 2: Migrate section.c**

Replace `open()`, `close()`, `pread()`, `pwrite()`, `lseek()` calls with `platform_file_*` equivalents. Replace `__builtin_ffs()` → `PLATFORM_FFS()`.

- [ ] **Step 3: Migrate index.c, sections.c**

Same pattern — replace all raw POSIX file I/O with `platform_file_*` calls. Existing `#ifdef _WIN32` guards can be removed since the platform abstraction handles it.

- [ ] **Step 4: Migrate file-stream.c**

Replace `open()`/`close()`/`lseek()` with `platform_file_open/close/seek()`. Remove existing `#ifdef _WIN32` guards.

- [ ] **Step 5: Migrate mkdir_p.c, rm_rf.c**

Use `platform_mkdir()` and `platform_file_unlink()`.

- [ ] **Step 6: Remove platform-specific file headers**

Remove `#include <unistd.h>`, `<fcntl.h>`, `<sys/stat.h>` from migrated files. Add `#include "Platform/platform.h"`.

- [ ] **Step 7: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build

- [ ] **Step 8: Commit**

```bash
git add src/BlockCache/ src/Streams/file-stream.c src/Util/mkdir_p.c src/Util/rm_rf.c
git commit -m "feat: migrate file I/O to platform_file API"
```

---

## Phase 10: Migrate Time, Random, Process, Compiler

### Task 19: Replace time/random/process calls across codebase

**Files:** `src/ClientLibs/c/offs_client.c`, `src/ClientAPI/WS/ws_frame.c`, `src/OFFStreams/ofd_cache.c`, `src/Network/relay_client.c`, `src/Network/Relay/relay_server_main.c`, `src/Util/threadding.c` (being deleted), `test/test_node_main.c`

- [ ] **Step 1: Replace usleep/sleep → platform_sleep_ms**

```c
/* Before: */                           /* After: */
usleep(10000);                           platform_sleep_ms(10);
usleep(delay_ms * 1000);                 platform_sleep_ms(delay_ms);
nanosleep(&timeout, NULL);               platform_sleep_ms((timeout.tv_sec * 1000) + (timeout.tv_nsec / 1000000));
```

- [ ] **Step 2: Replace clock_gettime → platform_monotonic_ns**

In `ofd_cache.c`:
```c
/* Before: */
struct timespec ts;
clock_gettime(CLOCK_MONOTONIC, &ts);
uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

/* After: */
uint64_t now = platform_monotonic_ns();
```

- [ ] **Step 3: Replace random / /dev/urandom → platform_random_bytes**

In `ws_frame.c`:
```c
/* Before: */
getentropy(mask_key, sizeof(mask_key));

/* After: */
platform_random_bytes(mask_key, sizeof(mask_key));
```

In `offs_client.c`, replace `/dev/urandom` key generation:
```c
/* Before: */
FILE* urandom = fopen("/dev/urandom", "rb");
fread(raw_key, 1, 16, urandom);
fclose(urandom);

/* After: */
platform_random_bytes(raw_key, 16);
```

- [ ] **Step 4: Replace getpid → platform_getpid, sysconf → platform_core_count**

```c
/* Before: */                           /* After: */
getpid();                                platform_getpid();
sysconf(_SC_NPROCESSORS_ONLN);           platform_core_count();
```

- [ ] **Step 5: Replace compiler attributes**

```c
/* Before: */                           /* After: */
__attribute__((unused))                  PLATFORM_UNUSED
__builtin_ffs(x)                         PLATFORM_FFS(x)
#pragma GCC diagnostic push              PLATFORM_DIAGNOSTIC_PUSH
#pragma GCC diagnostic pop               PLATFORM_DIAGNOSTIC_POP
#pragma GCC diagnostic ignored "-W..."   PLATFORM_DIAGNOSTIC_IGNORE(-W...)
```

Files: `quic_listener.c`, `relay_client.c`, `quic_peer_send.c`, `relay_server.c`, `section.c`, `ofd_cache.c`, `tuple_cache.c`, `sections.c`, `block_cache.c`, `index.c`

- [ ] **Step 6: Remove signal(SIGPIPE, SIG_IGN)**

In `test/test_node_main.c:1755`: remove `signal(SIGPIPE, SIG_IGN)` — `platform_socket_send()` handles this internally.

- [ ] **Step 7: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```
Expected: clean build

- [ ] **Step 8: Commit**

```bash
git add src/ClientLibs/c/offs_client.c src/ClientAPI/WS/ws_frame.c \
        src/OFFStreams/ofd_cache.c src/Network/ \
        src/BlockCache/section.c src/OFFStreams/tuple_cache.c \
        src/BlockCache/sections.c src/BlockCache/block_cache.c \
        src/BlockCache/index.c test/test_node_main.c
git commit -m "feat: migrate time/random/process/compiler to platform APIs"
```

---

## Phase 11: Migrate offs_client.c to poll-dancer

### Task 20: Replace blocking recv loop with poll-dancer event loop

**Files:**
- Modify: `src/ClientLibs/c/offs_client.c`

- [ ] **Step 1: Add poll-dancer includes and event loop setup**

Add `#include <poll-dancer/poll-dancer.h>`.

Add to `offs_client_t` struct:
```c
pd_loop_t* loop;         /* event loop for async I/O */
pd_watcher_t* watcher;   /* watcher on the socket */
```

- [ ] **Step 2: Replace _recv_thread with poll-dancer event loop**

Replace the `_recv_thread` function. Instead of a blocking `recv()` loop, the new `_recv_thread` creates a `pd_loop_t`, registers a `pd_watcher_t` on the socket for `PD_EVENT_READ`, and runs `pd_loop_run(loop, PD_RUN_DEFAULT)`.

The read callback:
```c
static void _client_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                   pd_event_t events, void* user_data) {
  offs_client_t* client = (offs_client_t*)user_data;
  (void)loop;

  if (events & (PD_EVENT_ERROR | PD_EVENT_HANGUP)) {
    client->connected = 0;
    pd_loop_stop(client->loop);
    return;
  }

  if (events & PD_EVENT_READ) {
    if (client->transport_type == OFFS_TRANSPORT_WS) {
      /* WS: read into recv_buf, parse frames */
      uint8_t buf[READ_BUFFER_SIZE];
      ssize_t bytes_read = _ws_recv(client, buf, sizeof(buf));
      if (bytes_read <= 0) {
        client->connected = 0;
        pd_loop_stop(client->loop);
        return;
      }
      /* Append to recv_buf, parse frames... (existing logic) */
    } else {
      /* Unix/TCP: read + stream_framer_feed */
      uint8_t buf[READ_BUFFER_SIZE];
      ssize_t bytes_read = platform_socket_recv(client->transport_sock, buf, sizeof(buf));
      if (bytes_read <= 0) {
        client->connected = 0;
        pd_loop_stop(client->loop);
        return;
      }
      stream_framer_feed(client->framer, buf, (size_t)bytes_read);
      /* Extract frames... (existing logic) */
    }
  }
}
```

The `_recv_thread` becomes:
```c
static void* _recv_thread(void* arg) {
  offs_client_t* client = (offs_client_t*)arg;
  platform_thread_setup_stack();

  client->loop = pd_loop_create(NULL);
  int sock_fd = platform_socket_fd(client->transport_sock);
  client->watcher = pd_watcher_create(client->loop, sock_fd,
                                       PD_EVENT_READ | PD_EVENT_ERROR | PD_EVENT_HANGUP,
                                       _client_read_callback, client);
  pd_watcher_start(client->watcher);

  while (client->running) {
    pd_loop_run_once(client->loop, 100);
  }

  pd_watcher_destroy(client->watcher);
  pd_loop_destroy(client->loop);
  client->loop = NULL;
  client->watcher = NULL;
  return NULL;
}
```

- [ ] **Step 3: Update offs_client_disconnect to stop the loop cleanly**

```c
client->running = 0;
if (client->loop != NULL) {
  pd_loop_async_send(client->loop, NULL); /* wake the loop */
}
/* Then platform_thread_join as before */
```

- [ ] **Step 4: Build and verify**

```bash
cmake --build build --target offs -- -j$(nproc)
```

- [ ] **Step 5: Run tests**

```bash
cmake --build build --target testliboffs -- -j$(nproc) && ./build/test/testliboffs
```
Expected: all client tests pass

- [ ] **Step 6: Commit**

```bash
git add src/ClientLibs/c/offs_client.c
git commit -m "feat: migrate offs_client to poll-dancer event-driven I/O"
```

---

## Phase 12: Cleanup — Delete threadding

### Task 21: Delete threadding.h and threadding.c

**Files:**
- Delete: `src/Util/threadding.h`
- Delete: `src/Util/threadding.c`

- [ ] **Step 1: Remove from CMakeLists.txt**

Remove `src/Util/threadding.c` from any `target_sources` entry.

- [ ] **Step 2: Delete the files**

```bash
rm src/Util/threadding.h src/Util/threadding.c
```

- [ ] **Step 3: Remove remaining #include "threadding.h" references**

Grep for any remaining `#include "threadding.h"` or `#include "../../Util/threadding.h"`. Replace with `#include "Platform/platform.h"` if found.

- [ ] **Step 4: Build the full project**

```bash
cmake --build build --target offs -- -j$(nproc)
cmake --build build --target testliboffs -- -j$(nproc)
```
Expected: clean build for both targets

- [ ] **Step 5: Run full test suite**

```bash
./build/test/testliboffs
```
Expected: all tests pass

- [ ] **Step 6: Run valgrind on key tests**

```bash
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
  ./build/test/testliboffs --gtest_filter='TestWsFrame.*:TestWsTransport.*:TestOffsWsClient.*'
```
Expected: zero new definite/indirect leaks

- [ ] **Step 7: Commit**

```bash
git rm src/Util/threadding.h src/Util/threadding.c
git commit -m "refactor: delete threadding.h/.c, fully replaced by platform_thread"
```

---

## Phase 13: Platform Unit Tests

### Task 22: Write tests for platform_time, platform_random, platform_thread

**Files:**
- Create: `test/test_platform_time.cpp`
- Create: `test/test_platform_random.cpp`
- Create: `test/test_platform_thread.cpp`

- [ ] **Step 1: Write test_platform_time.cpp**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "Platform/platform_time.h"
}

TEST(TestPlatformTime, SleepMsDoesNotCrash) {
  platform_sleep_ms(10); /* should return without error */
}

TEST(TestPlatformTime, MonotonicNsIncreases) {
  uint64_t start = platform_monotonic_ns();
  platform_sleep_ms(10);
  uint64_t end = platform_monotonic_ns();
  EXPECT_GT(end, start);
  EXPECT_GT(end - start, 5000000ULL); /* at least 5ms elapsed */
}
```

- [ ] **Step 2: Write test_platform_random.cpp**

```cpp
#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "Platform/platform_random.h"
}

TEST(TestPlatformRandom, BasicGeneration) {
  uint8_t buf1[32], buf2[32];
  memset(buf1, 0, 32);
  memset(buf2, 0, 32);
  EXPECT_EQ(platform_random_bytes(buf1, 32), 0);
  EXPECT_EQ(platform_random_bytes(buf2, 32), 0);
  /* Extremely unlikely to produce identical 32-byte sequences */
  EXPECT_NE(0, memcmp(buf1, buf2, 32));
}

TEST(TestPlatformRandom, SingleByte) {
  uint8_t byte;
  EXPECT_EQ(platform_random_bytes(&byte, 1), 0);
}

TEST(TestPlatformRandom, LargeBuffer) {
  uint8_t buf[1024];
  EXPECT_EQ(platform_random_bytes(buf, sizeof(buf)), 0);
}
```

- [ ] **Step 3: Write test_platform_thread.cpp**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "Platform/platform_thread.h"
}

static void* _test_thread_fn(void* arg) {
  int* val = (int*)arg;
  *val = 42;
  return NULL;
}

TEST(TestPlatformThread, CreateAndJoin) {
  int result = 0;
  platform_thread_t* t = platform_thread_create(_test_thread_fn, &result);
  ASSERT_NE(t, nullptr);
  platform_thread_join(t);
  EXPECT_EQ(result, 42);
}

TEST(TestPlatformMutex, LockUnlock) {
  platform_mutex_t* m = platform_mutex_create();
  ASSERT_NE(m, nullptr);
  platform_mutex_lock(m);
  platform_mutex_unlock(m);
  platform_mutex_destroy(m);
}

TEST(TestPlatformThread, SelfId) {
  uint64_t id1 = platform_thread_self();
  uint64_t id2 = platform_thread_self();
  EXPECT_EQ(id1, id2);
  EXPECT_GT(id1, 0ULL);
}
```

- [ ] **Step 4: Add tests to CMakeLists.txt and build**

Add the new .cpp files to `test/CMakeLists.txt`.

```bash
cmake --build build --target testliboffs -- -j$(nproc) && ./build/test/testliboffs --gtest_filter='TestPlatform*'
```
Expected: all platform tests pass

- [ ] **Step 5: Commit**

```bash
git add test/test_platform_time.cpp test/test_platform_random.cpp test/test_platform_thread.cpp test/CMakeLists.txt
git commit -m "test: add unit tests for platform_time, random, and thread"
```

### Task 23: Write tests for platform_socket and platform_local

**Files:**
- Create: `test/test_platform_socket.cpp`
- Create: `test/test_platform_local.cpp`

- [ ] **Step 1: Write test_platform_socket.cpp**

```cpp
#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "Platform/platform_socket.h"
#include "Platform/platform_local.h"
}

TEST(TestPlatformSocket, CreateAndDestroy) {
  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
  ASSERT_NE(sock, nullptr);
  EXPECT_GE(platform_socket_fd(sock), 0);
  platform_socket_destroy(sock);
}

TEST(TestPlatformAddress, ParseIPv4) {
  platform_address_t addr;
  EXPECT_EQ(platform_address_parse(&addr, "127.0.0.1", 8080), 0);
  EXPECT_EQ(addr.family, PLATFORM_AF_INET);
  EXPECT_EQ(addr.inet.port, 8080);
}

TEST(TestPlatformSocket, SetNonBlocking) {
  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
  ASSERT_NE(sock, nullptr);
  EXPECT_EQ(platform_socket_set_nonblocking(sock), 0);
  platform_socket_destroy(sock);
}

TEST(TestPlatformSocket, SetReuseAddr) {
  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
  ASSERT_NE(sock, nullptr);
  EXPECT_EQ(platform_socket_set_reuseaddr(sock), 0);
  platform_socket_destroy(sock);
}

TEST(TestPlatformSocket, BindAndListenAndConnect) {
  platform_address_t addr;
  ASSERT_EQ(platform_address_parse(&addr, "127.0.0.1", 19999), 0);

  platform_socket_t* server = platform_socket_create(PLATFORM_AF_INET, 1);
  ASSERT_NE(server, nullptr);
  ASSERT_EQ(platform_socket_set_reuseaddr(server), 0);
  ASSERT_EQ(platform_socket_bind(server, &addr), 0);
  ASSERT_EQ(platform_socket_listen(server, 1), 0);

  platform_socket_t* client = platform_socket_create(PLATFORM_AF_INET, 1);
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(platform_socket_connect(client, &addr), 0);

  platform_socket_t* accepted = platform_socket_accept(server, NULL);
  ASSERT_NE(accepted, nullptr);

  /* Send and receive */
  const char* msg = "hello";
  EXPECT_EQ(platform_socket_send(client, msg, 5), 5);
  char buf[16] = {0};
  EXPECT_EQ(platform_socket_recv(accepted, buf, 5), 5);
  EXPECT_STREQ(buf, "hello");

  platform_socket_destroy(accepted);
  platform_socket_destroy(client);
  platform_socket_destroy(server);
}
```

- [ ] **Step 2: Write test_platform_local.cpp**

```cpp
#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "Platform/platform_local.h"
#include "Platform/platform_socket.h"
}

TEST(TestPlatformLocal, ListenAndConnect) {
  const char* path = "/tmp/test_platform_local.sock";
  platform_local_cleanup(path);

  platform_socket_t* server = platform_local_listen(path);
  ASSERT_NE(server, nullptr);

  platform_socket_t* client = platform_local_connect(path);
  ASSERT_NE(client, nullptr);

  platform_socket_t* accepted = platform_local_accept(server);
  ASSERT_NE(accepted, nullptr);

  const char* msg = "hello_local";
  EXPECT_EQ(platform_socket_send(client, msg, 11), 11);
  char buf[32] = {0};
  EXPECT_EQ(platform_socket_recv(accepted, buf, 11), 11);
  EXPECT_STREQ(buf, "hello_local");

  platform_socket_destroy(accepted);
  platform_socket_destroy(client);
  platform_socket_destroy(server);
  platform_local_cleanup(path);
}
```

- [ ] **Step 3: Build and run tests**

```bash
cmake --build build --target testliboffs -- -j$(nproc) && ./build/test/testliboffs --gtest_filter='TestPlatformSocket*:TestPlatformLocal*'
```
Expected: all tests pass

- [ ] **Step 4: Commit**

```bash
git add test/test_platform_socket.cpp test/test_platform_local.cpp test/CMakeLists.txt
git commit -m "test: add unit tests for platform_socket and platform_local"
```

---

## Phase 14: Final Verification

### Task 24: Full build, full test suite, valgrind, de-wonk

- [ ] **Step 1: Clean rebuild**

```bash
cmake --build build --clean-first --target offs testliboffs -- -j$(nproc)
```
Expected: zero warnings, clean build

- [ ] **Step 2: Full test suite**

```bash
./build/test/testliboffs
```
Expected: all tests pass

- [ ] **Step 3: Valgrind leak check**

```bash
valgrind --leak-check=full --show-leak-kinds=definite,indirect ./build/test/testliboffs
```
Expected: only pre-existing leaks (OpenSSL CONF_modules, index_add_to_node)

- [ ] **Step 4: De-wonk audit**

Run the de-wonk skill on all changed files — check for TODOs, stubs, broken code, weird patterns.

- [ ] **Step 5: Commit any final fixes**

```bash
git add -A
git commit -m "chore: final verification fixes from de-wonk audit"
```
