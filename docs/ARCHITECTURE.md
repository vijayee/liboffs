# liboffs Architecture

## Overview

**liboffs** is a C library implementing an **Owner Free File System (OFFS)** - a content-addressable storage system where data is stored in fixed-size blocks identified by their cryptographic hash. The library provides language-agnostic foundations for building distributed, content-addressable storage systems.

## Core Concepts

### Content-Addressable Storage
- Data is split into fixed-size blocks
- Each block is identified by its BLAKE3 hash
- Blocks can be combined using XOR operations
- Content-addressing enables deduplication and ownership-free storage

### Key Properties
- **Deduplication**: Identical blocks are stored only once
- **Ownership-free**: Blocks can be shared across files/systems
- **Persistent**: Blocks stored on disk in section files
- **Cached**: Hot blocks kept in memory with LRU eviction
- **Async**: Promise-based I/O with thread pool workers

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         APPLICATION LAYER                        │
│                  (Language bindings / FFI interfaces)            │
└────────────────────────────────┬────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                          BLOCK CACHE                             │
│                      (Main Entry Point)                          │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐     │
│  │  Put Block   │  │  Get Block   │  │  Query Index      │     │
│  └──────────────┘  └──────────────┘  └────────────────────┘     │
└────────┬─────────────────────┬──────────────────────┬───────────┘
         │                     │                      │
         ▼                     ▼                      ▼
┌─────────────┐      ┌─────────────────┐    ┌──────────────────┐
│   LRU Cache │      │      INDEX      │    │     SECTIONS     │
│  (Hot Blocks)│      │  (Hash→Location)│    │ (Physical Files) │
│             │      │                 │    │                  │
│  Block Map  │      │  ┌───────────┐  │    │  ┌────────────┐  │
│  (hash→blk) │      │  │Binary Tree│  │    │  │ Section 0  │  │
│             │      │  └───────────┘  │    │  ├────────────┤  │
│             │      │  ┌───────────┐  │    │  │ Section 1  │  │
│             │      │  │Hit Counter│  │    │  ├────────────┤  │
│             │      │  │ (Fibonacci)│  │    │  │ Section N  │  │
│             │      │  └───────────┘  │    │  └────────────┘  │
│             │      │                 │    │                  │
│             │      │  ┌───────────┐  │    │  Fragment List  │
│             │      │  │    WAL    │  │    │  (Free Space)   │
│             │      │  └───────────┘  │    │                  │
└─────────────┘      └─────────────────┘    └──────────────────┘
         │                     │                      │
         │                     │                      │
         └─────────────────────┴──────────────────────┘
                               │
                ┌──────────────┴───────────────┐
                │                              │
                ▼                              ▼
        ┌──────────────┐            ┌─────────────────┐
        │    WORKERS    │            │ TIMING WHEEL   │
        │ (Thread Pool) │            │ (Scheduling)   │
        │               │            │                 │
        │ Priority Queue│            │ Days Wheel      │
        │ Work Items    │            │ Hours Wheel     │
        │ Promises      │            │ Minutes Wheel   │
        └───────────────┘            │ Seconds Wheel   │
                                      │ Millis Wheel    │
                                      └─────────────────┘

                    SUPPORTING COMPONENTS
    ┌──────────┬─────────────┬──────────────┬───────────┐
    │          │             │              │           │
    ▼          ▼             ▼              ▼           ▼
┌──────┐ ┌────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐
│Buffer│ │ Block  │  │RefCounter│  │  Stream  │  │  Time  │
│      │ │        │  │          │  │          │  │        │
│Bytes │ │Data+   │  │Memory    │  │Readable  │  │Debounce│
│Slice │ │Hash    │  │Mgmt      │  │Writable  │  │Throttle│
│XOR   │ │        │  │          │  │Duplex    │  │        │
│CBOR  │ │4 sizes │  │          │  │Transform │  │        │
└──────┘ └────────┘  └──────────┘  └──────────┘  └────────┘
```

## Data Flow

### Put Operation (Storing a Block)

```
User Data
    │
    ▼
┌─────────────────┐
│  Split into    │
│  Fixed Blocks  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Hash Block     │  ◄── BLAKE3 Hash
│  (BLAKE3)       │
└────────┬────────┘
         │
         ▼
┌─────────────────┐      ┌──────────┐
│  Check Index    │─Yes──▶│  Return  │
│  (Exists?)      │      │  Cached  │
└────────┬────────┘      └──────────┘
         │ No
         ▼
┌─────────────────┐
│  Select Section │  ◄── Round-robin
│  (Physical File)│
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Write to Disk  │
│  (Section File) │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Update Index   │
│  (Hash→Location)│
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Add to LRU     │
│  (Hot Cache)    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Return Promise │
│  (Async)        │
└─────────────────┘
```

### Get Operation (Retrieving a Block)

```
Hash Request
    │
    ▼
┌─────────────────┐      ┌──────────┐
│  Check LRU      │─Hit──▶│  Return  │
│  Cache          │      │  Block   │
└────────┬────────┘      └──────────┘
         │ Miss
         ▼
┌─────────────────┐      ┌──────────┐
│  Query Index    │─Miss─▶│  Reject  │
│  (Binary Tree)  │      │  Promise │
└────────┬────────┘      └──────────┘
         │ Hit
         ▼
