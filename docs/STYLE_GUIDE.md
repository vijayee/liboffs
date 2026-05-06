# liboffs Style Guide

This document captures the coding conventions, organizational patterns, and stylistic choices observed throughout the liboffs codebase. Follow these when extending the library.

## 1. Repository & Directory Layout

### Top-Level Structure

```
liboffs/
├── src/            # All library source code
├── test/           # Unit tests (GoogleTest, C++)
├── deps/           # Git submodule dependencies
├── docs/           # Architecture & standards documentation
├── sections/       # Runtime data: section storage files
├── block_index/    # Runtime data: index + WAL files
├── meta/           # Metadata directory (per-run state)
├── robin/          # Robin (internal tooling) state
├── BlockCacheTest/ # Ad-hoc block cache test data
└── CMakeLists.txt  # Root build file
```

### `src/` Module Layout

Each module gets its own PascalCase directory under `src/`. The directory name is the module name:

```
src/
├── BlockCache/      # Block caching orchestration
├── Buffer/          # Reference-counted byte arrays
├── Configuration/   # Config loading / parsing
├── RefCounter/      # Reference counting primitives
├── Streams/         # Event-driven I/O streams
├── Workers/         # Thread pool, promises, work queue, priority
├── Time/            # Timing wheel, ticker, debouncer
└── Util/            # Cross-cutting utilities (allocator, logging, threading, hash, vec, path)
```

Within each module directory:

```
src/ModuleName/
├── module_name.h    # Public header
├── module_name.c    # Public implementation
└── sub_module.h     # Sub-component (if module is complex)
    sub_module.c
```

### Test Layout

Tests live in `test/` and mirror module names:

```
test/
├── CMakeLists.txt
├── test_main.cpp
├── test_buffer.cpp
├── test_block.cpp
├── test_block_cache.cpp
├── test_file_stream.cpp
├── test_index.cpp
├── test_refcounter.cpp
├── test_section.cpp
└── test_time.cpp
```

## 2. Naming Conventions

### 2.1 Types

All types use `snake_case` with a `_t` suffix:

```c
typedef struct {
  uint16_t count;
  uint8_t yield;
} refcounter_t;

typedef struct buffer_t {
  refcounter_t refcounter;
  uint8_t* data;
  size_t size;
} buffer_t;
```

### 2.2 Enums

Enums use `snake_case` values with a `_e` suffix on the type:

```c
typedef enum {
  readable_stream = 0,
  writeable_stream = 1,
  duplex_stream = 2,
  transform_stream = 3
} stream_type_e;

typedef enum {
  mega = 1000000,
  standard = 128000,
  mini = 64000,
  nano = 136
} block_size_e;
```

Enums use explicit integer values where they carry semantic meaning (sizes, event IDs).

### 2.3 Functions

Functions follow the `module_action()` pattern. The module prefix is lowercase snake_case matching the filename:

```c
// Creation / destruction
block_t*      block_create(buffer_t* data);
void          block_destroy(block_t* block);
work_pool_t*  work_pool_create(size_t size);
void          work_pool_destroy(work_pool_t* pool);

// Actions
void          stream_init(stream_t* stream, ...);
void          stream_close(stream_t* stream);
void          work_execute(work_t* work);
size_t        stream_subscribe(stream_t* stream, stream_event_e event, ...);

// Queries
uint16_t      refcounter_count(refcounter_t* refcounter);
uint8_t       block_lru_cache_contains(block_lru_cache_t* lru, buffer_t* hash);
```

Private / internal functions use a leading underscore:

```c
void _block_cache_get(block_cache_get_ctx* ctx);
void _block_cache_get_abort(block_cache_get_ctx* ctx);
void _readable_pull_file_stream_on_pull(readable_pull_file_stream_t* stream);
```

### 2.4 Macros

Macros are `UPPER_CASE` with underscores:

```c
#define REFERENCE(N, T)   (T*) refcounter_reference((refcounter_t*) N)
#define DESTROY(N, T)     T##_destroy(N); N = NULL
#define CONSUME(N, T)     (T*) refcounter_consume((refcounter_t**) &N)
#define YIELD(N)          refcounter_yield((refcounter_t*) N)
#define ERROR(MESSAGE)    error_create(MESSAGE, (char*)__FILE__, (char*)__func__, __LINE__)
#define DEFAULT_CHUNK_SIZE 128000
```

Type-generic macros (REFERENCE, DESTROY, CONSUME) take both the variable and its type to provide type safety through casting.

### 2.5 Includes

Order: own header first, then project headers by module, then system headers.

```c
// In src/BlockCache/block_cache.c:
#include "block_cache.h"        // Own header first
#include "../Util/allocator.h"   // Project utilities
#include "../Util/hash.h"
#include "../Util/path_join.h"
#include "../Workers/error.h"
#include "../Workers/work.h"
#include <time.h>                // System headers last

// In test/test_buffer.cpp:
#include <gtest/gtest.h>         // Test framework first
extern "C" {
#include "../src/Buffer/buffer.h" // Tested module
#include <cbor.h>                 // Dependency headers
}
```

