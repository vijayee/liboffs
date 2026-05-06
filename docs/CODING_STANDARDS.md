# liboffs Coding Standards

This document defines coding standards and best practices for the liboffs project, with emphasis on C-specific issues that can cause subtle bugs.

## Critical: Passing Structs as Function Parameters

### The Problem: Large Structs by Value

**NEVER pass structs larger than 8 bytes by value in function parameters.**

This can cause **ABI (Application Binary Interface) corruption** where subsequent parameters receive garbage values due to calling convention mismatches.

#### Example of the Bug

```c
// ❌ WRONG - 16-byte struct passed by value
typedef struct {
  uint64_t time;
  uint64_t count;
} priority_t;  // 16 bytes!

void stream_init(stream_t* stream,
                 stream_force_e force,
                 stream_type_e type,
                 priority_t priority,    // ← BUG: 16 bytes by value!
                 uint8_t auto_push,
                 work_pool_t* pool,       // ← Receives garbage!
                 void (*destructor)(stream_t*));

// When called:
priority_t p = {12345, 67890};
stream_init(stream, force, type, p, 1, pool, destructor);
//                                     ^  ^^^^  ^^^^^^^^^^
//                                     These get WRONG values!
```

**What happens:**
1. Compiler must pass 16-byte struct according to ABI rules
2. x86-64 ABI allows 16-byte structs in registers OR on stack
3. Following parameters can be misaligned (read from wrong registers/stack offsets)
4. Result: `pool` and `destructor` contain garbage pointers → crashes or memory corruption

#### The Fix: Pass by Pointer

```c
// ✅ CORRECT - Pass by pointer (8 bytes, well-defined ABI)
void stream_init(stream_t* stream,
                 stream_force_e force,
                 stream_type_e type,
                 priority_t* priority,    // ← Pointer: 8 bytes, clean ABI
                 uint8_t auto_push,
                 work_pool_t* pool,       // ← Now receives correct value!
                 void (*destructor)(stream_t*));

// When called:
priority_t p = {12345, 67890};
stream_init(stream, force, type, &p, 1, pool, destructor);
//                                    ^^  ^^^^  ^^^^^^^^^^
//                                    All parameters correctly aligned!
```

### Why This Happens: ABI Complexity

#### x86-64 System V ABI Rules

For the first 6 arguments:
- Integer/pointer arguments → registers: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
- **Small structs (≤16 bytes)** → *might* go in registers or stack
- **Large structs (>16 bytes)** → always on stack with hidden pointer

The ambiguity for 16-byte structs creates portability issues:

| Compiler/Platform | Behavior for 16-byte struct |
|-------------------|------------------------------|
| GCC Linux | May use 2 registers (rdi:rsi) or stack |
| Clang Linux | Different optimization choices |
| MSVC Windows | Different calling convention entirely |
| ARM64 | Uses registers x0-x7, different layout |

#### Example Corruption Scenario

```c
// Function expects:
void func(priority_t p, void* ptr1, void* ptr2);

// Caller compiled with GCC -O2:
//   rdi = p.time
//   rsi = p.count
//   rdx = ptr1    ← WRONG! Should be stack
//   rcx = ptr2    ← WRONG!

// Callee compiled with GCC -O0:
//   Expects p on stack (16 bytes)
//   Expects ptr1 at stack+16
//   Expects ptr2 at stack+24
//   → Reads garbage for ptr1, ptr2!
```

### Rules

#### ✅ DO: Pass by Pointer for Large Structs

```c
// Structs > 8 bytes: ALWAYS pass by pointer
typedef struct {
  uint64_t time;
  uint64_t count;
} priority_t;  // 16 bytes

void work_create(priority_t* priority, ...);  // ✅

typedef struct {
  int x;
  int y;
} point_t;  // 8 bytes

void draw_point(point_t p);  // ✅ OK (≤8 bytes, POD)
```

#### ❌ DON'T: Pass Large Structs by Value

```c
void work_create(priority_t priority, ...);  // ❌ BUG RISK
```

#### ✅ DO: Use `const` for Read-Only Pointers

```c
void priority_compare(const priority_t* p1, const priority_t* p2);  // ✅
```

#### ✅ DO: Document Why Pointer is Used

