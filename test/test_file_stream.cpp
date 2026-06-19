#include <gtest/gtest.h>
#include <functional>
#include <gmock/gmock.h>
#include <future>
#include <atomic>
extern "C" {
#include "../src/Streams/file-stream.h"
#include "../src/Streams/stream.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Configuration/config.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/allocator.h"
}

//
// Wrapper around std::promise<void> that makes set_value / set_exception
// idempotent. Both `close_event` and `error_event` may fire on the same
// stream under various lifecycle orderings (notably on Windows where the
// file-close path emits both a close and an error if the close happens
// while a write is in flight). Subscribing both events to a single
// promise is convenient but the second set_* call would throw
// `future_error(promise_already_satisfied)` from inside the stream's
// actor thread. The compare_exchange guard here turns the second call
// into a silent no-op so either ordering produces the same test outcome.
//
struct PromiseOnce {
  std::promise<void> promise;
  std::atomic<bool> done{false};
  void set_value() {
    bool expected = false;
    if (done.compare_exchange_strong(expected, true)) {
      promise.set_value();
    }
  }
  void set_exception(std::exception_ptr e) {
    bool expected = false;
    if (done.compare_exchange_strong(expected, true)) {
      promise.set_exception(e);
    }
  }
};

using ::testing::_;
using ::testing::MockFunction;
using ::testing::AtLeast;
namespace PushFileStream {
  class TestPushFileStream : public testing::Test {
  public:
    scheduler_pool_t* pool;
    PromiseOnce r_close_promise;
    std::promise<void> r_complete_promise;
    PromiseOnce w_close_promise;
    MockFunction<void(void*, void*)> mock_data_callback;
    void SetUp() override {
      pool = scheduler_pool_create(4);
      scheduler_pool_start(pool);
    }
    void TearDown() override {
      scheduler_pool_wait_for_idle(pool);
      scheduler_pool_stop(pool);
      scheduler_pool_destroy(pool);
    }
  };
  void on_data(void* ctx, buffer_t* buffer) {
    REFERENCE(buffer, buffer_t);
    auto testFS = static_cast<TestPushFileStream*>(ctx);
    testFS->mock_data_callback.Call(ctx, (void*) buffer);
    DESTROY(buffer, buffer);
  }
  void on_close_r(void* ctx, void*) {
    auto testFS = static_cast<TestPushFileStream*>(ctx);
    testFS->r_close_promise.set_value();
  }
  void on_complete_r(void* ctx, void*) {
    auto testFS = static_cast<TestPushFileStream*>(ctx);
    testFS->r_complete_promise.set_value();
  }
  void on_close_w(void* ctx, void*) {
    auto testFS = static_cast<TestPushFileStream*>(ctx);
    testFS->w_close_promise.set_value();
  }
  void on_error_r(void* ctx,  async_error_t* error) {
    auto testFS = static_cast<TestPushFileStream*>(ctx);
    try {
      throw std::runtime_error((const char*)error->message);
    } catch(...) {
      testFS->r_close_promise.set_exception(std::current_exception());
    }
  }
  void on_error_w(void* ctx,  async_error_t* error) {
    auto testFS = static_cast<TestPushFileStream*>(ctx);
    try {
      throw std::runtime_error((const char*)error->message);
    } catch(...) {
      testFS->w_close_promise.set_exception(std::current_exception());
    }
  }
  TEST_F(TestPushFileStream, TestPushFileStreamPipe) {
    int error_code;
    std::string filename = "./test.pdf";
    EXPECT_CALL(mock_data_callback, Call(_,_)).Times(12);
    readable_push_file_stream_t* rs = readable_push_file_stream_create(pool, (char*) filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
    EXPECT_EQ(error_code, 0);
    if(error_code != 0) {
      GTEST_FATAL_FAILURE_("Stream Creation error");
    }
    stream_subscribe((stream_t*) rs, error_event, this, (void(*)(void*, void*)) on_error_r, NULL);
    stream_subscribe((stream_t*) rs, close_event, this, on_close_r, NULL );
    stream_subscribe((stream_t*) rs, complete_event, this, (void(*)(void*, void*)) on_complete_r, NULL);
    stream_subscribe((stream_t*) rs, data_event, this, (void(*)(void*, void*)) on_data, NULL);

    std::string filename2 = "./test2.pdf";
    writeable_push_file_stream_t*  ws = writeable_push_file_stream_create(pool, (char*) filename2.c_str());
    stream_subscribe((stream_t*) ws, error_event, this, (void(*)(void*, void*)) on_error_w, NULL);
    stream_subscribe((stream_t*) ws, close_event, this, (void(*)(void*, void*)) on_close_w, NULL);
    readable_push_stream_pipe((stream_t*) rs, (stream_t*) ws);

    std::future<void> r_close_future = r_close_promise.promise.get_future();
    std::future<void> w_close_future = w_close_promise.promise.get_future();
    try {
      w_close_future.get();
    } catch (const std::exception& e) {
      GTEST_FATAL_FAILURE_(e.what());
    }
    scheduler_pool_wait_for_idle(pool);
    DESTROY(rs, readable_push_file_stream);
    DESTROY(ws, writeable_push_file_stream);
  }
}

namespace PullFileStream {
  class TestPullFileStream : public testing::Test {
  public:
    scheduler_pool_t* pool;
    PromiseOnce r_close_promise;
    std::promise<void> r_complete_promise;
    PromiseOnce w_close_promise;
    MockFunction<void(void*, void*)> mock_data_callback;
    void SetUp() override {
      pool = scheduler_pool_create(4);
      scheduler_pool_start(pool);
    }
    void TearDown() override {
      scheduler_pool_wait_for_idle(pool);
      scheduler_pool_stop(pool);
      scheduler_pool_destroy(pool);
    }
  };
  void on_data(void* ctx, buffer_t* buffer) {
    REFERENCE(buffer, buffer_t);
    auto testFS = static_cast<TestPullFileStream*>(ctx);
    testFS->mock_data_callback.Call(ctx, (void*) buffer);
    DESTROY(buffer, buffer);
  }
  void on_close_r(void* ctx, void*) {
    auto testFS = static_cast<TestPullFileStream*>(ctx);
    testFS->r_close_promise.set_value();
  }
  void on_complete_r(void* ctx, void*) {
    auto testFS = static_cast<TestPullFileStream*>(ctx);
    testFS->r_complete_promise.set_value();
  }
  void on_close_w(void* ctx, void*) {
    auto testFS = static_cast<TestPullFileStream*>(ctx);
    testFS->w_close_promise.set_value();
  }
  void on_error_r(void* ctx,  async_error_t* error) {
    auto testFS = static_cast<TestPullFileStream*>(ctx);
    try {
      throw std::runtime_error((const char*)error->message);
    } catch(...) {
      testFS->r_close_promise.set_exception(std::current_exception());
    }
  }
  void on_error_w(void* ctx,  async_error_t* error) {
    auto testFS = static_cast<TestPullFileStream*>(ctx);
    try {
      throw std::runtime_error((const char*)error->message);
    } catch(...) {
      testFS->w_close_promise.set_exception(std::current_exception());
    }
  }

