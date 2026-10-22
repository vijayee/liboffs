//
// Created by victor on 3/18/25.
//
#include "refcounter.h"
#include "../Util/log.h"
#include <stdint.h>
#include <limits.h>
#include <stdatomic.h>

/* Rate-limited NULL-dereference visibility. Every destroy path funnels
   through refcounter_dereference / refcounter_dereference_is_zero, so a
   NULL argument here used to crash the process mid-flight (2026-10-22
   cross-peer hunt: rcx = 0 AV inside dereference_is_zero while the daemon
   was churning find-block requests). Nulls are now tolerated (a NULL has
   nothing to decrement) but the first occurrences are logged loudly so the
   caller-level logic error stays visible. */
#define REFCOUNTER_NULL_LOG_MAX 20
static ATOMIC(uint64_t) refcounter_null_deref_count = ATOMIC_VAR_INIT(0);

static void refcounter_log_null_deref(void) {
  uint64_t n = atomic_fetch_add_explicit(&refcounter_null_deref_count, 1,
                                         memory_order_relaxed);
  if (n < REFCOUNTER_NULL_LOG_MAX) {
    log_error("refcounter: NULL dereference #%llu — the caller destroyed a "
              "NULL pointer; tolerated, but the calling site is a bug "
              "(2026-10-22 daemon-crash hunt)",
              (unsigned long long)(n + 1));
  }
}

void refcounter_init(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  refcounter->lock = platform_mutex_create();
  platform_mutex_lock(refcounter->lock);
  refcounter->count++;
  refcounter->pending_deref = 0;
  platform_mutex_unlock(refcounter->lock);
#else
  atomic_store_explicit((_Atomic uint32_t*)&refcounter->packed_state,
                        refcounter_pack(1, 0, 0), memory_order_release);
  refcounter->is_actor = 0;
#endif
}

void refcounter_init_actor(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  refcounter_init(refcounter);
#else
  atomic_store_explicit((_Atomic uint32_t*)&refcounter->packed_state,
                        refcounter_pack(1, 0, 0), memory_order_release);
  refcounter->is_actor = 1;
#endif
}

void refcounter_yield(refcounter_t* refcounter) {
  if (refcounter == NULL) {
    refcounter_log_null_deref();
    return;
  }
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  refcounter->yield++;
  platform_mutex_unlock(refcounter->lock);
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint8_t yield = refcounter_packed_yield(state);
    if (yield == 0xFFu) return;  /* saturate; never wrap to 0 */
    desired = refcounter_pack(refcounter_packed_count(state), (uint8_t)(yield + 1),
                              refcounter_packed_pending(state));
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_release, memory_order_relaxed));
#endif
}

void* refcounter_reference(refcounter_t* refcounter) {
  if (refcounter == NULL) return NULL;
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  if (refcounter->yield > 0) {
    refcounter->yield--;
    if (refcounter->pending_deref > 0) {
      refcounter->pending_deref--;
      refcounter->count--;
    }
  } else if (refcounter->count < USHRT_MAX) {
    refcounter->count++;
  }
  platform_mutex_unlock(refcounter->lock);
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      uint8_t new_yield = (uint8_t)(yield - 1);
      uint16_t new_count = count;
      uint8_t new_pending = pending;
      if (pending > 0) {
        new_pending = (uint8_t)(pending - 1);
        new_count = (uint16_t)(count - 1);
      }
      desired = refcounter_pack(new_count, new_yield, new_pending);
    } else if (count < USHRT_MAX) {
      desired = refcounter_pack((uint16_t)(count + 1), yield, pending);
    } else {
      desired = state;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_release, memory_order_relaxed));
#endif
  return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
  if (refcounter == NULL) {
    refcounter_log_null_deref();
    return;
  }
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  } else if (refcounter->yield > 0) {
    refcounter->pending_deref++;
  }
  platform_mutex_unlock(refcounter->lock);
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      if (pending == 0xFFu) return;  /* saturate */
      desired = refcounter_pack(count, yield, (uint8_t)(pending + 1));
    } else if (count > 0) {
      desired = refcounter_pack((uint16_t)(count - 1), yield, pending);
    } else {
      desired = state;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_acq_rel, memory_order_relaxed));
#endif
}

bool refcounter_dereference_is_zero(refcounter_t* refcounter) {
  if (refcounter == NULL) {
    refcounter_log_null_deref();
    return false;  /* nothing to decrement; the caller must not free */
  }
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  } else if (refcounter->yield > 0) {
    refcounter->pending_deref++;
  }
  bool is_zero = (refcounter->count == 0 && refcounter->pending_deref == 0);
  platform_mutex_unlock(refcounter->lock);
  return is_zero;
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  bool is_zero;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      if (pending == 0xFFu) return false;  /* saturate; not zero */
      desired = refcounter_pack(count, yield, (uint8_t)(pending + 1));
      is_zero = false;
    } else if (count > 0) {
      desired = refcounter_pack((uint16_t)(count - 1), yield, pending);
      is_zero = (count == 1) && (pending == 0);
    } else {
      desired = state;
      is_zero = (pending == 0);
    }
  } while (!atomic_compare_exchange_weak_explicit(
      (_Atomic uint32_t*)&refcounter->packed_state, &state, desired,
      memory_order_acq_rel, memory_order_relaxed));
  return is_zero;
#endif
}

uint16_t refcounter_count(refcounter_t* refcounter) {
  if (refcounter == NULL) {
    refcounter_log_null_deref();
    return 0;
  }
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  uint16_t count = refcounter->count;
  platform_mutex_unlock(refcounter->lock);
  return count;
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  return refcounter_packed_count(state);
#endif
}

uint8_t refcounter_pending_derefs(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  uint8_t pending = refcounter->pending_deref;
  platform_mutex_unlock(refcounter->lock);
  return pending;
#else
  uint32_t state = atomic_load_explicit((_Atomic uint32_t*)&refcounter->packed_state, memory_order_relaxed);
  return refcounter_packed_pending(state);
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
  platform_mutex_destroy(refcounter->lock);
#else
  (void)refcounter;
#endif
}