//
// Created by victor on 3/22/25.
//

#include <gtest/gtest.h>
#include <functional>
#include <gmock/gmock.h>
#include <string.h>
extern "C" {
#include "../src/RefCounter/refcounter.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/frand.h"
#include "../src/BlockCache/block.h"
#include "../src/Util/allocator.h"
#include "../src/Workers/error.h"
#include "../src/Workers/promise.h"
#include "../src/Workers/priority.h"
#include "../src/Workers/work.h"
#include "../src/Workers/queue.h"
#include "../src/Workers/pool.h"
#include "../src/BlockCache/fibonacci.h"
#include "../src/BlockCache/index.h"
#include <time.h>
}

using ::testing::_;
using ::testing::MockFunction;
using ::testing::AtLeast;



TEST(TestFRand, TestFRandFunction) {
  uint8_t* rand = frand(20);

  buffer_t* buf1 = buffer_create_from_existing_memory((uint8_t *) get_clear_memory(20), 20);
  buffer_t* buf2 = buffer_create_from_existing_memory(rand, 20);
  buffer_t* buf3 = buffer_concat(buf1, buf2);

  buffer_destroy(buf1);
  buffer_destroy(buf2);
  buffer_destroy(buf3);
}

TEST(TestError, TestErrorCreateDestroy) {
  std::string message = "This is an error";
  char* cmessage = (char*)message.c_str();
  char* file = (char*)__FILE__;
  char* func = (char*)__func__;
  int line = __LINE__;
  async_error_t* error = error_create(cmessage, file, func, line);
  EXPECT_EQ(strcmp(error->message, cmessage), 0);
  EXPECT_NE(error->message, cmessage);
  EXPECT_EQ(strcmp(error->file, file), 0);
  EXPECT_NE(error->file, file);
  EXPECT_EQ(strcmp(error->function, func), 0);
  EXPECT_NE(error->function, func);
  EXPECT_EQ(error->line, line);
  error_destroy(error);
}



class TestPromise : public testing::Test {
public:

  MockFunction<void((void*, void*))> mockCallback;
  MockFunction<void((void*, async_error_t*))> mockErrCallback;
};

void callbackWrapper(void* ctx, void* payload) {
  auto test = static_cast<TestPromise*>(ctx);
  test->mockCallback.Call(ctx, payload);
}
void callbackErrWrapper(void* ctx, async_error_t* err) {
  auto test = static_cast<TestPromise*>(ctx);
  test->mockErrCallback.Call(ctx, err);
}

TEST_F(TestPromise, TestPromiseExecution) {
  std::string message = "This is an error";
  char* cmessage = (char*)message.c_str();
  char* file = (char*)__FILE__;
  char* func = (char*)__func__;
  int line = __LINE__;
  async_error_t* error = error_create(cmessage, file, func, line);
  promise_t* promise1 = promise_create(callbackWrapper, callbackErrWrapper, this);
  promise_t* promise2 = promise_create(callbackWrapper, callbackErrWrapper, this);
  EXPECT_CALL(mockCallback, Call(_,_)).Times(1);
  EXPECT_CALL(mockErrCallback, Call(_,_)).Times(1);
  promise_resolve(promise1, &message);
  promise_reject(promise1, error);
  promise_reject(promise2, error);
  promise_resolve(promise2, &message);
  promise_destroy(promise1);
  promise_destroy(promise2);
  error_destroy(error);
}

TEST(TestPriority, TestPriorityFunctions) {
  priority_init();
  priority_t priority1 = priority_get_next();
  priority_t priority2 = priority_get_next();
  priority_t priority3 = priority_get_next();
  priority_t priority4 = priority_get_next();
  priority_t priority5 = priority1;
  EXPECT_EQ(priority_compare(&priority1, &priority2), -1);
  EXPECT_EQ(priority_compare(&priority2, &priority3), -1);
  EXPECT_EQ(priority_compare(&priority4, &priority1), 1);
  EXPECT_EQ(priority_compare(&priority5, &priority1), 0);
}


class TestWork : public testing::Test {
public:
  MockFunction<void((void*, void*))> mockCallback;
  MockFunction<void((void*, async_error_t*))> mockErrCallback;
};