  TEST_F(TestPullFileStream, TestPullFileStreamPipe) {
    int error_code;
    std::string filename = "./test.pdf";
    EXPECT_CALL(mock_data_callback, Call(_,_)).Times(12);
    readable_pull_file_stream_t* rs = readable_pull_file_stream_create(pool, (char*) filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
    EXPECT_EQ(error_code, 0);
    if(error_code != 0) {
      GTEST_FATAL_FAILURE_("Stream Creation error");
    }
    stream_subscribe((stream_t*) rs, error_event, this, (void(*)(void*, void*)) on_error_r, NULL);
    stream_subscribe((stream_t*) rs, close_event, this, on_close_r, NULL );
    stream_subscribe((stream_t*) rs, complete_event, this, (void(*)(void*, void*)) on_complete_r, NULL);
    stream_subscribe((stream_t*) rs, data_event, this, (void(*)(void*, void*)) on_data, NULL);

    std::string filename2 = "./test2.pdf";
    writeable_pull_file_stream_t*  ws = writeable_pull_file_stream_create(pool, (char*) filename2.c_str());
    stream_subscribe((stream_t*) ws, error_event, this, (void(*)(void*, void*)) on_error_w, NULL);
    stream_subscribe((stream_t*) ws, close_event, this, (void(*)(void*, void*)) on_close_w, NULL);
    writeable_pull_stream_pipe((stream_t*) ws, (stream_t*) rs);

    std::future<void> w_close_future = w_close_promise.promise.get_future();
    try {
      w_close_future.get();
    } catch (const std::exception& e) {
      GTEST_FATAL_FAILURE_(e.what());
    }
    scheduler_pool_wait_for_idle(pool);
    stream_unsubscribe_pipe_notifiers((stream_t*)rs);
    stream_unsubscribe_pipe_notifiers((stream_t*)ws);
    DESTROY(ws, writeable_pull_file_stream);
    DESTROY(rs, readable_pull_file_stream);
  }
};