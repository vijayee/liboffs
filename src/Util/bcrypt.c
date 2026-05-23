#define _DEFAULT_SOURCE
#include "bcrypt.h"

#ifdef __linux__
#include <crypt.h>
#include <string.h>
#include <stdlib.h>

int bcrypt_check(const char* key, const char* hash) {
  if (key == NULL || hash == NULL) return -1;
  struct crypt_data data;
  memset(&data, 0, sizeof(data));
  char* result = crypt_r(key, hash, &data);
  if (result == NULL) return -1;
  return (strcmp(result, hash) == 0) ? 0 : -1;
}
#else
#include <string.h>

int bcrypt_check(const char* key, const char* hash) {
  (void)key;
  (void)hash;
  /* Non-Linux bcrypt not yet implemented.
     Auth will always fail on this platform until bcrypt is ported. */
  return -1;
}
#endif
