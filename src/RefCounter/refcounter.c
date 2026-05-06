//
// Created by victor on 3/18/25.
//
#include "refcounter.h"
#include <stdint.h>
#include <limits.h>

void refcounter_init(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_lock_init(&refcounter->lock);
  platform_lock(&refcounter->lock);
  refcounter->count++;
  refcounter->pending_deref = 0;
  platform_unlock(&refcounter->lock);
#else
  refcounter->count = 1;
  refcounter->yield = 0;
  refcounter->pending_deref = 0;
#endif
}

void refcounter_yield(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_lock(&refcounter->lock);
  refcounter->yield++;
  platform_unlock(&refcounter->lock);
#else
  __atomic_add_fetch(&refcounter->yield, 1, __ATOMIC_RELAXED);
#endif
}

void* refcounter_reference(refcounter_t* refcounter) {
  if (refcounter == NULL) {
    return NULL;
  }
#ifndef OFFS_ATOMIC
  platform_lock(&refcounter->lock);
  if (refcounter->yield > 0) {
    refcounter->yield--;
    if (refcounter->pending_deref > 0) {
      refcounter->pending_deref--;
      refcounter->count--;
    }
  } else if (refcounter->count < USHRT_MAX) {
    refcounter->count++;
  }
  platform_unlock(&refcounter->lock);
#else
  if (__atomic_load_n(&refcounter->yield, __ATOMIC_RELAXED) > 0) {
    __atomic_fetch_sub(&refcounter->yield, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&refcounter->pending_deref, __ATOMIC_RELAXED) > 0) {
      __atomic_fetch_sub(&refcounter->pending_deref, 1, __ATOMIC_RELAXED);
      __atomic_fetch_sub(&refcounter->count, 1, __ATOMIC_RELAXED);
    }
  } else if (__atomic_load_n(&refcounter->count, __ATOMIC_RELAXED) < USHRT_MAX) {
    __atomic_fetch_add(&refcounter->count, 1, __ATOMIC_RELAXED);
  }
#endif
  return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_lock(&refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  } else if (refcounter->yield > 0) {
    refcounter->pending_deref++;
  }
  platform_unlock(&refcounter->lock);
#else
  if (__atomic_load_n(&refcounter->yield, __ATOMIC_RELAXED) > 0) {
    __atomic_fetch_add(&refcounter->pending_deref, 1, __ATOMIC_RELAXED);
  } else if (__atomic_load_n(&refcounter->count, __ATOMIC_RELAXED) > 0) {
    __atomic_fetch_sub(&refcounter->count, 1, __ATOMIC_RELAXED);
  }
#endif
}

uint16_t refcounter_count(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_lock(&refcounter->lock);
  uint16_t count = refcounter->count;
  platform_unlock(&refcounter->lock);
  return count;
#else
  return __atomic_load_n(&refcounter->count, __ATOMIC_RELAXED);
#endif
}

uint8_t refcounter_pending_derefs(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_lock(&refcounter->lock);
  uint8_t pending = refcounter->pending_deref;
  platform_unlock(&refcounter->lock);
  return pending;
#else
  return __atomic_load_n(&refcounter->pending_deref, __ATOMIC_RELAXED);
#endif
}

refcounter_t* refcounter_consume(refcounter_t** refcounter) {
  refcounter_t* holder = *refcounter;
  refcounter_yield(holder);
  *refcounter = NULL;
  return holder;
}

void refcounter_destroy_lock(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_lock_destroy(&refcounter->lock);
#endif
}