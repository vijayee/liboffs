//
// Created by victor on 3/18/25.
//

#ifndef LIBOFFS_UTIL_H
#define LIBOFFS_UTIL_H
#if _WIN32
#include <windows>
#define PlATFORMLOCKTYPE(N) CRITICAL_SECTION N
void platform_lock(CRITICAL_SECTION* lock);
void platform_unlock(CRITICAL_SECTION* lock);
void platform_lock_init(CRITICAL_SECTION* lock);
void platform_lock_destroy(CRITICAL_SECTION* lock);
#else
#include <pthread.h>
#define PlATFORMLOCKTYPE(N) pthread_mutex_t N
void platform_lock(pthread_mutex_t* lock);
void platform_unlock(pthread_mutex_t* lock);
void platform_lock_init(pthread_mutex_t* lock);
void platform_lock_destroy(pthread_mutex_t* lock);
#endif
#endif //LIBOFFS_UTIL_H
