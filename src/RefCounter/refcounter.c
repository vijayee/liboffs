//
// Created by victor on 3/18/25.
//
#include "refcounter.h"
#include "../Util/util.h"
#include <stdint.h>
#include <limits.h>
#include "refcounter.p.h"

void refcounter_init(refcounter_t* refcounter) {
  platform_lock_init(&refcounter->lock);
  platform_lock(&refcounter->lock);
  refcounter->count++;
  platform_unlock(&refcounter->lock);
}

void refcounter_yield(refcounter_t* refcounter) {
  platform_lock(&refcounter->lock);
  refcounter->yield++;
  platform_unlock(&refcounter->lock);
}

void* refcounter_reference(refcounter_t* refcounter) {
  platform_lock(&refcounter->lock);
  if (refcounter->yield > 0) {
    refcounter->yield--;
  } else if (refcounter->count < USHRT_MAX) {
    refcounter->count++;
  }
  platform_unlock(&refcounter->lock);
  return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
  platform_lock(&refcounter->lock);
  if ((refcounter->yield == 0) && (refcounter->count > 0)) {
    refcounter->count--;
  }
  platform_unlock(&refcounter->lock);
}

uint16_t refcounter_count(refcounter_t* refcounter) {
  uint16_t count;
  platform_lock(&refcounter->lock);
  count = refcounter->count;
  platform_unlock(&refcounter->lock);
  return count;
}

void refcounter_destroy_lock(refcounter_t* refcounter) {
  platform_lock_destroy(&refcounter->lock);
}

