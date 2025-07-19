//
// Created by victor on 6/16/25.
//

#include <gtest/gtest.h>
#include <functional>
#include <gmock/gmock.h>
extern "C" {
#include "../src/Time/ticker.h"
#include "../src/Time/wheel.h"
#include "../src/Time/debouncer.h"
}
using ::testing::_;
using ::testing::MockFunction;
namespace timeTest {

  class TestTicker : public testing::Test {
  public:
    MockFunction<void((void * ))> mockCallback;
  };

  void callbackWrapper(void *ctx) {
    auto test = static_cast<TestTicker *>(ctx);
    test->mockCallback.Call(ctx);
  }

  TEST_F(TestTicker, TestTickerExecute) {
    ticker_t ticker = {0};
    ticker.cb = callbackWrapper;
    ticker.ctx = this;
    EXPECT_CALL(mockCallback, Call(_)).Times(1);
#if _WIN32
    ticker_start(ticker, 2000);
#else
    ticker_start(ticker, 2000000);
#endif
  }

  class TestTimingWheel : public testing::Test {
  public:
    work_pool_t* pool;
    MockFunction<void((void * ))> mockExecuteCallback;
    MockFunction<void((void * ))> mockAbortCallback;
    MockFunction<void((void*))> mockShutdownCallback;
    MockFunction<void((void*))> mockShutdownAbortCallback;
  };

  void onExecute(void *ctx) {
    auto test = static_cast<TestTimingWheel *>(ctx);
    test->mockExecuteCallback.Call(ctx);
  }

  void onAbort(void *ctx) {
    auto test = static_cast<TestTimingWheel *>(ctx);
    test->mockAbortCallback.Call(ctx);
  }

  void Shutdown(void* ctx) {
    auto test = static_cast<TestTimingWheel*>(ctx);
    platform_signal_condition(&test->pool->shutdown);
    test->mockShutdownCallback.Call(ctx);
  }
  void ShutdownAborted(void* ctx) {
    auto test = static_cast<TestTimingWheel*>(ctx);
    test->mockShutdownAbortCallback.Call(ctx);
  }

  TEST_F(TestTimingWheel, TestTimingWheelFunctions) {
    EXPECT_CALL(mockExecuteCallback, Call(_)).Times(6);
    EXPECT_CALL(mockShutdownCallback, Call(_)).Times(0);
    EXPECT_CALL(mockShutdownAbortCallback, Call(_)).Times(0);
    pool = work_pool_create(4);
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_simulate(wheel);
    hierarchical_timing_wheel_run(wheel);
    uint64_t timer1 = hierarchical_timing_wheel_set_timer(wheel, this, onExecute, onAbort, {.milliseconds = 20});
    uint64_t timer2 = hierarchical_timing_wheel_set_timer(wheel, this, onExecute, onAbort, {.milliseconds = 200});
    uint64_t timer3 = hierarchical_timing_wheel_set_timer(wheel, this, onExecute, onAbort, {.milliseconds = 500});
    uint64_t timer4 = hierarchical_timing_wheel_set_timer(wheel, this, onExecute, onAbort, {.milliseconds= 500});
    uint64_t timer5 = hierarchical_timing_wheel_set_timer(wheel, this, onExecute, onAbort, {.milliseconds = 2300});
    uint64_t timer6 = hierarchical_timing_wheel_set_timer(wheel, this, onExecute, onAbort, {.minutes = 1});
    uint64_t timer7 = hierarchical_timing_wheel_set_timer(wheel, this, onExecute, onAbort, {.minutes = 3});
    hierarchical_timing_wheel_cancel_timer(wheel, timer7);
    //uint64_t timer8 = hierarchical_timing_wheel_set_timer(wheel, this, Shutdown, ShutdownAborted, {.minutes= 1, .seconds = 3});
    //work_pool_wait_for_shutdown_signal(pool);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
    pool= NULL;
  }

  class TestDebouncer : public testing::Test {
  public:
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;
    MockFunction<void((void*))> mockExecuteCallback;
    MockFunction<void((void*))> mockAbortCallback;
    MockFunction<void((void*))> mockShutdownCallback;
    MockFunction<void((void*))> mockShutdownAbortCallback;
  };

  void onDebouncerExecute(void* ctx) {
    auto test = static_cast<TestDebouncer *>(ctx);
    test->mockExecuteCallback.Call(ctx);
  }

  void onDebouncerAbort(void* ctx) {
    auto test = static_cast<TestDebouncer *>(ctx);
    test->mockAbortCallback.Call(ctx);
    platform_signal_condition(&test->pool->shutdown);
  }
  void ShutdownDebouncer(void* ctx) {
    auto test = static_cast<TestDebouncer*>(ctx);
    hierarchical_timing_wheel_stop(test->wheel);
    platform_signal_condition(&test->pool->shutdown);
    test->mockShutdownCallback.Call(ctx);
  }
  void ShutdownDebouncerAborted(void* ctx) {
    auto test = static_cast<TestTimingWheel*>(ctx);
    test->mockShutdownAbortCallback.Call(ctx);
  }

  TEST_F(TestDebouncer, TestDebounceFunctions) {
    EXPECT_CALL(mockExecuteCallback, Call(_)).Times(2);
    EXPECT_CALL(mockShutdownCallback, Call(_)).Times(0);
    pool = work_pool_create(4);
    work_pool_launch(pool);
    wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    uint64_t wait = 200;
    uint64_t max_wait = 5000;
    debouncer_t* debouncer1 = debouncer_create(wheel, this, onDebouncerExecute, onDebouncerAbort, wait, max_wait);
    debouncer_t* debouncer2 = debouncer_create(wheel, this, onDebouncerExecute, onDebouncerAbort, wait, 0);
    //uint64_t timer = hierarchical_timing_wheel_set_timer(wheel, this, ShutdownDebouncer, ShutdownDebouncerAborted, { .seconds = 6});
    timeval_t start;
    timeval_t end;
    get_time(&start);
    get_time(&end);
    uint64_t elapsed = elapsed_time(start, end);
    while (elapsed < max_wait) {
      usleep(1000);
      debouncer_debounce(debouncer1);
      debouncer_debounce(debouncer2);
      get_time(&end);
      elapsed = elapsed_time(start, end);
    }
    while (elapsed < 5800) {
      debouncer_debounce(debouncer2);
      get_time(&end);
      elapsed = elapsed_time(start, end);
    }
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    hierarchical_timing_wheel_destroy(wheel);
    work_pool_destroy(pool);
    debouncer_destroy(debouncer1);
    debouncer_destroy(debouncer2);
    pool= NULL;
  }
  TEST(TestElapsedTime, TestTimeElapsed) {
    timeval_t start, end;
    get_time(&start);
    sleep(3);
    get_time(&end);

    uint64_t elapsed = elapsed_time(start, end);
    EXPECT_EQ(elapsed, 3000);
  }
}
