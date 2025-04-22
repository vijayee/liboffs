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
#include "../Util/util.h"
//#endif
//#endif

struct refcounter_t {
#ifdef OFFS_ATOMIC
  _Atomic uint16_t count;
  _Atomic uint8_t yield;
#else
  uint16_t count;
  uint8_t yield;
  PlATFORMLOCKTYPE(lock);
#endif
};

#endif //LIBOFFS_REFCOUNTER_P_H
