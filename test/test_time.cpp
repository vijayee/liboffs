//
// Created by victor on 6/16/25.
//

#include <gtest/gtest.h>
#include <functional>
#include <gmock/gmock.h>
extern "C" {
#include "../src/Time/ticker.h"
#include "../src/Time/wheel.h"
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
    ticker_start(ticker, 2000000000);
  }

  class TestTimingWheel : public testing::Test {
  public:
    work_pool_t *pool;
    MockFunction<void((void * ))> mockExecuteCallback;
    MockFunction<void((void * ))> mockAbortCallback;
    MockFunction<void((void*))> mockShutdownCallback;
    MockFunction<void((void*))> mockShutdownAbortCallback;
  };

  void onExecute(void *ctx) {
    printf("timer fired\n");
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
    //int corecnt = platform_core_count();
    EXPECT_CALL(mockExecuteCallback, Call(_)).Times(5);
    EXPECT_CALL(mockShutdownCallback, Call(_)).Times(1);
    EXPECT_CALL(mockShutdownAbortCallback, Call(_)).Times(0);
    pool = work_pool_create(4);
    work_pool_launch(pool);
    timing_wheel_t* wheel = timing_wheel_create(Time_Milliseconds, 16, pool);
    wheel->simulated = 0;
    timing_wheel_run(wheel);
    uint64_t timer1 = timing_wheel_set_timer(wheel, this,onExecute, onAbort, 20 * Time_Milliseconds);
    uint64_t timer2 = timing_wheel_set_timer(wheel, this,onExecute, onAbort, 200 * Time_Milliseconds);
    uint64_t timer3 = timing_wheel_set_timer(wheel, this,onExecute, onAbort, 500 * Time_Milliseconds);
    uint64_t timer4 = timing_wheel_set_timer(wheel, this,onExecute, onAbort, 500 * Time_Milliseconds);
    uint64_t timer5 = timing_wheel_set_timer(wheel, this,onExecute, onAbort, 2000 * Time_Milliseconds);
    uint64_t timer8 = timing_wheel_set_timer(wheel, this, Shutdown, ShutdownAborted, 2 * Time_Seconds);
    work_pool_wait_for_shutdown_signal(pool);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    pool= NULL;

  }
}