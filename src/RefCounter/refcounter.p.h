//
// Created by victor on 3/18/25.
//

#ifndef LIBOFFS_REFCOUNTER_P_H
#define LIBOFFS_REFCOUNTER_P_H
#include <stdint.h>
#include <stdlib.h>

//#ifndef OFFS_ATOMIC
//#ifndef __STDC_NO_ATOMICS__

//#else
#if _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
//#endif
//#endif

struct refcounter_t {
#ifdef OFFS_ATOMIC
  _Atomic uint16_t count;
  _Atomic uint8_t yield;
#else
  uint16_t count;
  uint8_t yield;
#if WIN32//TODO: Make this a macro
  CRITICAL_SECTION lock;
#else
  pthread_mutex_t lock;
#endif
#endif
};

#endif //LIBOFFS_REFCOUNTER_P_H
