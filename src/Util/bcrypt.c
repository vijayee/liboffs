//
// Created by victor on 5/23/25.
//

#include "bcrypt.h"
#include <string.h>

/* Vendored Solar Designer crypt_blowfish (deps/bcrypt submodule, built as the
   bcrypt_os CMake target). _crypt_blowfish_rn parses the $2b$cost$salt prefix
   from the supplied hash, recomputes the digest, and writes the full hash into
   the caller buffer; it returns that buffer on success or NULL on malformed
   input. The prototype is declared here rather than including the vendored
   crypt_blowfish.h, so this file's own bcrypt.h stays the only bcrypt header on
   the include path and there is no header-name interaction. The 64-byte buffer
   matches OpenBSD libcrypt's BCRYPT_HASHSIZE. */
#define OFFS_BCRYPT_HASHSIZE 64
extern char* _crypt_blowfish_rn(const char* key, const char* setting,
                                char* output, int size);

int bcrypt_check(const char* key, const char* hash) {
  if (key == NULL || hash == NULL) return -1;
  /* Only $2a/$2b/$2y are valid bcrypt prefixes. Reject $2x (the legacy
     sign-extension-bug variant) and anything malformed before hashing, so a
     bogus setting is never handed to _crypt_blowfish_rn. */
  if (hash[0] != '$' || hash[1] != '2' ||
      (hash[2] != 'a' && hash[2] != 'b' && hash[2] != 'y') ||
      hash[3] != '$') {
    return -1;
  }
  char out[OFFS_BCRYPT_HASHSIZE];
  if (_crypt_blowfish_rn(key, hash, out, (int)sizeof(out)) == NULL) return -1;
  return (strcmp(out, hash) == 0) ? 0 : -1;
}
