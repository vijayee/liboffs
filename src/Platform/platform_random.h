#ifndef OFFS_PLATFORM_RANDOM_H
#define OFFS_PLATFORM_RANDOM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int platform_random_bytes(uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_RANDOM_H */
