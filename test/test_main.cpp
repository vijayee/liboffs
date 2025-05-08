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
#include "../src/Util/threadding.h"
#include "../src/BlockCache/fibonacci.h"
#include "../src/BlockCache/index.h"
#include <time.h>
}

using ::testing::_;
using ::testing::MockFunction;
using ::testing::AtLeast;

TEST(TestRefCounter, TestRefCounterFunctions) {
  refcounter_t* refc1 = (refcounter_t*) calloc(sizeof(refcounter_t), 1);
  refcounter_init(refc1);

  refcounter_t* refc2 = (refcounter_t*) refcounter_reference(refc1);
  EXPECT_EQ(refc1, refc2);

  EXPECT_EQ(refcounter_count(refc1), 2);
  refcounter_yield((refcounter_t*) refc2);
  refc2 = NULL;
  refcounter_t* refc3 = (refcounter_t*) refcounter_reference(refc1);
  EXPECT_EQ(refcounter_count(refc3), 2);
  EXPECT_EQ(refc1, refc3);
  refcounter_dereference(refc3);
  refc3 = NULL;
  EXPECT_EQ(refcounter_count(refc1), 1);
  refcounter_destroy_lock(refc1);
  free(refc1);
}

TEST(TestBuffer, TestBufferCreation) {
  buffer_t* buf = buffer_create(25);
  ASSERT_EQ(buf->size, 25);
  for (size_t i = 0; i < buf->size; i++) {
    EXPECT_EQ(buffer_get_index(buf, i), 0);
  }
  uint8_t data[10] = {1,2,3,4,5,6,7,8};
  buffer_t* buf2 = buffer_create_from_pointer_copy((uint8_t*) &data, 8);
  for (size_t i = 0; i < 8; i++) {
    EXPECT_EQ(buffer_get_index(buf2, i), data[i]);
    EXPECT_NE(&buf2->data[i], &data[i]);
  }
  uint8_t* data2 = (uint8_t*) calloc(8, sizeof(uint8_t));
  memcpy(&data, data2,8);
  buffer_t* buf3 = buffer_create_from_existing_memory(data2, 8);
  for (size_t i = 0; i < 8; i++) {
    EXPECT_EQ(buffer_get_index(buf3, i), data[i]);
    EXPECT_EQ(&buf3->data[i], &data2[i]);
  }
  buffer_destroy(buf3);
  buffer_destroy(buf2);
  buffer_destroy(buf);
}

TEST(TestBuffer, TestBufferBitwise) {
  uint8_t data1[4] = {255, 255, 255,255};
  buffer_t* buf1 = buffer_create_from_pointer_copy((uint8_t*) data1, 4);
  uint8_t data2[4] = {0,0,0,0};
  buffer_t* buf2 = buffer_create_from_pointer_copy(data2, 4);
  buffer_t* notResult = buffer_not(buf1);
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(buffer_get_index(buf2, i), 0);
  }
  EXPECT_EQ(buffer_compare(notResult, buf2), 0);
  buffer_t* xorResult = buffer_xor(buf1, buf2);
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(buffer_get_index(xorResult, i), 255);
  }
  buffer_t* xorResult2 = buffer_xor(xorResult, buf1);
  EXPECT_EQ(buffer_compare(xorResult2, buf2), 0);
  uint8_t data3[5] ={0,0,0,0,1};
  buffer_t* buf3 = buffer_create_from_pointer_copy(data3, 5);
  EXPECT_EQ(buffer_compare(buf3, buf2), 1);
  EXPECT_EQ(buffer_compare(buf2, buf3), -1);

  buffer_t* andResult = buffer_and(buf1, buf2);
  EXPECT_EQ(buffer_compare(andResult, buf2), 0);
  buffer_t* orResult = buffer_or(buf1, buf2);
  EXPECT_EQ(buffer_compare(orResult, buf1), 0);
  buffer_t* sliceResult = buffer_slice(buf3, 3,5);
  EXPECT_EQ(sliceResult->size, 2);
  EXPECT_EQ(buffer_get_index(sliceResult, 0), 0);
  EXPECT_EQ(buffer_get_index(sliceResult, 1), 1);

  buffer_t* concatResult = buffer_concat(buf1, buf2);
  for (size_t i = 0; i < buf1->size; i++) {
    EXPECT_EQ(buffer_get_index(concatResult, i), buffer_get_index(buf1,i));
  }

  for (size_t i = buf1->size; i < buf1->size + buf2->size; i++) {
    EXPECT_EQ(buffer_get_index(concatResult, i), buffer_get_index(buf2,i - buf1->size));
  }
  buffer_destroy(concatResult);
  buffer_destroy(buf1);
  buffer_destroy(buf2);
  buffer_destroy(buf3);
  buffer_destroy(notResult);
  buffer_destroy(xorResult);
  buffer_destroy(xorResult2);
  buffer_destroy(andResult);
  buffer_destroy(orResult);
  buffer_destroy(sliceResult);
}