Local/private headers use quotes `""`. System headers use angle brackets `<>`.

## 3. File Conventions

### 3.1 Header Files

Every header has an include guard using the `#ifndef` pattern:

```c
// src/ModuleName/module_name.h
#ifndef OFFS_MODULE_NAME_H
#define OFFS_MODULE_NAME_H

#include <stdint.h>

// ... declarations ...

#endif // OFFS_MODULE_NAME_H
```

The guard prefix is `OFFS_` for liboffs headers.

Struct types are typically `typedef`'d anonymous structs. The struct tag is the same as the type name when needed for self-referential pointers:

```c
typedef struct block_lru_node_t block_lru_node_t;
struct block_lru_node_t {
  block_t* value;
  index_entry_t* entry;
  block_lru_node_t* next;
  block_lru_node_t* previous;
};
```

### 3.2 Source Files

Every `.c` file includes its own `.h` first:

```c
// src/BlockCache/block_cache.c
#include "block_cache.h"
#include "../Util/allocator.h"
// ...
```

### 3.3 File Header Comments

Source files start with a creation comment:

```c
//
// Created by victor on 3/30/25.
//
```

Files adapted from third-party sources retain their original copyright headers:

```c
/**
 * Copyright (c) 2014 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See LICENSE for details.
 */
```

## 4. Formatting & Style

### 4.1 Indentation & Spacing

- **2-space indentation**, no tabs.
- Opening braces on the same line (Egyptian style).
- Single space between `if`/`while`/`for` and the opening parenthesis.

```c
void work_pool_shutdown(work_pool_t* pool) {
  platform_lock(&pool->lock);
  pool->stop = 1;
  platform_broadcast_condition(&pool->condition);
  platform_unlock(&pool->lock);
}
```

### 4.2 Conditionals

Single-statement bodies go on their own line without braces:

```c
if (pool->stop) {
  platform_unlock(&pool->lock);
  return 1;
}
work_enqueue(&pool->queue, work);
platform_unlock(&pool->lock);
```

Prefer early returns over deep nesting:

```c
if (pool->stop) {
  platform_unlock(&pool->lock);
  return 1;
}
work_enqueue(&pool->queue, work);
platform_unlock(&pool->lock);
platform_signal_condition(&pool->condition);
return 0;
```

### 4.3 Switch Statements

Switch cases fall through with explicit `break`:

```c
switch (type) {
  case standard:
    folder = path_join(location, "blocks");
    break;
  case mini:
    folder = path_join(location, "mini");
    break;
  case nano:
    folder = path_join(location, "nano");
    break;
  case mega:
    folder = path_join(location, "nano");
    break;
}
```

### 4.4 Variable Declarations

Variables declared at the top of the function or at the top of a block scope, one per line:

```c
block_cache_get_ctx* ctx = get_memory(sizeof(block_cache_get_ctx));
ctx->promise = promise;
ctx->block_cache = block_cache;
ctx->hash = REFERENCE(hash, buffer_t);
```

Pointer `*` goes next to the variable name (not the type):

```c
block_t* block;        // ✅
block_t *block;        // ❌
```

## 5. Core Patterns

### 5.1 Reference Counting

Every heap-allocated shared object embeds `refcounter_t` as its first member. The refcounter is always initialized via `refcounter_init()` which sets the initial count to 1.

```c
buffer_t* buffer_create(size_t size) {
  buffer_t* buf = get_clear_memory(sizeof(buffer_t));
  buf->data = get_clear_memory(size);
  buf->size = size;
  refcounter_init((refcounter_t*) buf);
  return buf;
}
```

Destructors always follow the same pattern:

```c
void buffer_destroy(buffer_t* buf) {
  refcounter_dereference((refcounter_t*) buf);
  if (refcounter_count((refcounter_t*) buf) == 0) {
    free(buf->data);
    refcounter_destroy_lock(&buf->refcounter);
    free(buf);
  }
}
```

Ownership transfer macros:

| Macro | Effect |
|-------|--------|
| `REFERENCE(obj, T)` | Increment refcount, return typed pointer |
| `YIELD(obj)` | Transfer ownership without incrementing |
| `DESTROY(obj, T)` | Call destructor, null out pointer |
| `CONSUME(obj, T)` | Claim ownership from a yielded reference |

### 5.2 Memory Allocation

Always use the project wrappers — never raw `malloc`/`calloc`:

```c
void* ptr = get_memory(size);         // malloc wrapper, aborts on OOM
void* ptr = get_clear_memory(size);   // calloc wrapper, aborts on OOM
```

### 5.3 Platform Abstraction

