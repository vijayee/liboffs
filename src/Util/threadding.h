//
// Created by victor on 3/18/25.
//

#ifndef LIBOFFS_THREADDING_H
#define LIBOFFS_THREADDING_H
#if _WIN32
#include <windows>
#define PlATFORMLOCKTYPE(N) CRITICAL_SECTION N
#define PLATFORMCONDITIONTYPE(N) CONDITION_VARIABLE N
#define PLATFORMTHREADTYPE HANDLE
void platform_lock(CRITICAL_SECTION* lock);
void platform_unlock(CRITICAL_SECTION* lock);
void platform_lock_init(CRITICAL_SECTION* lock);
void platform_lock_destroy(CRITICAL_SECTION* lock);
void platform_condition_init(CONDITION_VARIABLE* condition);
void platform_condition_wait(CRITICAL_SECTION* lock, CONDITION_VARIABLE* condition);
#else
#include <pthread.h>
#define PlATFORMLOCKTYPE(N) pthread_mutex_t N
#define PLATFORMCONDITIONTYPE(N) pthread_cond_t N
#define PLATFORMTHREADTYPE pthread_t
void platform_lock(pthread_mutex_t* lock);
void platform_unlock(pthread_mutex_t* lock);
void platform_lock_init(pthread_mutex_t* lock);
void platform_lock_destroy(pthread_mutex_t* lock);
void platform_condition_init(pthread_cond_t* condition);
void platform_condition_wait(pthread_mutex_t* lock, pthread_cond_t* condition);
#endif
#endif //LIBOFFS_THREADDONG_H