TEST(TestFRand, TestFRandFunction) {
  uint8_t* rand = frand(20);

  buffer_t* buf1 = buffer_create_from_existing_memory((uint8_t *) get_clear_memory(20), 20);
  buffer_t* buf2 = buffer_create_from_existing_memory(rand, 20);
  buffer_t* buf3 = buffer_concat(buf1, buf2);

  buffer_destroy(buf1);
  buffer_destroy(buf2);
  buffer_destroy(buf3);
}

TEST(TestBlock, TestBlockOperations) {
  uint8_t data[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*) &data, 20);
  block_t* block1 = block_create(buf);
  block_t* block2 = block_create_random_block();
  block_t* block3 = block_xor(block1, block2);
  block_t* block4 = block_xor(block3, block2);
  EXPECT_EQ(buffer_compare(block1->data, block4->data), 0);

  buffer_t* hash1 = hash_data(block1->data);
  buffer_t* hash2 = hash_data(block1->data);
  buffer_t* hash3 = hash_data(block4->data);
  EXPECT_EQ(buffer_compare(hash1, hash2), 0);
  EXPECT_EQ(buffer_compare(hash3, hash2), 0);
  buffer_destroy(hash1);
  buffer_destroy(hash2);
  buffer_destroy(hash3);
  for (size_t i = 0; i < block1->data->size; i++) {
    EXPECT_EQ(buffer_get_index(block1->data, i), buffer_get_index(block4->data, i));
  }
  EXPECT_EQ(buffer_compare(block1->data, block4->data), 0);
  EXPECT_EQ(buffer_compare(block1->hash, block4->hash), 0);
  block_destroy(block4);
  block_destroy(block3);
  block_destroy(block2);
  block_destroy(block1);
  buffer_destroy(buf);
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
int littleEndian() {
  int n = 1;
  if(*(char *)&n == 1) {
    return 1;
  } else {
    return 0;
  }
}
TEST(TestIndex, TestBitFunctions) {
  uint8_t data[1] = {1};
  buffer_t* buf = buffer_create_from_existing_memory(data, 1);
  if (littleEndian()) {
    EXPECT_EQ(get_bit(buf, 0), 1);
    EXPECT_EQ(get_bit(buf, 1), 0);
    EXPECT_EQ(get_bit(buf, 2), 0);
    EXPECT_EQ(get_bit(buf, 3), 0);
    EXPECT_EQ(get_bit(buf, 4), 0);
    EXPECT_EQ(get_bit(buf, 5), 0);
    EXPECT_EQ(get_bit(buf, 6), 0);
    EXPECT_EQ(get_bit(buf, 7), 0);
  } else {
    EXPECT_EQ(get_bit(buf, 0), 0);
    EXPECT_EQ(get_bit(buf, 1), 0);
    EXPECT_EQ(get_bit(buf, 2), 0);
    EXPECT_EQ(get_bit(buf, 3), 0);
    EXPECT_EQ(get_bit(buf, 4), 0);
    EXPECT_EQ(get_bit(buf, 5), 0);
    EXPECT_EQ(get_bit(buf, 6), 0);
    EXPECT_EQ(get_bit(buf, 7), 1);
  }
}

TEST(TestIndex, TestIndexEntry) {
  block_t* block = block_create_random_block_by_type(nano);
  index_entry_t* entry = index_entry_create(block->hash);
  EXPECT_EQ(buffer_compare(entry->hash, block->hash), 0);
  EXPECT_EQ(entry->hash, block->hash);

  for (int i = 0; i < 1000; i++) {
    index_entry_increment(entry);
  }

  EXPECT_EQ(entry->counter.fib, 14);
  EXPECT_EQ(entry->counter.count, 377);
  uint64_t now = (uint64_t) time(NULL);
  index_entry_set_ejection_date(entry, now);
  EXPECT_EQ(entry->ejection_date, now);
  block_destroy(block);
  index_entry_destroy(entry);
}


