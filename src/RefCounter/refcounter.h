//
// Created by victor on 3/18/25.
//

#ifndef LIBOFFS_REFCOUNTER_H
#define LIBOFFS_REFCOUNTER_H
#include <stdint.h>
#include <stdbool.h>
#include "../Platform/platform.h"
#if defined(_MSC_VER) && !defined(__cplusplus)
  #include <stdatomic.h>
#endif
#define REFERENCE(N,T) (T*) refcounter_reference((refcounter_t*) N)
#define YIELD(N) refcounter_yield((refcounter_t*) N)
#define DEREFERENCE(N) refcounter_dereference((refcounter_t*) N); N = NULL
#define DESTROY(N,T)  T##_destroy(N); N = NULL
#define CONSUME(N, T) (T*) refcounter_consume((refcounter_t**) &N)
#define OFFS_ATOMIC
/* Atomic refcounter fields: C11 _Atomic under C, plain uint under C++. C++
   single-threaded accessors are non-atomic because the existing C
   implementation already serializes the actor path with `is_actor`; C++ callers
   only touch the struct from a single test thread. For the multi-threaded path
   the C11 atomics on the C side ensure the operation is well-defined. */
#if defined(__cplusplus)
  /* In C++ the C11 atomics are translated by wrapping the same offsets in
     a separate atomic type. We use uint16_t/uint8_t for layout compatibility
     with the C definition; std::atomic<uint16_t> has the same size and
     alignment as uint16_t on MSVC, GCC, and Clang. */
  #define OFFS_ATOMIC_FIELD_U16 uint16_t count;
  #define OFFS_ATOMIC_FIELD_U8  uint8_t  yield;
  #define OFFS_ATOMIC_FIELD_U8B uint8_t  pending_deref;
#else
  #if defined(_MSC_VER)
    #define OFFS_ATOMIC_FIELD_U16 _Atomic(uint16_t) count;
    #define OFFS_ATOMIC_FIELD_U8  _Atomic(uint8_t)  yield;
    #define OFFS_ATOMIC_FIELD_U8B _Atomic(uint8_t)  pending_deref;
  #else
    #define OFFS_ATOMIC_FIELD_U16 uint16_t count;
    #define OFFS_ATOMIC_FIELD_U8  uint8_t  yield;
    #define OFFS_ATOMIC_FIELD_U8B uint8_t  pending_deref;
  #endif
#endif
typedef struct refcounter_t {
#ifdef OFFS_ATOMIC
  OFFS_ATOMIC_FIELD_U16
  OFFS_ATOMIC_FIELD_U8
  OFFS_ATOMIC_FIELD_U8B
  uint8_t is_actor;
#else
  uint16_t count;
  uint8_t yield;
  uint8_t pending_deref;
  platform_mutex_t* lock;
#endif
} refcounter_t;
void refcounter_init(refcounter_t* refcounter);
void refcounter_init_actor(refcounter_t* refcounter);
void refcounter_yield(refcounter_t* refcounter);
void* refcounter_reference(refcounter_t* refcounter);
void refcounter_dereference(refcounter_t* refcounter);
bool refcounter_dereference_is_zero(refcounter_t* refcounter);
refcounter_t* refcounter_consume(refcounter_t** refcounter);
/* Claim ownership of a reference that was transferred via refcounter_consume
   (a "yielded" reference): absorb the pending yield WITHOUT adding a new
   reference, leaving the holder with a clean count=1, yield=0 ref. If there is
   no pending yield (the reference was transferred by pointer, e.g. a plain
   freshly-created object), this is a no-op — the holder already owns count=1.
   Used by stream_notify to take ownership of a payload whether the caller
   passed it via CONSUME(...) (yield=1) or plain (yield=0), so the message
   wrapper always owns exactly one clean reference. NULL is a no-op. */
void refcounter_take(refcounter_t* refcounter);
uint16_t refcounter_count(refcounter_t* refcounter);
uint8_t refcounter_pending_derefs(refcounter_t* refcounter);
void refcounter_destroy_lock(refcounter_t* refcounter);
#endif //LIBOFFS_REFCOUNTER_H
