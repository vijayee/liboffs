#include <gtest/gtest.h>
#include <atomic>
#include <future>
extern "C" {
#include "../src/Streams/stream.h"
#include "../src/Streams/file-stream.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/error.h"
}

//
// Same idempotent promise wrapper as test_file_stream.cpp. close_event and
// error_event may both fire on the same stream; the second set_value /
// set_exception would otherwise throw `promise_already_satisfied`.
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

namespace StreamActorTests {

class TestStreamActor : public testing::Test {
public:
  scheduler_pool_t* pool;
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

static void on_data_count(void* ctx, void*) {
  auto* count = static_cast<std::atomic<int>*>(ctx);
  count->fetch_add(1);
}

static void on_close_set_promise(void* ctx, void*) {
  auto* prom = static_cast<PromiseOnce*>(ctx);
  prom->set_value();
}

static void on_error_set_promise(void* ctx, async_error_t* error) {
  auto* prom = static_cast<PromiseOnce*>(ctx);
  try {
    throw std::runtime_error((const char*)error->message);
  } catch (...) {
    prom->set_exception(std::current_exception());
  }
}

TEST_F(TestStreamActor, TestPushFileStreamActorDispatch) {
  int error_code;
  std::string filename = "./test.pdf";
  readable_push_file_stream_t* rs = readable_push_file_stream_create(
      pool, (char*)filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
  ASSERT_EQ(error_code, 0);

  std::atomic<int> data_count{0};
  PromiseOnce close_promise;

  stream_subscribe((stream_t*)rs, data_event, &data_count,
                   (void(*)(void*, void*))on_data_count, NULL);
  stream_subscribe((stream_t*)rs, close_event, &close_promise,
                   on_close_set_promise, NULL);
  stream_subscribe((stream_t*)rs, error_event, &close_promise,
                   (void(*)(void*, void*))on_error_set_promise, NULL);

  auto close_future = close_promise.promise.get_future();
  close_future.wait();

  scheduler_pool_wait_for_idle(pool);

  EXPECT_GT(data_count.load(), 0);

  DESTROY(rs, readable_push_file_stream);
}

TEST_F(TestStreamActor, TestPullFileStreamActorDispatch) {
  int error_code;
  std::string filename = "./test.pdf";
  readable_pull_file_stream_t* rs = readable_pull_file_stream_create(
      pool, (char*)filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
  ASSERT_EQ(error_code, 0);

  std::string out_filename = "./test_pull_actor_out.pdf";
  writeable_pull_file_stream_t* ws = writeable_pull_file_stream_create(
      pool, (char*)out_filename.c_str());

  std::atomic<int> data_count{0};
  PromiseOnce w_close_promise;

  stream_subscribe((stream_t*)rs, data_event, &data_count,
                   (void(*)(void*, void*))on_data_count, NULL);
  stream_subscribe((stream_t*)ws, close_event, &w_close_promise,
                   on_close_set_promise, NULL);
  stream_subscribe((stream_t*)ws, error_event, &w_close_promise,
                   (void(*)(void*, void*))on_error_set_promise, NULL);

  writeable_pull_stream_pipe((stream_t*)ws, (stream_t*)rs);

  auto w_close_future = w_close_promise.promise.get_future();
  w_close_future.wait();

  scheduler_pool_wait_for_idle(pool);

  EXPECT_GT(data_count.load(), 0);

  stream_unsubscribe_pipe_notifiers((stream_t*)rs);
  stream_unsubscribe_pipe_notifiers((stream_t*)ws);
  DESTROY(ws, writeable_pull_file_stream);
  DESTROY(rs, readable_pull_file_stream);
}

TEST_F(TestStreamActor, TestStreamNotifyManyHandlers) {
  int error_code;
  std::string filename = "./test.pdf";
  readable_push_file_stream_t* rs = readable_push_file_stream_create(
      pool, (char*)filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
  ASSERT_EQ(error_code, 0);

  std::atomic<int> handler_count{0};
  static constexpr int NUM_HANDLERS = 20;

  for (int i = 0; i < NUM_HANDLERS; i++) {
    stream_subscribe((stream_t*)rs, data_event, &handler_count,
                     (void(*)(void*, void*))on_data_count, NULL);
  }

  PromiseOnce close_promise;
  stream_subscribe((stream_t*)rs, close_event, &close_promise,
                   on_close_set_promise, NULL);
  stream_subscribe((stream_t*)rs, error_event, &close_promise,
                   (void(*)(void*, void*))on_error_set_promise, NULL);

  auto close_future = close_promise.promise.get_future();
  close_future.wait();

  scheduler_pool_wait_for_idle(pool);

  EXPECT_GT(handler_count.load(), 0);

  DESTROY(rs, readable_push_file_stream);
}

TEST_F(TestStreamActor, TestIdleSignalWaitsForCompletion) {
  int error_code;
  std::string filename = "./test.pdf";
  readable_push_file_stream_t* rs = readable_push_file_stream_create(
      pool, (char*)filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
  ASSERT_EQ(error_code, 0);

  std::atomic<int> data_count{0};
  PromiseOnce close_promise;
  stream_subscribe((stream_t*)rs, data_event, &data_count,
                   (void(*)(void*, void*))on_data_count, NULL);
  stream_subscribe((stream_t*)rs, close_event, &close_promise,
                   on_close_set_promise, NULL);
  stream_subscribe((stream_t*)rs, error_event, &close_promise,
                   (void(*)(void*, void*))on_error_set_promise, NULL);

  auto close_future = close_promise.promise.get_future();
  close_future.wait();

  scheduler_pool_wait_for_idle(pool);

  EXPECT_GT(data_count.load(), 0);
  DESTROY(rs, readable_push_file_stream);
}

TEST_F(TestStreamActor, TestPushPipeEndToEnd) {
  int error_code;
  std::string filename = "./test.pdf";
  readable_push_file_stream_t* rs = readable_push_file_stream_create(
      pool, (char*)filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
  ASSERT_EQ(error_code, 0);

  std::string out_filename = "./test_pipe_actor_out.pdf";
  writeable_push_file_stream_t* ws = writeable_push_file_stream_create(
      pool, (char*)out_filename.c_str());

  PromiseOnce w_close_promise;
  stream_subscribe((stream_t*)ws, close_event, &w_close_promise,
                   on_close_set_promise, NULL);
  stream_subscribe((stream_t*)ws, error_event, &w_close_promise,
                   (void(*)(void*, void*))on_error_set_promise, NULL);

  readable_push_stream_pipe((stream_t*)rs, (stream_t*)ws);

  auto w_close_future = w_close_promise.promise.get_future();
  w_close_future.wait();

  scheduler_pool_wait_for_idle(pool);

  DESTROY(rs, readable_push_file_stream);
  DESTROY(ws, writeable_push_file_stream);
}

}