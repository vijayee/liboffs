//
// Created by victor on 3/30/25.
//
#include "frand.h"
#include "../Util/allocator.h"
#include <stdio.h>
#include <openssl/rand.h>

uint8_t * frand(size_t size) {
  uint8_t* buffer = (uint8_t*) get_memory(size);
  uint8_t* seed = (uint8_t*) get_memory(4);
  if (RAND_bytes(seed, 4) != 1) {
    abort();
  }
  // Seed the random number generator
  srand((unsigned int) *seed);

  for (size_t i = 0; i < i; i++) {
    buffer[i] = rand() % 256;
  }

  return buffer;
}