```c
/**
 * Note: priority passed by pointer to avoid ABI issues with 16-byte structs.
 * See CODING_STANDARDS.md for details.
 */
void stream_init(stream_t* stream,
                 stream_force_e force,
                 stream_type_e type,
                 priority_t* priority,  // Pass by pointer for ABI safety
                 uint8_t auto_push,
                 work_pool_t* pool,
                 void (*destructor)(stream_t*));
```

### Known Issues in Current Codebase

The following functions need fixing:

1. **`work_create()`** in `src/Workers/work.h`
   ```c
   // Current (broken):
   work_t* work_create(priority_t priority, void* ctx, void (*execute)(void*), void (*abort)(void*));

   // Fixed:
   work_t* work_create(priority_t* priority, void* ctx, void (*execute)(void*), void (*abort)(void*));
   ```

2. **`readable_push_file_stream_create()`** in `src/Streams/file-stream.h`

3. **`writeable_push_file_stream_create()`** in `src/Streams/file-stream.h`

4. **`block_cache_put()`** in `src/BlockCache/block_cache.h`

5. **`block_cache_get()`** in `src/BlockCache/block_cache.h`

6. **`block_cache_remove()`** in `src/BlockCache/block_cache.h`

## General C Best Practices

### Reference Counting

All heap-allocated objects use reference counting:

```c
// Create object (ref count = 1)
block_t* block = block_create();

// Share ownership
REFERENCE(block);  // ref count = 2

// Use object...

// Release ownership
DESTROY(block, block_t);  // ref count = 1, then freed
```