┌─────────────────┐
│  Read Location  │
│  (Section+Index)│
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Load Section   │
│  (LRU Cached)   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Read Block     │
│  from Disk      │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Add to LRU     │
│  (Hot Cache)    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Resolve Promise│
│  (Return Block) │
└─────────────────┘
```

## Component Details

### Block Cache (`src/BlockCache/`)
**Purpose**: Main orchestrator and entry point for all operations.

**Responsibilities**:
- Coordinates LRU cache, Index, and Sections
- Provides async API via promises
- Manages block lifecycle (put/get/remove)
- Handles worker pool coordination

**Key Files**:
- `block_cache.h/c` - Main cache implementation
- Configuration via `Configuration` module

### Index (`src/Index/`)
**Purpose**: Fast hash-to-location lookup using binary tree.

**Responsibilities**:
- Binary tree for O(log n) lookups
- Fibonacci hit counter for LFU-like eviction
- Write-Ahead Log (WAL) for persistence
- Thread-safe operations with locks

**Key Features**:
- Hash → Section ID + Section Index mapping
- Ejection date tracking
- Hit count updates on access
- CBOR serialization for WAL

### Sections (`src/Section/`)
**Purpose**: Physical storage management in files.

**Responsibilities**:
- Round-robin section file allocation
- Fragment list for free space tracking
- Block write/read/deallocate operations
- LRU cache for section file handles

**Block Sizes**:
- **Mega**: 1 MB
- **Standard**: 128 KB
- **Mini**: 64 KB
- **Nano**: 136 bytes

### Workers (`src/Workers/`)
**Purpose**: Thread pool for async I/O operations.

**Responsibilities**:
- Priority-based work queue
- Promise resolution/rejection
- Thread pool management
- Work item execution and abort

**Key Concepts**:
- Work items have `execute` and `abort` callbacks
- Promises for async result handling
- Priority ordering via timing wheels

### Streams (`src/Streams/`)
**Purpose**: Event-based I/O streaming abstraction.

**Stream Types**:
- **Readable**: Data source (read from file, network)
- **Writable**: Data sink (write to file, network)
- **Duplex**: Both readable and writable
- **Transform**: Modify data in-flight

**Events**:
- `data` - Chunk available
- `close` - Stream ended
- `error` - Error occurred
- `drain` - Writable ready for more

### Buffer (`src/Buffer/`)
**Purpose**: Reference-counted byte array wrapper.

**Operations**:
- Create, copy, slice
- Concatenation
- Bitwise operations (xor, or, and, not)
- CBOR serialization

### Timing Wheel (`src/Time/wheel.h`)
**Purpose**: Efficient scheduling of delayed callbacks.

**Hierarchy**:
- Days wheel
- Hours wheel
- Minutes wheel
- Seconds wheel
- Milliseconds wheel

**Use Cases**:
- Block eviction scheduling
- Debouncing
- Timeout handling

## Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| **BLAKE3** | - | Cryptographic hashing (content addressing) |
| **xxHash** | - | Fast non-cryptographic hashing (internal lookups) |
| **hashmap** | - | Generic hashmap implementation |
| **libcbor** | - | CBOR serialization (persistence format) |
| **OpenSSL** | - | Additional cryptographic operations |
| **googletest** | - | Unit testing framework |

## Thread Safety Model

### Reference Counting
All shared objects use `RefCounter` with macros:
- `REFERENCE(obj)` - Increment ref count
- `YIELD(obj)` - Transfer ownership
- `DEREFERENCE(obj)` - Decrement ref count
- `CONSUME(obj)` - Use and release

### Synchronization Primitives
Platform abstraction via `threadding.h`:
- Mutexes (`mutex_t`)
- Condition Variables (`condition_t`)
- Barriers (`barrier_t`)
- Read-Write Locks (`rwlock_t`)

### Lock Ordering
To prevent deadlocks, locks are acquired in order:
1. Block Cache lock
2. Index lock
3. Section lock
4. LRU cache lock

## Storage Format

### Section File Layout
```
┌────────────────────────────────┐
│         Section Header         │
│  (Metadata, fragment count)    │
├────────────────────────────────┤
│         Fragment List          │
│  (Offsets and sizes)           │
├────────────────────────────────┤
│         Block Data 0           │
├────────────────────────────────┤
│         Block Data 1           │
├────────────────────────────────┤
│            ...                 │
├────────────────────────────────┤
│         Block Data N           │
└────────────────────────────────┘
```

### Index WAL Format
CBOR-encoded entries containing:
- Hash (byte array)
- Section ID (integer)
- Section index (integer)
- Ejection date (timestamp)
- Hit count (Fibonacci number)

## Build System

**Requirements**:
- CMake 3.29+
- C11 compiler
- C++17 compiler (for tests)

**Build**:
```bash
mkdir build && cd build
cmake ..
make
make test  # Run unit tests
```

**Output**:
- Static library: `liboffs.a`
- Test executables in `build/test/`

## Design Patterns

### 1. Promise-Based Async
All I/O operations return promises:
```c
promise_t *block_cache_get(block_cache_t *cache, buffer_t *hash);
// Resolves to block_t* or rejects with error
```

### 2. Reference Counting
Memory management via reference counting:
```c
block_t *block = block_create();
REFERENCE(block);  // Increment ref count
// Use block...
DEREFERENCE(block);  // Decrement ref count, free if 0
```

### 3. Write-Ahead Logging
Index changes logged before applying:
```c
index_entry_t entry = {...};
index_wal_log(index, &entry);  // Persist first
index_add(index, &entry);      // Then apply
```

### 4. LRU Eviction
Multiple caches use LRU policy:
- Block cache (hot blocks in memory)
- Section cache (open file handles)
- Configurable size limits

## Current Development Status

**Branch**: `streams`

**Recent Commits**:
- Weird memory corruption on locks
- Memory leak closing streams
- Working readable stream data reads
- Streams progress

**Active Development**:
- Stream-based I/O implementation
- Memory management fixes
- Concurrency debugging

## Future Considerations

**Potential Enhancements**:
- Compression for stored blocks
- Encryption layer
- Network transparency
- Distributed storage backend
- Garbage collection for orphaned blocks
- Snapshot and backup mechanisms