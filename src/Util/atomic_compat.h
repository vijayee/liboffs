//
// Created by victor on 5/11/26.
//

#ifndef OFFS_ATOMIC_COMPAT_H
#define OFFS_ATOMIC_COMPAT_H

#ifdef __cplusplus
#include <atomic>
#define ATOMIC(T) std::atomic<T>
#define ATOMIC_STORE(ptr, val) (ptr)->store(val)
#define ATOMIC_LOAD(ptr) (ptr)->load()
#define ATOMIC_EXCHANGE(ptr, val) (ptr)->exchange(val)
#else
#include <stdatomic.h>
#define ATOMIC(T) _Atomic(T)
#define ATOMIC_STORE(ptr, val) atomic_store(ptr, val)
#define ATOMIC_LOAD(ptr) atomic_load(ptr)
#define ATOMIC_EXCHANGE(ptr, val) atomic_exchange(ptr, val)
#endif

#endif //OFFS_ATOMIC_COMPAT_H
