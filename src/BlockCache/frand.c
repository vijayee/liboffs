//
// Created by victor on 3/30/25.
//
#include "frand.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <openssl/rand.h>
#include <stdint.h>

/* Fill a buffer with cryptographically secure random bytes.
   The owner-free property depends on random blocks being indistinguishable
   from any other random data, which requires an unenumerable output space.
   Do not seed a weak PRNG here. */
uint8_t * frand(size_t size) {
  if (size == 0) {
    return (uint8_t*) get_memory(1);
  }
  if (size > INT_MAX) {
    log_error("frand: requested size %zu exceeds RAND_bytes limit", size);
    return NULL;
  }
  uint8_t* buffer = (uint8_t*) get_memory(size);
  if (RAND_bytes(buffer, (int) size) != 1) {
    log_error("frand: RAND_bytes failed to produce %zu bytes", size);
    free(buffer);
    return NULL;
  }
  return buffer;
}