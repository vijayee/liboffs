//
// Created by victor on 3/18/25.
//
#include "util.h"
#if _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#if _WIN32
void platform_lock(CRITICAL_SECTION* lock) {
  EnterCriticalSection(lock);
}
void platform_unlock(CRITICAL_SECTION* lock) {
  LeaveCriticalSection(lock);
}
void platform_lock_init(CRITICAL_SECTION* lock) {
  InitializeCriticalSection(lock);
}
void platform_lock_destroy(CRITICAL_SECTION* lock) {
  DeleteCriticalSection(lock, NULL);
}
#else
void platform_lock(pthread_mutex_t* lock) {
  pthread_mutex_lock(lock);
}
void platform_unlock(pthread_mutex_t* lock) {
  pthread_mutex_unlock(lock);
}
void platform_lock_init(pthread_mutex_t* lock) {
  pthread_mutex_init(lock, NULL);
}
void platform_lock_destroy(pthread_mutex_t* lock) {
  int result = pthread_mutex_destroy(lock);
}
#endif