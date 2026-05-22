#define _DEFAULT_SOURCE

#include "platform_random.h"

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>

  int platform_random_bytes(uint8_t* buf, size_t len) {
    /* BCryptGenRandom always uses the system-preferred RNG provider */
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
  }
#else
  #include <unistd.h>

  int platform_random_bytes(uint8_t* buf, size_t len) {
    return getentropy(buf, len);
  }
#endif