void promiseWrapper(void* ctx) {
  promise_t* promise = static_cast<promise_t *>(ctx);
  promise_resolve(promise, NULL);
  promise_destroy(promise);
}
void promiseErrWrapper(void* ctx) {
  promise_t* promise = static_cast<promise_t *>(ctx);
  promise_reject(promise, NULL);
  promise_destroy(promise);
}

TEST_F(TestWork, TestWorkExecution) {
  priority_init();
  promise_t* promise1 = promise_create(callbackWrapper, callbackErrWrapper, this);
  promise_t* promise2 = promise_create(callbackWrapper, callbackErrWrapper, this);
  EXPECT_CALL(mockCallback, Call(_,_)).Times(1);
  EXPECT_CALL(mockErrCallback, Call(_,_)).Times(1);
  work_t* work1 = work_create(priority_get_next(), refcounter_reference((refcounter_t*)promise1), promiseWrapper, promiseErrWrapper);
  work_t* work2 = work_create(priority_get_next(), refcounter_reference((refcounter_t*)promise2), promiseWrapper, promiseErrWrapper);
  work_execute(work1);
  work_abort(work2);
  work_destroy(work1);
  work_destroy(work2);
  promise_destroy(promise1);
  promise_destroy(promise2);
}
void fakeWork(void* ctx) {

}
TEST(TestWorkQueue, TestWorkQueueFunctions) {
  priority_init();
  priority_t firstPriority = priority_get_next();
  priority_t secondPriority = priority_get_next();
  priority_t thirdPriority = priority_get_next();
  priority_t fourthPriority = priority_get_next();
  work_t* work1 = work_create(secondPriority, NULL, fakeWork, fakeWork);
  work_t* work2 = work_create(fourthPriority, NULL, fakeWork, fakeWork);
  work_t* work3 = work_create(fourthPriority, NULL, fakeWork, fakeWork);
  work_t* work4 = work_create(secondPriority, NULL, fakeWork, fakeWork);
  work_t* work5 = work_create(firstPriority, NULL, fakeWork, fakeWork);
  work_queue_t queue = {0};
  work_queue_init(&queue);
  work_enqueue(&queue,work1);
  work_enqueue(&queue,work2);
  work_enqueue(&queue,work3);
  work_enqueue(&queue,work4);
  work_enqueue(&queue,work5);
  EXPECT_EQ(work_dequeue(&queue), work5);
  EXPECT_EQ(work_dequeue(&queue), work1);
  EXPECT_EQ(work_dequeue(&queue), work4);
  EXPECT_EQ(work_dequeue(&queue), work2);
  EXPECT_EQ(work_dequeue(&queue), work3);
  work_enqueue(&queue,work1);
  work_enqueue(&queue,work2);
  work_enqueue(&queue,work3);
  work_enqueue(&queue,work4);
  work_enqueue(&queue,work5);
  EXPECT_EQ(work_dequeue(&queue), work5);
  EXPECT_EQ(work_dequeue(&queue), work1);
  EXPECT_EQ(work_dequeue(&queue), work4);
  EXPECT_EQ(work_dequeue(&queue), work2);
  EXPECT_EQ(work_dequeue(&queue), work3);
  work_destroy(work1);
  work_destroy(work2);
  work_destroy(work3);
  work_destroy(work4);
  work_destroy(work5);
}

TEST(TestCoreCount, GetCoreCount) {
  int corecnt = platform_core_count();
  printf("Machine has %d cores\n", corecnt);
  EXPECT_GT(corecnt, 0);
}

class TestWorkerPool : public testing::Test {
public:
  work_pool_t* pool;
  MockFunction<void((void*))> mockExecuteCallback;
  MockFunction<void((void*))> mockAbortCallback;
  MockFunction<void((void*))> mockShutdownCallback;
  MockFunction<void((void*))> mockShutdownAbortCallback;
};

void onExecute(void* ctx) {
  auto test = static_cast<TestWorkerPool*>(ctx);
  test->mockExecuteCallback.Call(ctx);
}
void onAbort(void* ctx) {
  auto test = static_cast<TestWorkerPool*>(ctx);
  test->mockAbortCallback.Call(ctx);
}

void Shutdown(void* ctx) {
  auto test = static_cast<TestWorkerPool*>(ctx);
  platform_signal_condition(&test->pool->shutdown);
  test->mockShutdownCallback.Call(ctx);
}
void ShutdownAborted(void* ctx) {
  auto test = static_cast<TestWorkerPool*>(ctx);
  test->mockShutdownAbortCallback.Call(ctx);
}

