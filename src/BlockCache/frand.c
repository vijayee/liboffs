//
// Created by victor on 3/30/25.
//
#include "frand.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <stdio.h>
#include <openssl/rand.h>

uint8_t * frand(size_t size) {
  uint8_t* buffer = (uint8_t*) get_clear_memory(size);
  uint8_t* seed = (uint8_t*) get_clear_memory(4);
  if (RAND_bytes(seed, 4) != 1) {
    log_error("Out of entropy");
    abort();
  }
  // Seed the random number generator
  srand((unsigned int) *seed);

  for (size_t i = 0; i < size; i++) {
    if (i < 4) {
      buffer[i] = seed[i];
    } else {
      buffer[i] = rand() % 256;
    }
  }
  free(seed);
  return buffer;
}