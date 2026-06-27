//
// Created by victor on 3/18/25.
//
#include "refcounter.h"
#include <stdint.h>
#include <limits.h>

#if defined(_MSC_VER)
  #include <stdatomic.h>
  /* MSVC doesn't expose GCC's __atomic_* builtins; map the patterns we use
     onto C11 atomics. Under MSVC the refcounter struct members are
     _Atomic(uint16_t)/_Atomic(uint8_t), so &(p) is already the right
     atomic-qualified pointer. */
  #define OFFS_ATOMIC_LOAD_N(p, order)    atomic_load_explicit(&(p), (memory_order)(order))
  #define OFFS_ATOMIC_FETCH_ADD(p, v, order) atomic_fetch_add_explicit(&(p), (v), (memory_order)(order))
  #define OFFS_ATOMIC_FETCH_SUB(p, v, order) atomic_fetch_sub_explicit(&(p), (v), (memory_order)(order))
  #define OFFS_MO_RELAXED   memory_order_relaxed
  #define OFFS_MO_ACQ_REL   memory_order_acq_rel
#else
  #define OFFS_ATOMIC_LOAD_N(p, order)    __atomic_load_n(&(p), (order))
  #define OFFS_ATOMIC_FETCH_ADD(p, v, order) __atomic_fetch_add(&(p), (v), (order))
  #define OFFS_ATOMIC_FETCH_SUB(p, v, order) __atomic_fetch_sub(&(p), (v), (order))
  #define OFFS_MO_RELAXED   __ATOMIC_RELAXED
  #define OFFS_MO_ACQ_REL   __ATOMIC_ACQ_REL
#endif

void refcounter_init(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  refcounter->lock = platform_mutex_create();
  platform_mutex_lock(refcounter->lock);
  refcounter->count++;
  refcounter->pending_deref = 0;
  platform_mutex_unlock(refcounter->lock);
#else
  refcounter->count = 1;
  refcounter->yield = 0;
  refcounter->pending_deref = 0;
  refcounter->is_actor = 0;
#endif
}

void refcounter_init_actor(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  refcounter_init(refcounter);
#else
  refcounter->count = 1;
  refcounter->yield = 0;
  refcounter->pending_deref = 0;
  refcounter->is_actor = 1;
#endif
}

void refcounter_yield(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  refcounter->yield++;
  platform_mutex_unlock(refcounter->lock);
#else
  if (refcounter->is_actor) {
    refcounter->yield++;
  } else {
    OFFS_ATOMIC_FETCH_ADD(refcounter->yield, 1, OFFS_MO_RELAXED);
  }
#endif
}

void* refcounter_reference(refcounter_t* refcounter) {
  if (refcounter == NULL) {
    return NULL;
  }
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
  if (refcounter->is_actor) {
    if (refcounter->yield > 0) {
      refcounter->yield--;
      if (refcounter->pending_deref > 0) {
        refcounter->pending_deref--;
        refcounter->count--;
      }
    } else if (refcounter->count < USHRT_MAX) {
      refcounter->count++;
    }
  } else {
    if (OFFS_ATOMIC_LOAD_N(refcounter->yield, OFFS_MO_RELAXED) > 0) {
      OFFS_ATOMIC_FETCH_SUB(refcounter->yield, 1, OFFS_MO_RELAXED);
      if (OFFS_ATOMIC_LOAD_N(refcounter->pending_deref, OFFS_MO_RELAXED) > 0) {
        OFFS_ATOMIC_FETCH_SUB(refcounter->pending_deref, 1, OFFS_MO_RELAXED);
        OFFS_ATOMIC_FETCH_SUB(refcounter->count, 1, OFFS_MO_RELAXED);
      }
    } else if (OFFS_ATOMIC_LOAD_N(refcounter->count, OFFS_MO_RELAXED) < USHRT_MAX) {
      OFFS_ATOMIC_FETCH_ADD(refcounter->count, 1, OFFS_MO_RELAXED);
    }
  }
#endif
  return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  } else if (refcounter->yield > 0) {
    refcounter->pending_deref++;
  }
  platform_mutex_unlock(refcounter->lock);
#else
  if (refcounter->is_actor) {
    if (refcounter->yield == 0 && refcounter->count > 0) {
      refcounter->count--;
    } else if (refcounter->yield > 0) {
      refcounter->pending_deref++;
    }
  } else {
    if (OFFS_ATOMIC_LOAD_N(refcounter->yield, OFFS_MO_RELAXED) > 0) {
      OFFS_ATOMIC_FETCH_ADD(refcounter->pending_deref, 1, OFFS_MO_RELAXED);
    } else if (OFFS_ATOMIC_LOAD_N(refcounter->count, OFFS_MO_RELAXED) > 0) {
      OFFS_ATOMIC_FETCH_SUB(refcounter->count, 1, OFFS_MO_ACQ_REL);
    }
  }
#endif
}

bool refcounter_dereference_is_zero(refcounter_t* refcounter) {
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
  if (refcounter->is_actor) {
    if (refcounter->yield == 0 && refcounter->count > 0) {
      refcounter->count--;
    } else if (refcounter->yield > 0) {
      refcounter->pending_deref++;
    }
    return (refcounter->count == 0 && refcounter->pending_deref == 0);
  } else {
    if (OFFS_ATOMIC_LOAD_N(refcounter->yield, OFFS_MO_RELAXED) > 0) {
      OFFS_ATOMIC_FETCH_ADD(refcounter->pending_deref, 1, OFFS_MO_ACQ_REL);
      return false;
    } else {
      uint16_t old_count = OFFS_ATOMIC_FETCH_SUB(refcounter->count, 1, OFFS_MO_ACQ_REL);
      return old_count == 1;
    }
  }
#endif
}

uint16_t refcounter_count(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  uint16_t count = refcounter->count;
  platform_mutex_unlock(refcounter->lock);
  return count;
#else
  if (refcounter->is_actor) {
    return refcounter->count;
  } else {
    return OFFS_ATOMIC_LOAD_N(refcounter->count, OFFS_MO_RELAXED);
  }
#endif
}

uint8_t refcounter_pending_derefs(refcounter_t* refcounter) {
#ifndef OFFS_ATOMIC
  platform_mutex_lock(refcounter->lock);
  uint8_t pending = refcounter->pending_deref;
  platform_mutex_unlock(refcounter->lock);
  return pending;
#else
  if (refcounter->is_actor) {
    return refcounter->pending_deref;
  } else {
    return OFFS_ATOMIC_LOAD_N(refcounter->pending_deref, OFFS_MO_RELAXED);
  }
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