**Macros:**
- `REFERENCE(obj)` - Increment reference count
- `YIELD(obj)` - Transfer ownership (don't increment)
- `DEREFERENCE(obj)` - Decrement reference count
- `DESTROY(obj, type)` - Call destructor and set to NULL
- `CONSUME(obj, type)` - Take ownership from yielded reference

### Memory Allocation

Always use the project's allocation functions:

```c
// ✅ Use these:
void* p = get_memory(size);        // malloc wrapper
void* p = get_clear_memory(size);  // calloc wrapper

// ❌ Never use raw malloc/calloc:
void* p = malloc(size);    // Don't do this
```

**Why:** These wrappers handle out-of-memory errors uniformly and add debugging support.

### Thread Safety

#### Lock Ordering

To prevent deadlocks, always acquire locks in this order:

1. Block Cache lock
2. Index lock
3. Section lock
4. LRU cache lock

```c
// ✅ Correct order
platform_lock(&block_cache->lock);
platform_lock(&index->lock);
platform_lock(&section->lock);

// ❌ Wrong order → potential deadlock
platform_lock(&section->lock);
platform_lock(&index->lock);
platform_lock(&block_cache->lock);
```

#### Lock Types

Use platform abstractions from `threadding.h`:

```c
pthread_mutex_t lock;          // → PLATFORMLOCKTYPE(lock)
pthread_cond_t condition;      // → PLATFORMCONDITIONTYPE(condition)
pthread_rwlock_t rwlock;      // → PLATFORMRWLOCKTYPE(rwlock)
pthread_barrier_t barrier;     // → PLATFORMBARRIERTYPE(barrier)
```

### Error Handling

All errors use the `async_error_t` type:

```c
typedef struct {
  const char* message;
  int code;
} async_error_t;

// Use the ERROR macro:
async_error_t* err = ERROR("Stream has been destroyed");
stream_notify(stream, error_event, err);
```

### Naming Conventions

#### Types

```c
typedef struct {
  // ...
} block_t;        // _t suffix for types

typedef enum {
  readable_stream,
  writable_stream
} stream_type_e;  // _e suffix for enums
```

#### Functions

```c
// Verb_noun format
block_t* block_create();
void block_destroy();
void stream_init();

// Private functions: leading underscore
void _internal_helper();
```

#### Macros

```c
// UPPERCASE with underscores
#define DEFAULT_CHUNK_SIZE 128000
#define REFERENCE(obj, type) ...
```

### Documentation Comments

Use Doxygen-style comments for public APIs:

```c
/**
 * @brief Create a new block with the given data.
 *
 * @param data The data buffer (will be copied).
 * @param size Size of the data in bytes.
 * @return Reference-counted block_t*, or NULL on failure.
 *
 * @note The returned block has ref count = 1.
 * @see block_destroy()
 */
block_t* block_create(const uint8_t* data, size_t size);
```

### File Organization

```
src/
├── ModuleName/
│   ├── module-name.h      // Public header
│   ├── module-name.c      // Implementation
│   └── README.md          // Module documentation (optional)
```

**Header structure:**
```c
#ifndef MODULE_NAME_H
#define MODULE_NAME_H

// Includes
#include <stdint.h>

// Types
typedef struct {...} module_t;

// Public functions
module_t* module_create();
void module_destroy(module_t* module);

// Inline functions (if needed)
static inline int module_helper() { ... }

#endif // MODULE_NAME_H
```

## Testing Standards

### Test File Structure

```cpp
// test/test_module.cpp
#include <gtest/gtest.h>
#include <gmock/gmock.h>

extern "C" {
#include "../src/Module/module.h"
}

class TestModule : public testing::Test {
public:
  module_t* module;

  void SetUp() override {
    module = module_create();
  }

  void TearDown() override {
    module_destroy(module);
  }
};

TEST_F(TestModule, TestCreation) {
  EXPECT_NE(module, nullptr);
}
```

### Test Naming

```cpp
TEST_F(TestModule, TestFunctionName_Scenario_ExpectedResult) {
  // Test code...
}
```

## Build System

### CMake Structure

```cmake
# Library
add_library(offs STATIC
  src/Module/module.c
)

target_include_directories(offs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Tests
add_executable(test_module test/test_module.cpp)
target_link_libraries(test_module offs gtest gmock)
```

### Compiler Flags

Required flags (set in CMakeLists.txt):
```cmake
target_compile_options(offs PRIVATE
  -Wall           # Enable warnings
  -Wextra         # Extra warnings
  -Werror         # Treat warnings as errors
  -std=gnu11      # C11 with GNU extensions
)
```

## Performance Guidelines

### Avoid Unnecessary Copies

```c
// ❌ Bad: copies 16 bytes
priority_t get_priority() {
  return current_priority;  // 16-byte copy!
}

// ✅ Good: pass output parameter
void get_priority(priority_t* out) {
  *out = current_priority;  // Still copies, but explicit
}

// ✅ Best: return pointer to const
const priority_t* get_priority() {
  return &current_priority;  // No copy
}
```

### Use Appropriate Data Structures

- **Binary tree** → O(log n) lookups (Index)
- **Hash map** → O(1) average lookups (large datasets)
- **LRU cache** → For hot data only

### Profile Before Optimizing

```bash
# Build with profiling
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make

# Profile with perf
perf record ./test/testliboffs
perf report
```

## Debugging

### Memory Leaks

```bash
# Run with Valgrind
valgrind --leak-check=full --show-leak-kinds=all ./test/testliboffs
```

### Thread Sanitizer

```bash
# Compile with thread sanitizer
cmake -DCMAKE_C_FLAGS="-fsanitize=thread -g" ..
make
./test/testliboffs
```

### Address Sanitizer

```bash
# Compile with address sanitizer
cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" ..
make
./test/testliboffs
```

## Code Review Checklist

Before submitting code, verify:

- [ ] No structs >8 bytes passed by value
- [ ] All heap memory uses `get_memory()` / `get_clear_memory()`
- [ ] All shared objects use reference counting correctly
- [ ] Locks acquired in correct order
- [ ] No deadlocks (use timeout or try-lock where appropriate)
- [ ] Error handling with `async_error_t`
- [ ] Test coverage for new functionality
- [ ] Documentation comments on public APIs
- [ ] No compiler warnings
- [ ] Memory sanitizers pass
- [ ] Thread sanitizers pass (for concurrent code)

## References

- [x86-64 ABI Specification](https://gitlab.com/x86-psABIs/x86-64-ABI)
- [System V AMD64 ABI](https://en.wikipedia.org/wiki/X86_calling_conventions#System_V_AMD64_ABI)
- [C11 Standard](https://en.cppreference.com/w/c/11)
- [GNU C Extensions](https://gcc.gnu.org/onlinedocs/gcc/C-Extensions.html)