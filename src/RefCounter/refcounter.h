//
// Created by victor on 3/18/25.
//

#ifndef LIBOFFS_REFCOUNTER_H
#define LIBOFFS_REFCOUNTER_H
#include <stdint.h>
#include "../Util/threadding.h"

typedef struct refcounter_t {
#ifdef OFFS_ATOMIC
  _Atomic uint16_t count;
  _Atomic uint8_t yield;
#else
  uint16_t count;
  uint8_t yield;
  PlATFORMLOCKTYPE(lock);
#endif
} refcounter_t;
void refcounter_init(refcounter_t* refcounter);
void refcounter_yield(refcounter_t* refcounter);
void* refcounter_reference(refcounter_t* refcounter);
void refcounter_dereference(refcounter_t* refcounter);
uint16_t refcounter_count(refcounter_t* refcounter);
void refcounter_destroy_lock(refcounter_t* refcounter);
#endif //LIBOFFS_REFCOUNTER_H
