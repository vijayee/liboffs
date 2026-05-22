#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "../src/Platform/platform.h"
}

/* ================================================================
 * platform_time tests
 * ================================================================ */

TEST(TestPlatformTime, SleepDoesNotCrash) {
  platform_sleep_ms(1);
  SUCCEED();
}

TEST(TestPlatformTime, MonotonicIncreases) {
  uint64_t start = platform_monotonic_ns();
  platform_sleep_ms(10);
  uint64_t end = platform_monotonic_ns();
  EXPECT_GT(end, start);
}

TEST(TestPlatformTime, MonotonicIsNonZero) {
  uint64_t now = platform_monotonic_ns();
  EXPECT_GT(now, (uint64_t)0);
}

/* ================================================================
 * platform_random tests
 * ================================================================ */

TEST(TestPlatformRandom, ReturnsZeroOnSuccess) {
  uint8_t buf[32];
  int result = platform_random_bytes(buf, sizeof(buf));
  EXPECT_EQ(result, 0);
}

TEST(TestPlatformRandom, BytesAreNotAllZero) {
  uint8_t buf[64];
  memset(buf, 0, sizeof(buf));
  platform_random_bytes(buf, sizeof(buf));
  int nonzero = 0;
  for (size_t i = 0; i < sizeof(buf); i++) {
    if (buf[i] != 0) nonzero = 1;
  }
  EXPECT_EQ(nonzero, 1);
}

TEST(TestPlatformRandom, TwoCallsDiffer) {
  uint8_t buf1[32];
  uint8_t buf2[32];
  platform_random_bytes(buf1, sizeof(buf1));
  platform_random_bytes(buf2, sizeof(buf2));
  int same = (memcmp(buf1, buf2, sizeof(buf1)) == 0);
  EXPECT_EQ(same, 0);
}

/* ================================================================
 * platform_thread tests
 * ================================================================ */

TEST(TestPlatformThread, SetupStackReturnsZero) {
  int result = platform_thread_setup_stack();
  EXPECT_EQ(result, 0);
}

TEST(TestPlatformThread, CoreCountPositive) {
  int cores = platform_core_count();
  EXPECT_GT(cores, 0);
}

static void* _test_thread_fn(void* arg) {
  int* value = (int*)arg;
  *value = 42;
  return (void*)(intptr_t)(*value);
}

TEST(TestPlatformThread, CreateAndJoin) {
  int value = 0;
  platform_thread_t* thread = platform_thread_create(_test_thread_fn, &value);
  ASSERT_NE(thread, (platform_thread_t*)NULL);
  void* result = platform_thread_join(thread);
  EXPECT_EQ(value, 42);
  EXPECT_EQ((int)(intptr_t)result, 42);
}

TEST(TestPlatformThread, DetachRunsToCompletion) {
  int* value = (int*)calloc(1, sizeof(int));
  *value = 0;
  platform_thread_t* thread = platform_thread_create(_test_thread_fn, value);
  ASSERT_NE(thread, (platform_thread_t*)NULL);
  platform_thread_detach(thread);
  platform_sleep_ms(50);
  EXPECT_EQ(*value, 42);
  free(value);
}

TEST(TestPlatformThread, SelfIsNonZero) {
  uint64_t self = platform_thread_self();
  EXPECT_NE(self, (uint64_t)0);
}

/* --- Mutex --- */

static int _shared_counter = 0;
static platform_mutex_t* _counter_mutex = NULL;

static void* _mutex_increment_fn(void* arg) {
  int iterations = *(int*)arg;
  for (int i = 0; i < iterations; i++) {
    platform_mutex_lock(_counter_mutex);
    _shared_counter++;
    platform_mutex_unlock(_counter_mutex);
  }
  return NULL;
}

TEST(TestPlatformMutex, ConcurrentIncrement) {
  _shared_counter = 0;
  _counter_mutex = platform_mutex_create();
  ASSERT_NE(_counter_mutex, (platform_mutex_t*)NULL);

  int iterations = 10000;
  platform_thread_t* threads[4];
  for (int i = 0; i < 4; i++) {
    threads[i] = platform_thread_create(_mutex_increment_fn, &iterations);
    ASSERT_NE(threads[i], (platform_thread_t*)NULL);
  }

  for (int i = 0; i < 4; i++) {
    platform_thread_join(threads[i]);
  }

  EXPECT_EQ(_shared_counter, 4 * iterations);
  platform_mutex_destroy(_counter_mutex);
}

TEST(TestPlatformMutex, CreateDestroy) {
  platform_mutex_t* mutex = platform_mutex_create();
  ASSERT_NE(mutex, (platform_mutex_t*)NULL);
  platform_mutex_destroy(mutex);
}

/* --- RWLock --- */

TEST(TestPlatformRWLock, CreateDestroy) {
  platform_rwlock_t* rwlock = platform_rwlock_create();
  ASSERT_NE(rwlock, (platform_rwlock_t*)NULL);
  platform_rwlock_destroy(rwlock);
}