TEST_F(TestWorkerPool, TestPoolLaunch) {
  size_t size = 256;
  int workId[size];
  int corecnt = platform_core_count();
  pool = work_pool_create(corecnt);

  priority_t priority = priority_get_next();
  work_pool_launch(pool);
  EXPECT_CALL(mockExecuteCallback, Call(_)).Times(size);
  EXPECT_CALL(mockAbortCallback, Call(_)).Times(0);
  EXPECT_CALL(mockShutdownCallback, Call(_)).Times(1);
  EXPECT_CALL(mockShutdownAbortCallback, Call(_)).Times(0);

  for (int i = 0; i < size; i++) {
    workId[i] = i;
    work_t* work = work_create(priority, this, onExecute, onAbort);
    refcounter_yield((refcounter_t*) work);
    work_pool_enqueue(pool, work);
  }
  work_pool_wait_for_idle_signal(pool);
  work_t* work = work_create(priority, this, Shutdown, ShutdownAborted);
  refcounter_yield((refcounter_t*) work);
  work_pool_enqueue(pool, work);
  work_pool_wait_for_shutdown_signal(pool);
  work_pool_shutdown(pool);
  work_pool_join_all(pool);
  work_pool_destroy(pool);
  pool= NULL;
}

TEST_F(TestWorkerPool, TestPoolShutdown) {
  size_t size = 256;
  int workId[size];
  int corecnt = platform_core_count();
  pool = work_pool_create(corecnt);

  priority_t priority = priority_get_next();
  EXPECT_CALL(mockExecuteCallback, Call(_)).Times(AtLeast(1));
  EXPECT_CALL(mockAbortCallback, Call(_)).Times(AtLeast(1));
  EXPECT_CALL(mockShutdownCallback, Call(_)).Times(1);
  EXPECT_CALL(mockShutdownAbortCallback, Call(_)).Times(0);

  for (int i = 0; i < size; i++) {
    if(i == 100) {
      work_t* work = work_create(priority, this, Shutdown, ShutdownAborted);
      refcounter_yield((refcounter_t*) work);
      work_pool_enqueue(pool, work);
    }
    workId[i] = i;
    work_t *work = work_create(priority, this, onExecute, onAbort);
    refcounter_yield((refcounter_t *) work);
    work_pool_enqueue(pool, work);
  }

  work_pool_launch(pool);
  work_pool_wait_for_shutdown_signal(pool);
  work_pool_shutdown(pool);
  work_pool_join_all(pool);
  work_pool_destroy(pool);
  pool= NULL;
}

TEST(TestFibonacciHitCounter, HitCounterFunctions) {
  EXPECT_EQ(fibonacci(0), 0);
  EXPECT_EQ(fibonacci(1), 1);
  EXPECT_EQ(fibonacci(2), 1);
  fibonacci_hit_counter_t  counter1 = fibonacci_hit_counter_create();
  fibonacci_hit_counter_t  counter2 = fibonacci_hit_counter_from(6,5);
  fibonacci_hit_counter_t  counter3 = fibonacci_hit_counter_from(6,5);
  EXPECT_EQ(counter2.threshold, 8);
  EXPECT_EQ(fibonacci_hit_counter_compare(&counter1, &counter2), -1);
  EXPECT_EQ(fibonacci_hit_counter_compare(&counter2, &counter1), 1);
  EXPECT_EQ(fibonacci_hit_counter_compare(&counter2, &counter3), 0);
  for (int i = 0; i < 4; i++) {
    if (i == 3) {
      EXPECT_EQ(fibonacci_hit_counter_increment(&counter2), 1);
    } else {
      EXPECT_EQ(fibonacci_hit_counter_increment(&counter2), 0);
    }
  }
  EXPECT_EQ(counter2.threshold, 13);
  EXPECT_EQ(counter2.count, 0);
  EXPECT_EQ(fibonacci_hit_counter_decrement(&counter2), 1);
  EXPECT_EQ(counter2.threshold, 8);
  EXPECT_EQ(counter2.count, 8);
  EXPECT_EQ(fibonacci_hit_counter_decrement(&counter2), 0);
}