TEST(TestIndex, TestIndexFunctions) {

  block_t* block1 = block_create_random_block_by_type(nano);
  block_t* block2 = block_create_random_block_by_type(nano);
  block_t* block3 = block_create_random_block_by_type(nano);
  block_t* block4 = block_create_random_block_by_type(nano);
  block_t* block5 = block_create_random_block_by_type(nano);
  block_t* block6 = block_create_random_block_by_type(nano);
  block_t* block7 = block_create_random_block_by_type(nano);
  block_t* block8 = block_create_random_block_by_type(nano);

  index_entry_t* entry1 = index_entry_create(block1->hash);
  index_entry_t* entry2 = index_entry_create(block2->hash);
  index_entry_t* entry3 = index_entry_create(block3->hash);
  index_entry_t* entry4 = index_entry_create(block4->hash);
  index_entry_t* entry5 = index_entry_create(block5->hash);
  index_entry_t* entry6 = index_entry_create(block6->hash);
  index_entry_t* entry7 = index_entry_create(block7->hash);
  index_entry_t* entry8 = index_entry_create(block8->hash);

  index_t* index = index_create(3);

  index_add(index, entry1);
  index_add(index, entry2);
  index_add(index, entry3);
  index_add(index, entry4);
  index_add(index, entry5);
  index_add(index, entry6);
  index_add(index, entry7);
  index_add(index, entry8);

  index_entry_t* _entry1 = index_get(index, block1->hash);
  index_entry_t* _entry2 = index_get(index, block2->hash);
  index_entry_t* _entry3 = index_get(index, block3->hash);
  index_entry_t* _entry4 = index_get(index, block4->hash);
  index_entry_t* _entry5 = index_get(index, block5->hash);
  index_entry_t* _entry6 = index_get(index, block6->hash);
  index_entry_t* _entry7 = index_get(index, block7->hash);
  index_entry_t* _entry8 = index_get(index, block8->hash);

  EXPECT_EQ(buffer_compare(_entry1->hash, entry1->hash), 0);
  EXPECT_EQ(buffer_compare(_entry2->hash, entry2->hash), 0);
  EXPECT_EQ(buffer_compare(_entry3->hash, entry3->hash), 0);
  EXPECT_EQ(buffer_compare(_entry4->hash, entry4->hash), 0);
  EXPECT_EQ(buffer_compare(_entry5->hash, entry5->hash), 0);
  EXPECT_EQ(buffer_compare(_entry6->hash, entry6->hash), 0);
  EXPECT_EQ(buffer_compare(_entry7->hash, entry7->hash), 0);
  EXPECT_EQ(buffer_compare(_entry8->hash, entry8->hash), 0);

  index_remove(index, block1->hash);
  index_remove(index, block2->hash);
  index_remove(index, block3->hash);
  index_remove(index, block4->hash);
  index_remove(index, block5->hash);
  index_remove(index, block6->hash);
  index_remove(index, block7->hash);
  index_remove(index, block8->hash);


  _entry1 = index_get(index, block1->hash);
  _entry2 = index_get(index, block2->hash);
  _entry3 = index_get(index, block3->hash);
  _entry4 = index_get(index, block4->hash);
  _entry5 = index_get(index, block5->hash);
  _entry6 = index_get(index, block6->hash);
  _entry7 = index_get(index, block7->hash);
  _entry8 = index_get(index, block8->hash);

  EXPECT_TRUE(_entry1 == NULL);
  EXPECT_TRUE(_entry2 == NULL);
  EXPECT_TRUE(_entry3 == NULL);
  EXPECT_TRUE(_entry4 == NULL);
  EXPECT_TRUE(_entry5 == NULL);
  EXPECT_TRUE(_entry6 == NULL);
  EXPECT_TRUE(_entry7 == NULL);
  EXPECT_TRUE(_entry8 == NULL);

  index_entry_destroy(entry1);
  index_entry_destroy(entry2);
  index_entry_destroy(entry3);
  index_entry_destroy(entry4);
  index_entry_destroy(entry5);
  index_entry_destroy(entry6);
  index_entry_destroy(entry7);
  index_entry_destroy(entry8);


  block_destroy(block1);
  block_destroy(block2);
  block_destroy(block3);
  block_destroy(block4);
  block_destroy(block5);
  block_destroy(block6);
  block_destroy(block7);
  block_destroy(block8);

  index_destroy(index);


}