static platform_rwlock_t* _rwlock = NULL;
static int _rw_shared = 0;

static void* _rw_reader_fn(void* arg) {
  (void)arg;
  platform_rwlock_read_lock(_rwlock);
  int val = _rw_shared;
  platform_sleep_ms(5);
  /* value should not have changed while we hold read lock */
  EXPECT_EQ(_rw_shared, val);
  platform_rwlock_read_unlock(_rwlock);
  return NULL;
}

static void* _rw_writer_fn(void* arg) {
  (void)arg;
  platform_rwlock_write_lock(_rwlock);
  _rw_shared = 99;
  platform_sleep_ms(5);
  platform_rwlock_write_unlock(_rwlock);
  return NULL;
}

TEST(TestPlatformRWLock, ReadWriteExclusion) {
  _rwlock = platform_rwlock_create();
  ASSERT_NE(_rwlock, (platform_rwlock_t*)NULL);
  _rw_shared = 0;

  platform_thread_t* reader = platform_thread_create(_rw_reader_fn, NULL);
  platform_sleep_ms(1); /* let reader acquire read lock first */
  platform_thread_t* writer = platform_thread_create(_rw_writer_fn, NULL);

  platform_thread_join(reader);
  platform_thread_join(writer);
  platform_rwlock_destroy(_rwlock);
}

/* --- Condition Variable --- */

static platform_mutex_t* _cv_mutex = NULL;
static platform_condvar_t* _cv_cond = NULL;
static int _cv_ready = 0;

static void* _cv_waiter_fn(void* arg) {
  (void)arg;
  platform_mutex_lock(_cv_mutex);
  while (!_cv_ready) {
    platform_condvar_wait(_cv_cond, _cv_mutex);
  }
  platform_mutex_unlock(_cv_mutex);
  return NULL;
}

static void* _cv_signaller_fn(void* arg) {
  (void)arg;
  platform_sleep_ms(20);
  platform_mutex_lock(_cv_mutex);
  _cv_ready = 1;
  platform_condvar_signal(_cv_cond);
  platform_mutex_unlock(_cv_mutex);
  return NULL;
}

TEST(TestPlatformCondvar, SignalWakeup) {
  _cv_mutex = platform_mutex_create();
  _cv_cond = platform_condvar_create();
  ASSERT_NE(_cv_mutex, (platform_mutex_t*)NULL);
  ASSERT_NE(_cv_cond, (platform_condvar_t*)NULL);
  _cv_ready = 0;

  platform_thread_t* waiter = platform_thread_create(_cv_waiter_fn, NULL);
  platform_thread_t* signaller = platform_thread_create(_cv_signaller_fn, NULL);

  platform_thread_join(waiter);
  platform_thread_join(signaller);

  EXPECT_EQ(_cv_ready, 1);
  platform_condvar_destroy(_cv_cond);
  platform_mutex_destroy(_cv_mutex);
}

TEST(TestPlatformCondvar, CreateDestroy) {
  platform_condvar_t* cv = platform_condvar_create();
  ASSERT_NE(cv, (platform_condvar_t*)NULL);
  platform_condvar_destroy(cv);
}

/* --- Barrier --- */

static platform_barrier_t* _barrier = NULL;
static volatile int _barrier_phase1 = 0;
static volatile int _barrier_phase2 = 0;

static void* _barrier_worker_fn(void* arg) {
  (void)arg;
  _barrier_phase1 = 1;
  platform_barrier_wait(_barrier);
  _barrier_phase2 = 1;
  return NULL;
}

TEST(TestPlatformBarrier, TwoThreadSync) {
  _barrier = platform_barrier_create(2);
  ASSERT_NE(_barrier, (platform_barrier_t*)NULL);
  _barrier_phase1 = 0;
  _barrier_phase2 = 0;

  platform_thread_t* worker = platform_thread_create(_barrier_worker_fn, NULL);

  /* Spin until worker reaches barrier */
  while (_barrier_phase1 == 0) {
    platform_sleep_ms(1);
  }

  /* Phase 2 should NOT be set yet (worker blocked at barrier) */
  EXPECT_EQ(_barrier_phase2, 0);

  /* Main thread enters barrier — both threads released */
  int is_serial = platform_barrier_wait(_barrier);
  /* One thread gets PTHREAD_BARRIER_SERIAL_THREAD / true return */
  (void)is_serial;

  platform_thread_join(worker);

  EXPECT_EQ(_barrier_phase2, 1);
  platform_barrier_destroy(_barrier);
}

TEST(TestPlatformBarrier, CreateDestroy) {
  platform_barrier_t* barrier = platform_barrier_create(1);
  ASSERT_NE(barrier, (platform_barrier_t*)NULL);
  platform_barrier_destroy(barrier);
}