Cross-platform code uses macros from `threadding.h`:

```c
PLATFORMLOCKTYPE(lock);           // pthread_mutex_t / CRITICAL_SECTION
PLATFORMCONDITIONTYPE(cond);      // pthread_cond_t / CONDITION_VARIABLE
PLATFORMBARRIERTYPE(barrier);     // pthread_barrier_t / SYNCHRONIZATION_BARRIER
PLATFORMTHREADTYPE thread;        // pthread_t / HANDLE
```

Platform-conditional blocks are used inline with `#ifdef _WIN32` / `#else`:

```c
#ifdef _WIN32
  pool->workers[i] = CreateThread(NULL, 0, workerFunction, pool, 0, &pool->workerIds[i]);
#else
  pthread_create(&pool->workers[i], NULL, (void*)workerFunction, pool);
#endif
```

### 5.4 Async Work / Promise Pattern

Async operations build a context struct, wrap it in a `work_t`, and enqueue on the thread pool:

```c
// 1. Define context struct
typedef struct {
  block_cache_t* block_cache;
  buffer_t* hash;
  promise_t* promise;
} block_cache_get_ctx;

// 2. Create work item with execute + abort handlers
void block_cache_get(block_cache_t* block_cache, priority_t* priority, buffer_t* hash, promise_t* promise) {
  block_cache_get_ctx* ctx = get_memory(sizeof(block_cache_get_ctx));
  ctx->promise = promise;
  ctx->block_cache = block_cache;
  ctx->hash = REFERENCE(hash, buffer_t);
  YIELD(ctx->hash);
  work_t* work = work_create(priority, ctx, (void*)_block_cache_get, (void*)_block_cache_get_abort);
  work_pool_enqueue(block_cache->pool, CONSUME(work, work_t));
}

// 3. Execute handler — resolves or rejects promise
void _block_cache_get(block_cache_get_ctx* ctx) {
  // ... do work ...
  promise_resolve(promise, result);
  free(ctx);
}

// 4. Abort handler
void _block_cache_get_abort(block_cache_get_ctx* ctx) {
  promise_reject(promise, ERROR("Operation aborted"));
  free(ctx);
}
```

### 5.5 Error Handling

Errors use the `async_error_t` type, created with the `ERROR()` macro which captures file, function, and line:

```c
stream_notify((stream_t*)stream, error_event, ERROR("Stream is already destroyed"));
```

The `ERROR()` macro expands to:
```c
error_create(message, (char*)__FILE__, (char*)__func__, __LINE__)
```

## 6. Tests

Tests are written in C++ using GoogleTest. C headers are wrapped in `extern "C"`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Buffer/buffer.h"
#include <cbor.h>
}
```

Test naming follows `TestModule` fixture and `TestFunction_Scenario` test names:

```cpp
TEST(TestBuffer, TestBufferCreation) { ... }
TEST(TestBuffer, TestBufferBitwise) { ... }
```

Use `ASSERT_*` for fatal assertions (test stops) and `EXPECT_*` for non-fatal checks:

```cpp
ASSERT_EQ(buf->size, 25);
for (size_t i = 0; i < buf->size; i++) {
  EXPECT_EQ(buffer_get_index(buf, i), 0);
}
```

## 7. Build System

### CMake Patterns

- C standard: C11
- C++ standard: C++17 (tests only)
- Source collected via `file(GLOB_RECURSE C_SRC "src/*/*.c")`
- Test discovery via `gtest_discover_tests()`

### Dependency Management

Dependencies are:
- **Git submodules** for source-level deps (BLAKE3, xxHash, hashmap, libcbor)
- **ExternalProject** for cmake-based deps (libcbor build)
- **find_package** for system deps (OpenSSL)

## 8. Summary of Patterns

| Concern | Convention |
|---------|-----------|
| Source root | `src/ModuleName/module_name.c` |
| Public header | `src/ModuleName/module_name.h` |
| Type naming | `snake_case_t` |
| Enum naming | `snake_case_e` |
| Function naming | `module_action()` |
| Private functions | `_function_name()` |
| Macros | `UPPER_CASE` |
| Include guard | `#ifndef OFFS_MODULE_NAME_H` |
| Indentation | 2 spaces, no tabs |
| Brace style | Egyptian (same line) |
| Memory allocation | `get_memory()` / `get_clear_memory()` |
| Object lifecycle | `_create()` → refcounted → `_destroy()` |
| Ownership macros | `REFERENCE`, `YIELD`, `DESTROY`, `CONSUME` |
| Async pattern | Context struct + work_t + promise |
| Error creation | `ERROR("message")` macro |
| Platform abstraction | `PLATFORM*` macros from `threadding.h` |
| Test framework | GoogleTest, C++17, `extern "C"` wrappers |
| Test naming | `TEST(TestModule, TestFunction_Scenario)` |
