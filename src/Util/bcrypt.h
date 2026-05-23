#ifndef OFFS_UTIL_BCRYPT_H
#define OFFS_UTIL_BCRYPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Check a plaintext key against a bcrypt hash ($2b$ prefix).
   Returns 0 on match, -1 on mismatch or error. */
int bcrypt_check(const char* key, const char* hash);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_UTIL_BCRYPT_H */
